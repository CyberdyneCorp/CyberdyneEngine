#pragma once
// Concurrency diagnostics, and the critical path. Task 3.2.11.
//
// `core-jobs-and-concurrency` is unusually specific about what must be reportable, and about the
// order in which it must exist: "task profiling and critical-path reporting SHALL exist **before**
// work-stealing behaviour is tuned, since throughput improvements off the critical path do not
// shorten frames". That is why this header lands with the scheduler rather than after it.
//
// THE CRITICAL PATH IS THE POINT. A frame's duration is its longest dependency chain, not the sum
// of its task durations. Every completed task records the longest chain that ends at it — its own
// duration plus the largest chain of any dependency — and a link back to the predecessor that
// supplied that maximum. The frame's critical path is then the chain ending at the largest of
// those, walked backwards. It costs one comparison per dependency edge and one append per
// completion, which is why it can be on by default rather than being a mode somebody remembers to
// turn on.
//
// The frame log the walk reads is a fixed-capacity array, reset by `JobSystem::begin_frame`. When a
// frame overflows it the report says so — `entries_dropped` — rather than reporting a shorter path
// as though it were the whole one.
//
// Job and task events also go onto the M0 shared trace as TaskBegin and TaskEnd records, so task
// behaviour correlates with memory, streaming and simulation events on one timeline. That is the
// specification's "one trace, many producers" requirement, and it is why this module depends on
// cy::core-diagnostics privately.

#include <cy/core/jobs/types.h>

namespace cy::jobs {

/// A snapshot of the job system's counters. Every field is cumulative since `start()` unless its
/// comment says otherwise, and reading it takes no lock: each is a relaxed atomic, so a snapshot is
/// internally consistent to within a task or two and costs nothing to take.
struct JobSystemStats {
    u32 worker_count = 0;

    // --- Throughput -----------------------------------------------------------------------------
    u64 tasks_submitted = 0;
    u64 tasks_executed = 0;
    /// Tasks whose token was cancelled before their body began. Their bodies never ran.
    u64 tasks_cancelled = 0;
    /// Per priority class, so that "background work still progresses" is a number rather than a
    /// belief. Indexed by `static_cast<u32>(Priority)`.
    u64 executed_by_priority[kPriorityCount] = {0, 0, 0, 0, 0};

    // --- Stealing -------------------------------------------------------------------------------
    u64 steal_attempts = 0;
    u64 steal_successes = 0;
    /// The sum of every deque's length at the moment the snapshot was taken.
    u64 queue_depth = 0;

    // --- Latency and occupancy ------------------------------------------------------------------
    /// Nanoseconds between a task becoming ready and a worker starting it, summed, with the sample
    /// count beside it so that a mean can be taken without the producer choosing a window.
    u64 queue_latency_ns = 0;
    u64 queue_latency_samples = 0;
    /// Nanoseconds workers spent inside task bodies, and nanoseconds they spent looking for work.
    /// Utilisation is busy / (busy + idle); "idle workers while the frame is long" is the two of
    /// them beside the critical path.
    u64 worker_busy_ns = 0;
    u64 worker_idle_ns = 0;

    // --- Defects the watchdog found -------------------------------------------------------------
    /// Workers found inside a declared blocking region for longer than the configured threshold.
    u64 blocked_worker_detections = 0;
    /// Tasks that ran longer than the configured limit. Task 3.2.8.
    u64 long_task_detections = 0;
    /// Tasks that kept running past the grace period after their token was cancelled. Task 3.2.6.
    u64 unresponsive_cancellations = 0;
    /// Attempts to block on I/O or the GPU from a worker thread. Task 3.2.5. Always zero in a
    /// correct build; a non-zero value is a defect, not a slow path.
    u64 blocking_violations = 0;

    // --- Allocation -----------------------------------------------------------------------------
    /// General-heap allocations the scheduler made after `start()`. Task records come from
    /// per-worker slabs, so this is zero for any workload the slabs and deques were sized for; a
    /// non-zero value names exactly how often the system fell back.
    u64 scheduling_allocations = 0;
    /// Submissions refused because a participant's task slab was full.
    u64 slab_exhaustions = 0;
    /// The largest number of task records in use at once, so a slab size can be chosen from a
    /// measurement.
    u64 peak_tasks_in_flight = 0;
};

/// One task on the critical path, oldest first in `CriticalPath::entries`.
struct CriticalPathEntry {
    const char* name = "";
    WorkerIndex worker = kNotAWorker;
    Priority priority = Priority::Normal;
    /// How long the body ran.
    u64 duration_ns = 0;
    /// The longest chain ending at this task, this task included.
    u64 path_ns = 0;
};

inline constexpr u32 kMaxCriticalPathEntries = 64;

/// The frame's longest dependency chain.
struct CriticalPath {
    u64 frame_index = 0;
    /// The chain, oldest first. Truncated to the newest kMaxCriticalPathEntries when longer, and
    /// `truncated` says so.
    CriticalPathEntry entries[kMaxCriticalPathEntries] = {};
    u32 length = 0;
    bool truncated = false;
    /// The chain's total duration. This is the frame's lower bound: no amount of parallelism makes
    /// the frame shorter than its longest chain.
    u64 total_ns = 0;
    /// Completed tasks the frame log could not hold. A non-zero value means the path below may not
    /// be the real one — raise `frame_log_entries`.
    u64 entries_dropped = 0;
    /// Every completed task the frame recorded, path or not. The denominator for "the sum of the
    /// task durations far exceeds the critical path, so the frame is parallel enough".
    u64 tasks_recorded = 0;
    u64 total_task_ns = 0;
};

/// Emit the current statistics onto the M0 shared trace as counters plus one instant carrying the
/// snapshot. A no-op when no trace is open, so a caller does not have to ask first.
void jobs_trace_report(const JobSystemStats& stats) noexcept;

/// Emit the critical path onto the trace: one instant per entry, in chain order, plus a summary.
void jobs_trace_critical_path(const CriticalPath& path) noexcept;

/// Log the statistics at Info. What a headless host prints at shutdown.
void jobs_log_report(const JobSystemStats& stats) noexcept;

/// Log a watchdog finding at Warning, naming the task. The three findings are spelled the same way
/// so that a reader greps one string: "jobs.watchdog".
void jobs_log_watchdog(const char* what, const char* task_name, u64 duration_ns,
                       WorkerIndex worker) noexcept;

}  // namespace cy::jobs
