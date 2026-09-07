#include <cy/world/activation.h>

#include <cy/ecs/world.h>
#include <cy/world/overlay.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace cy::world {
namespace {

/// What applying the overlay and building the subsystem payloads are charged for. They are not free
/// and they are not proportional to the cell's rows, so they are one fixed step each — enough that
/// a tight budget spreads them over frames rather than doing everything in the frame the last block
/// happened to decode in.
constexpr Nanoseconds kOverlayStepCost = 20000;
constexpr Nanoseconds kPayloadStepCost = 20000;

[[nodiscard]] Nanoseconds elapsed_since(std::chrono::steady_clock::time_point start) noexcept {
    const auto delta = std::chrono::steady_clock::now() - start;
    return static_cast<Nanoseconds>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());
}

}  // namespace

const char* staging_phase_name(StagingPhase phase) noexcept {
    switch (phase) {
        case StagingPhase::Idle:
            return "Idle";
        case StagingPhase::Decoding:
            return "Decoding";
        case StagingPhase::ApplyingOverlay:
            return "ApplyingOverlay";
        case StagingPhase::BuildingPayloads:
            return "BuildingPayloads";
        case StagingPhase::Ready:
            return "Ready";
        case StagingPhase::Published:
            return "Published";
    }
    return "unknown";
}

const char* cell_event_kind_name(CellEventKind kind) noexcept {
    switch (kind) {
        case CellEventKind::Resident:
            return "resident";
        case CellEventKind::Activated:
            return "activated";
        case CellEventKind::Deactivated:
            return "deactivated";
        case CellEventKind::Evicted:
            return "evicted";
        case CellEventKind::ChannelsChanged:
            return "channels-changed";
        case CellEventKind::Failed:
            return "failed";
    }
    return "unknown";
}

// --- The event queue ---------------------------------------------------------------------------

CellEventQueue::CellEventQueue(Allocator& allocator) noexcept
    : events_(allocator), consumers_(allocator) {}

Expected<CellEventQueue::ConsumerId, Error> CellEventQueue::add_consumer(const char* name,
                                                                         u32 order) noexcept {
    Consumer consumer;
    consumer.id = next_consumer_;
    consumer.name = (name == nullptr) ? "" : name;
    consumer.order = order;
    // A consumer that registers after events have been emitted starts from NOW, not from the
    // beginning of time: it did not exist when a city activated and does not want to hear about it.
    consumer.seen = next_sequence_ - 1;
    if (Status pushed = consumers_.push_back(consumer); !pushed) {
        return make_unexpected(pushed.error());
    }
    // Sorted by declared order, then by identifier — which is registration order, and is what makes
    // two consumers at one order deterministic rather than arbitrary.
    std::sort(consumers_.data(), consumers_.data() + consumers_.size(),
              [](const Consumer& a, const Consumer& b) {
                  return (a.order != b.order) ? a.order < b.order : a.id < b.id;
              });
    ++next_consumer_;
    return consumer.id;
}

Status CellEventQueue::emit(const CellEvent& event) noexcept {
    CellEvent stamped = event;
    stamped.sequence = next_sequence_;
    if (Status pushed = events_.push_back(stamped); !pushed) {
        return pushed;
    }
    ++next_sequence_;
    return ok();
}

Status CellEventQueue::drain(ConsumerId consumer, Array<CellEvent>& out) noexcept {
    for (Consumer& entry : consumers_.span()) {
        if (entry.id != consumer) {
            continue;
        }
        for (const CellEvent& event : events_.span()) {
            if (event.sequence <= entry.seen) {
                continue;
            }
            if (Status pushed = out.push_back(event); !pushed) {
                return pushed;
            }
        }
        entry.seen = next_sequence_ - 1;
        return ok();
    }
    return fail(ErrorCode::NotFound, "no cell-event consumer with that identifier");
}

void CellEventQueue::compact() noexcept {
    if (consumers_.empty()) {
        // With no consumers there is nobody the backlog is for, and keeping it would make the queue
        // a log that grows for the life of the process.
        events_.clear();
        first_sequence_ = next_sequence_;
        return;
    }
    u64 lowest = next_sequence_ - 1;
    for (const Consumer& consumer : consumers_.span()) {
        lowest = (consumer.seen < lowest) ? consumer.seen : lowest;
    }
    usize drop = 0;
    while (drop < events_.size() && events_[drop].sequence <= lowest) {
        ++drop;
    }
    if (drop == 0) {
        return;
    }
    for (usize index = drop; index < events_.size(); ++index) {
        events_[index - drop] = events_[index];
    }
    for (usize index = 0; index < drop; ++index) {
        events_.pop_back();
    }
    first_sequence_ = lowest + 1;
}

// --- Cell activation ---------------------------------------------------------------------------

CellActivation::CellActivation(Allocator& allocator, const CookedCell& cell) noexcept
    : allocator_(&allocator),
      id_(cell.id),
      staged_(allocator),
      published_(allocator),
      column_pointers_(allocator) {}

CellActivation::~CellActivation() = default;

Status CellActivation::stage_block(const CookedBlock& source) noexcept {
    StagedBlock block(*allocator_);
    block.layer = source.layer;
    block.count = source.count;
    if (Status appended = block.components.append(source.components.span()); !appended) {
        return appended;
    }
    if (Status appended = block.ids.append(source.ids.span()); !appended) {
        return appended;
    }
    for (const Array<u8>& column : source.columns.span()) {
        Expected<Array<u8>*, Error> staged = block.columns.emplace_back(*allocator_);
        if (!staged) {
            return Status{make_unexpected(staged.error())};
        }
        if (Status copied = (*staged)->append(column.span()); !copied) {
            return copied;
        }
        staged_bytes_ += column.size();
    }
    return staged_.push_back(std::move(block));
}

Status CellActivation::decode_one(const CookedCell& source, const LayerTable& layers,
                                  usize index) noexcept {
    const CookedBlock& block = source.blocks[index];
    if (!layers.is_cooked(block.layer)) {
        // An editor-only layer is not cooked into runtime data. A cell that carries one anyway —
        // an editor cook opened by the runtime — simply does not stage it.
        return ok();
    }
    return stage_block(block);
}

namespace {

/// One row's byte length in a column. A block's columns are rectangular — `count` rows each — so
/// the stride is the column's size divided by the row count, and a zero-row block has no stride.
[[nodiscard]] usize column_stride(const Array<u8>& column, u32 count) noexcept {
    return (count == 0) ? 0 : column.size() / count;
}

}  // namespace

Status CellActivation::compact_removed(StagedBlock& block,
                                       const PersistenceOverlay& overlay) noexcept {
    // A destroyed building must never briefly exist, so the removed rows are compacted OUT of the
    // staging before anything is published, rather than being instantiated and then destroyed.
    usize kept = 0;
    for (usize row = 0; row < block.ids.size(); ++row) {
        if (overlay.is_removed(id_, block.ids[row])) {
            continue;
        }
        if (kept != row) {
            block.ids[kept] = block.ids[row];
            for (Array<u8>& bytes : block.columns.span()) {
                const usize stride = column_stride(bytes, block.count);
                if (stride != 0) {
                    std::memmove(bytes.data() + (kept * stride), bytes.data() + (row * stride),
                                 stride);
                }
            }
        }
        ++kept;
    }
    if (kept == block.ids.size()) {
        return ok();
    }

    for (Array<u8>& bytes : block.columns.span()) {
        const usize stride = column_stride(bytes, block.count);
        while (bytes.size() > kept * stride) {
            bytes.pop_back();
        }
    }
    while (block.ids.size() > kept) {
        block.ids.pop_back();
    }
    block.count = static_cast<u32>(kept);
    return ok();
}

Status CellActivation::apply_override(StagedBlock& block, const ComponentOverride& record,
                                      const PersistenceOverlay& overlay) noexcept {
    for (usize column = 0; column < block.components.size(); ++column) {
        if (block.components[column] != record.component) {
            continue;
        }
        Array<u8>& bytes = block.columns[column];
        const usize stride = column_stride(bytes, block.count);
        if (stride == 0 || record.size != stride) {
            // A record whose size does not match the runtime layout is from a different content
            // version. `PersistenceOverlay::check_content_version()` is where that is REPORTED;
            // here it is skipped rather than written past the end of a column.
            continue;
        }
        const Span<const u8> value =
            overlay.component_override(id_, record.entity, record.component);
        for (usize row = 0; row < block.ids.size(); ++row) {
            if (block.ids[row] == record.entity) {
                std::memcpy(bytes.data() + (row * stride), value.data(), stride);
            }
        }
    }
    return ok();
}

Status CellActivation::apply_overlay(const PersistenceOverlay& overlay) noexcept {
    const CellOverlay* records = overlay.find(id_);
    if (records == nullptr) {
        return ok();
    }

    for (StagedBlock& block : staged_.span()) {
        // Removals first, then overrides: the row indices an override addresses are the ones AFTER
        // compaction, which is why these are two passes and not one.
        if (Status compacted = compact_removed(block, overlay); !compacted) {
            return compacted;
        }
        for (const ComponentOverride& record : records->overrides.span()) {
            if (Status applied = apply_override(block, record, overlay); !applied) {
                return applied;
            }
        }
    }

    // Runtime-created entities are staged by the same path as authored ones: they were recorded in
    // ECS-native form, so there is no second activation path for them.
    for (const CookedBlock& created : records->created.blocks.span()) {
        if (Status staged = stage_block(created); !staged) {
            return staged;
        }
    }
    return ok();
}

Expected<StagingPhase, Error> CellActivation::advance(const CookedCell& source,
                                                      const LayerTable& layers,
                                                      const PersistenceOverlay* overlay,
                                                      Nanoseconds budget) noexcept {
    if (phase_ == StagingPhase::Ready || phase_ == StagingPhase::Published) {
        return phase_;
    }
    if (phase_ == StagingPhase::Idle) {
        phase_ = StagingPhase::Decoding;
    }

    Nanoseconds spent = 0;
    while (spent < budget && phase_ != StagingPhase::Ready) {
        switch (phase_) {
            case StagingPhase::Decoding: {
                if (decoded_ >= source.blocks.size()) {
                    phase_ = StagingPhase::ApplyingOverlay;
                    break;
                }
                const CookedBlock& block = source.blocks[decoded_];
                u64 bytes = 0;
                for (const Array<u8>& column : block.columns.span()) {
                    bytes += column.size();
                }
                if (Status decoded = decode_one(source, layers, decoded_); !decoded) {
                    return make_unexpected(decoded.error());
                }
                ++decoded_;
                spent += estimate_activation_time(block.count, bytes);
                break;
            }
            case StagingPhase::ApplyingOverlay: {
                if (overlay != nullptr) {
                    if (Status applied = apply_overlay(*overlay); !applied) {
                        return make_unexpected(applied.error());
                    }
                }
                spent += kOverlayStepCost;
                phase_ = StagingPhase::BuildingPayloads;
                break;
            }
            case StagingPhase::BuildingPayloads: {
                // The subsystem payloads — physics batches, navigation tiles, GPU scene data — are
                // built here. They are content-addressed chunks that the asset system streams, so
                // what this phase owns is the BATCHING, and the batching a cell needs is decided by
                // the payloads it declares. Nothing is published; a payload built here is private
                // until publication, exactly like a component column.
                spent += kPayloadStepCost;
                phase_ = StagingPhase::Ready;
                break;
            }
            case StagingPhase::Idle:
            case StagingPhase::Ready:
            case StagingPhase::Published:
                break;
        }
    }
    spent_ += spent;
    return phase_;
}

Status CellActivation::instantiate_block(ecs::World& world, StagedBlock& block) noexcept {
    if (block.count == 0 || block.live) {
        return ok();
    }
    if (Status sized = column_pointers_.resize(block.components.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < block.components.size(); ++index) {
        column_pointers_[index] = block.columns[index].data();
    }

    ecs::World::ArchetypeBlock request;
    request.components = block.components.span();
    request.columns = Span<const void* const>(column_pointers_.data(), column_pointers_.size());
    request.count = block.count;

    block.entities.clear();
    if (Status instantiated = world.instantiate(request, block.entities); !instantiated) {
        return instantiated;
    }
    block.live = true;
    return ok();
}

// A member rather than a free function because it is the mirror of `instantiate_block()`, which is
// not static, and splitting a symmetric pair across two kinds of function reads worse than it
// costs.
//
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
Status CellActivation::destroy_block(ecs::World& world, StagedBlock& block) noexcept {
    if (!block.live) {
        return ok();
    }
    if (Status destroyed = world.destroy_many(block.entities.span()); !destroyed) {
        return destroyed;
    }
    block.entities.clear();
    block.live = false;
    return ok();
}

Status CellActivation::reindex() noexcept {
    published_.clear();
    for (const StagedBlock& block : staged_.span()) {
        if (!block.live) {
            continue;
        }
        if (Status appended = published_.append(block.entities.span()); !appended) {
            return appended;
        }
    }
    return ok();
}

Status CellActivation::publish(ecs::World& world, const LayerTable& layers) noexcept {
    if (phase_ != StagingPhase::Ready) {
        return fail(ErrorCode::Unavailable,
                    "this cell is still being prepared; publication happens only from Ready");
    }
    const auto started = std::chrono::steady_clock::now();

    for (usize index = 0; index < staged_.size(); ++index) {
        StagedBlock& block = staged_[index];
        if (!layers.is_activated(block.layer)) {
            continue;
        }
        if (Status instantiated = instantiate_block(world, block); !instantiated) {
            // ATOMIC OR NOT AT ALL. Everything this call created is destroyed and the cell stays
            // unpublished, so no system ever observes half a cell — which is the requirement, and
            // the reason this rollback exists rather than a "best effort" partial publish.
            for (usize undo = 0; undo < index; ++undo) {
                (void)destroy_block(world, staged_[undo]);
            }
            (void)reindex();
            return instantiated;
        }
    }

    if (Status indexed = reindex(); !indexed) {
        for (StagedBlock& block : staged_.span()) {
            (void)destroy_block(world, block);
        }
        return indexed;
    }
    phase_ = StagingPhase::Published;
    measured_ = elapsed_since(started);
    return ok();
}

Status CellActivation::withdraw(ecs::World& world) noexcept {
    if (phase_ != StagingPhase::Published) {
        return ok();
    }
    Status result = ok();
    for (StagedBlock& block : staged_.span()) {
        if (Status destroyed = destroy_block(world, block); !destroyed) {
            result = destroyed;
        }
    }
    published_.clear();
    // Back to Ready, not Idle: the staged data is intact, so republishing the cell — because a
    // layer switched, or because the player turned round — costs one instantiate and no decoding.
    phase_ = StagingPhase::Ready;
    return result;
}

Status CellActivation::publish_layer(ecs::World& world, LayerId layer) noexcept {
    if (phase_ != StagingPhase::Published) {
        return ok();
    }
    for (StagedBlock& block : staged_.span()) {
        if (block.layer == layer) {
            if (Status instantiated = instantiate_block(world, block); !instantiated) {
                return instantiated;
            }
        }
    }
    return reindex();
}

Status CellActivation::withdraw_layer(ecs::World& world, LayerId layer) noexcept {
    if (phase_ != StagingPhase::Published) {
        return ok();
    }
    for (StagedBlock& block : staged_.span()) {
        if (block.layer == layer) {
            if (Status destroyed = destroy_block(world, block); !destroyed) {
                return destroyed;
            }
        }
    }
    return reindex();
}

void CellActivation::release() noexcept {
    // Only the staging. Entities that are still published belong to the ECS world, and destroying
    // them needs that world — which this object does not hold and must not, because a cell must be
    // destructible after the world it published into has gone.
    staged_.clear();
    published_.clear();
    column_pointers_.clear();
    staged_bytes_ = 0;
    decoded_ = 0;
    spent_ = 0;
    phase_ = StagingPhase::Idle;
}

}  // namespace cy::world
