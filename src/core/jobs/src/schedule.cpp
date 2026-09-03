// The schedule: registration, the derived dependency graph, the batches, and the deferred commands.
//
// The graph is an adjacency bitset — one bit per (before, after) pair — rather than a list per
// system. With 128 systems that is two kilobytes, every edge test is one shift and one mask, and
// the reachability test that rejects a cycle is a bitset traversal instead of a recursive walk.
//
// THE COMMIT SORT is an insertion sort over the key (system, partition, sequence). It is stable,
// it is O(n) on the already-nearly-sorted input a stage actually produces — systems finish roughly
// in batch order — and it sorts a buffer whose size is fixed before the stage began. What matters
// is not the algorithm but the key: it contains no worker index, so two runs that scheduled the
// same work onto different workers commit in the same order.

#include <cy/core/jobs/schedule.h>

#include <cy/core/base/assert.h>

#include <cstring>
#include <new>

namespace cy::jobs {
namespace {

/// The key a flush sorts on. `system` is 32 bits, `partition` and `sequence` 32 each, so the whole
/// key does not fit one machine word and the comparison is written out rather than packed.
bool commits_before(const StructuralCommand& first, const StructuralCommand& second) noexcept {
    if (first.system != second.system) {
        return first.system < second.system;
    }
    if (first.partition != second.partition) {
        return first.partition < second.partition;
    }
    return first.sequence < second.sequence;
}

}  // namespace

const char* structural_op_name(StructuralOp op) noexcept {
    switch (op) {
        case StructuralOp::CreateEntity:
            return "CreateEntity";
        case StructuralOp::DestroyEntity:
            return "DestroyEntity";
        case StructuralOp::AddComponent:
            return "AddComponent";
        case StructuralOp::RemoveComponent:
            return "RemoveComponent";
        case StructuralOp::Custom:
            return "Custom";
    }
    return "Unknown";
}

// --- DeferredCommands -----------------------------------------------------------------------------

DeferredCommands::~DeferredCommands() {
    delete[] commands_;
    commands_ = nullptr;
}

Status DeferredCommands::initialize(u32 capacity) noexcept {
    if (capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "a command store of zero commands stores none");
    }
    delete[] commands_;
    commands_ = new (std::nothrow) StructuralCommand[capacity];
    if (commands_ == nullptr) {
        capacity_ = 0;
        return fail(ErrorCode::OutOfMemory, "the deferred command store could not be allocated");
    }
    capacity_ = capacity;
    used_.store(0);
    refused_.store(0);
    return ok();
}

Status DeferredCommands::append(const StructuralCommand& command) noexcept {
    if (commands_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the deferred command store was never initialised");
    }
    const u32 index = used_.fetch_add(1, std::memory_order_acq_rel);
    if (index >= capacity_) {
        // Put the counter back so a long stage cannot run it past the capacity check.
        used_.store(capacity_, std::memory_order_relaxed);
        refused_.fetch_add(1, std::memory_order_relaxed);
        return fail(ErrorCode::OutOfRange,
                    "the deferred command store is full; size it for the stage's worst frame, "
                    "because growing it during parallel execution is the allocation this buffer "
                    "exists to avoid");
    }
    commands_[index] = command;
    return ok();
}

u64 DeferredCommands::flush(ApplyFn apply, void* user) noexcept {
    if (commands_ == nullptr) {
        return 0;
    }
    u32 count = used_.load(std::memory_order_acquire);
    if (count > capacity_) {
        count = capacity_;
    }

    for (u32 i = 1; i < count; ++i) {
        const StructuralCommand command = commands_[i];
        u32 j = i;
        while (j > 0 && commits_before(command, commands_[j - 1])) {
            commands_[j] = commands_[j - 1];
            --j;
        }
        commands_[j] = command;
    }

    if (apply != nullptr) {
        for (u32 i = 0; i < count; ++i) {
            apply(commands_[i], user);
        }
    }
    used_.store(0, std::memory_order_release);
    return count;
}

u32 DeferredCommands::pending() const noexcept {
    const u32 count = used_.load(std::memory_order_relaxed);
    return count > capacity_ ? capacity_ : count;
}

u64 DeferredCommands::refused() const noexcept {
    return refused_.load(std::memory_order_relaxed);
}

void DeferredCommands::clear() noexcept {
    used_.store(0, std::memory_order_release);
    refused_.store(0, std::memory_order_relaxed);
}

Status CommandRecorder::record(StructuralOp op, ComponentTypeId component, u64 entity,
                               u64 payload) noexcept {
    if (store_ == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "this stage was run without a deferred command store, so a structural change "
                    "has nowhere to be recorded; pass one to SystemSchedule::run");
    }
    StructuralCommand command;
    command.op = op;
    command.component = component;
    command.entity = entity;
    command.payload = payload;
    command.system = system_;
    command.partition = partition_;
    command.sequence = next_sequence_++;
    return store_->append(command);
}

// --- SystemSchedule -------------------------------------------------------------------------------

bool SystemSchedule::has_edge(SystemId before, SystemId after) const noexcept {
    return (edges_[before][after / 64] & (1ull << (after % 64))) != 0;
}

void SystemSchedule::set_edge(SystemId before, SystemId after) noexcept {
    edges_[before][after / 64] |= 1ull << (after % 64);
}

bool SystemSchedule::reaches(SystemId from, SystemId to) const noexcept {
    // Breadth-first over the bitset, with a visited mask. No recursion and no allocation: the whole
    // frontier is `count_` bits.
    u64 visited[kEdgeWords] = {};
    SystemId queue[kMaxSystems];
    u32 head = 0;
    u32 tail = 0;

    queue[tail++] = from;
    visited[from / 64] |= 1ull << (from % 64);

    while (head < tail) {
        const SystemId current = queue[head++];
        if (current == to && current != from) {
            return true;
        }
        for (u32 word = 0; word < kEdgeWords; ++word) {
            u64 bits = edges_[current][word] & ~visited[word];
            while (bits != 0) {
                const u32 bit = static_cast<u32>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const SystemId next = word * 64 + bit;
                if (next == to) {
                    return true;
                }
                visited[word] |= 1ull << bit;
                queue[tail++] = next;
            }
        }
    }
    return false;
}

Expected<SystemId, cy::Error> SystemSchedule::add(const SystemDesc& desc) noexcept {
    if (desc.name == nullptr || desc.name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument,
                    "a system needs a name; it is what every conflict and ordering diagnostic "
                    "identifies it by");
    }
    if (desc.body == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a system needs a body");
    }
    if (count_ == kMaxSystems) {
        return fail(ErrorCode::OutOfRange,
                    "a stage may hold at most SystemSchedule::kMaxSystems systems");
    }
    for (u32 i = 0; i < count_; ++i) {
        if (std::strcmp(systems_[i].name, desc.name) == 0) {
            return fail(ErrorCode::AlreadyExists,
                        "a system with this name is already registered in this stage");
        }
    }
    for (u32 i = 0; i < desc.after_count; ++i) {
        if (desc.after[i] >= count_) {
            return fail(ErrorCode::NotFound,
                        "an ordering constraint names a system that is not registered; a "
                        "constraint may only name a system registered before this one");
        }
    }

    const SystemId id = count_;
    systems_[id].name = desc.name;
    systems_[id].body = desc.body;
    systems_[id].user = desc.user;
    systems_[id].access = desc.access;
    systems_[id].batch = kInvalidSystem;
    ++count_;

    for (u32 i = 0; i < desc.after_count; ++i) {
        set_edge(desc.after[i], id);
    }

    // The conflict test, run here rather than at run time. An overlapping write between this system
    // and one already registered becomes an ordering edge immediately; the direction is registration
    // order, which is the "stable deterministic order" the specification asks for when no explicit
    // constraint says otherwise.
    for (SystemId other = 0; other < id; ++other) {
        AccessConflict conflict;
        if (!systems_[other].access.conflicts_with(systems_[id].access, conflict)) {
            continue;
        }
        if (!has_edge(other, id) && !has_edge(id, other)) {
            set_edge(other, id);
        }
    }

    built_ = false;
    return id;
}

Status SystemSchedule::order(SystemId before, SystemId after) noexcept {
    if (before >= count_ || after >= count_) {
        return fail(ErrorCode::NotFound, "an ordering constraint names a system that is not "
                                         "registered");
    }
    if (before == after) {
        return fail(ErrorCode::InvalidArgument, "a system cannot be ordered against itself");
    }
    if (has_edge(before, after)) {
        return ok();
    }
    // The cycle test, before the edge exists rather than after: `after` must not already reach
    // `before`, or the new edge closes a loop and no execution order satisfies the stage.
    if (reaches(after, before)) {
        // Reported as a value, not asserted. A caller composing a stage from configuration can
        // legitimately hand this function a constraint that does not fit, and refusing it is the
        // answer; CY_ASSERT is for a condition that is true unless the *engine* is wrong.
        return fail(ErrorCode::InvalidArgument,
                    "this ordering constraint closes a cycle: the system that would run second is "
                    "already ordered ahead of the one that would run first");
    }
    set_edge(before, after);
    built_ = false;
    return ok();
}

Status SystemSchedule::build() noexcept {
    // Kahn's algorithm, with ties broken by registration index. The tie-break is what makes the plan
    // identical on every machine: without it, the order would depend on the traversal, which is
    // stable here but would not survive the first refactor.
    u32 indegree[kMaxSystems] = {};
    for (SystemId before = 0; before < count_; ++before) {
        for (SystemId after = 0; after < count_; ++after) {
            if (has_edge(before, after)) {
                ++indegree[after];
            }
        }
    }

    // Reset the batch assignment: `build()` may be called again after a system or a constraint was
    // added, and a stale assignment would make every system look as though it had already been
    // placed.
    for (SystemId system = 0; system < count_; ++system) {
        systems_[system].batch = kInvalidSystem;
    }

    u32 placed = 0;
    batch_count_ = 0;
    batch_first_[0] = 0;

    while (placed < count_) {
        // Everything with no unplaced predecessor forms one batch: by construction no two of them
        // are ordered against each other, and two systems that conflict are always ordered.
        const u32 batch_begin = placed;
        for (SystemId system = 0; system < count_; ++system) {
            if (indegree[system] == 0 && systems_[system].batch == kInvalidSystem) {
                order_[placed++] = system;
                systems_[system].batch = batch_count_;
            }
        }
        if (placed == batch_begin) {
            // Unreachable: `order()` refuses a cycle and `add()` cannot create one. Reported rather
            // than looping, because an unreachable state that silently spins is worse than one that
            // says what happened.
            for (SystemId system = 0; system < count_; ++system) {
                systems_[system].batch = kInvalidSystem;
            }
            batch_count_ = 0;
            return fail(ErrorCode::Internal,
                        "the system graph contains a cycle; `order()` should have refused the "
                        "constraint that created it");
        }
        for (u32 i = batch_begin; i < placed; ++i) {
            for (SystemId after = 0; after < count_; ++after) {
                if (has_edge(order_[i], after)) {
                    --indegree[after];
                }
            }
        }
        ++batch_count_;
        batch_first_[batch_count_] = placed;
    }

    built_ = true;
    return ok();
}

const char* SystemSchedule::name_of(SystemId system) const noexcept {
    return system < count_ ? systems_[system].name : "";
}

const AccessSet& SystemSchedule::access_of(SystemId system) const noexcept {
    static const AccessSet kEmpty;
    return system < count_ ? systems_[system].access : kEmpty;
}

SystemBody SystemSchedule::body_of(SystemId system) const noexcept {
    return system < count_ ? systems_[system].body : nullptr;
}

void* SystemSchedule::user_of(SystemId system) const noexcept {
    return system < count_ ? systems_[system].user : nullptr;
}

bool SystemSchedule::ordered_before(SystemId before, SystemId after) const noexcept {
    if (before >= count_ || after >= count_) {
        return false;
    }
    return has_edge(before, after) || reaches(before, after);
}

bool SystemSchedule::conflicts(SystemId first, SystemId second, AccessConflict& out) const noexcept {
    if (first >= count_ || second >= count_ || first == second) {
        return false;
    }
    return systems_[first].access.conflicts_with(systems_[second].access, out);
}

u32 SystemSchedule::batch_size(u32 batch) const noexcept {
    if (!built_ || batch >= batch_count_) {
        return 0;
    }
    return batch_first_[batch + 1] - batch_first_[batch];
}

SystemId SystemSchedule::batch_member(u32 batch, u32 index) const noexcept {
    if (index >= batch_size(batch)) {
        return kInvalidSystem;
    }
    return order_[batch_first_[batch] + index];
}

u32 SystemSchedule::batch_of(SystemId system) const noexcept {
    return system < count_ ? systems_[system].batch : kInvalidSystem;
}

namespace {

/// What one system's job needs, copied into the task record so that running a stage allocates
/// nothing. Twenty-four bytes.
struct SystemInvocation {
    SystemSchedule* schedule;
    DeferredCommands* commands;
    SystemId system;
};

static_assert(sizeof(SystemInvocation) <= kMaxInlineArgumentBytes,
              "a system invocation must travel inside the task record");

}  // namespace

/// Defined out of line rather than as a lambda so that the body is an ordinary function pointer:
/// JobBody is a function pointer precisely because a std::function would allocate.
void run_one_system(const TaskContext& task, void* user) noexcept;

Status SystemSchedule::run(JobSystem& jobs, DeferredCommands* commands,
                           DeferredCommands::ApplyFn apply, void* apply_user) noexcept {
    if (!built_) {
        if (auto status = build(); !status) {
            return status;
        }
    }
    if (!jobs.is_running()) {
        return fail(ErrorCode::Unavailable, "the job system is not running");
    }

    JobHandle handles[kMaxSystems];
    for (u32 batch = 0; batch < batch_count_; ++batch) {
        const u32 size = batch_size(batch);
        for (u32 i = 0; i < size; ++i) {
            SystemInvocation invocation{this, commands, batch_member(batch, i)};
            JobDesc desc;
            desc.body = &run_one_system;
            desc.name = systems_[invocation.system].name;
            desc.inline_data = &invocation;
            desc.inline_size = static_cast<u32>(sizeof(invocation));
            auto submitted = jobs.submit(desc);
            if (!submitted) {
                jobs.wait_all(handles, i);
                return fail(submitted.error().code, submitted.error().message,
                            submitted.error().system_code);
            }
            handles[i] = submitted.value();
        }
        // The batch is a synchronisation point: every system in it has finished before the next
        // batch begins, which is what makes the ordering edges mean anything.
        jobs.wait_all(handles, size);
    }

    // The stage's flush point. Structural changes recorded during parallel execution are applied
    // here, in commit order, on this thread.
    if (commands != nullptr) {
        commands->flush(apply, apply_user);
    }
    return ok();
}

void run_one_system(const TaskContext& task, void* user) noexcept {
    (void)user;
    SystemInvocation invocation{};
    std::memcpy(&invocation, task.data, sizeof(invocation));

    SystemSchedule& schedule = *invocation.schedule;
    const AccessSet& access = schedule.access_of(invocation.system);

    SystemContext context;
    context.task = &task;
    context.system = invocation.system;
    context.partition = 0;
    context.access = &access;
    context.guard = SystemAccessGuard(schedule.name_of(invocation.system), access);

    CommandRecorder recorder(invocation.commands, invocation.system, context.partition);
    context.commands = &recorder;

    schedule.body_of(invocation.system)(context, schedule.user_of(invocation.system));
}

void SystemSchedule::clear() noexcept {
    count_ = 0;
    batch_count_ = 0;
    built_ = false;
    std::memset(edges_, 0, sizeof(edges_));
}

}  // namespace cy::jobs
