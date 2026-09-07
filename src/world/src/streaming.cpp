#include <cy/world/streaming.h>

#include <cy/ecs/world.h>

#include <algorithm>
#include <ranges>

namespace cy::world {
namespace {

/// How many channel bits are set. The I/O cost of a channel delta is charged pro rata against the
/// cell's total cooked bytes, which is what a cost model at this granularity can honestly say.
[[nodiscard]] u32 channel_bits(ChannelMask mask) noexcept {
    u32 count = 0;
    for (u16 bits = mask.bits; bits != 0; bits >>= 1) {
        count += bits & 1u;
    }
    return count;
}

[[nodiscard]] bool contains(Span<const CellId> cells, CellId cell) noexcept {
    return std::ranges::find(cells, cell) != cells.end();
}

[[nodiscard]] u64 delta_bytes(const CookedCell& cell, ChannelMask delta) noexcept {
    const u32 total = channel_bits(cell.channels);
    if (total == 0) {
        return 0;
    }
    return (cell.cost.io_bytes * channel_bits(delta)) / total;
}

}  // namespace

WorldStreaming::CellRuntime::CellRuntime(Allocator& allocator, CookedCell&& source) noexcept
    : cooked(std::move(source)), activation(allocator, cooked) {}

WorldStreaming::WorldStreaming(Allocator& allocator, const Partitioner& partitioner,
                               ecs::World& ecs) noexcept
    : allocator_(&allocator),
      partitioner_(&partitioner),
      ecs_(&ecs),
      cells_(allocator),
      index_(allocator),
      sources_(allocator),
      layers_(allocator),
      hlod_(allocator),
      events_(allocator),
      dynamic_(allocator),
      representations_(allocator),
      requirements_(allocator),
      deferred_(allocator),
      work_(allocator) {}

WorldStreaming::~WorldStreaming() {
    shutdown();
}

void WorldStreaming::shutdown() noexcept {
    // Withdraw first, release second, and in that order for every cell: a cell that is published
    // has entities in the ECS world, and freeing its staging without withdrawing them would leave
    // rows in the world that nothing owns. A cell that is mid-preparation has published nothing, so
    // the withdrawal is a no-op and only the staging is freed — which is why tearing down during
    // activation is safe rather than lucky.
    for (CellRuntime& cell : cells_.span()) {
        (void)cell.activation.withdraw(*ecs_);
        cell.activation.release();
        cell.state = CellState::Metadata;
        cell.resident_channels = ChannelMask::none();
    }
}

void WorldStreaming::set_profile(WorldProfile profile) noexcept {
    profile_ = profile;
}

Status WorldStreaming::add_cell(CookedCell&& cell) noexcept {
    const CellId id = cell.id;
    if (!id.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "a cooked cell carries a valid cell identifier");
    }
    if (index_.contains(id)) {
        return fail(ErrorCode::AlreadyExists, "that cell is already in the world index");
    }
    if (Status pushed = cells_.push_back(CellRuntime(*allocator_, std::move(cell))); !pushed) {
        return pushed;
    }
    Expected<usize*, Error> indexed = index_.insert(id, cells_.size() - 1);
    if (!indexed) {
        cells_.pop_back();
        return Status{make_unexpected(indexed.error())};
    }
    return ok();
}

bool WorldStreaming::has_cell(CellId cell) const noexcept {
    return index_.contains(cell);
}

WorldStreaming::CellRuntime* WorldStreaming::find_cell(CellId cell) noexcept {
    const usize* index = index_.find(cell);
    return (index == nullptr) ? nullptr : &cells_[*index];
}

const WorldStreaming::CellRuntime* WorldStreaming::find_cell(CellId cell) const noexcept {
    const usize* index = index_.find(cell);
    return (index == nullptr) ? nullptr : &cells_[*index];
}

CellState WorldStreaming::state_of(CellId cell) const noexcept {
    const CellRuntime* runtime = find_cell(cell);
    return (runtime == nullptr) ? CellState::Unloaded : runtime->state;
}

Span<const ecs::Entity> WorldStreaming::entities_of(CellId cell) const noexcept {
    const CellRuntime* runtime = find_cell(cell);
    return (runtime == nullptr) ? Span<const ecs::Entity>{} : runtime->activation.entities();
}

Status WorldStreaming::request(CellId cell, ChannelMask channels, bool activate) noexcept {
    return deferred_.push_back(DeferredRequest{cell, channels, activate});
}

Expected<SourceId, Error> WorldStreaming::prefetch(const WorldPosition& centre, f32 radius,
                                                   RequestClass klass,
                                                   ChannelMask channels) noexcept {
    // A prefetch is a SOURCE, not a second mechanism. It therefore combines with every other source
    // by the ordinary rule, is visible in the diagnostics as the thing requiring a cell, and is
    // cancelled by removing it. The gameplay API never names a cell.
    StreamingSource source;
    source.shape = SourceShape::Sphere;
    source.position = centre;
    source.radius = radius;
    source.channels = channels;
    source.klass = klass;
    source.importance = (klass == RequestClass::Critical) ? 1.0f : 0.6f;
    // A prefetch buys RESIDENCY and not activation: that is the separation of the two axes at the
    // point a caller asks for something, and it is what makes "resident but inactive" reachable
    // from the gameplay API rather than only from a test.
    source.activates = false;
    return sources_.add(source);
}

Status WorldStreaming::set_layer_state(LayerId layer, LayerState state) noexcept {
    if (Status set = layers_.set_state(layer, state); !set) {
        return set;
    }
    // ONE OPERATION over whole blocks. Every published cell publishes or withdraws the blocks
    // belonging to this layer; no entity is visited individually, which is what makes a scenario
    // switch cost the number of (cell, archetype) pairs rather than the number of entities.
    for (CellRuntime& cell : cells_.span()) {
        if (cell.state != CellState::Activated) {
            continue;
        }
        const Status applied = (state == LayerState::Activated)
                                   ? cell.activation.publish_layer(*ecs_, layer)
                                   : cell.activation.withdraw_layer(*ecs_, layer);
        if (!applied) {
            return applied;
        }
    }
    if (overlay_ != nullptr) {
        return overlay_->record_layer_state(layer, state);
    }
    return ok();
}

Status WorldStreaming::emit(CellEventKind kind, const CellRuntime& cell) noexcept {
    CellEvent event;
    event.kind = kind;
    event.cell = cell.cooked.id;
    event.channels = cell.resident_channels;
    return events_.emit(event);
}

Status WorldStreaming::gather_requirements() noexcept {
    requirements_.clear();
    if (Status required = sources_.require_all(*partitioner_, requirements_); !required) {
        return required;
    }

    const ChannelMask allowed = profile_channels(profile_);
    for (CellRuntime& cell : cells_.span()) {
        cell.required = false;
        cell.wanted_active = false;
        cell.requiring_sources = 0;
        cell.leading_source = kInvalidSource;
        cell.priority = 0.0f;
        cell.klass = RequestClass::Background;
        cell.time_until_needed = 0;
        cell.required_channels = ChannelMask::none();
        cell.blocking = "unrequested";
    }

    for (const CellRequirement& requirement : requirements_.span()) {
        CellRuntime* cell = find_cell(requirement.cell);
        if (cell == nullptr) {
            // A source requires a region of space; the world has cells only where there is content.
            // A requirement with no cell behind it is not an error, it is empty space.
            continue;
        }
        cell->required = true;
        ++cell->requiring_sources;
        cell->wanted_active = cell->wanted_active || requirement.activate;
        cell->required_channels =
            cell->required_channels | (requirement.channels & allowed & cell->cooked.channels);
        if (static_cast<u8>(requirement.klass) < static_cast<u8>(cell->klass)) {
            cell->klass = requirement.klass;
        }
        if (requirement.priority > cell->priority) {
            cell->priority = requirement.priority;
            cell->leading_source = requirement.source;
        }
        cell->time_until_needed = requirement.time_until_needed;
        cell->blocking = "";
    }

    // A pinned request outlives one tick, unlike a source's requirement.
    for (CellRuntime& cell : cells_.span()) {
        if (!cell.pinned) {
            continue;
        }
        cell.required = true;
        cell.wanted_active = cell.wanted_active || cell.pinned_activate;
        cell.required_channels =
            cell.required_channels | (cell.pinned_channels & allowed & cell.cooked.channels);
        if (cell.klass == RequestClass::Background) {
            cell.klass = RequestClass::Gameplay;
        }
        cell.priority = (cell.priority > 0.75f) ? cell.priority : 0.75f;
        cell.blocking = "";
    }
    return ok();
}

Status WorldStreaming::close_hard_dependencies() noexcept {
    // A `RequireLoaded` reference means the target must be resident whenever the holder is, so the
    // closure is transitive. Bounded by the cell count: each pass can only add cells, and a pass
    // that adds none is the fixed point.
    bool changed = true;
    usize passes = 0;
    while (changed && passes <= cells_.size()) {
        changed = false;
        ++passes;
        for (const CellRuntime& holder : cells_.span()) {
            if (!holder.required) {
                continue;
            }
            const f32 priority = holder.priority;
            const RequestClass klass = holder.klass;
            const ChannelMask channels = holder.required_channels;
            for (const CellId target : holder.cooked.hard_dependencies.span()) {
                CellRuntime* dependency = find_cell(target);
                if (dependency == nullptr || dependency->required) {
                    continue;
                }
                dependency->required = true;
                dependency->required_channels =
                    dependency->required_channels | (channels & dependency->cooked.channels);
                dependency->klass = klass;
                dependency->priority = priority;
                dependency->blocking = "";
                changed = true;
            }
        }
    }
    return ok();
}

void WorldStreaming::order_work() noexcept {
    work_.clear();
    for (usize index = 0; index < cells_.size(); ++index) {
        if (work_.push_back(static_cast<u32>(index))) {
            continue;
        }
        // A failed push here means the tick works on a prefix of the cells rather than all of them;
        // the next tick retries. Refusing to tick at all because a scratch array could not grow
        // would be a worse answer than doing less work.
        break;
    }
    const Array<CellRuntime>& cells = cells_;
    std::sort(work_.data(), work_.data() + work_.size(), [&cells](u32 a, u32 b) {
        const CellRuntime& left = cells[a];
        const CellRuntime& right = cells[b];
        if (left.klass != right.klass) {
            return static_cast<u8>(left.klass) < static_cast<u8>(right.klass);
        }
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        if (left.time_until_needed != right.time_until_needed) {
            return left.time_until_needed < right.time_until_needed;
        }
        // The tie-break that makes the order the SAME on two machines. Without it the order is the
        // order cells happened to be added in, which is the order the cook happened to emit them.
        return left.cooked.id < right.cooked.id;
    });
}

Status WorldStreaming::spend_io(const StreamingBudget& budget, TickReport& report) noexcept {
    for (const u32 index : work_.span()) {
        CellRuntime& cell = cells_[index];
        if (!cell.required || cell.required_channels.empty()) {
            continue;
        }
        ++report.cells_requested;
        if (cell.state == CellState::Evictable) {
            // Required again before anything reclaimed it. Its bytes are still here, so this is a
            // state change and not a load — which is the whole value of `Evictable` being a state
            // rather than an immediate free.
            cell.state = CellState::Resident;
        }

        const ChannelMask missing = cell.required_channels.without(cell.resident_channels);
        if (missing.empty()) {
            continue;
        }
        const u64 bytes = delta_bytes(cell.cooked, missing);
        if (report.io_bytes_spent + bytes > budget.io_bytes_per_tick) {
            // Marked and skipped rather than breaking the loop: a single very large cell at the
            // head of the queue would otherwise block every small one behind it for as long as it
            // took, which is head-of-line blocking dressed up as priority.
            if (cell.state == CellState::Metadata) {
                cell.state = CellState::Prefetching;
            }
            cell.blocking = "budget-blocked: I/O";
            ++report.deferred;
            continue;
        }

        report.io_bytes_spent += bytes;
        cell.resident_bytes += bytes;
        const bool was_resident =
            cell.state == CellState::Resident || cell.state == CellState::Activated;
        cell.resident_channels = cell.resident_channels | missing;
        if (was_resident) {
            // "WHEN a source's channel mask gains physics, THEN the physics payload SHALL stream
            // for the cells already resident, WITHOUT RELOADING THEM." The state is untouched and
            // only the delta was paid for.
            ++report.channel_deltas;
            if (Status emitted = emit(CellEventKind::ChannelsChanged, cell); !emitted) {
                return emitted;
            }
            continue;
        }
        cell.state = CellState::Resident;
        ++report.cells_made_resident;
        if (Status emitted = emit(CellEventKind::Resident, cell); !emitted) {
            return emitted;
        }
    }
    return ok();
}

Status WorldStreaming::spend_activation(const StreamingBudget& budget,
                                        TickReport& report) noexcept {
    for (const u32 index : work_.span()) {
        CellRuntime& cell = cells_[index];
        if (!cell.required || !cell.wanted_active) {
            continue;
        }
        if (cell.state != CellState::Resident) {
            // Resident and nothing else. A cell that is not resident has no bytes to prepare from,
            // and one that is already activated is done — which is the whole of "residency and
            // activation are independently controllable" as control flow.
            continue;
        }
        const Nanoseconds remaining =
            budget.activation_time_per_tick - report.activation_time_spent;
        if (remaining <= 0) {
            cell.blocking = "budget-blocked: activation time";
            ++report.deferred;
            break;
        }

        const Nanoseconds before = cell.activation.spent_preparing();
        Expected<StagingPhase, Error> phase =
            cell.activation.advance(cell.cooked, layers_, overlay_, remaining);
        if (!phase) {
            (void)emit(CellEventKind::Failed, cell);
            return Status{make_unexpected(phase.error())};
        }
        report.activation_time_spent += cell.activation.spent_preparing() - before;

        if (*phase != StagingPhase::Ready) {
            cell.blocking = "preparing";
            ++report.deferred;
            continue;
        }

        const Nanoseconds estimate = cell.cooked.cost.activation_time;
        if (Status published = cell.activation.publish(*ecs_, layers_); !published) {
            cell.blocking = "publication failed";
            (void)emit(CellEventKind::Failed, cell);
            return published;
        }
        report.activation_time_spent += estimate_activation_time(cell.cooked.row_count(), 0);
        cell.state = CellState::Activated;
        cell.blocking = "";
        ++report.cells_activated;
        ++total_activations_;

        // "Estimates SHALL be validated against measured runtime cost, and significant divergence
        // SHALL be reported so the model does not silently drift."
        const Nanoseconds measured = cell.activation.measured_activation_time();
        if (estimate > 0) {
            const f32 divergence =
                static_cast<f32>(static_cast<f64>(measured) / static_cast<f64>(estimate));
            if (divergence > report.worst_cost_divergence) {
                report.worst_cost_divergence = divergence;
                report.worst_cost_divergence_cell = cell.cooked.id;
            }
        }
        if (Status emitted = emit(CellEventKind::Activated, cell); !emitted) {
            return emitted;
        }
    }
    return ok();
}

Status WorldStreaming::withdraw_unrequired(TickReport& report) noexcept {
    for (CellRuntime& cell : cells_.span()) {
        if (cell.state != CellState::Activated || (cell.required && cell.wanted_active)) {
            continue;
        }
        cell.state = CellState::Deactivating;
        if (Status withdrawn = cell.activation.withdraw(*ecs_); !withdrawn) {
            return withdrawn;
        }
        // A checkpoint at deactivation: the persistent position of everything standing in this cell
        // is written now, which is one of the three moments the specification names and the reason
        // a moving entity's position is not written continuously.
        if (overlay_ != nullptr) {
            if (Status checkpointed = dynamic_.checkpoint(partitioner_->config(), *overlay_);
                !checkpointed) {
                return checkpointed;
            }
        }
        cell.state = cell.required ? CellState::Resident : CellState::Evictable;
        ++report.cells_deactivated;
        if (Status emitted = emit(CellEventKind::Deactivated, cell); !emitted) {
            return emitted;
        }
    }
    for (CellRuntime& cell : cells_.span()) {
        if (!cell.required && cell.state == CellState::Resident) {
            cell.state = CellState::Evictable;
        }
    }
    return ok();
}

Status WorldStreaming::evict(const StreamingBudget& budget, TickReport& report) noexcept {
    auto staged_total = [this]() noexcept {
        u64 total = 0;
        for (const CellRuntime& cell : cells_.span()) {
            total += cell.activation.staged_bytes();
        }
        return total;
    };

    u64 total = staged_total();
    while (total > budget.entity_memory_bytes) {
        // Lowest priority first, and only cells nothing requires. An evictable cell is "resident
        // but not needed; memory reclaimable" — evicting a required one would be thrash rather than
        // pressure relief.
        CellRuntime* victim = nullptr;
        for (CellRuntime& cell : cells_.span()) {
            if (cell.required || cell.state != CellState::Evictable ||
                cell.activation.staged_bytes() == 0) {
                continue;
            }
            if (victim == nullptr || cell.priority < victim->priority) {
                victim = &cell;
            }
        }
        if (victim == nullptr) {
            break;
        }
        const u64 freed = victim->activation.staged_bytes();
        victim->activation.release();
        // Back to `Metadata`, not `Unloaded`: the index entry, the bounds, the cost and the
        // dependencies are still here — only the streamed data went. `Unloaded` is what
        // `state_of()` answers for a cell that is not in this world at all.
        victim->state = CellState::Metadata;
        victim->resident_channels = ChannelMask::none();
        victim->resident_bytes = 0;
        ++report.cells_evicted;
        ++total_evictions_;
        if (Status emitted = emit(CellEventKind::Evicted, *victim); !emitted) {
            return emitted;
        }
        total = (freed >= total) ? 0 : total - freed;
    }
    if (total > budget.entity_memory_bytes) {
        // Reported, not silently exceeded: everything left is required, and the caller has to know
        // that the budget it set cannot be met by what it is asking for.
        report.memory_shortfall = total - budget.entity_memory_bytes;
    }
    return ok();
}

Status WorldStreaming::refresh_hlod() noexcept {
    const WorldStreaming& self = *this;
    return hlod_.refresh([&self](CellId cell) noexcept {
        const CellRuntime* runtime = self.find_cell(cell);
        return runtime != nullptr && runtime->state == CellState::Activated;
    });
}

Expected<TickReport, Error> WorldStreaming::tick(const StreamingBudget& budget) noexcept {
    TickReport report;

    // Deferred requests are taken up HERE and nowhere else. A consumer that reacted to last tick's
    // activation by requesting another cell is serviced at the start of this one, which is what
    // "the request SHALL be queued, not processed re-entrantly" means once the callbacks are gone.
    for (const DeferredRequest& deferred : deferred_.span()) {
        CellRuntime* cell = find_cell(deferred.cell);
        if (cell == nullptr) {
            continue;
        }
        cell->pinned = true;
        cell->pinned_channels = cell->pinned_channels | deferred.channels;
        cell->pinned_activate = cell->pinned_activate || deferred.activate;
    }
    deferred_.clear();

    if (Status gathered = gather_requirements(); !gathered) {
        return make_unexpected(gathered.error());
    }
    if (Status closed = close_hard_dependencies(); !closed) {
        return make_unexpected(closed.error());
    }
    order_work();
    if (Status spent = spend_io(budget, report); !spent) {
        return make_unexpected(spent.error());
    }
    if (Status spent = spend_activation(budget, report); !spent) {
        return make_unexpected(spent.error());
    }
    if (Status withdrawn = withdraw_unrequired(report); !withdrawn) {
        return make_unexpected(withdrawn.error());
    }
    if (Status evicted = evict(budget, report); !evicted) {
        return make_unexpected(evicted.error());
    }
    if (Status refreshed = refresh_hlod(); !refreshed) {
        return make_unexpected(refreshed.error());
    }
    events_.compact();
    return report;
}

CellExplanation WorldStreaming::explain(CellId cell) const noexcept {
    CellExplanation explanation;
    const CellRuntime* runtime = find_cell(cell);
    if (runtime == nullptr) {
        explanation.blocking = "this cell is not in the world index";
        return explanation;
    }
    explanation.known = true;
    explanation.state = runtime->state;
    explanation.phase = runtime->activation.phase();
    explanation.requested = runtime->required;
    explanation.wanted_active = runtime->wanted_active;
    explanation.requiring_sources = runtime->requiring_sources;
    explanation.leading_source = runtime->leading_source;
    explanation.priority = runtime->priority;
    explanation.klass = runtime->klass;
    explanation.time_until_needed = runtime->time_until_needed;
    explanation.required_channels = runtime->required_channels;
    explanation.resident_channels = runtime->resident_channels;
    explanation.blocking = runtime->blocking;
    return explanation;
}

StreamingStats WorldStreaming::stats() const noexcept {
    StreamingStats stats;
    for (const CellRuntime& cell : cells_.span()) {
        stats.by_state[static_cast<u32>(cell.state)] += 1;
        stats.staged_bytes += cell.activation.staged_bytes();
        stats.resident_io_bytes += cell.resident_bytes;
        stats.published_entities += cell.activation.published_rows();
    }
    stats.total_evictions = total_evictions_;
    stats.total_activations = total_activations_;
    return stats;
}

Status WorldStreaming::close_dependencies(const CellRuntime& holder, Array<CellId>& closure,
                                          u64& bytes) noexcept {
    // Breadth-first over `RequireLoaded` links: the transitive closure of hard cell dependencies
    // the cooker has to compute, with `closure` doubling as the queue and the visited set.
    bytes = 0;
    for (const CellId direct : holder.cooked.hard_dependencies.span()) {
        if (Status pushed = closure.push_back(direct); !pushed) {
            return pushed;
        }
    }
    for (usize index = 0; index < closure.size(); ++index) {
        const CellRuntime* linked = find_cell(closure[index]);
        if (linked == nullptr) {
            continue;
        }
        bytes += linked->cooked.cost.io_bytes;
        for (const CellId next : linked->cooked.hard_dependencies.span()) {
            if (next == holder.cooked.id || contains(closure.span(), next)) {
                continue;
            }
            if (Status pushed = closure.push_back(next); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status WorldStreaming::dependency_report(u32 cell_limit, Array<DependencyReport>& out) noexcept {
    Array<CellId> closure(*allocator_);
    for (const CellRuntime& cell : cells_.span()) {
        closure.clear();
        DependencyReport report;
        report.cell = cell.cooked.id;
        if (Status closed = close_dependencies(cell, closure, report.forced_bytes); !closed) {
            return closed;
        }
        if (closure.empty()) {
            continue;
        }
        // "The report SHALL name the reference chain responsible, so the cause is actionable rather
        // than a number." The first link is where the chain starts.
        report.first_link = closure[0];
        report.forced_cells = static_cast<u32>(closure.size());
        report.over_threshold = report.forced_cells > cell_limit;
        if (Status pushed = out.push_back(report); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::world
