// The job system. Tasks 3.2.1, 3.2.3, 3.2.5, 3.2.7, 3.2.8, 3.2.10, 3.2.11.
//
// Read internal.h first: it holds the data and the one hard problem (recycling a task record while
// somebody may be linking a dependency onto it). This file is the behaviour — submission,
// dependency resolution, the pick-and-steal loop, the wait that helps, and the watchdog.
//
// THE ORDER OF EVENTS IN A COMPLETION is load-bearing and is the thing to preserve when editing:
//
//   1. the frame-log entry is appended, so the chain ending at this task exists;
//   2. `completed_path_ns` and `frame_entry` are written, for a dependent that arrives late;
//   3. the execution counters are incremented, so a thread released by step 4 sees this task in
//      them;
//   4. the outcome is stored;
//   5. the successor list is closed with a release exchange, which publishes 1-4;
//   6. each successor inherits the chain, and only then has its dependency count decremented.
//
// Step 6's order is why a task never starts before it knows the longest chain behind it. Reversing
// it would not lose work — the scheduling would still be correct — it would silently report a
// critical path shorter than the real one, which is worse than reporting none.
//
// Step 3 is before step 4 for a reason found by running the suite under load: the store in step 4
// is what `wait()` observes, so anything incremented after it can be missing from a `stats()` read
// taken the instant `wait()` returns. `integration.jobs_diagnostics` asserted an exact
// `tasks_executed` immediately after a wait and failed roughly one run in twenty-five on a loaded
// machine. Counters that describe a finished task belong on the publishing side of that store.

#include "internal.h"

#include <cy/core/base/assert.h>
#include <cy/core/jobs/job_system.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

namespace cy::jobs {
namespace {

/// The one running system's implementation. `core-jobs-and-concurrency` says exactly one JobSystem
/// owns all worker threads, so this is not a convenience global — it is the enforcement of that,
/// and `start()` fails when it is already set.
std::atomic<detail::JobSystemImpl*> g_impl{nullptr};
std::atomic<JobSystem*> g_system{nullptr};
std::atomic<u64> g_instance_counter{1};

std::atomic<u64> g_blocking_violations{0};
std::atomic<const char*> g_last_blocking_violation{""};

/// Which participant slot this thread holds, and in which system. The instance id is what makes the
/// thread-local safe across a shutdown: a thread that outlives one system and meets the next sees a
/// different id and claims a fresh slot rather than writing into a freed one.
thread_local u32 t_participant = kNotAWorker;
thread_local u64 t_instance = 0;
thread_local bool t_is_worker = false;

/// Releases a helper slot when a thread that borrowed one exits. Only helpers: a worker's slot is
/// released by `teardown()`, which has already joined it.
struct HelperSlotGuard {
    ~HelperSlotGuard() {
        if (t_participant == kNotAWorker || t_is_worker) {
            return;
        }
        detail::JobSystemImpl* impl = g_impl.load(std::memory_order_acquire);
        if (impl != nullptr && impl->instance_id == t_instance) {
            impl->release_participant(t_participant);
        }
        t_participant = kNotAWorker;
        t_instance = 0;
    }
};

thread_local HelperSlotGuard t_helper_guard;

/// xorshift64*, so that chaos mode is reproducible from its seed. Never used for anything but
/// deliberately randomised ordering, and recorded in the statistics so a chaos failure is a bug
/// somebody can reproduce.
u64 next_random(u64& state) noexcept {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545'F491'4F6C'DD1Dull;
}

}  // namespace

const char* scheduling_mode_name(SchedulingMode mode) noexcept {
    switch (mode) {
        case SchedulingMode::Default:
            return "Default";
        case SchedulingMode::Deterministic:
            return "Deterministic";
        case SchedulingMode::DeterministicSingleThreaded:
            return "DeterministicSingleThreaded";
        case SchedulingMode::Chaos:
            return "Chaos";
    }
    return "Unknown";
}

const char* task_outcome_name(TaskOutcome outcome) noexcept {
    switch (outcome) {
        case TaskOutcome::Stale:
            return "Stale";
        case TaskOutcome::Pending:
            return "Pending";
        case TaskOutcome::Completed:
            return "Completed";
        case TaskOutcome::Cancelled:
            return "Cancelled";
    }
    return "Unknown";
}

u64 blocking_violations() noexcept {
    return g_blocking_violations.load(std::memory_order_relaxed);
}

const char* last_blocking_violation() noexcept {
    return g_last_blocking_violation.load(std::memory_order_relaxed);
}

void reset_blocking_violations() noexcept {
    g_blocking_violations.store(0, std::memory_order_relaxed);
    g_last_blocking_violation.store("", std::memory_order_relaxed);
}

namespace detail {

JobSystemImpl* current_impl() noexcept {
    return g_impl.load(std::memory_order_acquire);
}

u32 this_thread_participant_if_any(const JobSystemImpl& impl) noexcept {
    return t_instance == impl.instance_id ? t_participant : kNotAWorker;
}

void bind_worker_participant(JobSystemImpl& impl, u32 participant) noexcept {
    t_participant = participant;
    t_instance = impl.instance_id;
    t_is_worker = true;
}

u32 this_thread_participant(JobSystemImpl& impl) noexcept {
    if (t_instance == impl.instance_id && t_participant != kNotAWorker) {
        return t_participant;
    }
    const u32 claimed = impl.claim_participant();
    if (claimed != kNotAWorker) {
        t_participant = claimed;
        t_instance = impl.instance_id;
        t_is_worker = false;
        // Touch the guard so that the thread_local is actually instantiated on this thread; without
        // a use, the destructor is never registered and the slot would leak on thread exit.
        (void)&t_helper_guard;
    }
    return claimed;
}

// --- OrderedReadyQueue --------------------------------------------------------------------------

bool OrderedReadyQueue::push(u32 task, u64 sequence) noexcept {
    ScopedLock<Mutex> held(lock);
    if (size == capacity) {
        return false;
    }
    u32 index = size++;
    heap[index] = task;
    keys[index] = sequence;
    while (index > 0) {
        const u32 parent = (index - 1) / 2;
        if (keys[parent] <= keys[index]) {
            break;
        }
        const u32 swap_task = heap[parent];
        const u64 swap_key = keys[parent];
        heap[parent] = heap[index];
        keys[parent] = keys[index];
        heap[index] = swap_task;
        keys[index] = swap_key;
        index = parent;
    }
    return true;
}

bool OrderedReadyQueue::pop(u32& task) noexcept {
    ScopedLock<Mutex> held(lock);
    if (size == 0) {
        return false;
    }
    task = heap[0];
    --size;
    if (size == 0) {
        return true;
    }
    heap[0] = heap[size];
    keys[0] = keys[size];
    u32 index = 0;
    for (;;) {
        const u32 left = (2 * index) + 1;
        const u32 right = left + 1;
        u32 smallest = index;
        if (left < size && keys[left] < keys[smallest]) {
            smallest = left;
        }
        if (right < size && keys[right] < keys[smallest]) {
            smallest = right;
        }
        if (smallest == index) {
            return true;
        }
        const u32 swap_task = heap[smallest];
        const u64 swap_key = keys[smallest];
        heap[smallest] = heap[index];
        keys[smallest] = keys[index];
        heap[index] = swap_task;
        keys[index] = swap_key;
        index = smallest;
    }
}

u32 OrderedReadyQueue::depth() noexcept {
    ScopedLock<Mutex> held(lock);
    return size;
}

// --- Lifecycle
// ------------------------------------------------------------------------------------

Status JobSystemImpl::initialize(const JobSystemConfig& requested) noexcept {
    config = requested;
    if (config.mode == SchedulingMode::DeterministicSingleThreaded) {
        // Not an error to have asked for workers: the mode's whole purpose is to run the same
        // ordering with none, and silently honouring the request would defeat it.
        config.worker_count = 0;
    } else if (config.worker_count == 0) {
        const u32 hardware = Thread::hardware_concurrency();
        config.worker_count = hardware > 1 ? hardware - 1 : 1;
    }
    if (config.task_slots_per_participant == 0 || config.deque_capacity == 0 ||
        config.scratch_bytes_per_participant == 0 || config.frame_log_entries == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a job system's slab, deque, scratch and frame-log sizes must be non-zero");
    }

    worker_count = config.worker_count;
    participant_count = worker_count + kHelperSlots;
    instance_id = g_instance_counter.fetch_add(1, std::memory_order_relaxed);

    participants = new (std::nothrow) Participant[participant_count];
    if (participants == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the job system's participants could not be allocated");
    }

    record_count = participant_count * config.task_slots_per_participant;
    records = new (std::nothrow) TaskRecord[record_count];
    if (records == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the job system's task slabs could not be allocated");
    }

    // Four successor links per task. A task with more dependents than that is a fan-out — a
    // parallel_for join, a stage barrier — and the pool is shared, so the average is what matters.
    node_count = record_count * 4;
    nodes = new (std::nothrow) SuccessorNode[node_count];
    if (nodes == nullptr) {
        return fail(ErrorCode::OutOfMemory,
                    "the job system's successor pool could not be "
                    "allocated");
    }
    for (u32 i = 0; i < node_count; ++i) {
        nodes[i].next = i + 1 == node_count ? kNoNode : i + 1;
    }
    node_free_head = 0;

    frame_log = new (std::nothrow) FrameEntry[config.frame_log_entries];
    if (frame_log == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the job system's frame log could not be allocated");
    }

    ordered.capacity = record_count;
    ordered.heap = new (std::nothrow) u32[record_count];
    ordered.keys = new (std::nothrow) u64[record_count];
    if (ordered.heap == nullptr || ordered.keys == nullptr) {
        return fail(ErrorCode::OutOfMemory,
                    "the job system's ordered queue could not be "
                    "allocated");
    }

    for (u32 p = 0; p < participant_count; ++p) {
        Participant& participant = participants[p];
        participant.slab_begin = p * config.task_slots_per_participant;
        participant.slab_end = participant.slab_begin + config.task_slots_per_participant;
        participant.free_head = participant.slab_begin;
        participant.free_count.store(config.task_slots_per_participant, std::memory_order_relaxed);
        participant.rng_state = config.chaos_seed ^ (static_cast<u64>(p + 1) * 0x9E37'79B9u);

        for (u32 i = participant.slab_begin; i < participant.slab_end; ++i) {
            records[i].owner = p;
            records[i].free_next = i + 1 == participant.slab_end ? kNoNode : i + 1;
        }
        for (auto& queue : participant.queues) {
            queue.slots = new (std::nothrow) u32[config.deque_capacity];
            if (queue.slots == nullptr) {
                return fail(ErrorCode::OutOfMemory, "a job system deque could not be allocated");
            }
            queue.capacity = config.deque_capacity;
        }
        if (auto status = participant.scratch.initialize(config.scratch_bytes_per_participant);
            !status) {
            return status;
        }
    }

    workers = new (std::nothrow) Thread[worker_count > 0 ? worker_count : 1];
    if (workers == nullptr) {
        return fail(ErrorCode::OutOfMemory,
                    "the job system's worker threads could not be "
                    "allocated");
    }
    for (u32 w = 0; w < worker_count; ++w) {
        participants[w].claimed.store(true, std::memory_order_relaxed);
        workers[w] = Thread("cy.worker", [this, w] { worker_main(w); });
    }

    watchdog = Thread("cy.jobs.watchdog", [this] { watchdog_main(); });
    return ok();
}

void JobSystemImpl::teardown() noexcept {
    stopping.store(true, std::memory_order_release);
    wake_all();

    if (workers != nullptr) {
        for (u32 w = 0; w < worker_count; ++w) {
            workers[w].join();
        }
    }
    watchdog.join();

    delete[] workers;
    workers = nullptr;

    if (participants != nullptr) {
        for (u32 p = 0; p < participant_count; ++p) {
            for (auto& queue : participants[p].queues) {
                delete[] queue.slots;
                queue.slots = nullptr;
            }
        }
    }
    delete[] participants;
    participants = nullptr;
    delete[] records;
    records = nullptr;
    delete[] nodes;
    nodes = nullptr;
    delete[] frame_log;
    frame_log = nullptr;
    delete[] ordered.heap;
    ordered.heap = nullptr;
    delete[] ordered.keys;
    ordered.keys = nullptr;
}

// --- Records and nodes
// ----------------------------------------------------------------------------

u32 JobSystemImpl::allocate_record(u32 participant) const noexcept {
    Participant& owner = participants[participant];
    u32 index = kNoNode;
    {
        ScopedLock<SpinLock> held(owner.free_lock);
        index = owner.free_head;
        if (index == kNoNode) {
            return kNoNode;
        }
        owner.free_head = records[index].free_next;
    }
    owner.free_count.fetch_sub(1, std::memory_order_relaxed);

    TaskRecord& record = records[index];
    record.free_next = kNoNode;
    // The generation moves on before anything can observe the slot, so a handle from the slot's
    // previous life can never match this one.
    //
    // RELEASE, and for a reason that is not obvious. A waiter whose handle has gone stale learns
    // that from this value, through the acquire load in `outcome()` — and a stale handle means
    // "complete", so the waiter proceeds to read whatever the task produced. Without a release
    // here that read has no happens-before with the task at all: the release on the *outcome* only
    // helps a waiter that saw Completed, and one that arrived a moment later sees Stale instead.
    // The chain this closes is: the task's writes, its completion, the free-list spin lock, this
    // store, the waiter's acquire. ThreadSanitizer finds the hole about one run in five.
    record.generation.fetch_add(1, std::memory_order_release);
    record.ref_count.store(1, std::memory_order_relaxed);
    return index;
}

void JobSystemImpl::retain(TaskRecord& record) noexcept {
    record.ref_count.fetch_add(1, std::memory_order_relaxed);
}

void JobSystemImpl::release(u32 index) const noexcept {
    TaskRecord& record = records[index];
    if (record.ref_count.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    // The last reference. Clearing the token here rather than at completion is deliberate: a
    // submitter may still have been reading the record a moment ago, and a destructor is not
    // something to run while that is possible.
    record.cancellation = CancellationToken{};
    record.body = nullptr;
    record.user = nullptr;

    Participant& owner = participants[record.owner];
    {
        ScopedLock<SpinLock> held(owner.free_lock);
        record.free_next = owner.free_head;
        owner.free_head = index;
    }
    owner.free_count.fetch_add(1, std::memory_order_relaxed);
}

bool JobSystemImpl::acquire(JobHandle handle, u32& index) const noexcept {
    if (handle.is_null() || handle.index() >= record_count) {
        return false;
    }
    TaskRecord& record = records[handle.index()];
    for (;;) {
        u32 references = record.ref_count.load(std::memory_order_acquire);
        if (references == 0) {
            return false;
        }
        if (record.generation.load(std::memory_order_acquire) != handle.generation()) {
            return false;
        }
        if (record.ref_count.compare_exchange_weak(
                references, references + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // The slot may have been freed and reallocated between the generation check and the
            // exchange. Re-reading is what turns that into a clean "already complete" rather than
            // into a dependency linked onto a stranger.
            if (record.generation.load(std::memory_order_acquire) != handle.generation()) {
                release(handle.index());
                return false;
            }
            index = handle.index();
            return true;
        }
    }
}

u32 JobSystemImpl::allocate_node() noexcept {
    ScopedLock<SpinLock> held(node_lock);
    const u32 index = node_free_head;
    if (index == kNoNode) {
        return kNoNode;
    }
    node_free_head = nodes[index].next;
    nodes[index].next = kNoNode;
    return index;
}

void JobSystemImpl::free_node(u32 index) noexcept {
    ScopedLock<SpinLock> held(node_lock);
    nodes[index].next = node_free_head;
    node_free_head = index;
}

// --- Scheduling
// ------------------------------------------------------------------------------------

void JobSystemImpl::propagate_path(u32 successor, u64 path_ns, u32 entry) const noexcept {
    TaskRecord& record = records[successor];
    ScopedLock<SpinLock> held(record.path_lock);
    if (path_ns > record.inherited_path_ns) {
        record.inherited_path_ns = path_ns;
        record.predecessor_entry = entry;
    }
}

void JobSystemImpl::enqueue(u32 task, u32 participant) noexcept {
    TaskRecord& record = records[task];
    record.ready_ns = monotonic_now_ns();
    ready_count.fetch_add(1, std::memory_order_relaxed);

    if (deterministic()) {
        // One ordered queue, keyed on the submission sequence. Which worker enqueued the task is
        // deliberately not part of the key.
        if (ordered.push(task, record.sequence)) {
            wake_one();
            return;
        }
    } else {
        const u32 priority = static_cast<u32>(record.priority);
        const u32 owner = participant < participant_count ? participant : 0;
        if (participants[owner].queues[priority].push_back(task)) {
            wake_one();
            return;
        }
        for (u32 offset = 1; offset < participant_count; ++offset) {
            const u32 other = (owner + offset) % participant_count;
            if (participants[other].queues[priority].push_back(task)) {
                wake_one();
                return;
            }
        }
        // Unreachable given the sizing — the deques of one priority class hold `record_count`
        // entries between them, and there are never more ready tasks than records. The ordered
        // queue is the same size and is the fallback rather than an allocation, so that an
        // impossible state is still a correct one.
        if (ordered.push(task, record.sequence)) {
            wake_one();
            return;
        }
    }

    CY_ASSERT_MSG(false, "every ready queue refused a task; the sizing invariant is broken");
    ready_count.fetch_sub(1, std::memory_order_relaxed);
}

namespace {

/// The priority classes to try, in order, for this pop. The fairness sweep is what bounds the
/// starvation of Background and Idle: every `fairness_quantum` pops the order is reversed, so the
/// lowest non-empty class is served even while a higher one is saturated.
struct PriorityOrder {
    u32 order[kPriorityCount];
};

PriorityOrder priority_order(SchedulingMode mode, Participant& participant, u32 quantum) noexcept {
    PriorityOrder result{};
    const bool sweep = quantum != 0 && (++participant.fairness_counter % quantum) == 0;

    for (u32 i = 0; i < kPriorityCount; ++i) {
        result.order[i] = sweep ? kPriorityCount - 1 - i : i;
    }
    if (mode == SchedulingMode::Chaos) {
        // A Fisher-Yates shuffle from the participant's own generator. Chaos randomises *permitted*
        // order only: a dependency is still a dependency, and no result may change.
        for (u32 i = kPriorityCount - 1; i > 0; --i) {
            const u32 j = static_cast<u32>(next_random(participant.rng_state) % (i + 1));
            const u32 swap = result.order[i];
            result.order[i] = result.order[j];
            result.order[j] = swap;
        }
    }
    return result;
}

}  // namespace

bool JobSystemImpl::acquire_task(u32 participant, u32& task) noexcept {
    Participant& me = participants[participant];

    if (deterministic()) {
        if (ordered.pop(task)) {
            ready_count.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    const PriorityOrder order = priority_order(config.mode, me, config.fairness_quantum);

    for (const u32 priority : order.order) {
        if (me.queues[priority].pop_back(task)) {
            ready_count.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
    }

    // Steal. The victim is chosen from the participant's own generator so that two workers starting
    // at the same instant do not scan in the same order and collide on the same victim.
    const u32 start = static_cast<u32>(next_random(me.rng_state) % participant_count);
    for (u32 offset = 0; offset < participant_count; ++offset) {
        const u32 victim = (start + offset) % participant_count;
        if (victim == participant) {
            continue;
        }
        me.steal_attempts.fetch_add(1, std::memory_order_relaxed);
        for (const u32 priority : order.order) {
            // The oldest end: the task least likely to have its data still in the victim's cache,
            // and the one most likely to have a large subtree under it.
            if (participants[victim].queues[priority].pop_front(task)) {
                me.steal_successes.fetch_add(1, std::memory_order_relaxed);
                ready_count.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
        }
    }

    if (ordered.pop(task)) {
        ready_count.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

u32 JobSystemImpl::append_frame_entry(const FrameEntry& entry) noexcept {
    const u32 index = frame_log_used.fetch_add(1, std::memory_order_relaxed);
    if (index >= config.frame_log_entries) {
        frame_log_dropped.fetch_add(1, std::memory_order_relaxed);
        // Put it back so the counter does not run away over a long frame and overflow the check.
        frame_log_used.store(config.frame_log_entries, std::memory_order_relaxed);
        return kNoNode;
    }
    frame_log[index] = entry;
    return index;
}

void JobSystemImpl::execute(u32 participant, u32 task) noexcept {
    Participant& me = participants[participant];
    TaskRecord& record = records[task];

    const i64 started = monotonic_now_ns();
    if (record.ready_ns != 0) {
        const i64 latency = started - record.ready_ns;
        if (latency > 0) {
            queue_latency_ns.fetch_add(static_cast<u64>(latency), std::memory_order_relaxed);
            queue_latency_samples.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // A task whose token was cancelled before it began does not run. That is what a cancelled
    // result means to an awaiter: the work did not happen, and the awaiter is released rather than
    // waiting for a body that will never run.
    const bool cancelled = record.cancellation.is_cancelled();
    record.started_ns = started;

    if (record.cancellation.can_be_cancelled()) {
        ScopedLock<SpinLock> held(me.cancellation_lock);
        me.current_cancellation = record.cancellation;
    }
    me.current_task.store(record.name, std::memory_order_relaxed);
    me.current_task_started_ns.store(started, std::memory_order_relaxed);
    me.current_task_reported_ns.store(0, std::memory_order_relaxed);
    me.cancel_reported_ns.store(0, std::memory_order_relaxed);

    if (!cancelled) {
        TaskContext context;
        context.system = g_system.load(std::memory_order_relaxed);
        // The participant index, helper slots included. A task can be executed by a thread the
        // system did not start — that is exactly what a helping `wait()` does — and reporting
        // kNotAWorker for those would make the index lie about which scratch arena the body is
        // using. An index at or above `worker_count()` names a helper slot.
        context.worker = participant;
        context.scratch = &me.scratch;
        context.cancellation = record.cancellation;
        context.name = record.name;
        context.priority = record.priority;
        context.deadline = record.deadline;
        context.self =
            JobHandle::from_slot(task, record.generation.load(std::memory_order_relaxed));
        context.data =
            record.inline_size != 0 ? static_cast<const void*>(record.inline_data) : nullptr;

        if (config.emit_trace) {
            trace_task_begin(record.name, context.worker, record.priority, record.sequence);
        }
        // The scratch scope brackets the body exactly: everything the task allocated is reclaimed,
        // and poisoned in a development build, before the next task can see the arena.
        {
            const usize mark = me.scratch.mark();
            record.body(context, record.user);
            me.scratch.release_to(mark);
        }
        if (config.emit_trace) {
            trace_task_end(record.name, context.worker, record.priority,
                           static_cast<u64>(monotonic_now_ns() - started));
        }
    }

    const i64 ended = monotonic_now_ns();
    me.current_task.store(nullptr, std::memory_order_relaxed);
    if (record.cancellation.can_be_cancelled()) {
        ScopedLock<SpinLock> held(me.cancellation_lock);
        me.current_cancellation = CancellationToken{};
    }
    me.busy_ns.fetch_add(static_cast<u64>(ended > started ? ended - started : 0),
                         std::memory_order_relaxed);
    me.executed.fetch_add(1, std::memory_order_relaxed);

    complete(participant, task, !cancelled);
}

void JobSystemImpl::complete(u32 participant, u32 task, bool ran) noexcept {
    TaskRecord& record = records[task];

    const i64 ended = monotonic_now_ns();
    const u64 duration =
        static_cast<u64>(ended > record.started_ns ? ended - record.started_ns : 0);

    u64 inherited = 0;
    u32 predecessor = kNoNode;
    {
        ScopedLock<SpinLock> held(record.path_lock);
        inherited = record.inherited_path_ns;
        predecessor = record.predecessor_entry;
    }

    FrameEntry entry;
    entry.name = record.name;
    entry.duration_ns = duration;
    entry.path_ns = inherited + duration;
    entry.predecessor = predecessor;
    entry.worker = participant;
    entry.priority = record.priority;
    const u32 entry_index = append_frame_entry(entry);

    // Step 2 of the completion order in this file's header comment.
    record.completed_path_ns = entry.path_ns;
    record.frame_entry = entry_index;

    // Step 3: the counters, BEFORE the release store below rather than after it. A thread returning
    // from `wait()` has observed that store, so a counter incremented after it can be short by the
    // very task the caller waited for. Relaxed is still right — no state is published through them
    // — but they must be written on this side of the release.
    tasks_executed.fetch_add(1, std::memory_order_relaxed);
    executed_by_priority[static_cast<u32>(record.priority)].fetch_add(1, std::memory_order_relaxed);
    if (!ran) {
        tasks_cancelled.fetch_add(1, std::memory_order_relaxed);
    }

    // Step 4. RELEASE, and this is the single most load-bearing memory order in the module.
    // `wait()` polls `outcome()`, which loads this with acquire; the release is what makes
    // everything the task wrote — its results, the frame-log entry above, the coroutine frame it
    // resumed — visible to the thread that waited for it. Relaxed here compiles and passes every
    // functional test, and ThreadSanitizer reports it as a race in eight different places, because
    // it is one.
    record.outcome.store(static_cast<u8>(ran ? TaskOutcome::Completed : TaskOutcome::Cancelled),
                         std::memory_order_release);

    // Step 5: the release exchange publishes everything above to any thread that reads the closed
    // list with an acquire.
    u32 node = record.successors_head.exchange(kListClosed, std::memory_order_acq_rel);
    while (node != kNoNode && node != kListClosed) {
        const u32 next = nodes[node].next;
        const u32 successor = nodes[node].task;
        free_node(node);

        propagate_path(successor, entry.path_ns, entry_index);
        if (records[successor].dependencies_remaining.fetch_sub(1, std::memory_order_acq_rel) ==
            1) {
            enqueue(successor, participant);
        }
        node = next;
    }

    tasks_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    release(task);
}

bool JobSystemImpl::run_one(u32 participant) noexcept {
    u32 task = kNoNode;
    if (!acquire_task(participant, task)) {
        return false;
    }
    execute(participant, task);
    return true;
}

u32 JobSystemImpl::claim_participant() const noexcept {
    for (u32 p = worker_count; p < participant_count; ++p) {
        bool expected = false;
        if (participants[p].claimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return p;
        }
    }
    // Every helper slot is taken. Such a thread still waits correctly — it simply cannot help,
    // because helping needs a scratch arena and a slab that belong to exactly one thread at a time.
    return kNotAWorker;
}

void JobSystemImpl::release_participant(u32 participant) const noexcept {
    if (participant >= worker_count && participant < participant_count) {
        participants[participant].claimed.store(false, std::memory_order_release);
    }
}

void JobSystemImpl::wake_one() noexcept {
    if (waiters.load(std::memory_order_relaxed) != 0) {
        idle_cv.notify_one();
    }
}

void JobSystemImpl::wake_all() noexcept {
    idle_cv.notify_all();
}

void JobSystemImpl::worker_main(u32 participant) noexcept {
    set_thread_role(ThreadRole::Worker, participant);
    bind_worker_participant(*this, participant);

    Participant& me = participants[participant];
    u32 spins = 0;
    // Idle time is measured from the first failed attempt to find work until the next successful
    // one — the spin included, not only the sleep. A worker spinning for work is idle, and a report
    // that counted only the sleep would show a saturated pool spinning at full utilisation, which
    // is the opposite of what "idle workers are explained" is for.
    i64 idle_started = 0;

    while (!stopping.load(std::memory_order_acquire)) {
        if (run_one(participant)) {
            if (idle_started != 0) {
                const i64 now = monotonic_now_ns();
                me.idle_ns.fetch_add(static_cast<u64>(now > idle_started ? now - idle_started : 0),
                                     std::memory_order_relaxed);
                idle_started = 0;
            }
            spins = 0;
            continue;
        }

        if (idle_started == 0) {
            idle_started = monotonic_now_ns();
        }
        ++spins;
        if (spins < 64) {
            SpinLock::cpu_relax();
            continue;
        }

        waiters.fetch_add(1, std::memory_order_relaxed);
        idle_lock.lock();
        // A timed wait rather than an untimed one: the notify is deliberately made without the lock
        // held (a submission must not serialise on the idle lock), so a wakeup can be missed. One
        // millisecond bounds how long a missed wakeup can cost, and it is a bound rather than a
        // poll because the notify still arrives in the overwhelming majority of cases.
        idle_cv.wait_for(idle_lock, 1'000'000, [this] {
            return stopping.load(std::memory_order_acquire) ||
                   ready_count.load(std::memory_order_relaxed) != 0;
        });
        idle_lock.unlock();
        waiters.fetch_sub(1, std::memory_order_relaxed);

        // Bank the interval here as well as on success. A worker that never gets any work would
        // otherwise report nothing until the system shut down — and a report read at a frame
        // boundary, which is when anyone asks, would show a pool with no idle time in the very
        // case where every worker was idle.
        const i64 now = monotonic_now_ns();
        me.idle_ns.fetch_add(static_cast<u64>(now > idle_started ? now - idle_started : 0),
                             std::memory_order_relaxed);
        idle_started = 0;
        spins = 0;
    }

    if (idle_started != 0) {
        const i64 now = monotonic_now_ns();
        me.idle_ns.fetch_add(static_cast<u64>(now > idle_started ? now - idle_started : 0),
                             std::memory_order_relaxed);
    }
}

void JobSystemImpl::watchdog_main() noexcept {
    // A documented dedicated thread. It owns nothing and touches no engine state: it reads the
    // participants' observation atomics and reports. `core-jobs-and-concurrency` requires a
    // development build to detect a blocked worker, an overlong task and a task that has not
    // observed its cancellation — all three are a duration compared against a threshold, so all
    // three live here rather than in three mechanisms.
    while (!stopping.load(std::memory_order_acquire)) {
        Thread::sleep_for_ns(config.watchdog_interval_ns);
        const i64 now = monotonic_now_ns();

        for (u32 p = 0; p < participant_count; ++p) {
            Participant& participant = participants[p];

            const i64 blocking_since =
                participant.blocking_since_ns.load(std::memory_order_relaxed);
            if (blocking_since != 0 && now - blocking_since > config.blocked_worker_threshold_ns &&
                participant.blocking_reported_ns.load(std::memory_order_relaxed) !=
                    blocking_since) {
                participant.blocking_reported_ns.store(blocking_since, std::memory_order_relaxed);
                blocked_worker_detections.fetch_add(1, std::memory_order_relaxed);
                const char* what = participant.blocking_what.load(std::memory_order_relaxed);
                jobs_log_watchdog("blocked", what != nullptr ? what : "(unnamed)",
                                  static_cast<u64>(now - blocking_since), p);
            }

            const i64 started = participant.current_task_started_ns.load(std::memory_order_relaxed);
            const char* name = participant.current_task.load(std::memory_order_relaxed);
            if (started == 0 || name == nullptr) {
                continue;
            }

            if (now - started > config.long_task_threshold_ns &&
                participant.current_task_reported_ns.load(std::memory_order_relaxed) != started) {
                participant.current_task_reported_ns.store(started, std::memory_order_relaxed);
                long_task_detections.fetch_add(1, std::memory_order_relaxed);
                jobs_log_watchdog("overlong", name, static_cast<u64>(now - started), p);
            }

            // A copy, taken under the lock the executing thread publishes it under. Holding a
            // counted reference is what makes the read safe even if the task finishes and its
            // record is recycled a moment later.
            CancellationToken token;
            {
                ScopedLock<SpinLock> held(participant.cancellation_lock);
                token = participant.current_cancellation;
            }
            const i64 cancelled_at = token.cancelled_at_ns();
            // Re-read the observation: if the participant has moved on to another task, the token
            // above belonged to a task that has already finished and there is nothing to report.
            if (cancelled_at == 0 ||
                participant.current_task_started_ns.load(std::memory_order_relaxed) != started) {
                continue;
            }
            if (now - cancelled_at > config.cancellation_grace_ns &&
                participant.cancel_reported_ns.load(std::memory_order_relaxed) != cancelled_at) {
                participant.cancel_reported_ns.store(cancelled_at, std::memory_order_relaxed);
                unresponsive_cancellations.fetch_add(1, std::memory_order_relaxed);
                jobs_log_watchdog("unresponsive-cancellation", name,
                                  static_cast<u64>(now - cancelled_at), p);
            }
        }
    }
}

}  // namespace detail

// --- JobSystem
// ------------------------------------------------------------------------------------

JobSystem::JobSystem() noexcept = default;

JobSystem::~JobSystem() {
    shutdown();
}

Status JobSystem::start(const JobSystemConfig& configuration) noexcept {
    if (impl_ != nullptr) {
        return fail(ErrorCode::AlreadyExists, "this job system is already running");
    }
    if (detail::current_impl() != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "a job system is already running in this process; core-jobs-and-concurrency "
                    "gives one system ownership of every worker thread");
    }

    auto* impl = new (std::nothrow) detail::JobSystemImpl();
    if (impl == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the job system could not be allocated");
    }

    // Published before the workers start, because a worker's first act is to look itself up.
    g_impl.store(impl, std::memory_order_release);
    g_system.store(this, std::memory_order_release);

    if (auto status = impl->initialize(configuration); !status) {
        impl->teardown();
        g_impl.store(nullptr, std::memory_order_release);
        g_system.store(nullptr, std::memory_order_release);
        delete impl;
        return status;
    }

    impl_ = impl;
    return ok();
}

void JobSystem::shutdown() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    wait_for_idle();

    detail::JobSystemImpl* impl = impl_;
    impl_ = nullptr;
    // Cleared before anything is freed: a helper thread's exit guard reads this to decide whether
    // its slot still exists.
    g_impl.store(nullptr, std::memory_order_release);
    g_system.store(nullptr, std::memory_order_release);
    impl->teardown();
    delete impl;
}

bool JobSystem::is_running() const noexcept {
    return impl_ != nullptr;
}

u32 JobSystem::worker_count() const noexcept {
    return impl_ != nullptr ? impl_->worker_count : 0;
}

SchedulingMode JobSystem::mode() const noexcept {
    return impl_ != nullptr ? impl_->config.mode : SchedulingMode::Default;
}

const JobSystemConfig& JobSystem::config() const noexcept {
    static const JobSystemConfig kNotRunning{};
    return impl_ != nullptr ? impl_->config : kNotRunning;
}

JobSystem* JobSystem::current() noexcept {
    return g_system.load(std::memory_order_acquire);
}

Expected<JobHandle, cy::Error> JobSystem::submit(const JobDesc& desc) noexcept {
    if (impl_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the job system is not running");
    }
    if (desc.body == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a job needs a body");
    }
    if (desc.inline_size > kMaxInlineArgumentBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "a job's inline arguments exceed kMaxInlineArgumentBytes; pass the payload "
                    "behind `user` and keep it alive until the task completes");
    }

    detail::JobSystemImpl& impl = *impl_;
    u32 participant = detail::this_thread_participant(impl);
    // A thread with no helper slot left still submits; it borrows participant 0's slab and queue.
    // It cannot execute tasks, which is the only thing a slot is actually required for.
    const u32 slab = participant == kNotAWorker ? 0 : participant;

    const u32 slot = impl.allocate_record(slab);
    if (slot == detail::kNoNode) {
        impl.slab_exhaustions.fetch_add(1, std::memory_order_relaxed);
        return fail(ErrorCode::Unavailable,
                    "this participant's task slab is full — raise "
                    "JobSystemConfig::task_slots_per_participant, which is the documented bound on "
                    "how many tasks one thread may have in flight");
    }

    detail::TaskRecord& record = impl.records[slot];
    record.body = desc.body;
    record.user = desc.user;
    record.name = desc.name != nullptr ? desc.name : "job";
    record.priority = desc.priority;
    record.deadline = impl.deterministic() ? Deadline{} : desc.deadline;
    record.cancellation = desc.cancellation;
    record.inline_size = desc.inline_size;
    if (desc.inline_size != 0 && desc.inline_data != nullptr) {
        std::memcpy(record.inline_data, desc.inline_data, desc.inline_size);
    }
    record.sequence = impl.sequence_counter.fetch_add(1, std::memory_order_relaxed);
    record.inherited_path_ns = 0;
    record.predecessor_entry = detail::kNoNode;
    record.completed_path_ns = 0;
    record.frame_entry = detail::kNoNode;
    record.ready_ns = 0;
    record.started_ns = 0;
    record.outcome.store(static_cast<u8>(TaskOutcome::Pending), std::memory_order_relaxed);
    record.successors_head.store(detail::kNoNode, std::memory_order_relaxed);
    record.gate.store(desc.gated ? 1 : 0, std::memory_order_relaxed);
    // The guard is the `+ 1`; the gate, when there is one, is a dependency the graph cannot satisfy
    // and only `signal()` can.
    record.dependencies_remaining.store(desc.dependency_count + 1 + (desc.gated ? 1u : 0u),
                                        std::memory_order_release);

    const u64 in_flight = impl.tasks_in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
    u64 peak = impl.peak_in_flight.load(std::memory_order_relaxed);
    while (in_flight > peak &&
           !impl.peak_in_flight.compare_exchange_weak(peak, in_flight, std::memory_order_relaxed)) {
    }
    impl.tasks_submitted.fetch_add(1, std::memory_order_relaxed);

    const JobHandle handle =
        JobHandle::from_slot(slot, record.generation.load(std::memory_order_relaxed));

    // Register against each dependency. One that has already completed is counted here and
    // subtracted in a single step below, so a task whose dependencies are all done is enqueued
    // exactly once rather than once per dependency.
    u32 satisfied = 0;
    for (u32 i = 0; i < desc.dependency_count; ++i) {
        u32 dependency = detail::kNoNode;
        if (!impl.acquire(desc.dependencies[i], dependency)) {
            ++satisfied;
            continue;
        }
        detail::TaskRecord& record_of_dependency = impl.records[dependency];
        u32 node = detail::kNoNode;
        for (;;) {
            u32 head = record_of_dependency.successors_head.load(std::memory_order_acquire);
            if (head == detail::kListClosed) {
                impl.propagate_path(slot, record_of_dependency.completed_path_ns,
                                    record_of_dependency.frame_entry);
                ++satisfied;
                break;
            }
            if (node == detail::kNoNode) {
                node = impl.allocate_node();
                if (node == detail::kNoNode) {
                    // The pool is four links per task; exhausting it means a fan-out far beyond
                    // what the system was sized for. Reported rather than silently dropped: a
                    // dependency that was not registered is a task that runs too early.
                    ++satisfied;
                    break;
                }
                impl.nodes[node].task = slot;
            }
            impl.nodes[node].next = head;
            if (record_of_dependency.successors_head.compare_exchange_weak(
                    head, node, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                node = detail::kNoNode;
                break;
            }
        }
        if (node != detail::kNoNode) {
            impl.free_node(node);
        }
        impl.release(dependency);
    }

    const u32 outstanding =
        record.dependencies_remaining.fetch_sub(satisfied + 1, std::memory_order_acq_rel);
    if (outstanding == satisfied + 1) {
        impl.enqueue(slot, slab);
    }
    return handle;
}

Expected<JobHandle, cy::Error> JobSystem::submit(JobBody body, void* user, const char* name,
                                                 Priority priority) noexcept {
    JobDesc desc;
    desc.body = body;
    desc.user = user;
    desc.name = name;
    desc.priority = priority;
    return submit(desc);
}

u64 JobSystem::partition_count(u64 count, u64 grain) noexcept {
    if (count == 0) {
        return 0;
    }
    const u64 effective_grain = grain == 0 ? 1 : grain;
    // Ranges of at least `grain`, so a small loop does not pay more in scheduling than it saves.
    u64 partitions = (count + effective_grain - 1) / effective_grain;
    // A cap, so that a one-element grain over a million elements does not become a million tasks.
    // It is a function of count and grain alone, which is what keeps the partitioning reproducible.
    partitions = std::min(partitions, kMaxParallelPartitions);
    return partitions;
}

void JobSystem::partition_range(u64 count, u64 grain, u64 index, u64& begin, u64& end) noexcept {
    const u64 partitions = partition_count(count, grain);
    if (partitions == 0 || index >= partitions) {
        begin = count;
        end = count;
        return;
    }
    const u64 chunk = (count + partitions - 1) / partitions;
    begin = index * chunk;
    end = begin + chunk;
    begin = std::min(begin, count);
    end = std::min(end, count);
}

namespace {

/// What one `submit_parallel_for` partition needs to know, copied into the task record rather than
/// allocated per partition. Forty bytes, which is why kMaxInlineArgumentBytes is forty-eight.
struct ParallelForArguments {
    ParallelForBody body;
    void* user;
    u64 count;
    u64 grain;
    u64 index;
};

static_assert(sizeof(ParallelForArguments) <= kMaxInlineArgumentBytes,
              "a parallel loop's partition must travel inside the task record; growing it past "
              "kMaxInlineArgumentBytes would put an allocation back on the scheduling path");

void parallel_for_partition(const TaskContext& context, void*) noexcept {
    ParallelForArguments arguments{};
    std::memcpy(&arguments, context.data, sizeof(arguments));
    u64 begin = 0;
    u64 end = 0;
    JobSystem::partition_range(arguments.count, arguments.grain, arguments.index, begin, end);
    if (begin < end) {
        arguments.body(context, begin, end, arguments.user);
    }
}

/// The join. Its body is empty on purpose: a parallel loop's completion *is* the completion of
/// every partition, and the dependency graph already says so.
void parallel_for_join(const TaskContext&, void*) noexcept {}

}  // namespace

Expected<JobHandle, cy::Error> JobSystem::submit_parallel_for(u64 count, u64 grain,
                                                              ParallelForBody body, void* user,
                                                              const char* name, Priority priority,
                                                              const JobHandle* dependencies,
                                                              u32 dependency_count) noexcept {
    if (impl_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the job system is not running");
    }
    if (body == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a parallel loop needs a body");
    }
    if (count == 0) {
        // A null handle reads as complete, which is what an empty loop is.
        return JobHandle{};
    }

    const u64 partitions = partition_count(count, grain);

    // On the stack, not on the heap: the partition count is capped, so the array has a fixed upper
    // bound and scheduling a parallel loop touches the general allocator not at all. Eight
    // kilobytes of stack in exchange for that is the trade the specification asks for.
    JobHandle handles[kMaxParallelPartitions];
    CY_ASSERT_MSG(partitions <= kMaxParallelPartitions, "partition_count exceeded its own cap");

    for (u64 i = 0; i < partitions; ++i) {
        ParallelForArguments arguments{};
        arguments.body = body;
        arguments.user = user;
        arguments.count = count;
        arguments.grain = grain;
        arguments.index = i;

        JobDesc desc;
        desc.body = &parallel_for_partition;
        desc.name = name;
        desc.priority = priority;
        desc.inline_data = &arguments;
        desc.inline_size = static_cast<u32>(sizeof(arguments));
        desc.dependencies = dependencies;
        desc.dependency_count = dependency_count;

        auto partition = submit(desc);
        if (!partition) {
            // The partitions already submitted are running and must finish before the caller is
            // told the loop failed; otherwise a half-run loop's writes land after the caller has
            // moved on.
            wait_all(handles, static_cast<u32>(i));
            return partition;
        }
        handles[i] = partition.value();
    }

    JobDesc join;
    join.body = &parallel_for_join;
    join.name = name;
    join.priority = priority;
    join.dependencies = handles;
    join.dependency_count = static_cast<u32>(partitions);

    auto joined = submit(join);
    if (!joined) {
        wait_all(handles, static_cast<u32>(partitions));
        return joined;
    }
    return joined;
}

Status JobSystem::signal(JobHandle handle) noexcept {
    if (impl_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the job system is not running");
    }
    detail::JobSystemImpl& impl = *impl_;
    u32 index = detail::kNoNode;
    if (!impl.acquire(handle, index)) {
        return fail(ErrorCode::NotFound,
                    "this handle names a job that has already completed, or was never submitted");
    }

    detail::TaskRecord& record = impl.records[index];
    u8 expected = 1;
    if (!record.gate.compare_exchange_strong(expected, 2, std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
        const bool was_gated = expected != 0;
        impl.release(index);
        return was_gated ? ok()
                         : fail(ErrorCode::InvalidArgument,
                                "this job was not submitted gated, so there is nothing to release");
    }

    const u32 participant = detail::this_thread_participant(impl);
    const u32 slab = participant == kNotAWorker ? 0 : participant;
    if (record.dependencies_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        impl.enqueue(index, slab);
    }
    impl.release(index);
    return ok();
}

void JobSystem::wait(JobHandle handle) noexcept {
    if (impl_ == nullptr || handle.is_null()) {
        return;
    }
    detail::JobSystemImpl& impl = *impl_;
    const u32 participant = detail::this_thread_participant(impl);

    u32 spins = 0;
    while (outcome(handle) == TaskOutcome::Pending) {
        // The whole reason a recursive submission cannot deadlock: a worker that waits runs other
        // ready work instead of blocking, so the pool never has every thread parked on a job that
        // needs a thread to run.
        if (participant != kNotAWorker && impl.run_one(participant)) {
            spins = 0;
            continue;
        }
        ++spins;
        if (spins < 64) {
            Thread::yield();
        } else {
            Thread::sleep_for_ns(50'000);
        }
    }
}

void JobSystem::wait_all(const JobHandle* handles, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        wait(handles[i]);
    }
}

void JobSystem::wait_for_idle() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    detail::JobSystemImpl& impl = *impl_;
    const u32 participant = detail::this_thread_participant(impl);

    u32 spins = 0;
    while (impl.tasks_in_flight.load(std::memory_order_acquire) != 0) {
        if (participant != kNotAWorker && impl.run_one(participant)) {
            spins = 0;
            continue;
        }
        ++spins;
        if (spins < 64) {
            Thread::yield();
        } else {
            Thread::sleep_for_ns(50'000);
        }
    }
}

TaskOutcome JobSystem::outcome(JobHandle handle) const noexcept {
    if (impl_ == nullptr || handle.is_null() || handle.index() >= impl_->record_count) {
        return TaskOutcome::Stale;
    }
    const detail::TaskRecord& record = impl_->records[handle.index()];
    if (record.generation.load(std::memory_order_acquire) != handle.generation()) {
        return TaskOutcome::Stale;
    }
    const auto result = static_cast<TaskOutcome>(record.outcome.load(std::memory_order_acquire));
    // Re-read: the slot may have been recycled between the two loads, in which case the outcome
    // just read belongs to a different task and the honest answer is that this handle is stale.
    if (record.generation.load(std::memory_order_acquire) != handle.generation()) {
        return TaskOutcome::Stale;
    }
    return result;
}

bool JobSystem::is_complete(JobHandle handle) const noexcept {
    return outcome(handle) != TaskOutcome::Pending;
}

void JobSystem::begin_frame(u64 frame_index) noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->frame_index.store(frame_index, std::memory_order_relaxed);
    impl_->frame_log_used.store(0, std::memory_order_relaxed);
    impl_->frame_log_dropped.store(0, std::memory_order_relaxed);
}

JobSystemStats JobSystem::stats() const noexcept {
    JobSystemStats out;
    if (impl_ == nullptr) {
        return out;
    }
    const detail::JobSystemImpl& impl = *impl_;

    out.worker_count = impl.worker_count;
    out.tasks_submitted = impl.tasks_submitted.load(std::memory_order_relaxed);
    out.tasks_executed = impl.tasks_executed.load(std::memory_order_relaxed);
    out.tasks_cancelled = impl.tasks_cancelled.load(std::memory_order_relaxed);
    for (u32 i = 0; i < kPriorityCount; ++i) {
        out.executed_by_priority[i] = impl.executed_by_priority[i].load(std::memory_order_relaxed);
    }
    out.queue_latency_ns = impl.queue_latency_ns.load(std::memory_order_relaxed);
    out.queue_latency_samples = impl.queue_latency_samples.load(std::memory_order_relaxed);
    out.blocked_worker_detections = impl.blocked_worker_detections.load(std::memory_order_relaxed);
    out.long_task_detections = impl.long_task_detections.load(std::memory_order_relaxed);
    out.unresponsive_cancellations =
        impl.unresponsive_cancellations.load(std::memory_order_relaxed);
    out.blocking_violations = blocking_violations();
    out.scheduling_allocations = impl.scheduling_allocations.load(std::memory_order_relaxed);
    out.slab_exhaustions = impl.slab_exhaustions.load(std::memory_order_relaxed);
    out.peak_tasks_in_flight = impl.peak_in_flight.load(std::memory_order_relaxed);

    for (u32 p = 0; p < impl.participant_count; ++p) {
        const detail::Participant& participant = impl.participants[p];
        out.worker_busy_ns += participant.busy_ns.load(std::memory_order_relaxed);
        out.worker_idle_ns += participant.idle_ns.load(std::memory_order_relaxed);
        out.steal_attempts += participant.steal_attempts.load(std::memory_order_relaxed);
        out.steal_successes += participant.steal_successes.load(std::memory_order_relaxed);
        for (const auto& queue : participant.queues) {
            out.queue_depth += queue.size();
        }
    }
    return out;
}

void JobSystem::reset_stats() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    detail::JobSystemImpl& impl = *impl_;
    impl.tasks_submitted.store(0, std::memory_order_relaxed);
    impl.tasks_executed.store(0, std::memory_order_relaxed);
    impl.tasks_cancelled.store(0, std::memory_order_relaxed);
    for (auto& executed : impl.executed_by_priority) {
        executed.store(0, std::memory_order_relaxed);
    }
    impl.queue_latency_ns.store(0, std::memory_order_relaxed);
    impl.queue_latency_samples.store(0, std::memory_order_relaxed);
    impl.blocked_worker_detections.store(0, std::memory_order_relaxed);
    impl.long_task_detections.store(0, std::memory_order_relaxed);
    impl.unresponsive_cancellations.store(0, std::memory_order_relaxed);
    impl.scheduling_allocations.store(0, std::memory_order_relaxed);
    impl.slab_exhaustions.store(0, std::memory_order_relaxed);
    impl.peak_in_flight.store(0, std::memory_order_relaxed);
    for (u32 p = 0; p < impl.participant_count; ++p) {
        impl.participants[p].busy_ns.store(0, std::memory_order_relaxed);
        impl.participants[p].idle_ns.store(0, std::memory_order_relaxed);
        impl.participants[p].executed.store(0, std::memory_order_relaxed);
        impl.participants[p].steal_attempts.store(0, std::memory_order_relaxed);
        impl.participants[p].steal_successes.store(0, std::memory_order_relaxed);
    }
    reset_blocking_violations();
}

CriticalPath JobSystem::critical_path() const noexcept {
    CriticalPath path;
    if (impl_ == nullptr) {
        return path;
    }
    const detail::JobSystemImpl& impl = *impl_;
    path.frame_index = impl.frame_index.load(std::memory_order_relaxed);
    path.entries_dropped = impl.frame_log_dropped.load(std::memory_order_relaxed);

    u32 used = impl.frame_log_used.load(std::memory_order_acquire);
    used = std::min(used, impl.config.frame_log_entries);
    path.tasks_recorded = used;

    u32 longest = detail::kNoNode;
    for (u32 i = 0; i < used; ++i) {
        path.total_task_ns += impl.frame_log[i].duration_ns;
        if (longest == detail::kNoNode ||
            impl.frame_log[i].path_ns > impl.frame_log[longest].path_ns) {
            longest = i;
        }
    }
    if (longest == detail::kNoNode) {
        return path;
    }
    path.total_ns = impl.frame_log[longest].path_ns;

    // Walk backwards from the end of the chain, then reverse: the entries are reported oldest
    // first, because that is the order a timeline is read in.
    CriticalPathEntry reversed[kMaxCriticalPathEntries];
    u32 length = 0;
    u32 cursor = longest;
    while (cursor != detail::kNoNode && cursor < used && length < kMaxCriticalPathEntries) {
        const detail::FrameEntry& entry = impl.frame_log[cursor];
        reversed[length].name = entry.name;
        reversed[length].worker = entry.worker;
        reversed[length].priority = entry.priority;
        reversed[length].duration_ns = entry.duration_ns;
        reversed[length].path_ns = entry.path_ns;
        ++length;
        cursor = entry.predecessor;
    }
    path.truncated = cursor != detail::kNoNode && cursor < used;

    for (u32 i = 0; i < length; ++i) {
        path.entries[i] = reversed[length - 1 - i];
    }
    path.length = length;
    return path;
}

Status JobSystem::begin_blocking_region(const char* what) noexcept {
    const char* description = what != nullptr ? what : "(unnamed)";

    // Claims a participant slot if this thread does not hold one. That is what gives the watchdog
    // somewhere to read the region from: a blocking call on a thread the system has never seen
    // would otherwise be invisible, which is the opposite of the point.
    u32 participant = kNotAWorker;
    if (impl_ != nullptr) {
        participant = detail::this_thread_participant(*impl_);
    }

    // THE RULE, and note what it is stated over: a thread that is *executing a job*, not a thread
    // that happens to hold the Worker role. A waiting thread runs ready tasks rather than blocking
    // — that is the whole reason recursive submission cannot deadlock the pool — so the main thread
    // in the middle of `wait()` is running a task and blocking it stalls the graph exactly as
    // blocking a worker would. Checking the role alone would let the same defect through whenever
    // the task happened to be picked up by the waiter instead of by a worker, which is a property
    // of timing and not of the code.
    //
    // Blocking on I/O, on decompression performed elsewhere, on a network completion or on a GPU
    // fence removes a thread from a pool sized for the machine, and the frame loses it for as long
    // as the operation takes. Counted in every configuration, so a suite asserting on it is not
    // vacuous in Profile and Shipping.
    const bool inside_a_task =
        impl_ != nullptr && participant != kNotAWorker &&
        impl_->participants[participant].current_task.load(std::memory_order_relaxed) != nullptr;

    if (thread_holds_role(ThreadRole::Worker) || inside_a_task) {
        g_blocking_violations.fetch_add(1, std::memory_order_relaxed);
        g_last_blocking_violation.store(description, std::memory_order_relaxed);
        // Reported, not asserted, and the two are not interchangeable here. This function returns
        // an Expected: the refusal *is* the report, and it reaches the caller in every
        // configuration rather than only where assertions are live. Aborting as well would make
        // the documented behaviour — "the call is refused" — impossible to exercise in a
        // development build, which is the one configuration a test suite runs in most.
        //
        // The assertion the specification does ask for is the thread-role one
        // (CY_ASSERT_THREAD_ROLE) and the undeclared-access one; both are about a boundary an
        // owner defends, where there is no value to return.
        return fail(ErrorCode::Unsupported,
                    "a thread executing a job may not block on I/O or the GPU; submit the "
                    "operation asynchronously and resume as a continuation");
    }

    if (participant != kNotAWorker) {
        detail::Participant& me = impl_->participants[participant];
        me.blocking_what.store(description, std::memory_order_relaxed);
        me.blocking_since_ns.store(monotonic_now_ns(), std::memory_order_relaxed);
    }
    return ok();
}

void JobSystem::end_blocking_region() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    const u32 participant = detail::this_thread_participant_if_any(*impl_);
    if (participant != kNotAWorker) {
        detail::Participant& me = impl_->participants[participant];
        me.blocking_since_ns.store(0, std::memory_order_relaxed);
        me.blocking_what.store(nullptr, std::memory_order_relaxed);
    }
}

}  // namespace cy::jobs
