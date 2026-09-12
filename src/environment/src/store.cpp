// The sparse tiled store: the lattice arithmetic, the two sampling paths, staged writes, and the
// change events derived work is invalidated by. Tasks 1.1, 1.3 and 1.4.

#include <cy/environment/store.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::environment {
namespace {

/// Floor division for a positive divisor. `a / b` truncates toward zero in C++, which puts the
/// tile boundary at the origin in the wrong place for negative coordinates — cell -1 would land in
/// tile 0 with cell 0, and a world west of its origin would fold onto itself.
[[nodiscard]] i64 floor_div(i64 value, i64 divisor) noexcept {
    const i64 quotient = value / divisor;
    return ((value % divisor != 0) && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

[[nodiscard]] i64 ifloor(f64 value) noexcept {
    return static_cast<i64>(std::floor(value));
}

[[nodiscard]] i64 clamp_i64(i64 value, i64 low, i64 high) noexcept {
    return (value < low) ? low : ((value > high) ? high : value);
}

[[nodiscard]] f32 clamp_f32(f32 value, f32 low, f32 high) noexcept {
    return (value < low) ? low : ((value > high) ? high : value);
}

/// The counters `FieldDiagnostics` reports. Best-effort: a diagnostic that could fail a sample
/// would be a diagnostic nobody leaves on, so an allocation failure here costs a count and nothing
/// else.
void bump(HashMap<u64, u64>& counters, u64 key, u64 amount) noexcept {
    if (u64* existing = counters.find(key); existing != nullptr) {
        *existing += amount;
        return;
    }
    Expected<u64*, Error> inserted = counters.insert(key, amount);
    (void)inserted;
}

[[nodiscard]] u64 counter_of(const HashMap<u64, u64>& counters, u64 key) noexcept {
    const u64* value = counters.find(key);
    return (value == nullptr) ? 0 : *value;
}

}  // namespace

const char* field_change_kind_name(FieldChangeKind kind) noexcept {
    switch (kind) {
        case FieldChangeKind::Values:
            return "values";
        case FieldChangeKind::Resident:
            return "resident";
        case FieldChangeKind::Evicted:
            return "evicted";
    }
    return "unknown";
}

FieldBounds tile_bounds(const FieldDeclaration& declaration, const TileAddress& address) noexcept {
    const f64 metres = static_cast<f64>(declaration.levels[address.level].cell_metres) *
                       static_cast<f64>(kTileCells);
    FieldBounds bounds;
    bounds.min_x = static_cast<f64>(address.x) * metres;
    bounds.min_z = static_cast<f64>(address.z) * metres;
    bounds.max_x = bounds.min_x + metres;
    bounds.max_z = bounds.min_z + metres;
    return bounds;
}

// --- Encoding
// -------------------------------------------------------------------------------------

void encode_value(const FieldDeclaration& declaration, const FieldValue& value,
                  u8* destination) noexcept {
    const u32 components = declaration.components();
    const f32 span = declaration.range_max - declaration.range_min;
    for (u32 index = 0; index < components; ++index) {
        const f32 raw = value.components[index];
        u8* slot = destination + (static_cast<usize>(index) * encoding_bytes(declaration.encoding));
        switch (declaration.encoding) {
            case FieldEncoding::F32: {
                std::memcpy(slot, &raw, sizeof(f32));
                break;
            }
            case FieldEncoding::UNorm8: {
                const f32 unit = clamp_f32((raw - declaration.range_min) / span, 0.0F, 1.0F);
                // Round to nearest rather than truncate: truncation biases every stored value
                // downward by half a quantum, which is visible where a field's whole range is one.
                const auto quantised = static_cast<u8>(unit * 255.0F + 0.5F);
                slot[0] = quantised;
                break;
            }
            case FieldEncoding::UNorm16: {
                const f32 unit = clamp_f32((raw - declaration.range_min) / span, 0.0F, 1.0F);
                const auto quantised = static_cast<u16>(unit * 65535.0F + 0.5F);
                std::memcpy(slot, &quantised, sizeof(u16));
                break;
            }
            case FieldEncoding::Uint8: {
                const auto quantised = static_cast<u8>(clamp_f32(raw, 0.0F, 255.0F));
                slot[0] = quantised;
                break;
            }
            case FieldEncoding::Uint16: {
                const auto quantised = static_cast<u16>(clamp_f32(raw, 0.0F, 65535.0F));
                std::memcpy(slot, &quantised, sizeof(u16));
                break;
            }
        }
    }
}

FieldValue decode_value(const FieldDeclaration& declaration, const u8* source) noexcept {
    FieldValue value;
    const u32 components = declaration.components();
    const f32 span = declaration.range_max - declaration.range_min;
    for (u32 index = 0; index < components; ++index) {
        const u8* slot =
            source + (static_cast<usize>(index) * encoding_bytes(declaration.encoding));
        switch (declaration.encoding) {
            case FieldEncoding::F32: {
                f32 raw = 0.0F;
                std::memcpy(&raw, slot, sizeof(f32));
                value.components[index] = raw;
                break;
            }
            case FieldEncoding::UNorm8: {
                value.components[index] =
                    declaration.range_min + ((static_cast<f32>(slot[0]) / 255.0F) * span);
                break;
            }
            case FieldEncoding::UNorm16: {
                u16 raw = 0;
                std::memcpy(&raw, slot, sizeof(u16));
                value.components[index] =
                    declaration.range_min + ((static_cast<f32>(raw) / 65535.0F) * span);
                break;
            }
            case FieldEncoding::Uint8: {
                value.components[index] = static_cast<f32>(slot[0]);
                break;
            }
            case FieldEncoding::Uint16: {
                u16 raw = 0;
                std::memcpy(&raw, slot, sizeof(u16));
                value.components[index] = static_cast<f32>(raw);
                break;
            }
        }
    }
    return value;
}

u32 FieldStore::tile_bytes(const FieldDeclaration& declaration) noexcept {
    return kTileCells * kTileCells * declaration.vertical_cells * declaration.value_bytes();
}

u32 FieldStore::lattice_offset(const FieldDeclaration& declaration, u32 x, u32 y, u32 z) noexcept {
    const u32 plane = kTileCells * kTileCells;
    return (((y * plane) + (z * kTileCells)) + x) * declaration.value_bytes();
}

// --- The change queue
// -------------------------------------------------------------------------------

FieldChangeQueue::FieldChangeQueue(Allocator& allocator) noexcept
    : changes_(allocator), consumers_(allocator) {}

Expected<FieldChangeQueue::ConsumerId, Error> FieldChangeQueue::add_consumer(const char* name,
                                                                             u32 order) noexcept {
    Consumer consumer;
    consumer.id = static_cast<ConsumerId>(consumers_.size() + 1);
    consumer.name = (name == nullptr) ? "" : name;
    consumer.order = order;
    // A consumer registered after events were raised has not missed them: it has never seen them,
    // and `dropped_` is how many the queue has already compacted away. Starting it at the current
    // floor rather than at zero is what keeps `drain()` from replaying history to a late arrival.
    consumer.seen = dropped_ + static_cast<u64>(changes_.size());
    if (Status pushed = consumers_.push_back(consumer); !pushed) {
        return make_unexpected(pushed.error());
    }
    // Ordered by the declared key, so a dispatcher walking `consumers()` runs them in the order the
    // configuration states rather than the order they registered in.
    std::stable_sort(
        consumers_.begin(), consumers_.end(),
        [](const Consumer& a, const Consumer& b) noexcept { return a.order < b.order; });
    return consumer.id;
}

Status FieldChangeQueue::emit(const FieldChange& change) noexcept {
    FieldChange stamped = change;
    stamped.sequence = next_sequence_++;
    return changes_.push_back(stamped);
}

Status FieldChangeQueue::drain(ConsumerId consumer, Array<FieldChange>& out) noexcept {
    for (Consumer& entry : consumers_) {
        if (entry.id != consumer) {
            continue;
        }
        for (const FieldChange& change : changes_.span()) {
            const u64 position = dropped_ + static_cast<u64>(&change - changes_.data());
            if (position < entry.seen) {
                continue;
            }
            if (Status pushed = out.push_back(change); !pushed) {
                return pushed;
            }
        }
        entry.seen = dropped_ + static_cast<u64>(changes_.size());
        return ok();
    }
    return fail(ErrorCode::NotFound, "environment: no such change consumer");
}

void FieldChangeQueue::compact() noexcept {
    if (consumers_.empty()) {
        // Nothing is reading. Without this the queue is a log that grows for the life of the
        // process — `world::CellEventQueue` makes the same call for the same reason.
        dropped_ += static_cast<u64>(changes_.size());
        changes_.clear();
        return;
    }
    u64 lowest = consumers_[0].seen;
    for (const Consumer& consumer : consumers_.span()) {
        lowest = (consumer.seen < lowest) ? consumer.seen : lowest;
    }
    const u64 removable = (lowest > dropped_) ? (lowest - dropped_) : 0;
    if (removable == 0) {
        return;
    }
    const usize count = static_cast<usize>(removable);
    for (usize index = count; index < changes_.size(); ++index) {
        changes_[index - count] = changes_[index];
    }
    for (usize index = 0; index < count; ++index) {
        changes_.pop_back();
    }
    dropped_ += removable;
}

// --- The store
// ---------------------------------------------------------------------------------------

FieldStore::FieldStore(Allocator& allocator, const FieldRegistry& registry,
                       const world::PartitionConfig& partition) noexcept
    : allocator_(&allocator),
      registry_(&registry),
      partition_(&partition),
      tiles_(allocator),
      index_(allocator),
      changes_(allocator),
      samples_(allocator),
      lattice_reads_(allocator) {}

namespace {

/// The map key. A packed integer rather than the address itself, so that the index is
/// `HashMap<u64, usize>` and the address is compared once, on the tile, rather than hashed as a
/// struct with padding in it.
///
/// The field's 64-bit identity is folded with the tile coordinates rather than truncated, and the
/// tile record carries the address so a lookup VERIFIES rather than trusts the fold — a hash
/// collision returns "not resident", which is a defined answer, and never someone else's tile.
[[nodiscard]] u64 tile_key(const TileAddress& address) noexcept {
    u64 key = address.field.value;
    key = hash_combine(key, static_cast<u64>(static_cast<u32>(address.x)));
    key = hash_combine(key, static_cast<u64>(static_cast<u32>(address.z)));
    key = hash_combine(key, (static_cast<u64>(address.level) << 8U) | address.layer);
    return key;
}

}  // namespace

FieldStore::Tile* FieldStore::find_tile(const TileAddress& address) noexcept {
    const usize* slot = index_.find(tile_key(address));
    if (slot == nullptr) {
        return nullptr;
    }
    Tile& tile = tiles_[*slot];
    return (tile.address == address) ? &tile : nullptr;
}

const FieldStore::Tile* FieldStore::find_tile(const TileAddress& address) const noexcept {
    const usize* slot = index_.find(tile_key(address));
    if (slot == nullptr) {
        return nullptr;
    }
    const Tile& tile = tiles_[*slot];
    return (tile.address == address) ? &tile : nullptr;
}

Status FieldStore::emit_change(FieldChangeKind kind, const FieldDeclaration& declaration,
                               const TileAddress& address, u64 version) noexcept {
    FieldChange change;
    change.kind = kind;
    change.address = address;
    change.bounds = tile_bounds(declaration, address);
    change.version = version;
    return changes_.emit(change);
}

Status FieldStore::place_tile(const TileAddress& address, Span<const u8> data, bool guaranteed,
                              FieldChangeKind kind) noexcept {
    const FieldDeclaration* declaration = registry_->declaration(address.field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    if (data.size() != tile_bytes(*declaration)) {
        return fail(ErrorCode::InvalidArgument,
                    "environment: a tile's bytes must match the field's declared tile size");
    }

    if (Tile* existing = find_tile(address); existing != nullptr) {
        std::memcpy(existing->data.data(), data.data(), data.size());
        ++existing->version;
        existing->guaranteed = existing->guaranteed || guaranteed;
        return emit_change(kind, *declaration, address, existing->version);
    }

    Tile tile(*allocator_);
    tile.address = address;
    tile.guaranteed = guaranteed;
    if (Status appended = tile.data.append(data); !appended) {
        return appended;
    }
    const u64 version = tile.version;
    if (Status pushed = tiles_.push_back(std::move(tile)); !pushed) {
        return pushed;
    }
    if (Expected<usize*, Error> inserted = index_.insert(tile_key(address), tiles_.size() - 1);
        !inserted) {
        tiles_.pop_back();
        return make_unexpected(inserted.error());
    }
    bytes_ += data.size();
    return emit_change(kind, *declaration, address, version);
}

Status FieldStore::insert_tile(const ProducerToken& token, const TileAddress& address,
                               Span<const u8> data, bool guaranteed) noexcept {
    if (!token.valid() || !(token.field() == address.field)) {
        return fail(ErrorCode::PermissionDenied,
                    "environment: a tile may only be inserted by the field's own producer");
    }
    return place_tile(address, data, guaranteed, FieldChangeKind::Resident);
}

Status FieldStore::insert_default_tile(const ProducerToken& token, const TileAddress& address,
                                       bool guaranteed) noexcept {
    const FieldDeclaration* declaration = registry_->declaration(address.field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    Array<u8> data(*allocator_);
    const u32 bytes = tile_bytes(*declaration);
    if (Status resized = data.reserve(bytes); !resized) {
        return resized;
    }
    for (u32 index = 0; index < bytes; ++index) {
        if (Status pushed = data.push_back(u8{0}); !pushed) {
            return pushed;
        }
    }
    const u32 points = kTileCells * kTileCells * declaration->vertical_cells;
    for (u32 point = 0; point < points; ++point) {
        encode_value(*declaration, declaration->default_value,
                     data.data() + (static_cast<usize>(point) * declaration->value_bytes()));
    }
    return insert_tile(token, address, data.span(), guaranteed);
}

Status FieldStore::evict_tile(const ProducerToken& token, const TileAddress& address) noexcept {
    if (!token.valid() || !(token.field() == address.field)) {
        return fail(ErrorCode::PermissionDenied,
                    "environment: a tile may only be evicted by the field's own producer");
    }
    const FieldDeclaration* declaration = registry_->declaration(address.field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    const usize* slot = index_.find(tile_key(address));
    if (slot == nullptr || !(tiles_[*slot].address == address)) {
        return fail(ErrorCode::NotFound, "environment: that tile is not resident");
    }
    if (tiles_[*slot].guaranteed) {
        // "Macro-level data SHALL be resident for the whole world where a field declares it."
        // Evicting the guaranteed level would make every coarser answer undefined, so the request
        // is refused rather than honoured and reported.
        return fail(ErrorCode::PermissionDenied,
                    "environment: a guaranteed tile is the coarse fallback and is not evictable");
    }

    const usize victim = *slot;
    bytes_ -= tiles_[victim].data.size();
    index_.remove(tile_key(address));
    const usize last = tiles_.size() - 1;
    if (victim != last) {
        const TileAddress moved = tiles_[last].address;
        tiles_[victim] = std::move(tiles_[last]);
        if (usize* moved_slot = index_.find(tile_key(moved)); moved_slot != nullptr) {
            *moved_slot = victim;
        }
    }
    tiles_.pop_back();
    return emit_change(FieldChangeKind::Evicted, *declaration, address, 0);
}

bool FieldStore::is_resident(const TileAddress& address) const noexcept {
    return find_tile(address) != nullptr;
}

u64 FieldStore::version_of(const TileAddress& address) const noexcept {
    const Tile* tile = find_tile(address);
    return (tile == nullptr) ? 0 : tile->version;
}

Span<const u8> FieldStore::tile_data(const TileAddress& address) const noexcept {
    const Tile* tile = find_tile(address);
    return (tile == nullptr) ? Span<const u8>() : tile->data.span();
}

Status FieldStore::tiles_of(FieldId field, FieldResidency level,
                            Array<TileAddress>& out) const noexcept {
    for (const Tile& tile : tiles_.span()) {
        if (!(tile.address.field == field) || tile.address.level != static_cast<u8>(level)) {
            continue;
        }
        if (Status pushed = out.push_back(tile.address); !pushed) {
            return pushed;
        }
    }
    // Address order, so that a GPU image built from this list is byte-identical between two runs
    // that made the same tiles resident in a different order. Streaming order is not content.
    std::sort(out.begin(), out.end(), [](const TileAddress& a, const TileAddress& b) noexcept {
        if (a.layer != b.layer) {
            return a.layer < b.layer;
        }
        if (a.z != b.z) {
            return a.z < b.z;
        }
        return a.x < b.x;
    });
    return ok();
}

// --- Sampling
// -----------------------------------------------------------------------------------------

namespace {

/// Everything one level's lattice arithmetic needs, computed once per sample rather than per
/// corner.
struct Lattice {
    i64 i0 = 0;  // first horizontal lattice index along x
    i64 k0 = 0;  // ... along z
    i64 j0 = 0;  // ... along the column
    f32 fx = 0.0F;
    f32 fz = 0.0F;
    f32 fy = 0.0F;
    i64 tile_x = 0;
    i64 tile_z = 0;
    bool linear = false;
};

[[nodiscard]] Lattice lattice_of(const FieldDeclaration& declaration, u8 level,
                                 const world::WorldVec3d& at) noexcept {
    const f64 metres = static_cast<f64>(declaration.levels[level].cell_metres);
    Lattice lattice;
    lattice.linear = declaration.interpolation == FieldInterpolation::Linear;

    // The cell containing the position decides which tile answers, whatever the interpolation
    // reaches for: a sample is resolved by the tile it is IN.
    lattice.tile_x = floor_div(ifloor(at.x / metres), static_cast<i64>(kTileCells));
    lattice.tile_z = floor_div(ifloor(at.z / metres), static_cast<i64>(kTileCells));

    if (lattice.linear) {
        // Values sit at cell CENTRES, so the continuous coordinate is offset by half a cell. A
        // lattice on cell corners would make a tile's own edge values belong to its neighbour.
        const f64 u = (at.x / metres) - 0.5;
        const f64 w = (at.z / metres) - 0.5;
        lattice.i0 = ifloor(u);
        lattice.k0 = ifloor(w);
        lattice.fx = static_cast<f32>(u - static_cast<f64>(lattice.i0));
        lattice.fz = static_cast<f32>(w - static_cast<f64>(lattice.k0));
    } else {
        lattice.i0 = ifloor(at.x / metres);
        lattice.k0 = ifloor(at.z / metres);
    }

    const f64 vertical = (at.y - static_cast<f64>(declaration.vertical_origin_metres)) /
                         static_cast<f64>(declaration.vertical_metres);
    if (lattice.linear && declaration.vertical_cells > 1) {
        const f64 v = vertical - 0.5;
        lattice.j0 = ifloor(v);
        lattice.fy = static_cast<f32>(v - static_cast<f64>(lattice.j0));
    } else {
        // A planar field is the degenerate case of the volumetric one, not a second path: the
        // column index is clamped to its only value and the vertical weight stays zero.
        lattice.j0 = ifloor(vertical);
    }
    return lattice;
}

}  // namespace

FieldValue FieldStore::read_lattice_point(const FieldDeclaration& declaration,
                                          const TileAddress& centre_address, const Tile& centre,
                                          i64 gi, i64 gk, i64 gj) const noexcept {
    // One lattice point, from whichever tile holds it. A corner whose tile is not resident is
    // clamped to the resident tile's own edge — see this file's header note: never a fault, never a
    // blend against a default that happens to be next door.
    const i64 span = static_cast<i64>(kTileCells);
    const i64 j = clamp_i64(gj, 0, static_cast<i64>(declaration.vertical_cells) - 1);
    i64 tx = floor_div(gi, span);
    i64 tz = floor_div(gk, span);
    const Tile* tile = &centre;
    if (tx != centre_address.x || tz != centre_address.z) {
        TileAddress neighbour = centre_address;
        neighbour.x = static_cast<i32>(tx);
        neighbour.z = static_cast<i32>(tz);
        tile = find_tile(neighbour);
        if (tile == nullptr) {
            tile = &centre;
            tx = centre_address.x;
            tz = centre_address.z;
        }
    }
    const i64 lx = clamp_i64(gi - (tx * span), 0, span - 1);
    const i64 lz = clamp_i64(gk - (tz * span), 0, span - 1);
    const u32 offset = lattice_offset(declaration, static_cast<u32>(lx), static_cast<u32>(j),
                                      static_cast<u32>(lz));
    return decode_value(declaration, tile->data.data() + offset);
}

FieldStore::LayerSample FieldStore::sample_layer(const FieldDeclaration& declaration, FieldId field,
                                                 u8 level, FieldLayer layer,
                                                 const world::WorldVec3d& at) const noexcept {
    LayerSample result;
    const Lattice lattice = lattice_of(declaration, level, at);

    TileAddress centre_address;
    centre_address.field = field;
    centre_address.level = level;
    centre_address.layer = static_cast<u8>(layer);
    centre_address.x = static_cast<i32>(lattice.tile_x);
    centre_address.z = static_cast<i32>(lattice.tile_z);

    const Tile* centre = find_tile(centre_address);
    if (centre == nullptr) {
        return result;
    }
    result.resolved = true;
    result.version = centre->version;

    if (!lattice.linear) {
        result.value = read_lattice_point(declaration, centre_address, *centre, lattice.i0,
                                          lattice.k0, lattice.j0);
        result.reads = 1;
        return result;
    }

    const u32 components = declaration.components();
    const u32 vertical_taps = (declaration.vertical_cells > 1) ? 2 : 1;
    FieldValue accumulated;
    for (u32 dy = 0; dy < vertical_taps; ++dy) {
        const f32 wy = (vertical_taps == 1) ? 1.0F : ((dy == 0) ? (1.0F - lattice.fy) : lattice.fy);
        for (u32 dz = 0; dz < 2; ++dz) {
            const f32 wz = (dz == 0) ? (1.0F - lattice.fz) : lattice.fz;
            for (u32 dx = 0; dx < 2; ++dx) {
                const f32 wx = (dx == 0) ? (1.0F - lattice.fx) : lattice.fx;
                const FieldValue corner = read_lattice_point(
                    declaration, centre_address, *centre, lattice.i0 + static_cast<i64>(dx),
                    lattice.k0 + static_cast<i64>(dz), lattice.j0 + static_cast<i64>(dy));
                const f32 weight = wx * wz * wy;
                for (u32 index = 0; index < components; ++index) {
                    accumulated.components[index] += corner.components[index] * weight;
                }
                ++result.reads;
            }
        }
    }
    result.value = accumulated;
    return result;
}

FieldSample FieldStore::sample_level(const FieldDeclaration& declaration, FieldId field, u8 level,
                                     const world::WorldVec3d& at) const noexcept {
    FieldSample sample;
    sample.level = static_cast<FieldResidency>(level);
    sample.cell_metres = declaration.levels[level].cell_metres;
    sample.value = declaration.default_value;
    if (!declaration.levels[level].declared()) {
        return sample;
    }

    const LayerSample base = sample_layer(declaration, field, level, FieldLayer::Base, at);
    const LayerSample delta = sample_layer(declaration, field, level, FieldLayer::Delta, at);
    sample.reads = base.reads + delta.reads;
    if (!base.resolved && !delta.resolved) {
        return sample;
    }

    // The declared rule, applied by every reader identically. A delta with no base under it
    // composes against the DECLARED DEFAULT rather than against zero, so `Multiply` over a field
    // whose default is one is the identity it reads as.
    FieldValue value = base.resolved ? base.value : declaration.default_value;
    if (delta.resolved) {
        value =
            combine_layers(declaration.layer_rule, value, delta.value, declaration.components());
    }
    sample.value = value;
    sample.resolved = true;
    sample.version = base.resolved ? base.version : delta.version;
    return sample;
}

FieldSample FieldStore::sample(FieldId field, const world::WorldVec3d& at) const noexcept {
    const FieldDeclaration* declaration = registry_->declaration(field);
    if (declaration == nullptr) {
        return FieldSample{};
    }
    FieldSample answer;
    answer.value = declaration->default_value;
    u64 reads = 0;
    // Finest first. `FieldResidency` is ordered so this walk is the specification's "the finest
    // resident level at that position" rather than a search over an unordered set.
    for (u32 level = 0; level < kFieldResidencyCount; ++level) {
        if (!declaration->levels[level].declared()) {
            continue;
        }
        const FieldSample sample = sample_level(*declaration, field, static_cast<u8>(level), at);
        reads += sample.reads;
        answer.level = sample.level;
        answer.cell_metres = sample.cell_metres;
        if (sample.resolved) {
            answer = sample;
            break;
        }
    }
    answer.reads = reads;
    bump(samples_, field.value, 1);
    bump(lattice_reads_, field.value, reads);
    return answer;
}

FieldSample FieldStore::sample(FieldId field, const world::WorldPosition& at) const noexcept {
    return sample(field, world::to_absolute(*partition_, at));
}

FieldSample FieldStore::sample_at(FieldId field, const world::WorldVec3d& at,
                                  FieldResidency level) const noexcept {
    const FieldDeclaration* declaration = registry_->declaration(field);
    if (declaration == nullptr) {
        return FieldSample{};
    }
    FieldSample sample = sample_level(*declaration, field, static_cast<u8>(level), at);
    bump(samples_, field.value, 1);
    bump(lattice_reads_, field.value, sample.reads);
    return sample;
}

FieldSample FieldStore::sample_deterministic(FieldId field,
                                             const world::WorldVec3d& at) const noexcept {
    const FieldDeclaration* declaration = registry_->declaration(field);
    if (declaration == nullptr) {
        return FieldSample{};
    }
    // ONE level, and the declared default where it has no data. Nothing here consults residency,
    // which is the whole point: the answer is a function of the field's contents and the position,
    // and a tile arriving or leaving cannot change it. M10 tasks.md 1.4.
    return sample_at(field, at, declaration->gameplay_level);
}

Status FieldStore::sample_many(FieldId field, Span<const world::WorldVec3d> positions,
                               Span<FieldSample> out) const noexcept {
    const FieldDeclaration* declaration = registry_->declaration(field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    if (out.size() < positions.size()) {
        return fail(ErrorCode::BufferTooSmall,
                    "environment: the output span is shorter than the position span");
    }
    u64 reads = 0;
    for (usize index = 0; index < positions.size(); ++index) {
        FieldSample answer;
        answer.value = declaration->default_value;
        for (u32 level = 0; level < kFieldResidencyCount; ++level) {
            if (!declaration->levels[level].declared()) {
                continue;
            }
            const FieldSample sample =
                sample_level(*declaration, field, static_cast<u8>(level), positions[index]);
            reads += sample.reads;
            answer.level = sample.level;
            answer.cell_metres = sample.cell_metres;
            if (sample.resolved) {
                answer = sample;
                break;
            }
        }
        out[index] = answer;
    }
    // One counter update for the batch rather than one per position: the batched call exists so a
    // system sampling ten thousand positions does not pay per-sample overhead, and a diagnostic
    // that charged it per sample would be that overhead.
    bump(samples_, field.value, positions.size());
    bump(lattice_reads_, field.value, reads);
    return ok();
}

Status FieldStore::sample_many_deterministic(FieldId field, Span<const world::WorldVec3d> positions,
                                             Span<FieldSample> out) const noexcept {
    const FieldDeclaration* declaration = registry_->declaration(field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    if (out.size() < positions.size()) {
        return fail(ErrorCode::BufferTooSmall,
                    "environment: the output span is shorter than the position span");
    }
    const auto level = static_cast<u8>(declaration->gameplay_level);
    u64 reads = 0;
    for (usize index = 0; index < positions.size(); ++index) {
        FieldSample sample = sample_level(*declaration, field, level, positions[index]);
        reads += sample.reads;
        out[index] = sample;
    }
    bump(samples_, field.value, positions.size());
    bump(lattice_reads_, field.value, reads);
    return ok();
}

FieldPointQuery FieldStore::point_query(FieldId field, const world::WorldVec3d& at) const noexcept {
    FieldPointQuery query;
    const FieldRecord* record = registry_->find(field);
    if (record == nullptr) {
        return query;
    }
    const FieldDeclaration& declaration = record->declaration;
    query.field_name = declaration.name;
    query.producer = record->claimed ? record->producer_name : "(unclaimed)";
    query.sample = sample(field, at);

    const auto level = static_cast<u8>(query.sample.level);
    const LayerSample base = sample_layer(declaration, field, level, FieldLayer::Base, at);
    const LayerSample delta = sample_layer(declaration, field, level, FieldLayer::Delta, at);
    query.base = base.value;
    query.delta = delta.value;
    query.has_base = base.resolved;
    query.has_delta = delta.resolved;
    query.version = query.sample.version;
    return query;
}

FieldDiagnostics FieldStore::diagnostics(FieldId field) const noexcept {
    FieldDiagnostics report;
    report.field = field;
    const FieldRecord* record = registry_->find(field);
    if (record != nullptr) {
        report.field_name = record->declaration.name;
        report.producer = record->claimed ? record->producer_name : "(unclaimed)";
    }

    bool any = false;
    u8 finest = 0;
    for (u32 level = 0; level < kFieldResidencyCount; ++level) {
        if (record != nullptr && record->declaration.levels[level].declared()) {
            finest = static_cast<u8>(level);
            break;
        }
    }
    for (const Tile& tile : tiles_.span()) {
        if (!(tile.address.field == field)) {
            continue;
        }
        report.tiles[tile.address.level] += 1;
        report.bytes += tile.data.size();
        if (tile.address.level != finest) {
            continue;
        }
        if (!any) {
            report.min_tile_x = tile.address.x;
            report.max_tile_x = tile.address.x;
            report.min_tile_z = tile.address.z;
            report.max_tile_z = tile.address.z;
            any = true;
            continue;
        }
        report.min_tile_x = std::min(report.min_tile_x, tile.address.x);
        report.max_tile_x = std::max(report.max_tile_x, tile.address.x);
        report.min_tile_z = std::min(report.min_tile_z, tile.address.z);
        report.max_tile_z = std::max(report.max_tile_z, tile.address.z);
    }
    report.samples = counter_of(samples_, field.value);
    report.lattice_reads = counter_of(lattice_reads_, field.value);
    return report;
}

// --- Writing
// ---------------------------------------------------------------------------------------

Expected<FieldWriter, Error> FieldStore::open_writer(const ProducerToken& token) noexcept {
    if (!token.valid()) {
        return fail(ErrorCode::PermissionDenied,
                    "environment: a writer needs the field's producer token");
    }
    const FieldDeclaration* declaration = registry_->declaration(token.field());
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    return FieldWriter(*this, token.field(), *declaration);
}

FieldWriter::FieldWriter(FieldStore& store, FieldId field,
                         const FieldDeclaration& declaration) noexcept
    : store_(&store), declaration_(&declaration), field_(field), staged_(*store.allocator_) {}

FieldWriter::Staged* FieldWriter::find_staged(const TileAddress& address) noexcept {
    for (Staged& staged : staged_) {
        if (staged.address == address) {
            return &staged;
        }
    }
    return nullptr;
}

Status FieldWriter::stage(const TileAddress& address) noexcept {
    if (!(address.field == field_)) {
        return fail(ErrorCode::PermissionDenied,
                    "environment: this writer holds a different field's token");
    }
    if (address.layer >= kFieldLayerCount || address.level >= kFieldResidencyCount) {
        return fail(ErrorCode::OutOfRange, "environment: no such layer or residency level");
    }
    if (!declaration_->levels[address.level].declared()) {
        return fail(ErrorCode::InvalidArgument,
                    "environment: this field does not declare that residency level");
    }
    if (find_staged(address) != nullptr) {
        return ok();
    }

    Staged staged(*store_->allocator_);
    staged.address = address;
    const FieldStore::Tile* resident = store_->find_tile(address);
    if (resident != nullptr) {
        // Copy the resident values in. An edit of one lattice point must not erase the rest, and a
        // staging buffer that started empty would do exactly that on publish.
        if (Status appended = staged.data.append(resident->data.span()); !appended) {
            return appended;
        }
        staged.existed = true;
    } else {
        const u32 bytes = FieldStore::tile_bytes(*declaration_);
        if (Status reserved = staged.data.reserve(bytes); !reserved) {
            return reserved;
        }
        for (u32 index = 0; index < bytes; ++index) {
            if (Status pushed = staged.data.push_back(u8{0}); !pushed) {
                return pushed;
            }
        }
        const u32 points = kTileCells * kTileCells * declaration_->vertical_cells;
        for (u32 point = 0; point < points; ++point) {
            encode_value(
                *declaration_, declaration_->default_value,
                staged.data.data() + (static_cast<usize>(point) * declaration_->value_bytes()));
        }
    }
    return staged_.push_back(std::move(staged));
}

Status FieldWriter::set(const TileAddress& address, u32 x, u32 y, u32 z,
                        const FieldValue& value) noexcept {
    Staged* staged = find_staged(address);
    if (staged == nullptr) {
        return fail(ErrorCode::NotFound, "environment: stage the tile before writing into it");
    }
    if (x >= kTileCells || z >= kTileCells || y >= declaration_->vertical_cells) {
        return fail(ErrorCode::OutOfRange, "environment: that lattice point is outside the tile");
    }
    encode_value(*declaration_, value,
                 staged->data.data() + FieldStore::lattice_offset(*declaration_, x, y, z));
    return ok();
}

Status FieldWriter::fill(const TileAddress& address, const FieldValue& value) noexcept {
    Staged* staged = find_staged(address);
    if (staged == nullptr) {
        return fail(ErrorCode::NotFound, "environment: stage the tile before writing into it");
    }
    const u32 points = kTileCells * kTileCells * declaration_->vertical_cells;
    for (u32 point = 0; point < points; ++point) {
        encode_value(
            *declaration_, value,
            staged->data.data() + (static_cast<usize>(point) * declaration_->value_bytes()));
    }
    return ok();
}

Status FieldWriter::publish() noexcept {
    for (Staged& staged : staged_) {
        if (Status placed = store_->place_tile(staged.address, staged.data.span(),
                                               /*guaranteed=*/false, FieldChangeKind::Values);
            !placed) {
            return placed;
        }
    }
    staged_.clear();
    return ok();
}

namespace {

/// The two fields must agree about where their lattice points are, or "evolve toward potential"
/// would be resampling one grid onto another every step and the recovery would depend on the
/// resampling. Refused rather than approximated.
[[nodiscard]] Status recovery_shapes_agree(const FieldDeclaration& current,
                                           const FieldDeclaration& potential) noexcept {
    for (u32 level = 0; level < kFieldResidencyCount; ++level) {
        if (current.levels[level].cell_metres != potential.levels[level].cell_metres) {
            return fail(ErrorCode::InvalidArgument,
                        "environment: a field recovers toward a potential declared at the same "
                        "resolutions");
        }
    }
    if (potential.vertical_cells != current.vertical_cells ||
        potential.components() != current.components()) {
        return fail(ErrorCode::InvalidArgument,
                    "environment: a field recovers toward a potential of the same shape");
    }
    return ok();
}

}  // namespace

Status FieldStore::recover_tile(FieldWriter& writer, const FieldDeclaration& current,
                                const FieldDeclaration& potential, const TileAddress& address,
                                f32 fraction) noexcept {
    TileAddress source = address;
    source.field = current.potential;
    const Tile* target = find_tile(source);
    if (target == nullptr) {
        // No potential over this region. Not an error: a world may hold current state where no
        // potential was ever cooked, and it simply does not recover there.
        return ok();
    }
    if (Status staged = writer.stage(address); !staged) {
        return staged;
    }

    const Tile* live = find_tile(address);
    const u32 components = current.components();
    const u32 plane = kTileCells * kTileCells;
    const u32 points = plane * current.vertical_cells;
    for (u32 point = 0; point < points; ++point) {
        const usize offset = static_cast<usize>(point) * current.value_bytes();
        const FieldValue now = decode_value(current, live->data.data() + offset);
        const FieldValue goal = decode_value(potential, target->data.data() + offset);
        FieldValue next = now;
        for (u32 index = 0; index < components; ++index) {
            next.components[index] = now.components[index] +
                                     ((goal.components[index] - now.components[index]) * fraction);
        }
        const u32 remainder = point % plane;
        if (Status written = writer.set(address, remainder % kTileCells, point / plane,
                                        remainder / kTileCells, next);
            !written) {
            return written;
        }
    }
    return ok();
}

Status FieldStore::advance_recovery(const ProducerToken& token, f32 seconds) noexcept {
    const FieldRecord* record = registry_->find(token.field());
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    const FieldDeclaration& current = record->declaration;
    if (!current.potential.is_valid() || current.recovery_per_second <= 0.0F || seconds <= 0.0F) {
        return ok();
    }
    const FieldDeclaration* potential = registry_->declaration(current.potential);
    if (potential == nullptr) {
        return fail(ErrorCode::NotFound,
                    "environment: the declared potential field is not declared");
    }
    if (Status agreed = recovery_shapes_agree(current, *potential); !agreed) {
        return agreed;
    }

    Expected<FieldWriter, Error> writer = open_writer(token);
    if (!writer) {
        return make_unexpected(writer.error());
    }

    // The fraction of the remaining gap closed in this step. Exponential rather than linear, so the
    // rate is independent of how often the caller steps it: two half-second steps and one
    // one-second step leave the world in the same state, which a linear approach would not.
    const f32 fraction = 1.0F - std::exp(-current.recovery_per_second * seconds);

    // The addresses are taken first, because recovering a tile stages one and publishing appends to
    // `tiles_` — iterating the live array while it grows is how a container invalidates a reference
    // under a reader that did not expect it to.
    Array<TileAddress> addresses(*allocator_);
    for (const Tile& tile : tiles_.span()) {
        if ((tile.address.field == token.field()) && tile.address.layer == 0) {
            if (Status pushed = addresses.push_back(tile.address); !pushed) {
                return pushed;
            }
        }
    }
    for (const TileAddress& address : addresses.span()) {
        if (Status recovered = recover_tile(*writer, current, *potential, address, fraction);
            !recovered) {
            return recovered;
        }
    }
    return writer->publish();
}

// --- Readers
// ---------------------------------------------------------------------------------------

Expected<FieldReader, Error> FieldReader::open(const FieldStore& store, FieldId field,
                                               determinism::SimulationClass reader) noexcept {
    const FieldDeclaration* declaration = store.registry().declaration(field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    if (!determinism::may_read(reader, declaration->classification)) {
        // The firewall, through `simulation-and-determinism`'s own predicate. A gameplay system
        // asking for a field declared visual is refused here, and `FieldRegistry::validate()` finds
        // the same crossing over the whole configuration before a frame has run.
        return fail(ErrorCode::PermissionDenied,
                    "environment: the determinism firewall refuses this read — a visual field "
                    "cannot be read by authoritative simulation");
    }
    return FieldReader(store, field, *declaration, declaration->gameplay_visible());
}

FieldSample FieldReader::sample(const world::WorldVec3d& at) const noexcept {
    return deterministic_ ? store_->sample_deterministic(field_, at) : store_->sample(field_, at);
}

FieldSample FieldReader::sample(const world::WorldPosition& at) const noexcept {
    return sample(world::to_absolute(store_->partition(), at));
}

Status FieldReader::sample_many(Span<const world::WorldVec3d> positions,
                                Span<FieldSample> out) const noexcept {
    return deterministic_ ? store_->sample_many_deterministic(field_, positions, out)
                          : store_->sample_many(field_, positions, out);
}

}  // namespace cy::environment
