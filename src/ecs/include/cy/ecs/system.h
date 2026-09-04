#pragma once
// Systems, stages and the schedule they run in. Task 2.5.
//
// `ecs-core` — "Systems and access declarations": a system declares its component, resource and
// event access as part of its type and is registered into a stage; within a stage the scheduler
// derives a dependency graph from those declarations and runs non-conflicting systems in parallel,
// with explicit `before`/`after` where a semantic order is needed beyond data conflicts.
//
// THE SCHEDULER IS M1'S AND THIS FILE DOES NOT REIMPLEMENT IT. `<cy/core/jobs/schedule.h>` already
// derives the graph from `AccessSet`s, refuses a cycle naming both systems, levels the graph into
// batches with ties broken by registration order, and runs a batch on the job system. What this
// file adds is the ECS's half:
//
//   * the stage list `ecs-core` fixes, and which half of the frame each stage belongs to;
//   * the binding from a system body to a `World`, a `CommandBuffer` and a version;
//   * the per-system measurements task 2.12 reports.
//
// M2 IS THE FIRST REAL CONSUMER OF THE CONFLICT CHECKER, and the finding is worth stating here
// rather than only in a report: the access model expresses what a real system needs, on one
// condition — that the query and the declaration are the same object. A system that writes down its
// access separately from the query it runs can drift, and nothing catches the drift, because a
// declaration is only checked against other declarations. `QueryDesc` therefore *is* the
// declaration (query.h): every term records itself into the `AccessSet` as it is added, and
// `SystemDesc::access` is normally `query.desc().access()`. The one thing the model cannot express
// is a system whose access depends on its input — none exist yet, and one would have to declare the
// union.

#include <cy/core/base/expected.h>
#include <cy/core/jobs/access.h>
#include <cy/core/jobs/schedule.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/command_buffer.h>
#include <cy/ecs/world.h>

namespace cy::ecs {

/// The stages, in execution order. The first four are the fixed step and the last four are the
/// variable one; `engine-architecture`'s loop runs N of the first group per one of the second.
enum class Stage : u8 {
    PreSimulation = 0,
    Physics = 1,
    Simulation = 2,
    PostSimulation = 3,
    Frame = 4,
    Animation = 5,
    UI = 6,
    Render = 7,
};

inline constexpr u32 kStageCount = 8;

const char* stage_name(Stage stage) noexcept;

/// True for the four stages that run on the fixed simulation step. The split is what M2's tick loop
/// and M9's determinism rules are both written against.
[[nodiscard]] constexpr bool stage_is_fixed_step(Stage stage) noexcept {
    return static_cast<u8>(stage) <= static_cast<u8>(Stage::PostSimulation);
}

using SystemId = jobs::SystemId;
inline constexpr SystemId kInvalidSystem = jobs::kInvalidSystem;

/// What a system body is handed.
struct SystemContext {
    World* world = nullptr;
    /// Where a structural change goes. One per system, merged at the stage's flush in
    /// (system, thread, record) order — see command_buffer.h.
    CommandBuffer* commands = nullptr;
    /// The job system's own context, for a body that submits jobs or uses scratch. Null when the
    /// stage was run serially.
    const jobs::SystemContext* jobs = nullptr;
    void* user = nullptr;
    Stage stage = Stage::Simulation;
    SystemId system = kInvalidSystem;
};

using SystemBody = void (*)(const SystemContext& context) noexcept;

struct SystemDesc {
    /// A string literal, or storage that outlives the schedule. The identity a duplicate is
    /// rejected against, and what every diagnostic names.
    const char* name = nullptr;
    SystemBody body = nullptr;
    void* user = nullptr;
    /// Normally `query.desc().access()`. See the header comment on why they are the same object.
    jobs::AccessSet access;
    /// Systems in the same stage this one must run after. Each must already be registered.
    Span<const SystemId> after;
};

/// What one system cost, for task 2.12.
struct SystemProfile {
    const char* name = "";
    Stage stage = Stage::Simulation;
    u64 runs = 0;
    u64 total_ns = 0;
    u64 last_ns = 0;
    u64 commands_recorded = 0;
};

/// One world's systems, grouped by stage.
class Schedule {
public:
    explicit Schedule(World& world) noexcept : world_(&world) {}
    ~Schedule();

    Schedule(const Schedule&) = delete;
    Schedule& operator=(const Schedule&) = delete;
    Schedule(Schedule&&) = delete;
    Schedule& operator=(Schedule&&) = delete;

    /// Register a system into a stage. Conflicts with systems already in that stage are resolved
    /// into ordering edges here, and a declaration that cannot be scheduled at all — a duplicate
    /// name, an `after` naming a system that does not exist, a constraint closing a cycle — is
    /// refused now rather than discovered while running.
    [[nodiscard]] Expected<SystemId, Error> add(Stage stage, const SystemDesc& desc) noexcept;

    /// An explicit ordering constraint between two systems of one stage. `ecs-core`'s escape hatch
    /// for a semantic order that no shared data expresses.
    [[nodiscard]] Status order(Stage stage, SystemId before, SystemId after) noexcept;

    /// Turn every stage's edges into batches. Must be called before a stage runs; calling it again
    /// after adding a system is how a schedule is extended.
    [[nodiscard]] Status build() noexcept;

    /// Run one stage on the job system: every batch in order, every system within a batch in
    /// parallel. The world's version is advanced first, so everything written during the stage
    /// carries one version; the command buffers are flushed afterwards, which is the stage's
    /// flush point.
    [[nodiscard]] Status run(Stage stage, jobs::JobSystem& jobs) noexcept;

    /// The same, on the calling thread, batch by batch and system by system in batch order. The
    /// result is identical — that is the point of deriving parallelism rather than declaring it —
    /// and it is what a test without a job system runs.
    [[nodiscard]] Status run_serial(Stage stage) noexcept;

    [[nodiscard]] u32 system_count(Stage stage) const noexcept;
    [[nodiscard]] u32 batch_count(Stage stage) const noexcept;
    [[nodiscard]] u32 batch_size(Stage stage, u32 batch) const noexcept;
    /// True when the two systems of one stage were ordered, whether explicitly or by a conflict.
    [[nodiscard]] bool ordered_before(Stage stage, SystemId before, SystemId after) const noexcept;
    /// True when the two systems' declarations conflict, with the reason in `out`.
    [[nodiscard]] bool conflicts(Stage stage, SystemId first, SystemId second,
                                 jobs::AccessConflict& out) const noexcept;

    [[nodiscard]] const SystemProfile* profile(Stage stage, SystemId system) const noexcept;
    [[nodiscard]] CommandBuffer* commands(Stage stage, SystemId system) noexcept;

    /// Every stage's systems, in stage then registration order. What the diagnostics report walks.
    [[nodiscard]] Status collect_profiles(Array<SystemProfile>& out) const noexcept;

private:
    /// One system, allocated on its own so that its address — which the job schedule holds as the
    /// body's `user` pointer — does not move when the stage gains another system.
    struct Registration {
        Schedule* owner = nullptr;
        SystemBody body = nullptr;
        void* user = nullptr;
        Stage stage = Stage::Simulation;
        SystemId system = kInvalidSystem;
        SystemProfile profile;
        /// Constructed with a provisional merge key and given its real one by `build()`, which
        /// ranks the stage's systems by name. The flush order is therefore a function of what the
        /// systems are called rather than of the order they were registered in — see
        /// command_buffer.h for why that distinction is worth a pass over the list.
        CommandBuffer commands;

        Registration(World& world, u32 order) noexcept : commands(world, order, 0) {}
    };

    struct StageState {
        jobs::SystemSchedule schedule;
        Array<Registration*> systems;
        bool built = false;

        explicit StageState(Allocator& allocator) noexcept : systems(allocator) {}
    };

    static void trampoline(const jobs::SystemContext& context, void* user) noexcept;

    [[nodiscard]] Expected<StageState*, Error> stage_state(Stage stage) noexcept;
    [[nodiscard]] const StageState* stage_state(Stage stage) const noexcept {
        return stages_[static_cast<u32>(stage)];
    }
    /// Advance the version, run the bodies, flush. The half `run` and `run_serial` share.
    [[nodiscard]] Status finish_stage(StageState& state) noexcept;

    /// Give every system in the stage the merge key its name earns it. Called by `build()`, which
    /// is the first point at which every name is known — see command_buffer.h.
    ///
    /// Static because everything it needs is in the stage: a schedule-wide fact leaking into a
    /// per-stage ranking is exactly the kind of coupling that would make two stages' merge orders
    /// depend on each other.
    [[nodiscard]] static Status assign_merge_keys(StageState& state) noexcept;

    World* world_;
    StageState* stages_[kStageCount] = {};
};

}  // namespace cy::ecs
