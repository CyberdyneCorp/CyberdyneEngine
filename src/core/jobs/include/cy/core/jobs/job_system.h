#pragma once
// The one job system. Tasks 3.2.1, 3.2.5, 3.2.7, 3.2.8, 3.2.10.
//
// `core-jobs-and-concurrency`: the engine creates exactly one JobSystem at startup which owns all
// general-purpose worker threads, and subsystems do not spawn their own except for the documented
// dedicated threads. That is enforced here rather than documented: `start()` fails if a system is
// already running in this process, and names the one that is.
//
// WHAT IS DECIDED HERE, AND WHY
//
// * Per-worker deques with work stealing. The owner pushes and pops at one end (newest first: the
//   task it just created is the one whose data is still in cache) and a thief takes from the other
//   (oldest first: the oldest task is the one most likely to have a large subtree under it).
//
// * A waiting worker does useful work. `wait()` does not block on a worker thread; it runs other
//   ready tasks until the one it is waiting for is done. That is what makes recursive submission —
//   a task that submits and waits — impossible to deadlock, and it is why the main thread can
//   participate too: with zero workers the whole system still runs, on the caller.
//
// * Fairness is a quantum, not a hope. A worker takes from the highest non-empty class, except
//   every `fairness_quantum` pops, when it takes from the lowest. Background and Idle therefore
//   receive bounded minimum progress under sustained Critical load, which is the specification's
//   requirement in the form of a number a test can assert on.
//
// * Determinism is a mode. In `Deterministic` the ready set is ordered by submission sequence, not
//   by priority, deadline, or which worker got there first; deadline hints are ignored;
//   `DeterministicSingleThreaded` runs the same order with no workers at all, so a discrepancy
//   between the two localises a scheduling-dependent defect. `Chaos` deliberately randomises the
//   permitted order from a recorded seed, so that a system relying on order fails in testing.
//
// * A worker never blocks on I/O or the GPU. `begin_blocking_region` refuses on any thread that is
//   executing a job — it returns an error and counts the violation, both in every configuration.
//   Blocking belongs on the dedicated I/O thread (async.h), which is exactly why that thread is in
//   the thread-role table.

#include <cy/core/jobs/cancellation.h>
#include <cy/core/jobs/context.h>
#include <cy/core/jobs/diagnostics.h>
#include <cy/core/jobs/thread_role.h>
#include <cy/core/jobs/types.h>

namespace cy::jobs {

namespace detail {
struct JobSystemImpl;
}

/// How the scheduler orders the ready set.
enum class SchedulingMode : u8 {
    /// Priority classes, fairness quantum, deadline hints, work stealing. The shipping mode.
    Default = 0,
    /// A fixed order — submission sequence — across however many workers are running. Deadline
    /// hints are ignored. Parallel, and reproducible.
    Deterministic = 1,
    /// The same fixed order with no worker threads: everything runs on the thread that waits.
    /// `simulation-and-determinism`'s debugging mode — a result that differs from Deterministic
    /// localises a scheduling-dependent defect.
    DeterministicSingleThreaded = 2,
    /// Permitted order randomised from `chaos_seed`, so that an undeclared ordering dependency
    /// surfaces in testing rather than in production.
    Chaos = 3,
};

const char* scheduling_mode_name(SchedulingMode mode) noexcept;

/// What happened to a task, as its awaiters see it.
enum class TaskOutcome : u8 {
    /// The handle names a slot whose generation has moved on. The task it named completed before
    /// the slot was reused — a slot is never reused before that — so a stale handle is complete.
    Stale = 0,
    Pending = 1,
    Completed = 2,
    /// The token was cancelled before the body began, so the body never ran. This is what a
    /// cancelled result means to an awaiter: the work did not happen, and the awaiter is released
    /// rather than left waiting for a task that will never run.
    Cancelled = 3,
};

const char* task_outcome_name(TaskOutcome outcome) noexcept;

/// The most partitions an indexed parallel loop is split into.
///
/// A cap is needed because a grain of one over a million elements would otherwise be a million
/// tasks, each costing more to schedule than to run. It is a constant rather than a function of the
/// worker count on purpose: the partitioning must be a function of the count and the grain alone,
/// or a loop's reduction order would change with the machine it ran on.
inline constexpr u64 kMaxParallelPartitions = 1024;

struct JobSystemConfig {
    /// Zero means `hardware_concurrency() - 1`, reserving the main thread — the specification's
    /// default. `DeterministicSingleThreaded` forces it to zero whatever is asked for.
    u32 worker_count = 0;

    SchedulingMode mode = SchedulingMode::Default;
    /// The seed Chaos randomises from. Recorded in the statistics and in the trace, because a chaos
    /// run that cannot be reproduced has found a bug nobody can fix.
    u64 chaos_seed = 0x9E37'79B9'7F4A'7C15ull;

    /// Task records per participant, taken from that participant's slab. The hard bound on how many
    /// tasks one thread can have in flight.
    u32 task_slots_per_participant = 4096;
    /// Ready-queue capacity per participant per priority class. Overflow spills to a shared queue,
    /// which is the only thing here that can touch the general heap after `start()`; it is counted.
    u32 deque_capacity = 4096;
    /// Scratch bytes per participant. One allocation each, made by `start()`.
    usize scratch_bytes_per_participant = 1u << 20;
    /// Completed-task entries the critical-path log holds per frame.
    u32 frame_log_entries = 16384;

    /// Pops between fairness sweeps. Every Nth pop takes from the lowest non-empty class instead of
    /// the highest, which is what bounds the starvation of Background and Idle.
    u32 fairness_quantum = 16;

    /// A worker inside a declared blocking region for longer than this is reported.
    i64 blocked_worker_threshold_ns = 50'000'000;   // 50 ms
    /// A task running longer than this is reported. Task 3.2.8: long work is chunked or yields.
    i64 long_task_threshold_ns = 250'000'000;       // 250 ms
    /// A cancelled task still running this long after cancellation is reported.
    i64 cancellation_grace_ns = 100'000'000;        // 100 ms
    /// How often the watchdog looks. It also services timers, so this bounds a timer's resolution.
    i64 watchdog_interval_ns = 2'000'000;           // 2 ms

    /// Emit TaskBegin and TaskEnd onto the M0 shared trace. Off by default: a trace record per task
    /// is the right default for a profiling session and the wrong one for a benchmark.
    bool emit_trace = false;
};

/// One job to submit.
struct JobDesc {
    JobBody body = nullptr;
    void* user = nullptr;
    /// A string literal, or storage that outlives the task. Named jobs are what a profile is read
    /// with; an unnamed job is a bar with no label.
    const char* name = "job";
    Priority priority = Priority::Normal;
    /// A scheduling preference, never a correctness contract. Ignored in every deterministic mode.
    Deadline deadline;
    /// The token the body observes. Default-constructed means the task cannot be cancelled.
    CancellationToken cancellation;
    /// Arguments copied into the task record, reaching the body as `TaskContext::data`. At most
    /// kMaxInlineArgumentBytes; a larger payload belongs behind `user`. The type must be trivially
    /// copyable — the record is memcpy'd and never destructs what it holds.
    const void* inline_data = nullptr;
    u32 inline_size = 0;

    /// Jobs that must complete first. A null or stale handle is already complete and is skipped,
    /// so a caller does not have to filter its own list.
    const JobHandle* dependencies = nullptr;
    u32 dependency_count = 0;

    /// Hold the job until something outside the graph releases it with `JobSystem::signal`.
    ///
    /// This is the seam every asynchronous completion goes through: an I/O completion, a GPU fence,
    /// a timer, a coroutine finishing. Without it each of those would need its own way to make a
    /// job runnable, and a worker would end up waiting on one of them. With it there is exactly one
    /// mechanism — a dependency the graph cannot satisfy on its own — and the thread that completes
    /// the operation releases it from wherever it happens to be.
    bool gated = false;
};

/// The engine's job system. One per process.
class JobSystem {
public:
    JobSystem() noexcept;
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Start the workers. Fails with ErrorCode::AlreadyExists when a system is already running:
    /// `core-jobs-and-concurrency` says exactly one owns all worker threads, and two would each own
    /// half of them.
    Status start(const JobSystemConfig& config) noexcept;

    /// Drain what has been submitted, stop the workers, and release everything. Idempotent.
    void shutdown() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] u32 worker_count() const noexcept;
    [[nodiscard]] SchedulingMode mode() const noexcept;
    [[nodiscard]] const JobSystemConfig& config() const noexcept;

    // --- Submission -------------------------------------------------------------------------

    Expected<JobHandle, cy::Error> submit(const JobDesc& desc) noexcept;

    /// The short spelling: a body, its state, and a name.
    Expected<JobHandle, cy::Error> submit(JobBody body, void* user, const char* name,
                                          Priority priority = Priority::Normal) noexcept;

    /// An indexed parallel loop. Work is partitioned into ranges of at least `grain`, so a small
    /// loop does not pay more in scheduling than it saves, and the partitioning is a function of
    /// `count` and `grain` alone — never of the worker count — which is what makes the loop
    /// reproducible. Returns a handle that completes when every partition has.
    ///
    /// A count of zero returns a null handle, which reads as complete.
    Expected<JobHandle, cy::Error> submit_parallel_for(u64 count, u64 grain, ParallelForBody body,
                                                       void* user, const char* name,
                                                       Priority priority = Priority::Normal,
                                                       const JobHandle* dependencies = nullptr,
                                                       u32 dependency_count = 0) noexcept;

    /// How many partitions `submit_parallel_for` will make for this count and grain. Exposed
    /// because a deterministic reduction has to allocate exactly one accumulator per partition, and
    /// it must be able to ask rather than reimplement the rule.
    [[nodiscard]] static u64 partition_count(u64 count, u64 grain) noexcept;

    /// The half-open range partition `index` covers. Together with `partition_count` this is the
    /// whole of the fixed partitioning, and it is a pure function.
    static void partition_range(u64 count, u64 grain, u64 index, u64& begin, u64& end) noexcept;

    // --- Waiting ----------------------------------------------------------------------------

    /// Wait for a job. On a worker — or on any thread the system has a participant slot for — this
    /// runs other ready tasks rather than blocking, so a task that waits cannot deadlock the pool.
    /// Release a gated job. Safe from any thread, including one the job system does not own — an
    /// I/O thread, a driver's completion callback, a timer. Idempotent: a second signal is ignored.
    ///
    /// Fails with NotFound when the handle is stale, and with InvalidArgument when the job was not
    /// submitted gated.
    Status signal(JobHandle handle) noexcept;

    void wait(JobHandle handle) noexcept;
    void wait_all(const JobHandle* handles, u32 count) noexcept;

    /// Run ready tasks until every submitted task has completed. What a frame's flush point does,
    /// and what `shutdown()` does first.
    void wait_for_idle() noexcept;

    [[nodiscard]] bool is_complete(JobHandle handle) const noexcept;
    [[nodiscard]] TaskOutcome outcome(JobHandle handle) const noexcept;

    // --- Frames, statistics and the critical path --------------------------------------------

    /// Open a frame: resets the critical-path log and stamps the frame index onto everything
    /// recorded until the next call. Optional — a headless tool that never calls it gets one long
    /// frame — and cheap.
    void begin_frame(u64 frame_index) noexcept;

    [[nodiscard]] JobSystemStats stats() const noexcept;

    /// The longest dependency chain recorded since the last `begin_frame`.
    [[nodiscard]] CriticalPath critical_path() const noexcept;

    /// Reset every counter. For a benchmark that measures one phase, and for a test that asserts on
    /// a count rather than on a delta.
    void reset_stats() noexcept;

    // --- The blocking rule --------------------------------------------------------------------

    /// Declare that the calling thread is about to block.
    ///
    /// On a thread that is *executing a job* this REFUSES: it returns ErrorCode::Unsupported and
    /// counts the violation, both in every configuration. It does not additionally assert — the
    /// refusal is the report, and it must be exercisable in the development build a suite runs in.
    ///
    /// "Executing a job", not "holding the Worker role", is the exact rule. A waiting thread runs
    /// ready tasks instead of blocking, so the main thread inside `wait()` is running a task, and
    /// blocking it stalls the graph exactly as blocking a worker would. Which of the two picked the
    /// task up is a property of timing, and a rule that depended on it would let the same defect
    /// through half the time.
    ///
    /// On a thread that is not running a task — the main thread between frames, the simulation,
    /// audio or asset-I/O thread — it succeeds and starts the watchdog's clock, so that a region
    /// held past `blocked_worker_threshold_ns` is reported by name rather than being invisible.
    Status begin_blocking_region(const char* what) noexcept;
    void end_blocking_region() noexcept;

    /// The running system, or null. `core-jobs-and-concurrency` says there is one; this is how a
    /// subsystem that was not handed a reference finds it, and it is deliberately the only global.
    static JobSystem* current() noexcept;

private:
    friend struct detail::JobSystemImpl;
    detail::JobSystemImpl* impl_ = nullptr;
};

/// RAII over `begin_blocking_region` / `end_blocking_region`.
///
///     cy::jobs::BlockingRegion region(system, "read the package index");
///     if (!region.permitted()) { return cy::fail(...); }
///
/// `permitted()` is not optional to check: on a worker the region was refused, and doing the
/// blocking call anyway is the defect the refusal was reporting.
class BlockingRegion {
public:
    BlockingRegion(JobSystem& system, const char* what) noexcept
        : system_(&system), permitted_(system.begin_blocking_region(what).has_value()) {}

    ~BlockingRegion() {
        if (permitted_) {
            system_->end_blocking_region();
        }
    }

    BlockingRegion(const BlockingRegion&) = delete;
    BlockingRegion& operator=(const BlockingRegion&) = delete;

    [[nodiscard]] bool permitted() const noexcept { return permitted_; }

private:
    JobSystem* system_;
    bool permitted_;
};

/// How many times a worker thread has attempted to block. Compiled into every configuration, so
/// that a suite asserting on it is not silently vacuous in Profile and Shipping.
u64 blocking_violations() noexcept;
/// What the most recent blocking violation was attempting, or "" when there has been none.
const char* last_blocking_violation() noexcept;
void reset_blocking_violations() noexcept;

}  // namespace cy::jobs
