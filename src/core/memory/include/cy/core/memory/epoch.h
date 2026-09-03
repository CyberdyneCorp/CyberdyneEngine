#pragma once
// Retirement and frame epochs. Task 2.8.
//
// `core-memory-and-containers` — "Retirement and frame epochs": a resource that may still be
// referenced by an in-flight frame or a running task is RETIRED rather than freed — logically
// released, then reclaimed once no in-flight consumer can still reference it. ONE mechanism serves
// every consumer: GPU resources, asset pages, published snapshots, command buffers, task records.
// Retirement queues are bounded and reportable, and a queue growing without draining is detectable,
// since it indicates a consumer that never completes.
//
// THE MODEL IN THREE NUMBERS.
//   `current()`    the epoch work is being submitted into. `advance()` moves it on, once a frame.
//   `completed()`  the newest epoch every consumer has finished with. Consumers report it.
//   a retirement's own epoch, which is `current()` at the moment it was retired.
// A retirement is reclaimable when its epoch is at or below `completed()`. That is the whole rule,
// and it is why one mechanism can serve five subsystems: each of them means something different by
// "finished", and all of them mean it by reporting a completed epoch.
//
// WHY IT IS BOUNDED. An unbounded retirement queue turns a consumer that stopped completing into
// an out-of-memory failure a long way from the cause. A bounded one turns it into a refusal at the
// point of retirement, with `stalled_epochs()` naming the epoch that has not advanced.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>

#include <mutex>

namespace cy {

using Epoch = u64;

/// How a retired resource is actually released. A function pointer and a user word rather than a
/// std::function: retirement happens on paths that must not allocate, and a captured lambda would.
using ReclaimFn = void (*)(void* resource, void* user) noexcept;

struct RetirementStats {
    u64 retired = 0;    // total ever retired
    u64 reclaimed = 0;  // total ever reclaimed
    u32 depth = 0;      // waiting now
    u32 peak_depth = 0;
    u32 capacity = 0;
    u64 refused = 0;  // retirements the bounded queue could not accept
    Epoch current = 0;
    Epoch completed = 0;
    Epoch oldest_pending = 0;
};

/// One retirement, as a report presents it.
struct RetirementEntry {
    void* resource = nullptr;
    Epoch epoch = 0;
    const char* tag = "";
};

class EpochManager {
public:
    /// `capacity` is the bound. A retirement beyond it is refused and counted, which is the
    /// reportable failure the specification asks for rather than a silent resize.
    explicit EpochManager(u32 capacity = 4096, Allocator& allocator = default_allocator()) noexcept;

    EpochManager(const EpochManager&) = delete;
    EpochManager& operator=(const EpochManager&) = delete;

    /// Reserve the queue. Called once; a manager whose queue was never reserved refuses every
    /// retirement, which is a loud failure rather than an unbounded one.
    [[nodiscard]] Status initialize() noexcept;

    [[nodiscard]] Epoch current() const noexcept;
    [[nodiscard]] Epoch completed() const noexcept;

    /// Begin a new epoch and return it. One call per frame, from the thread that owns the frame.
    Epoch advance() noexcept;

    /// Report that every consumer has finished with `epoch` and everything before it. Monotonic: a
    /// report older than one already made is ignored, so two consumers reporting out of order
    /// cannot move the completed epoch backwards.
    void complete(Epoch epoch) noexcept;

    /// Retire a resource into the current epoch.
    [[nodiscard]] Status retire(void* resource, ReclaimFn on_reclaim, void* user,
                                const char* tag) noexcept;

    /// Reclaim everything whose epoch is at or below `completed()`. Returns how many were released.
    /// The reclaim functions are called OUTSIDE the lock, so a reclaim that itself retires — a
    /// resource whose release frees a second one — does not deadlock.
    u32 reclaim() noexcept;

    /// Reclaim everything regardless of epoch. Shutdown only: it is correct exactly when no
    /// consumer is in flight, and calling it while one is, is the bug retirement exists to prevent.
    u32 reclaim_all() noexcept;

    [[nodiscard]] RetirementStats stats() const noexcept;

    /// How many epochs have passed without the completed epoch moving. A queue whose depth is
    /// growing and whose stall count is rising has a consumer that never completes, and that pair
    /// of numbers is what a report says instead of "memory is going up".
    [[nodiscard]] u64 stalled_epochs() const noexcept;

    /// Copy the pending retirements, oldest first. For a report; not a hot path.
    u32 pending(RetirementEntry* out, u32 capacity) const noexcept;

private:
    struct Retirement {
        void* resource = nullptr;
        ReclaimFn reclaim = nullptr;
        void* user = nullptr;
        const char* tag = "";
        Epoch epoch = 0;
    };

    /// Move the reclaimable entries out under the lock and return how many. The caller runs them.
    u32 take_reclaimable(Retirement* out, u32 capacity, bool everything) noexcept;

    mutable std::mutex mutex_;
    Array<Retirement> queue_;  // oldest first; retirement appends, reclaim removes from the front
    u32 capacity_;
    Epoch current_ = 1;
    Epoch completed_ = 0;
    u64 retired_ = 0;
    u64 reclaimed_ = 0;
    u64 refused_ = 0;
    u32 peak_depth_ = 0;
    /// The epoch `completed_` was last moved at, so a stall is measurable in epochs rather than in
    /// wall-clock time — which is the unit the failure is actually in.
    Epoch completed_moved_at_ = 1;
};

/// The process's epoch manager. ONE mechanism, many consumers — see the note at the top of this
/// file. A subsystem that wants its own deferral scheme should be using this instead.
[[nodiscard]] EpochManager& default_epoch_manager() noexcept;

}  // namespace cy
