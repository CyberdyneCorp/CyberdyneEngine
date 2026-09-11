// Checkpoints, retained in memory, restored without restarting.

#include <cy/save/checkpoint.h>

#include <cy/save/container.h>

namespace cy::save {
namespace {

/// Count what a restore reports, so the caller sees what it got rather than inferring it.
void count(const Overlay& overlay, u32& regions, u32& entries) noexcept {
    regions = static_cast<u32>(overlay.regions().size());
    entries = 0;
    for (const Region& region : overlay.regions()) {
        entries += static_cast<u32>(region.entries.size());
    }
}

}  // namespace

Expected<u64, Error> measure_overlay_bytes(const Overlay& overlay) noexcept {
    Array<u8> scratch(current_allocator());
    u64 total = 0;
    for (const Region& region : overlay.regions()) {
        const Status encoded = encode_region(overlay, region.key, scratch);
        if (!encoded) {
            return fail(encoded.error().code, "a region could not be measured");
        }
        total += scratch.size();
    }
    for (u8 scope = 0; scope < static_cast<u8>(Scope::Count); ++scope) {
        const Status encoded = encode_fragments(overlay, static_cast<Scope>(scope), scratch);
        if (!encoded) {
            return fail(encoded.error().code, "a scope's fragments could not be measured");
        }
        total += scratch.size();
    }
    return total;
}

Status CheckpointStore::configure(const CheckpointConfig& config) noexcept {
    if (config.memory_slots == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a checkpoint store with no slots retains nothing; do not create one");
    }
    config_ = config;
    while (static_cast<u32>(slots_.size()) > config_.memory_slots) {
        evict_oldest();
    }
    return {};
}

u64 CheckpointStore::resident_bytes() const noexcept {
    u64 total = 0;
    for (const Slot& slot : slots_.span()) {
        total += slot.bytes;
    }
    return total;
}

CheckpointInfo* CheckpointStore::find_info(u32 id) noexcept {
    for (CheckpointInfo& info : history_.span()) {
        if (info.id == id) {
            return &info;
        }
    }
    return nullptr;
}

void CheckpointStore::evict_oldest() noexcept {
    if (slots_.empty()) {
        return;
    }
    const u32 id = slots_[0].id;
    if (CheckpointInfo* info = find_info(id); info != nullptr) {
        info->resident = false;  // it existed, and it is gone; both facts survive
    }
    slots_.erase(0);
    ++evictions_;
}

Expected<CheckpointInfo, Error> CheckpointStore::take(const Overlay& live,
                                                      determinism::SimulationPoint point) noexcept {
    const Expected<u64, Error> measured = measure_overlay_bytes(live);
    if (!measured) {
        return fail(measured.error().code, "the checkpoint could not be measured");
    }
    const u64 bytes = measured.value();
    if (bytes > config_.memory_budget_bytes) {
        // Refusing is the honest answer. Evicting everything and retaining it anyway would put the
        // store over the budget it was given, and retaining nothing silently would turn this into a
        // restore failure at the moment a player died.
        return fail(ErrorCode::OutOfMemory,
                    "one checkpoint alone exceeds the store's memory budget");
    }

    while (!slots_.empty() && (static_cast<u32>(slots_.size()) >= config_.memory_slots ||
                               resident_bytes() + bytes > config_.memory_budget_bytes)) {
        evict_oldest();
    }

    Slot slot(*allocator_);
    slot.id = next_id_;
    slot.point = point;
    slot.bytes = bytes;
    const Status cloned = live.clone_into(slot.state);
    if (!cloned) {
        return fail(cloned.error().code, "the checkpoint's state could not be cloned");
    }

    CheckpointInfo info;
    info.id = next_id_;
    info.point = point;
    info.bytes = bytes;
    info.resident = true;
    count(slot.state, info.regions, info.entries);

    if (const Status pushed = slots_.push_back(std::move(slot)); !pushed) {
        return fail(pushed.error().code, "the checkpoint could not be retained");
    }
    if (const Status recorded = history_.push_back(info); !recorded) {
        return fail(recorded.error().code, "the checkpoint's history entry could not be recorded");
    }
    ++next_id_;
    return info;
}

Expected<RestoreReport, Error> CheckpointStore::restore(Overlay& out,
                                                        determinism::EpochCounter& epoch) noexcept {
    if (slots_.empty()) {
        return fail(ErrorCode::NotFound, "no checkpoint is retained");
    }
    return restore(slots_[slots_.size() - 1].id, out, epoch);
}

Expected<RestoreReport, Error> CheckpointStore::restore(u32 id, Overlay& out,
                                                        determinism::EpochCounter& epoch) noexcept {
    const Slot* found = nullptr;
    for (const Slot& slot : slots_.span()) {
        if (slot.id == id) {
            found = &slot;
            break;
        }
    }
    if (found == nullptr) {
        // Two answers, not one: never taken, or taken and evicted. A caller that retries is doing
        // the right thing in the first case and the wrong thing in the second.
        const bool existed = find_info(id) != nullptr;
        return fail(existed ? ErrorCode::Unavailable : ErrorCode::NotFound,
                    existed ? "that checkpoint was evicted to stay inside the memory budget"
                            : "no checkpoint with that identifier was ever taken");
    }

    out.clear();
    if (const Status merged = out.merge(found->state); !merged) {
        return fail(merged.error().code, "the checkpoint could not be restored into the overlay");
    }

    RestoreReport report;
    report.id = found->id;
    report.captured_at = found->point;
    report.epoch_before = epoch.current();
    // THE REQUIREMENT. Every temporal cache, handle, history and log that carries a stamp compares
    // epochs; a restore that left the epoch alone would move the tick backwards inside one timeline
    // and every one of them would believe itself current.
    report.epoch_after = epoch.advance(determinism::EpochReason::CheckpointRestore);
    count(out, report.regions, report.entries);
    return report;
}

void CheckpointStore::clear() noexcept {
    while (!slots_.empty()) {
        evict_oldest();
    }
    history_.clear();
    next_id_ = 1;
    evictions_ = 0;
}

}  // namespace cy::save
