#pragma once
// The task context. Task 3.2.3.
//
// `core-jobs-and-concurrency`: every task receives a context providing at minimum its worker index,
// a scratch allocator for the task's lifetime, and its cancellation token. Everything else here —
// the task's name, its priority, its deadline hint, its own handle — is what a task needs in order
// to submit more work that inherits its position in the frame, and having it in one struct is what
// stops each subsystem from threading four parameters through its own call graph.
//
// The context is passed by const reference and is valid only for the duration of the body. A
// pointer to it, or to its scratch, that outlives the call is the defect the scratch poison exists
// to make visible.

#include <cy/core/jobs/cancellation.h>
#include <cy/core/jobs/scratch.h>
#include <cy/core/jobs/types.h>

namespace cy::jobs {

class JobSystem;

/// How many bytes of arguments travel *inside* a task record.
///
/// A submitter usually passes state through `user`, which it must keep alive until the task
/// completes. That is the right shape for a long-lived object and the wrong one for a handful of
/// numbers: keeping five words alive across a task means allocating them, and
/// `core-jobs-and-concurrency` requires that scheduling a task does not call the general allocator.
/// Arguments up to this size are copied into the record instead, and reach the body through
/// `TaskContext::data`. Forty-eight bytes is what `submit_parallel_for` needs for a partition,
/// which is the case that would otherwise allocate once per loop.
inline constexpr usize kMaxInlineArgumentBytes = 48;

struct TaskContext {
    /// The system that is running this task. Submitting from inside a task goes through it, so
    /// nested work lands in the same pool rather than in a second one.
    JobSystem* system = nullptr;

    /// Which participant is running the body. A diagnostic and the scratch selector — never an
    /// ordering key, because work stealing makes it a function of timing.
    ///
    /// An index below `JobSystem::worker_count()` is one of the system's own workers. At or above
    /// it is a *helper slot*: a thread the system did not start, running a task because it called
    /// `wait()` and a waiting thread does useful work rather than blocking. Both are running the
    /// task legitimately, which is why the index names the slot rather than reporting "not a
    /// worker" for half of them.
    WorkerIndex worker = kNotAWorker;

    /// This task's scratch. Reclaimed in bulk when the body returns.
    ScratchArena* scratch = nullptr;

    /// Cooperative cancellation. Long-running work checks it at bounded intervals.
    CancellationToken cancellation;

    /// The name the task was submitted with. Never null; "job" when the submitter named none.
    const char* name = "job";

    Priority priority = Priority::Normal;
    Deadline deadline;

    /// This task's own handle, so that work it submits can name it as a dependency or a parent.
    JobHandle self;

    /// The arguments copied into the task record at submission, or null when the submitter passed
    /// none. Valid for the body's duration and no longer — the record is recycled afterwards.
    const void* data = nullptr;

    /// The one question long-running work asks at a natural boundary.
    [[nodiscard]] bool is_cancelled() const noexcept { return cancellation.is_cancelled(); }

    /// Temporary storage for this task. Null when the arena is exhausted; the caller falls back
    /// rather than being asserted at, and the refusal is counted.
    template <class T>
    [[nodiscard]] T* allocate(usize count) const noexcept {
        return scratch != nullptr ? scratch->allocate_array<T>(count) : nullptr;
    }
};

/// A task body. Takes the context and the pointer the submitter supplied.
///
/// A function pointer rather than a type-erased callable: a std::function would allocate, and
/// `core-jobs-and-concurrency` requires that scheduling a task does not call the general allocator.
/// State travels in `user`, which is the submitter's to keep alive until the task completes.
using JobBody = void (*)(const TaskContext& context, void* user) noexcept;

/// One partition of an indexed parallel loop. `[begin, end)` is a half-open range of the loop's
/// index space, and the partitioning is a function of the count and the grain alone — never of the
/// worker count — which is what makes a parallel loop reproducible.
using ParallelForBody = void (*)(const TaskContext& context, u64 begin, u64 end,
                                 void* user) noexcept;

}  // namespace cy::jobs
