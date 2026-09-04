#pragma once
// The scheduler built from access declarations, and the deferred command buffer. Task 3.2.2.
//
// `core-jobs-and-concurrency`: "The scheduler SHALL build a dependency graph per stage from these
// declarations and run systems in parallel when their access sets do not conflict." Two systems
// that do conflict are ordered — by an explicit constraint where one exists and by a stable
// deterministic order otherwise — rather than being refused. Parallelism is therefore derived, not
// declared: nobody writes down which systems may run together, and nobody can get that wrong.
//
// EVERYTHING IS DECIDED AT REGISTRATION. `add()` runs the conflict test against every system
// already registered and records the ordering edge there and then; `build()` turns the edges into
// batches; `run()` executes a plan that is already complete. Nothing about a system's access is
// discovered while it runs, which is the whole difference between "safe by construction" and "safe
// because the test happened to cover it".
//
// WHAT `add()` AND `order()` REFUSE, each because there is no schedule that satisfies it:
//   * a system with no name, no body, or a name another system already has;
//   * an ordering constraint naming a system that does not exist;
//   * an ordering constraint that closes a cycle — reported with both system names.
// A contradictory *self*-declaration (Read and Write of one component) is refused earlier still, by
// AccessSet::declare.
//
// STRUCTURAL CHANGES ARE DEFERRED. A system running in parallel may not create or destroy an entity
// or add or remove a component; it records the intent into a command buffer applied at the stage's
// flush point. The commit order is (system, partition, local sequence) — stable logical identity,
// with no worker or thread index anywhere in it, because work stealing makes a worker's identity a
// function of timing and `simulation-and-determinism` forbids a commit order that depends on it.

#include <cy/core/jobs/access.h>
#include <cy/core/jobs/context.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/types.h>

namespace cy::jobs {

using SystemId = u32;
inline constexpr SystemId kInvalidSystem = 0xFFFF'FFFFu;

// --- Deferred structural changes -----------------------------------------------------------------

/// What a recorded structural change does. `Custom` is the escape hatch for a subsystem with its
/// own deferred operation; the payload is its to interpret.
enum class StructuralOp : u8 {
    CreateEntity = 0,
    DestroyEntity = 1,
    AddComponent = 2,
    RemoveComponent = 3,
    Custom = 4,
};

const char* structural_op_name(StructuralOp op) noexcept;

struct StructuralCommand {
    StructuralOp op = StructuralOp::Custom;
    ComponentTypeId component = 0;
    u64 entity = 0;
    u64 payload = 0;

    // The deterministic commit key, filled by the recorder. No worker index: see the header.
    SystemId system = kInvalidSystem;
    u32 partition = 0;
    u32 sequence = 0;
};

/// The store every recorder appends to. One per stage; cleared by `flush`.
class DeferredCommands {
public:
    DeferredCommands() noexcept = default;
    ~DeferredCommands();

    DeferredCommands(const DeferredCommands&) = delete;
    DeferredCommands& operator=(const DeferredCommands&) = delete;

    /// Reserve room for `capacity` commands. One allocation, made before the stage runs, so that
    /// recording during parallel execution never touches the general allocator.
    Status initialize(u32 capacity) noexcept;

    /// Append. Thread-safe, lock-free, and it is where a full buffer is reported rather than grown:
    /// growing under a stage's parallel execution is the allocation this design exists to avoid.
    Status append(const StructuralCommand& command) noexcept;

    /// What a flush does with one command.
    using ApplyFn = void (*)(const StructuralCommand& command, void* user) noexcept;

    /// Sort into commit order — (system, partition, sequence) — apply, and empty the store.
    /// Returns how many commands were applied.
    u64 flush(ApplyFn apply, void* user) noexcept;

    [[nodiscard]] u32 pending() const noexcept;
    /// Commands refused because the store was full. A non-zero value means the stage lost work.
    [[nodiscard]] u64 refused() const noexcept;
    void clear() noexcept;

private:
    StructuralCommand* commands_ = nullptr;
    u32 capacity_ = 0;
    Atomic<u32> used_{0};
    Atomic<u64> refused_{0};
};

/// The handle a system body records through. One per (system, partition), so its sequence counter
/// is a plain integer rather than an atomic: the pair is executed by one thread at a time.
class CommandRecorder {
public:
    /// `store` may be null: that is a stage run without a command store, and every record then
    /// fails with Unavailable rather than being silently discarded. A body that records nothing
    /// never notices; a body that does gets told, at the point it tried.
    constexpr CommandRecorder(DeferredCommands* store, SystemId system, u32 partition) noexcept
        : store_(store), system_(system), partition_(partition) {}

    Status create_entity(u64 entity) noexcept {
        return record(StructuralOp::CreateEntity, 0, entity, 0);
    }
    Status destroy_entity(u64 entity) noexcept {
        return record(StructuralOp::DestroyEntity, 0, entity, 0);
    }
    Status add_component(u64 entity, ComponentTypeId component, u64 payload = 0) noexcept {
        return record(StructuralOp::AddComponent, component, entity, payload);
    }
    Status remove_component(u64 entity, ComponentTypeId component) noexcept {
        return record(StructuralOp::RemoveComponent, component, entity, 0);
    }

    Status record(StructuralOp op, ComponentTypeId component, u64 entity, u64 payload) noexcept;

    [[nodiscard]] u32 recorded() const noexcept { return next_sequence_; }

private:
    DeferredCommands* store_;
    SystemId system_;
    u32 partition_;
    u32 next_sequence_ = 0;
};

// --- Systems
// --------------------------------------------------------------------------------------

class SystemSchedule;

/// What a system body is handed. It carries the task context underneath, so a system may submit
/// jobs and use scratch exactly as any other task can.
struct SystemContext {
    const TaskContext* task = nullptr;
    SystemId system = kInvalidSystem;
    /// Which partition of the system is running. Zero until systems are partitioned at M2; it is
    /// already part of the command commit key, so partitioning later changes no ordering rule.
    u32 partition = 0;
    const AccessSet* access = nullptr;
    /// The declaration check for this body. `CY_ASSERT_DECLARED_ACCESS(context.guard, …)`.
    SystemAccessGuard guard;
    /// Where a structural change goes. Null when the stage was run without a command store, which
    /// is the shape a system that makes no structural changes runs in.
    CommandRecorder* commands = nullptr;
};

using SystemBody = void (*)(const SystemContext& context, void* user) noexcept;

struct SystemDesc {
    /// A string literal, or storage that outlives the schedule. Used in every diagnostic, and it is
    /// the identity a duplicate is rejected against.
    const char* name = nullptr;
    SystemBody body = nullptr;
    void* user = nullptr;
    AccessSet access;
    /// Systems this one must run after. Each must already be registered.
    const SystemId* after = nullptr;
    u32 after_count = 0;
};

/// One stage's systems, their derived dependency graph, and the batches it runs in.
class SystemSchedule {
public:
    /// Systems in one stage. A stage with more than this is a stage that wants splitting.
    static constexpr u32 kMaxSystems = 128;

    SystemSchedule() noexcept = default;

    /// Register a system. Runs the conflict test against every system already registered and
    /// records the ordering it derives. See the header for what is refused and why.
    Expected<SystemId, cy::Error> add(const SystemDesc& desc) noexcept;

    /// Add an explicit ordering constraint after the fact. Refuses a constraint that closes a
    /// cycle, naming both systems — which is the only way a cycle can arise, since a constraint
    /// given to `add()` can only name a system that already exists.
    Status order(SystemId before, SystemId after) noexcept;

    /// Turn the edges into batches: a topological levelling in which every system in a batch is
    /// independent of every other system in it. Ties are broken by registration order, so the plan
    /// is the same on every machine and every run.
    Status build() noexcept;

    [[nodiscard]] u32 system_count() const noexcept { return count_; }
    [[nodiscard]] const char* name_of(SystemId system) const noexcept;
    [[nodiscard]] const AccessSet& access_of(SystemId system) const noexcept;
    [[nodiscard]] SystemBody body_of(SystemId system) const noexcept;
    [[nodiscard]] void* user_of(SystemId system) const noexcept;

    /// True when the graph orders `before` ahead of `after`, whether the edge was explicit or was
    /// derived from a conflict.
    [[nodiscard]] bool ordered_before(SystemId before, SystemId after) const noexcept;

    /// True when the two systems' declarations conflict, with the reason in `out`. Two systems that
    /// conflict are always ordered; two that do not are always allowed to run together.
    [[nodiscard]] bool conflicts(SystemId first, SystemId second,
                                 AccessConflict& out) const noexcept;

    [[nodiscard]] u32 batch_count() const noexcept { return batch_count_; }
    [[nodiscard]] u32 batch_size(u32 batch) const noexcept;
    [[nodiscard]] SystemId batch_member(u32 batch, u32 index) const noexcept;
    /// Which batch a system runs in, or kInvalidSystem before `build()`.
    [[nodiscard]] u32 batch_of(SystemId system) const noexcept;

    /// Run the stage: every batch in order, every system in a batch in parallel, then the deferred
    /// commands applied in commit order. `commands` may be null for a stage that records none.
    Status run(JobSystem& jobs, DeferredCommands* commands = nullptr,
               DeferredCommands::ApplyFn apply = nullptr, void* apply_user = nullptr) noexcept;

    void clear() noexcept;

private:
    static constexpr u32 kEdgeWords = (kMaxSystems + 63) / 64;

    struct SystemRecord {
        const char* name = nullptr;
        SystemBody body = nullptr;
        void* user = nullptr;
        AccessSet access;
        u32 batch = kInvalidSystem;
    };

    [[nodiscard]] bool has_edge(SystemId before, SystemId after) const noexcept;
    void set_edge(SystemId before, SystemId after) noexcept;
    /// True when `from` can already reach `to` through the graph — the cycle test.
    [[nodiscard]] bool reaches(SystemId from, SystemId to) const noexcept;

    SystemRecord systems_[kMaxSystems];
    /// edges_[before] has a bit set for every system that must run after `before`.
    u64 edges_[kMaxSystems][kEdgeWords] = {};
    u32 count_ = 0;

    SystemId order_[kMaxSystems] = {};
    u32 batch_first_[kMaxSystems + 1] = {};
    u32 batch_count_ = 0;
    bool built_ = false;
};

}  // namespace cy::jobs
