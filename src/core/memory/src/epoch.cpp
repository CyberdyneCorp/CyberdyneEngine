// Retirement and frame epochs. Task 2.8.

#include <cy/core/memory/epoch.h>

namespace cy {

EpochManager::EpochManager(u32 capacity, Allocator& allocator) noexcept
    : queue_(allocator), capacity_(capacity) {}

Status EpochManager::initialize() noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return queue_.reserve(capacity_);
}

Epoch EpochManager::current() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return current_;
}

Epoch EpochManager::completed() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return completed_;
}

Epoch EpochManager::advance() noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return ++current_;
}

void EpochManager::complete(Epoch epoch) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (epoch > completed_) {
        completed_ = epoch;
        completed_moved_at_ = current_;
    }
}

Status EpochManager::retire(void* resource, ReclaimFn on_reclaim, void* user,
                            const char* tag) noexcept {
    if (on_reclaim == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a retirement needs a reclaim function");
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    if (queue_.size() >= capacity_) {
        ++refused_;
        return fail(ErrorCode::OutOfRange,
                    "the retirement queue is full — a consumer is not completing its epochs");
    }
    Retirement entry;
    entry.resource = resource;
    entry.reclaim = on_reclaim;
    entry.user = user;
    entry.tag = (tag != nullptr) ? tag : "";
    entry.epoch = current_;
    if (Status pushed = queue_.push_back(entry); !pushed) {
        ++refused_;
        return pushed;
    }
    ++retired_;
    const auto depth = static_cast<u32>(queue_.size());
    peak_depth_ = (depth > peak_depth_) ? depth : peak_depth_;
    return ok();
}

u32 EpochManager::take_reclaimable(Retirement* out, u32 capacity, bool everything) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    u32 taken = 0;
    // The queue is in retirement order, so the reclaimable entries are a prefix of it: epochs never
    // decrease. Stopping at the first entry that is not reclaimable is therefore correct and makes
    // this O(reclaimed) rather than O(depth).
    while (taken < capacity && !queue_.empty() && (everything || queue_[0].epoch <= completed_)) {
        out[taken++] = queue_[0];
        queue_.erase(0);
    }
    reclaimed_ += taken;
    return taken;
}

u32 EpochManager::reclaim() noexcept {
    // A fixed batch per call, drained repeatedly: the reclaim functions run outside the lock, so
    // they are copied out first, and copying the whole queue would need an allocation on a path
    // that must not have one.
    constexpr u32 kBatch = 64;
    Retirement batch[kBatch];
    u32 total = 0;
    for (;;) {
        const u32 taken = take_reclaimable(batch, kBatch, false);
        for (u32 index = 0; index < taken; ++index) {
            batch[index].reclaim(batch[index].resource, batch[index].user);
        }
        total += taken;
        if (taken < kBatch) {
            return total;
        }
    }
}

u32 EpochManager::reclaim_all() noexcept {
    constexpr u32 kBatch = 64;
    Retirement batch[kBatch];
    u32 total = 0;
    for (;;) {
        const u32 taken = take_reclaimable(batch, kBatch, true);
        for (u32 index = 0; index < taken; ++index) {
            batch[index].reclaim(batch[index].resource, batch[index].user);
        }
        total += taken;
        if (taken < kBatch) {
            return total;
        }
    }
}

RetirementStats EpochManager::stats() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    RetirementStats out;
    out.retired = retired_;
    out.reclaimed = reclaimed_;
    out.depth = static_cast<u32>(queue_.size());
    out.peak_depth = peak_depth_;
    out.capacity = capacity_;
    out.refused = refused_;
    out.current = current_;
    out.completed = completed_;
    out.oldest_pending = queue_.empty() ? 0 : queue_[0].epoch;
    return out;
}

u64 EpochManager::stalled_epochs() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return (queue_.empty() || current_ <= completed_moved_at_) ? 0 : current_ - completed_moved_at_;
}

u32 EpochManager::pending(RetirementEntry* out, u32 capacity) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto count = static_cast<u32>((queue_.size() < capacity) ? queue_.size() : capacity);
    for (u32 index = 0; index < count; ++index) {
        out[index].resource = queue_[index].resource;
        out[index].epoch = queue_[index].epoch;
        out[index].tag = queue_[index].tag;
    }
    return count;
}

EpochManager& default_epoch_manager() noexcept {
    static EpochManager manager;
    static const bool initialised = manager.initialize().has_value();
    (void)initialised;
    return manager;
}

}  // namespace cy
