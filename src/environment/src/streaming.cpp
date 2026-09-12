// Field tiles bound to world cells and driven through the shared residency policy. Task 1.3.

#include <cy/environment/streaming.h>

#include <algorithm>
#include <cmath>

namespace cy::environment {
namespace {

[[nodiscard]] i64 floor_div(i64 value, i64 divisor) noexcept {
    const i64 quotient = value / divisor;
    return ((value % divisor != 0) && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

[[nodiscard]] i64 ifloor(f64 value) noexcept {
    return static_cast<i64>(std::floor(value));
}

/// The tile a metre coordinate falls in, at one level's cell size.
[[nodiscard]] i64 tile_of(f64 metres, f64 cell_metres) noexcept {
    return floor_div(ifloor(metres / cell_metres), static_cast<i64>(kTileCells));
}

}  // namespace

u64 page_of_tile(u32 field_slot, const TileAddress& address) noexcept {
    // 1 + 10 + 2 + 1 + 21 + 21 = 55 bits under `kFieldPageBit`, which is bit 55, so the whole
    // identifier fits the 56 bits `residency::PageKey` gives a subsystem with nothing to spare and
    // nothing wasted. The coordinates are biased rather than sign-extended so that comparing two
    // packed identifiers orders them the way comparing two tiles would.
    const auto biased_x =
        static_cast<u64>(static_cast<i64>(address.x) + kMaxPageTile) & 0x1F'FFFFULL;
    const auto biased_z =
        static_cast<u64>(static_cast<i64>(address.z) + kMaxPageTile) & 0x1F'FFFFULL;
    return kFieldPageBit | (static_cast<u64>(field_slot & 0x3FFU) << 45U) |
           (static_cast<u64>(address.level & 0x3U) << 43U) |
           (static_cast<u64>(address.layer & 0x1U) << 42U) | (biased_x << 21U) | biased_z;
}

Status cell_tile_footprint(const world::PartitionConfig& partition, const world::CellCoord& coord,
                           const FieldDeclaration& declaration, FieldId field, u8 level,
                           Array<TileAddress>& out) noexcept {
    if (level >= kFieldResidencyCount || !declaration.levels[level].declared()) {
        return fail(ErrorCode::InvalidArgument,
                    "environment: this field does not declare that residency level");
    }
    const world::WorldVec3d origin = world::simulation_origin(partition, coord);
    const f64 size = partition.cell_size(coord.level);
    const f64 cell_metres = static_cast<f64>(declaration.levels[level].cell_metres);

    const i64 min_x = tile_of(origin.x, cell_metres);
    const i64 min_z = tile_of(origin.z, cell_metres);
    // The cell's far edge belongs to the next cell, so the footprint is taken just inside it: a
    // cell whose extent is an exact multiple of the tile size must not claim the row of tiles its
    // neighbour owns.
    const f64 epsilon = cell_metres * 1.0e-6;
    const i64 max_x = tile_of(origin.x + size - epsilon, cell_metres);
    const i64 max_z = tile_of(origin.z + size - epsilon, cell_metres);

    for (i64 z = min_z; z <= max_z; ++z) {
        for (i64 x = min_x; x <= max_x; ++x) {
            TileAddress address;
            address.field = field;
            address.level = level;
            address.layer = static_cast<u8>(FieldLayer::Base);
            address.x = static_cast<i32>(x);
            address.z = static_cast<i32>(z);
            if (Status pushed = out.push_back(address); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

FieldStreaming::FieldStreaming(Allocator& allocator, FieldStore& store,
                               residency::ResidencyServer& residency) noexcept
    : allocator_(&allocator),
      store_(&store),
      residency_(&residency),
      fields_(allocator),
      bindings_(allocator),
      cells_(allocator),
      drained_(allocator),
      wanted_(allocator),
      wanted_pages_(allocator),
      resident_(allocator),
      resident_pages_(allocator),
      load_buffer_(allocator) {
    schedule_.admissions = Array<residency::Admission>(allocator);
    schedule_.evictions = Array<residency::EvictionOrder>(allocator);
}

FieldStreaming::Adopted* FieldStreaming::find_field(FieldId field) noexcept {
    for (Adopted& adopted : fields_) {
        if (adopted.field == field) {
            return &adopted;
        }
    }
    return nullptr;
}

const FieldStreaming::Adopted* FieldStreaming::find_field(FieldId field) const noexcept {
    for (const Adopted& adopted : fields_.span()) {
        if (adopted.field == field) {
            return &adopted;
        }
    }
    return nullptr;
}

i32 FieldStreaming::slot_of(FieldId field) const noexcept {
    for (usize index = 0; index < fields_.size(); ++index) {
        if (fields_[index].field == field) {
            return static_cast<i32>(index);
        }
    }
    return -1;
}

Status FieldStreaming::adopt(ProducerToken&& token, FieldResidency finest) noexcept {
    if (!token.valid()) {
        return fail(ErrorCode::InvalidArgument, "environment: that producer token is empty");
    }
    const FieldDeclaration* declaration = store_->registry().declaration(token.field());
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    if (find_field(token.field()) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "environment: that field is already adopted");
    }
    if (fields_.size() >= 1024) {
        // The page identifier carries a ten-bit field slot; a world with more than a thousand
        // streamed fields needs a wider identifier, and finding that out here is better than
        // finding it out as two fields sharing a page.
        return fail(ErrorCode::OutOfRange,
                    "environment: a page identifier carries at most 1024 streamed fields");
    }
    Adopted adopted;
    adopted.token = std::move(token);
    adopted.field = adopted.token.field();
    adopted.finest = static_cast<u8>(finest);
    return fields_.push_back(std::move(adopted));
}

Status FieldStreaming::set_loader(FieldId field, TileLoader loader, void* user) noexcept {
    Adopted* adopted = find_field(field);
    if (adopted == nullptr) {
        return fail(ErrorCode::NotFound, "environment: that field is not adopted by this streamer");
    }
    adopted->loader = loader;
    adopted->user = user;
    return ok();
}

Status FieldStreaming::declare_cell(world::CellId cell, const world::CellCoord& coord) noexcept {
    if (!cell.is_valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "environment: a cell identifier of zero names no cell");
    }
    if (world::CellCoord* existing = cells_.find(cell); existing != nullptr) {
        *existing = coord;
        return ok();
    }
    Expected<world::CellCoord*, Error> inserted = cells_.insert(cell, coord);
    if (!inserted) {
        return make_unexpected(inserted.error());
    }
    return ok();
}

Status FieldStreaming::attach(world::CellEventQueue& events, u32 order) noexcept {
    if (!residency_->registered(residency::Subsystem::WorldCells)) {
        return fail(ErrorCode::Unavailable,
                    "environment: register residency::Subsystem::WorldCells with a budget before "
                    "attaching the field streamer — this module does not decide that budget");
    }
    Expected<world::CellEventQueue::ConsumerId, Error> consumer =
        events.add_consumer("environment-fields", order);
    if (!consumer) {
        return make_unexpected(consumer.error());
    }
    events_ = &events;
    consumer_ = consumer.value();
    return ok();
}

Status FieldStreaming::materialise_guaranteed(Adopted& adopted, const FieldDeclaration& declaration,
                                              const TileAddress& address, f64 now) noexcept {
    if (store_->is_resident(address)) {
        return ok();
    }
    load_buffer_.clear();
    const bool cooked = adopted.loader != nullptr &&
                        adopted.loader(adopted.user, address, load_buffer_).has_value();
    const Status inserted =
        cooked ? store_->insert_tile(adopted.token, address, load_buffer_.span(),
                                     /*guaranteed=*/true)
               : store_->insert_default_tile(adopted.token, address, /*guaranteed=*/true);
    if (!inserted) {
        return inserted;
    }

    // Reported to the policy, not requested from it. The bytes belong in the shared budget —
    // `residency`'s own `guaranteed` flag is what says they are not a candidate for eviction — and
    // a module that held them outside it would be the private pool the capability's "shared policy,
    // separate storage" split exists to prevent.
    Expected<residency::PageKey, Error> key = page_of(address);
    if (!key) {
        return make_unexpected(key.error());
    }
    residency::ResidentReport resident;
    resident.key = key.value();
    resident.bytes = FieldStore::tile_bytes(declaration);
    resident.guaranteed = true;
    resident.cost = residency::CostClass::Streamed;
    return residency_->note_resident(resident, now);
}

Status FieldStreaming::guarantee_macro(FieldId field, i32 min_tile_x, i32 min_tile_z,
                                       i32 max_tile_x, i32 max_tile_z, f64 now) noexcept {
    Adopted* adopted = find_field(field);
    if (adopted == nullptr) {
        return fail(ErrorCode::NotFound, "environment: that field is not adopted by this streamer");
    }
    const FieldDeclaration* declaration = store_->registry().declaration(field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    const auto level = static_cast<u8>(FieldResidency::Macro);
    if (!declaration->levels[level].declared() || !declaration->levels[level].resident_everywhere) {
        return fail(ErrorCode::InvalidArgument,
                    "environment: this field does not declare a macro level resident everywhere");
    }

    // Guaranteed tiles do not go through the policy's admission: they are the coarse fallback every
    // other answer is defined in terms of, and a budget that could refuse them would make the
    // specification's "the whole world has weather" scenario a matter of memory pressure.
    for (i32 z = min_tile_z; z <= max_tile_z; ++z) {
        for (i32 x = min_tile_x; x <= max_tile_x; ++x) {
            TileAddress address;
            address.field = field;
            address.level = level;
            address.layer = static_cast<u8>(FieldLayer::Base);
            address.x = x;
            address.z = z;
            if (Status placed = materialise_guaranteed(*adopted, *declaration, address, now);
                !placed) {
                return placed;
            }
        }
    }
    return ok();
}

Expected<residency::PageKey, Error> FieldStreaming::page_of(
    const TileAddress& address) const noexcept {
    const i32 slot = slot_of(address.field);
    if (slot < 0) {
        return fail(ErrorCode::NotFound, "environment: that field is not adopted by this streamer");
    }
    return residency::PageKey{residency::Subsystem::WorldCells,
                              page_of_tile(static_cast<u32>(slot), address)};
}

Status FieldStreaming::bind_cell(world::CellId cell, FieldStreamingReport& report) noexcept {
    const world::CellCoord* coord = cells_.find(cell);
    if (coord == nullptr) {
        // A cell nobody declared has no footprint this module can compute. Not an error: a world
        // may hold cells that carry no field channel at all, and refusing the whole tick for one of
        // them would make an unrelated cell's absence a streaming failure.
        return ok();
    }
    for (const Binding& binding : bindings_.span()) {
        if (binding.cell == cell) {
            return ok();
        }
    }

    Binding binding(*allocator_);
    binding.cell = cell;
    for (const Adopted& adopted : fields_.span()) {
        const FieldDeclaration* declaration = store_->registry().declaration(adopted.field);
        if (declaration == nullptr) {
            continue;
        }
        // The finest level this field streams, and every coarser declared one. A consumer that
        // needs coarse data does not force fine data resident, and a cell arriving does not force
        // the macro level to be re-requested — it is guaranteed and already there.
        for (u8 level = adopted.finest; level < kFieldResidencyCount; ++level) {
            if (!declaration->levels[level].declared() ||
                declaration->levels[level].resident_everywhere) {
                continue;
            }
            if (Status footprint = cell_tile_footprint(store_->partition(), *coord, *declaration,
                                                       adopted.field, level, binding.tiles);
                !footprint) {
                return footprint;
            }
        }
    }
    ++report.cells_bound;
    return bindings_.push_back(std::move(binding));
}

Status FieldStreaming::release_cell(world::CellId cell, FieldStreamingReport& report) noexcept {
    for (usize index = 0; index < bindings_.size(); ++index) {
        if (!(bindings_[index].cell == cell)) {
            continue;
        }
        // The tiles are not dropped here. "Evicted with them" is the policy's decision, and a tile
        // wanted by a neighbouring cell that is still bound must survive this one leaving — which
        // is exactly what dropping it here would break. What leaving does is stop asking, and the
        // next tick's eviction ordering is where a tile nobody wants goes.
        for (const TileAddress& address : bindings_[index].tiles.span()) {
            if (Expected<residency::PageKey, Error> key = page_of(address); key) {
                (void)residency_->set_active(key.value(), false);
            }
        }
        bindings_.remove_unordered(index);
        ++report.cells_released;
        return ok();
    }
    return ok();
}

Status FieldStreaming::consume_cell_events(FieldStreamingReport& report) noexcept {
    if (events_ == nullptr) {
        return ok();
    }
    drained_.clear();
    if (Status drained = events_->drain(consumer_, drained_); !drained) {
        return drained;
    }
    for (const world::CellEvent& event : drained_.span()) {
        switch (event.kind) {
            case world::CellEventKind::Resident:
            case world::CellEventKind::ChannelsChanged:
                // The channel mask is what decides. A cell cooked without the field channel is a
                // cell with no field data to stream, and binding it would ask the policy for tiles
                // nothing can load.
                if (Status changed = event.channels.has(world::Channel::Fields)
                                         ? bind_cell(event.cell, report)
                                         : release_cell(event.cell, report);
                    !changed) {
                    return changed;
                }
                break;
            case world::CellEventKind::Evicted:
            case world::CellEventKind::Failed:
                if (Status released = release_cell(event.cell, report); !released) {
                    return released;
                }
                break;
            case world::CellEventKind::Activated:
            case world::CellEventKind::Deactivated:
                // Activation is the ECS axis, not the residency one.
                // `world-partition-and-streaming` states them as separate axes and collapsing them
                // here would make a field's tiles depend on whether entities were published.
                break;
        }
    }
    return ok();
}

Status FieldStreaming::collect_wanted() noexcept {
    // Everything the bound cells want, in one order. One request per tile per tick whatever the
    // number of cells asking for it — the policy deduplicates too, and asking it to do so for a
    // hundred copies of one tile would be this module leaning on that.
    wanted_.clear();
    for (const Binding& binding : bindings_.span()) {
        if (Status appended = wanted_.append(binding.tiles.span()); !appended) {
            return appended;
        }
    }
    std::sort(wanted_.begin(), wanted_.end(),
              [](const TileAddress& a, const TileAddress& b) noexcept {
                  if (!(a.field == b.field)) {
                      return a.field.value < b.field.value;
                  }
                  if (a.level != b.level) {
                      return a.level < b.level;
                  }
                  if (a.z != b.z) {
                      return a.z < b.z;
                  }
                  return a.x < b.x;
              });
    return ok();
}

Status FieldStreaming::submit_requests(FieldStreamingReport& report) noexcept {
    // The page identifier of each wanted tile, computed alongside the request so that resolving an
    // admission back to a tile is a lookup rather than a second pass of the packing. A duplicate or
    // an unresolvable tile carries a zero, which is not a valid field page.
    wanted_pages_.clear();
    const TileAddress* previous = nullptr;
    for (const TileAddress& address : wanted_.span()) {
        const i32 slot = slot_of(address.field);
        const FieldDeclaration* declaration = store_->registry().declaration(address.field);
        const bool duplicate = previous != nullptr && *previous == address;
        const u64 page = (duplicate || slot < 0 || declaration == nullptr)
                             ? 0U
                             : page_of_tile(static_cast<u32>(slot), address);
        if (Status pushed = wanted_pages_.push_back(page); !pushed) {
            return pushed;
        }
        previous = &address;
        if (page == 0U || store_->is_resident(address)) {
            continue;
        }

        residency::Request request;
        request.key = residency::PageKey{residency::Subsystem::WorldCells, page};
        request.bytes = FieldStore::tile_bytes(*declaration);
        // A finer level is a bigger improvement and scores higher, through the policy's own
        // `detail_deficit` term rather than through a weight invented here.
        request.inputs.detail_deficit = kFieldResidencyCount - address.level;
        request.inputs.importance = 1.0F - (static_cast<f32>(address.level) * 0.25F);
        request.inputs.screen_coverage = request.inputs.importance;
        request.inputs.cost = residency::CostClass::Streamed;
        if (Status submitted = residency_->request(request); !submitted) {
            return submitted;
        }
        ++report.tiles_requested;
    }
    return ok();
}

const TileAddress* FieldStreaming::wanted_tile_of(u64 page) const noexcept {
    for (usize index = 0; index < wanted_pages_.size(); ++index) {
        if (wanted_pages_[index] == page) {
            return &wanted_[index];
        }
    }
    return nullptr;
}

Status FieldStreaming::load_admission(const residency::Admission& admission, f64 now,
                                      FieldStreamingReport& report) noexcept {
    const TileAddress* address = wanted_tile_of(admission.key.page);
    Adopted* adopted = (address == nullptr) ? nullptr : find_field(address->field);
    if (adopted == nullptr) {
        // Admitted, and nothing wants it any more — a cell released between the request and the
        // decision. The bytes go back rather than being held against a page that will never arrive.
        (void)residency_->cancel_admission(admission.key);
        ++report.loads_abandoned;
        return ok();
    }

    bool loaded = false;
    if (adopted->loader != nullptr) {
        load_buffer_.clear();
        if (adopted->loader(adopted->user, *address, load_buffer_).has_value()) {
            if (Status inserted = store_->insert_tile(adopted->token, *address, load_buffer_.span(),
                                                      /*guaranteed=*/false);
                !inserted) {
                return inserted;
            }
            loaded = true;
        }
    } else {
        // No cooked source: the field's declared default, which is the right answer for a field
        // written at run time and the honest one for a cooked source nobody has written yet.
        if (Status inserted =
                store_->insert_default_tile(adopted->token, *address, /*guaranteed=*/false);
            !inserted) {
            return inserted;
        }
        loaded = true;
    }
    if (!loaded) {
        // The loader had nothing for that tile. `residency`'s own `abandoned_admissions` counter is
        // what would otherwise grow.
        (void)residency_->cancel_admission(admission.key);
        ++report.loads_abandoned;
        return ok();
    }

    residency::ResidentReport resident;
    resident.key = admission.key;
    resident.bytes = admission.bytes;
    resident.level = kFieldResidencyCount - address->level;
    resident.cost = residency::CostClass::Streamed;
    if (Status noted = residency_->note_resident(resident, now); !noted) {
        return noted;
    }
    ++report.tiles_loaded;
    return ok();
}

Status FieldStreaming::index_resident_pages() noexcept {
    // A page identifier is opaque to the residency layer and one-way by design, so an eviction
    // order is resolved by asking this module's own store which tile carries that page rather than
    // by unpacking bits into an address that may no longer exist. Built only when there is an
    // eviction to resolve.
    resident_.clear();
    resident_pages_.clear();
    for (const Adopted& adopted : fields_.span()) {
        const i32 slot = slot_of(adopted.field);
        if (slot < 0) {
            continue;
        }
        const usize first = resident_.size();
        for (u8 level = 0; level < kFieldResidencyCount; ++level) {
            if (Status listed =
                    store_->tiles_of(adopted.field, static_cast<FieldResidency>(level), resident_);
                !listed) {
                return listed;
            }
        }
        for (usize index = first; index < resident_.size(); ++index) {
            if (Status pushed = resident_pages_.push_back(
                    page_of_tile(static_cast<u32>(slot), resident_[index]));
                !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status FieldStreaming::apply_evictions(f64 now, FieldStreamingReport& report) noexcept {
    if (schedule_.evictions.empty()) {
        return ok();
    }
    if (Status indexed = index_resident_pages(); !indexed) {
        return indexed;
    }
    for (const residency::EvictionOrder& eviction : schedule_.evictions.span()) {
        if ((eviction.key.page & kFieldPageBit) == 0) {
            continue;
        }
        for (usize index = 0; index < resident_pages_.size(); ++index) {
            if (resident_pages_[index] != eviction.key.page) {
                continue;
            }
            Adopted* adopted = find_field(resident_[index].field);
            if (adopted == nullptr) {
                break;
            }
            if (Status evicted = store_->evict_tile(adopted->token, resident_[index]); !evicted) {
                // A guaranteed tile. The order is refused and counted: macro data must exist for
                // regions that are not loaded, and a budget is not allowed to take that away.
                ++report.evictions_refused;
            } else {
                ++report.tiles_evicted;
                (void)residency_->note_released(eviction.key, now);
            }
            break;
        }
    }
    return ok();
}

Expected<FieldStreamingReport, Error> FieldStreaming::tick(f64 now) noexcept {
    // The tick in five steps, each its own function: what the world said, what the bound cells
    // want, what the policy was asked for, what it admitted, and what it took back. A tick written
    // as one function was 126 of cognitive complexity and unreviewable; these are the seams a
    // reader already has names for.
    FieldStreamingReport report;
    if (Status consumed = consume_cell_events(report); !consumed) {
        return make_unexpected(consumed.error());
    }
    if (Status collected = collect_wanted(); !collected) {
        return make_unexpected(collected.error());
    }
    if (Status submitted = submit_requests(report); !submitted) {
        return make_unexpected(submitted.error());
    }

    schedule_.clear();
    residency::ScheduleOptions options;
    options.now = now;
    if (Status scheduled = residency_->schedule(options, schedule_); !scheduled) {
        return make_unexpected(scheduled.error());
    }

    for (const residency::Admission& admission : schedule_.admissions.span()) {
        if ((admission.key.page & kFieldPageBit) == 0) {
            // Somebody else's page in the shared subsystem. Not this module's to act on, and the
            // reason bit 55 exists.
            continue;
        }
        ++report.tiles_admitted;
        if (Status loaded = load_admission(admission, now, report); !loaded) {
            return make_unexpected(loaded.error());
        }
    }
    if (Status evicted = apply_evictions(now, report); !evicted) {
        return make_unexpected(evicted.error());
    }
    return report;
}

}  // namespace cy::environment
