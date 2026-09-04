#pragma once
// The job system's private machinery: task records, per-participant deques and slabs, the frame
// log the critical path is read from, and the watchdog's observation points.
//
// Private to src/core/jobs/. Nothing outside the module includes it, and nothing in it appears in a
// public signature — job_system.h names `detail::JobSystemImpl` and never defines it, so a change
// in here is not a rebuild of everything that submits a job.
//
// THE ONE HARD PROBLEM IN THIS FILE is recycling a task record while another thread may be about to
// register a dependency on it. A slot is reused as soon as its task completes, so a submitter that
// read a handle a moment ago can be part-way through linking itself into a list that now belongs to
// a different task. The reference count on TaskRecord is what makes that safe: a submitter acquires
// the record it is about to link into, the acquire fails when the generation has moved on, and the
// slot returns to its slab only when the last reference goes. Without it the system would either
// leak slots until quiescence — which a frame that submits continuously never reaches — or link a
// dependency onto the wrong task, which is a hang with no evidence in it.

#include <cy/core/jobs/context.h>
#include <cy/core/jobs/diagnostics.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/scratch.h>
#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/types.h>

#include <atomic>

namespace cy::jobs::detail {

/// The end of a successor list.
inline constexpr u32 kNoNode = 0xFFFF'FFFFu;
/// The head value a completed task's successor list carries. A thread that sees it knows the task
/// finished and satisfies its own dependency immediately instead of linking.
inline constexpr u32 kListClosed = 0xFFFF'FFFEu;

/// Participant slots kept for threads the system did not start — the main thread, a dedicated
/// thread, a test's own thread. A thread beyond them still waits correctly; it just cannot help.
inline constexpr u32 kHelperSlots = 8;

/// One task. Allocated from the submitting participant's slab, returned to it when the last
/// reference goes.
struct TaskRecord {
    JobBody body = nullptr;
    void* user = nullptr;
    const char* name = "job";
    CancellationToken cancellation;
    Deadline deadline;
    Priority priority = Priority::Normal;

    /// Arguments copied by value at submission. See kMaxInlineArgumentBytes: this is what lets
    /// `submit_parallel_for` describe a partition without allocating one object per partition.
    alignas(16) u8 inline_data[kMaxInlineArgumentBytes] = {};
    u32 inline_size = 0;

    /// Bumped every time the slot is allocated. A handle whose generation does not match names a
    /// task that has already completed — a slot is never reused before that.
    std::atomic<u32> generation{0};
    /// While non-zero the slot is in use. See the file comment.
    std::atomic<u32> ref_count{0};
    std::atomic<u8> outcome{static_cast<u8>(TaskOutcome::Stale)};
    /// 0 = not gated, 1 = gated and waiting, 2 = released. The exchange from 1 to 2 is what makes
    /// `signal()` idempotent under a race between two completions.
    std::atomic<u8> gate{0};

    /// Unsatisfied dependencies, plus one for the submitter's own guard. The guard is released once
    /// every dependency has been registered, so a task whose dependencies all completed during
    /// registration is enqueued once rather than once per dependency.
    std::atomic<u32> dependencies_remaining{0};
    /// Head of the successor list, or kNoNode, or kListClosed.
    std::atomic<u32> successors_head{kNoNode};

    /// The deterministic order key: the value of a single global counter at submission. Never a
    /// worker index — work stealing makes that a function of timing.
    u64 sequence = 0;

    /// The longest chain ending at any completed dependency, and the frame-log entry of the
    /// dependency that supplied it. Written under `path_lock` because the two must agree.
    u64 inherited_path_ns = 0;
    u32 predecessor_entry = kNoNode;
    SpinLock path_lock;

    /// Written once at completion and published by the release exchange on `successors_head`, so a
    /// submitter that arrives after the task finished reads the same chain a successor linked in
    /// time would have inherited. Without them a late-registered dependency would silently
    /// contribute nothing to the critical path.
    u64 completed_path_ns = 0;
    u32 frame_entry = kNoNode;

    /// Which participant's slab owns the slot, so it is returned to the right freelist.
    u32 owner = 0;
    /// The freelist's next pointer while the slot is free.
    u32 free_next = kNoNode;

    i64 ready_ns = 0;
    i64 started_ns = 0;
};

/// A link in a task's successor list. Nodes come from one pool and are recycled by the completion
/// that walks them.
struct SuccessorNode {
    u32 task = kNoNode;
    u32 next = kNoNode;
};

/// A ready queue: the owner pushes and pops at the tail, a thief takes from the head.
///
/// Guarded by a spin lock rather than written as a Chase-Lev deque. The critical section is four
/// stores, the owner is almost always uncontended, and a lock-free deque here would be the hardest
/// thing in the module to prove correct for a throughput difference the critical path does not
/// notice. The choice is recorded rather than assumed: if a profile ever shows this lock, the
/// replacement is a Chase-Lev deque behind this same interface.
struct Deque {
    SpinLock lock;
    u32* slots = nullptr;
    u32 capacity = 0;
    u32 head = 0;
    u32 tail = 0;
    /// Read without the lock by the depth statistic and by a thief deciding where to try.
    std::atomic<u32> count{0};

    bool push_back(u32 task) noexcept {
        ScopedLock<SpinLock> held(lock);
        const u32 size = count.load(std::memory_order_relaxed);
        if (size == capacity) {
            return false;
        }
        slots[tail] = task;
        tail = tail + 1 == capacity ? 0 : tail + 1;
        count.store(size + 1, std::memory_order_relaxed);
        return true;
    }

    bool pop_back(u32& task) noexcept {
        ScopedLock<SpinLock> held(lock);
        const u32 size = count.load(std::memory_order_relaxed);
        if (size == 0) {
            return false;
        }
        tail = tail == 0 ? capacity - 1 : tail - 1;
        task = slots[tail];
        count.store(size - 1, std::memory_order_relaxed);
        return true;
    }

    bool pop_front(u32& task) noexcept {
        ScopedLock<SpinLock> held(lock);
        const u32 size = count.load(std::memory_order_relaxed);
        if (size == 0) {
            return false;
        }
        task = slots[head];
        head = head + 1 == capacity ? 0 : head + 1;
        count.store(size - 1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] u32 size() const noexcept { return count.load(std::memory_order_relaxed); }
};

/// One thread's state: its queues, its slab, its scratch, and the two atomics the watchdog reads.
struct Participant {
    Deque queues[kPriorityCount];
    ScratchArena scratch;

    /// The slab: a half-open range of the shared record array, with its own freelist.
    u32 slab_begin = 0;
    u32 slab_end = 0;
    SpinLock free_lock;
    u32 free_head = kNoNode;
    std::atomic<u32> free_count{0};

    // --- What the watchdog reads --------------------------------------------------------------
    //
    // The running task's cancellation token, copied here by the thread executing it and read by the
    // watchdog under the same lock. The watchdog cannot read the token out of the task record
    // instead: a record is recycled the moment its task completes, so the watchdog would be reading
    // a `CancellationToken` another thread is assigning — a data race, and one ThreadSanitizer
    // finds immediately. The copy costs a reference count operation, and only for a task that
    // actually carries a token.
    SpinLock cancellation_lock;
    CancellationToken current_cancellation;

    std::atomic<const char*> current_task{nullptr};
    std::atomic<i64> current_task_started_ns{0};
    std::atomic<i64> current_task_reported_ns{0};
    std::atomic<const char*> blocking_what{nullptr};
    std::atomic<i64> blocking_since_ns{0};
    std::atomic<i64> blocking_reported_ns{0};
    std::atomic<i64> cancel_reported_ns{0};

    // --- Statistics ---------------------------------------------------------------------------
    std::atomic<u64> busy_ns{0};
    std::atomic<u64> idle_ns{0};
    std::atomic<u64> executed{0};
    std::atomic<u64> steal_attempts{0};
    std::atomic<u64> steal_successes{0};

    /// Pops since the last fairness sweep, and the chaos generator's state. Both are touched only
    /// by the owning thread, so neither is atomic.
    u32 fairness_counter = 0;
    u64 rng_state = 0;

    /// True while a thread holds this slot. Helper slots are claimed and released; worker slots are
    /// held for the system's lifetime.
    std::atomic<bool> claimed{false};
};

/// One completed task, as the critical-path walk reads it.
struct FrameEntry {
    const char* name = "";
    u64 duration_ns = 0;
    u64 path_ns = 0;
    u32 predecessor = kNoNode;
    WorkerIndex worker = kNotAWorker;
    Priority priority = Priority::Normal;
};

/// The ready set in a deterministic mode: a min-heap on the submission sequence.
///
/// One shared, ordered structure rather than per-worker deques, because determinism is exactly the
/// property that "whichever worker got there first" destroys. It costs a lock per pop, which is the
/// price of a reproducible order and is paid only in the modes that ask for one.
struct OrderedReadyQueue {
    Mutex lock;
    u32* heap = nullptr;
    u64* keys = nullptr;
    u32 capacity = 0;
    u32 size = 0;

    bool push(u32 task, u64 sequence) noexcept;
    bool pop(u32& task) noexcept;
    [[nodiscard]] u32 depth() noexcept;
};

struct JobSystemImpl {
    JobSystemConfig config;
    u64 instance_id = 0;

    Participant* participants = nullptr;
    u32 participant_count = 0;  // workers + kHelperSlots
    u32 worker_count = 0;

    TaskRecord* records = nullptr;
    u32 record_count = 0;

    SuccessorNode* nodes = nullptr;
    u32 node_count = 0;
    SpinLock node_lock;
    u32 node_free_head = kNoNode;

    OrderedReadyQueue ordered;

    FrameEntry* frame_log = nullptr;
    std::atomic<u32> frame_log_used{0};
    std::atomic<u64> frame_log_dropped{0};
    std::atomic<u64> frame_index{0};

    std::atomic<u64> sequence_counter{1};
    std::atomic<u64> tasks_in_flight{0};
    /// Tasks sitting in a ready queue. Read by an idling worker's wait predicate, so that waking is
    /// one relaxed load rather than a scan of every deque.
    std::atomic<u64> ready_count{0};
    std::atomic<u64> peak_in_flight{0};

    // --- Counters -------------------------------------------------------------------------------
    std::atomic<u64> tasks_submitted{0};
    std::atomic<u64> tasks_executed{0};
    std::atomic<u64> tasks_cancelled{0};
    std::atomic<u64> executed_by_priority[kPriorityCount] = {};
    std::atomic<u64> queue_latency_ns{0};
    std::atomic<u64> queue_latency_samples{0};
    std::atomic<u64> blocked_worker_detections{0};
    std::atomic<u64> long_task_detections{0};
    std::atomic<u64> unresponsive_cancellations{0};
    std::atomic<u64> scheduling_allocations{0};
    std::atomic<u64> slab_exhaustions{0};

    // --- Threads ---------------------------------------------------------------------------------
    Thread* workers = nullptr;
    Thread watchdog;
    std::atomic<bool> stopping{false};

    Mutex idle_lock;
    ConditionVariable idle_cv;
    std::atomic<u32> waiters{0};

    /// The helper slots a non-worker thread claims when it first participates.
    SpinLock helper_lock;

    // --- Lifecycle ---------------------------------------------------------------------------
    Status initialize(const JobSystemConfig& requested) noexcept;
    void teardown() noexcept;

    // --- Records ------------------------------------------------------------------------------
    //
    // `const` on several members below is narrower than it reads. The state a job system owns
    // — the record array, the participant array, the deques — lives behind raw pointers, so a
    // member that mutates it changes nothing in the object's own bytes and is const in the
    // language's sense. It is not a claim that the call has no effect; `allocate_record` takes a
    // slot off a free list and `release` returns one to it. Read the qualifier as "does not
    // repoint the system", never as "does not change anything".
    u32 allocate_record(u32 participant) const noexcept;
    static void retain(TaskRecord& record) noexcept;
    void release(u32 index) const noexcept;
    /// Take a reference on the record `handle` names, or fail because it has completed.
    bool acquire(JobHandle handle, u32& index) const noexcept;

    u32 allocate_node() noexcept;
    void free_node(u32 index) noexcept;

    // --- Scheduling ---------------------------------------------------------------------------
    void enqueue(u32 task, u32 participant) noexcept;
    /// Propagate a completed dependency's chain into its successor. Takes the successor's path
    /// lock, because the length and the predecessor that supplied it must agree.
    void propagate_path(u32 successor, u64 path_ns, u32 entry) const noexcept;
    bool acquire_task(u32 participant, u32& task) noexcept;
    void execute(u32 participant, u32 task) noexcept;
    void complete(u32 participant, u32 task, bool ran) noexcept;
    /// Run one ready task if there is one. The unit both `wait()` and a worker loop are built from.
    bool run_one(u32 participant) noexcept;

    u32 claim_participant() const noexcept;
    void release_participant(u32 participant) const noexcept;

    void wake_one() noexcept;
    void wake_all() noexcept;

    void worker_main(u32 participant) noexcept;
    void watchdog_main() noexcept;

    u32 append_frame_entry(const FrameEntry& entry) noexcept;

    [[nodiscard]] bool deterministic() const noexcept {
        return config.mode == SchedulingMode::Deterministic ||
               config.mode == SchedulingMode::DeterministicSingleThreaded;
    }
};

/// The running system's implementation, or null. Set by start(), cleared by shutdown() before
/// anything is freed, so a thread-local cleanup can tell "still running" from "gone".
JobSystemImpl* current_impl() noexcept;

/// The participant slot this thread holds in `impl`, claiming one if it does not hold it yet.
/// kNotAWorker when every helper slot is taken — such a thread waits without helping.
u32 this_thread_participant(JobSystemImpl& impl) noexcept;

/// The participant slot this thread already holds, without claiming one.
u32 this_thread_participant_if_any(const JobSystemImpl& impl) noexcept;

/// Bind this thread to a participant slot for the system's lifetime. Called by a worker on start.
void bind_worker_participant(JobSystemImpl& impl, u32 participant) noexcept;

// --- The seam onto the M0 shared trace ----------------------------------------------------------
//
// Declared here and defined in diagnostics.cpp so that job_system.cpp includes no diagnostics
// header. That keeps the dependency on cy::core-diagnostics in one translation unit, which is what
// makes it a private dependency in the honest sense rather than only in the CMake sense.

void trace_task_begin(const char* name, WorkerIndex worker, Priority priority,
                      u64 sequence) noexcept;
void trace_task_end(const char* name, WorkerIndex worker, Priority priority,
                    u64 duration_ns) noexcept;

}  // namespace cy::jobs::detail
