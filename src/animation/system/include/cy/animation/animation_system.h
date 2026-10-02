// SPDX-License-Identifier: MIT
#pragma once
// The per-frame animation system: every entity carrying an `Animator` is advanced, evaluated and
// published into one pose world, in `ecs::Stage::Animation`, with no per-sample code in the game.
// Issue #76 stage 1.
//
// ================================================================================================
// WHY THIS IS A MODULE OF ITS OWN
// ================================================================================================
//
// `src/animation/` deliberately names no `World`: "which entity owns one, and which stage the
// evaluation runs in, is the bridge's business — the same line src/servers/physics/ draws against
// src/physics/". This is that bridge. It owns the scheduling policy the runtime refuses to decide —
// one `AnimationBatch` per rig, the deterministic half once per simulation tick, the pose half
// split across job workers, the pose world the skinning pass reads — and nothing in the runtime
// moved to make room for it.
//
// ================================================================================================
// WHAT RUNS, AND IN WHICH STAGE
// ================================================================================================
//
// `install()` registers two systems, because the work has two halves with different rules:
//
//   Stage::PostSimulation, once per SIMULATION TICK — the deterministic half.
//     sync        every entity with an `Animator` and no instance gets one; every instance whose
//                 entity died or lost its `Animator` is removed. Neither rebuilds anything: a batch
//                 is packed by moving its last instance into the hole, and the pose world reuses
//                 the freed range. No other entity's handle moves.
//     advance     the state machines, the clip clocks, root motion and events, at the clock's fixed
//                 step, through `AnimationBatch::advance_all` with one `EventBuffer`. It runs in
//                 the fixed step, after gameplay's `Simulation` stage has set this tick's requests,
//                 so it is a function of the tick and the parameters and never of the frame rate:
//                 two runs that simulate the same ticks with the same requests produce the same
//                 state, root motion and events bit for bit, however many frames they took.
//     root motion each tick's delta goes where the instance's `RootMotionMode` says.
//
//   Stage::Animation, once per FRAME — the pose half.
//     evaluate    split into slices of a batch that job workers take. Each slice takes its pose
//                 buffers from its worker's scratch arena and writes skinning matrices straight
//                 into the pose world's staging half; the commits that make them current are then
//                 made in instance order on one thread. A frame that ran no tick evaluates nothing
//                 new: the pose is a function of simulation time.
//
// LEVEL OF DETAIL. An instance's `LodTier` decides how often its pose is evaluated
// (`evaluation_hertz`, counted in SIMULATION time so it is as deterministic as the advance), which
// bone level of detail it is evaluated at (`bone_lod_for`), whether IK runs, and — at `Baked` —
// that it is never evaluated at all and keeps the reference pose it was published with. Every tier
// is advanced every tick: root motion and events do not depend on the tier.
//
// WHAT IS NOT CAPTURED. The instances' state is this object's, not the world's, so it is not in a
// rollback capture or the state hash. A host that restores a checkpoint re-simulates from the
// animation state it has, which is correct for presentation and not for root motion a rollback
// must reproduce. Capturing it is a `StateProvider`, and it is listed under the guide's "Not built
// yet".

#include <cy/animation/evaluate.h>
#include <cy/animation/lod.h>
#include <cy/animation/pose_world.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>

namespace cy::scene {
class SceneTree;
}

namespace cy::animation {

/// A rig registered with one `AnimationSystem`. An index into its rig table.
using RigId = u32;
inline constexpr RigId kNoRig = 0xFFFFFFFFU;

/// Where an instance's extracted root motion goes. `animation-and-skinning` lists the four, and
/// which one applies is a per-instance setting rather than the system's decision.
enum class RootMotionMode : u8 {
    /// Computed, and thrown away. A character whose movement gameplay owns.
    Ignore = 0,
    /// Composed onto the entity's `LocalTransform` each tick, in the entity's own frame.
    ApplyToTransform = 1,
    /// Accumulated for a character controller, which takes it with `take_root_motion()` and moves
    /// the body through physics rather than teleporting the transform.
    Controller = 2,
    /// Reported through `root_motion()` for the last tick and applied nowhere.
    ExtractOnly = 3,
};

[[nodiscard]] const char* root_motion_mode_name(RootMotionMode mode) noexcept;

/// The system's own handle for one instance. Generational, so a handle kept past its instance's
/// removal is recognised as stale instead of naming whichever instance took the slot.
struct AnimatorHandle {
    u32 index = 0xFFFFFFFFU;
    u32 generation = 0;

    [[nodiscard]] bool valid() const noexcept { return index != 0xFFFFFFFFU; }
    friend bool operator==(const AnimatorHandle& a, const AnimatorHandle& b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
};

/// The ECS component. Plain data, trivially copyable, so it lives in chunk storage like any other.
///
/// The first five fields are the game's: which rig, how detailed, whether events are emitted, where
/// root motion goes and how fast the instance plays. The last two are the SYSTEM's, written when it
/// creates the instance: a game reads `pose` to find the instance's matrices in the pose world, and
/// never writes either.
struct Animator {
    RigId rig = kNoRig;
    LodTier tier = LodTier::Full;
    EventPolicy events = EventPolicy::Emit;
    RootMotionMode root_motion = RootMotionMode::Ignore;
    f32 play_rate = 1.0F;
    AnimatorHandle instance;
    PoseHandle pose;
};

inline constexpr const char* kAnimatorComponentName = "cy::animation::Animator";

/// Register `Animator` in a world. Idempotent by name, so two systems over one world share one id.
[[nodiscard]] Expected<ecs::ComponentTypeId, Error> register_animator(ecs::World& world) noexcept;

struct AnimationSystemConfig {
    /// Instances in one slice of a batch: the unit of work a job worker takes. Small enough that a
    /// batch of a few hundred spreads across the workers, large enough that a slice is not mostly
    /// scheduling.
    u32 slice = 32;
};

/// What the last run did, and what the system has done over its life. Every counter is something a
/// test asserts on.
struct AnimationSystemStats {
    /// Batches holding at least one instance. One per rig in use.
    u32 batches = 0;
    u32 instances = 0;
    /// Ticks the last run advanced, and over the system's life.
    u32 ticks = 0;
    u64 ticks_total = 0;
    /// Poses evaluated by the last run, and instances skipped by their tier's rate or by `Baked`.
    u32 evaluated = 0;
    u32 skipped = 0;
    /// The slices the last evaluation was split into.
    u32 slices = 0;
    /// One bit per job participant that evaluated a slice, over the system's life. Bit 63 is the
    /// calling thread when it is not a worker.
    u64 workers = 0;
    u64 instances_added = 0;
    u64 instances_removed = 0;
    /// `Animator`s naming a rig this system does not have. Counted rather than failing the frame,
    /// because one entity authored wrongly must not stop every other one animating; the reason is
    /// in `last_error()`.
    u64 refused = 0;
};

/// One world's animation.
///
/// Not thread-safe as an object: it runs in `Stage::Animation` on the thread that runs the stage,
/// and spreads its own evaluation over job workers. The rigs it is given, the world and the tree
/// must outlive it.
class AnimationSystem {
public:
    /// `tree` is needed only for `RootMotionMode::ApplyToTransform`, which writes `LocalTransform`
    /// and marks it changed so propagation derives `WorldTransform`; it may be null otherwise, and
    /// an instance asking for it then is refused.
    AnimationSystem(Allocator& allocator, ecs::World& world, ecs::ComponentTypeId animator,
                    scene::SceneTree* tree = nullptr,
                    const AnimationSystemConfig& config = {}) noexcept;
    ~AnimationSystem();

    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;
    AnimationSystem(AnimationSystem&&) = delete;
    AnimationSystem& operator=(AnimationSystem&&) = delete;

    /// Register a bound rig, and the batch its instances will share. The rig is not copied: it is
    /// the shared, immutable half every instance reads, and it must outlive this system.
    [[nodiscard]] Expected<RigId, Error> add_rig(const AnimationRig& rig) noexcept;
    [[nodiscard]] u32 rig_count() const noexcept { return static_cast<u32>(batches_.size()); }

    /// Bring the instances in line with the `Animator` components: create the missing, remove the
    /// orphaned, and copy each component's settings onto its instance.
    [[nodiscard]] Status sync() noexcept;

    /// The deterministic half, `ticks` times at `tick_seconds` each: state machines, clocks, root
    /// motion and events. Clears `events()` first.
    [[nodiscard]] Status advance(u32 ticks, f32 tick_seconds) noexcept;

    /// The pose half, over the job system when one is given and on this thread otherwise. The
    /// result is identical either way; only who computes it differs.
    [[nodiscard]] Status evaluate(jobs::JobSystem* jobs) noexcept;

    /// `sync`, then `advance`, then `evaluate`: one frame of a host that has no schedule.
    [[nodiscard]] Status run(u32 ticks, f32 tick_seconds, jobs::JobSystem* jobs) noexcept;

    /// The two systems the header describes: the tick half in `Stage::PostSimulation` and the pose
    /// half in `Stage::Animation`. Returns the `Animation` stage's system. `clock` supplies the
    /// fixed step and must outlive the schedule.
    ///
    /// Scheduled, `events()` holds every event of the ticks since the last frame's evaluation —
    /// read it from a frame stage or after `Simulation::frame` — and the next tick clears it.
    [[nodiscard]] Expected<ecs::SystemId, Error> install(
        ecs::Schedule& schedule, const determinism::SimulationClock& clock) noexcept;
    [[nodiscard]] ecs::SystemId tick_system() const noexcept { return tick_system_; }

    /// What the last scheduled run reported, since a system body cannot return a `Status`.
    [[nodiscard]] const Status& last_error() const noexcept { return last_error_; }

    // --- Per entity ------------------------------------------------------------------------------

    /// The entity's instance, or null when it has none. Mutable for a caller that drives the
    /// instance directly — a test, or a gameplay system that writes many parameters by index.
    [[nodiscard]] AnimationInstance* instance(ecs::Entity entity) noexcept;
    [[nodiscard]] const AnimationInstance* instance(ecs::Entity entity) const noexcept;
    [[nodiscard]] const AnimationRig* rig_of(ecs::Entity entity) const noexcept;

    /// Set one of the program's parameters by name. Refused, naming nothing it could not find, when
    /// the entity has no instance or the program declares no such parameter.
    [[nodiscard]] Status set_parameter(ecs::Entity entity, Name parameter, f32 value) noexcept;
    [[nodiscard]] Expected<f32, Error> parameter(ecs::Entity entity, Name parameter) const noexcept;

    /// The root motion of the entity's last advanced tick.
    [[nodiscard]] RootDelta root_motion(ecs::Entity entity) const noexcept;
    /// The total the entity has travelled since its instance was created.
    [[nodiscard]] Vec3 travelled(ecs::Entity entity) const noexcept;
    /// For `RootMotionMode::Controller`: everything accumulated since the last take, composed in
    /// order, and cleared.
    [[nodiscard]] RootDelta take_root_motion(ecs::Entity entity) noexcept;

    [[nodiscard]] PoseHandle pose_of(ecs::Entity entity) const noexcept;

    /// The events the last `advance()` emitted, in tick then batch then instance order, and the
    /// entity each came from.
    [[nodiscard]] const EventBuffer& events() const noexcept { return events_; }
    [[nodiscard]] ecs::Entity entity_of(const EmittedEvent& event) const noexcept;

    [[nodiscard]] PoseWorld& poses() noexcept { return poses_; }
    [[nodiscard]] const PoseWorld& poses() const noexcept { return poses_; }
    [[nodiscard]] const AnimationSystemStats& stats() const noexcept { return stats_; }

    /// Remove every instance and forget every rig. Idempotent; the destructor calls it.
    void teardown() noexcept;

private:
    struct Slot {
        ecs::Entity entity;
        RigId rig = kNoRig;
        u32 index = 0;
        u32 generation = 1;
        PoseHandle pose;
        RootMotionMode mode = RootMotionMode::Ignore;
        /// Simulation time since the pose was last evaluated, for a tier with a rate.
        f32 since_evaluated = 0.0F;
        /// What a controller has not taken yet.
        RootDelta pending;
        bool live = false;
        /// Set by `advance` for every instance that has to be evaluated this frame.
        bool due = false;
    };

    struct Batch {
        Batch(Allocator& allocator, const AnimationRig& rig) noexcept;
        AnimationBatch instances;
        /// The slot owning each instance, parallel to `instances`.
        Array<u32> owners;
    };

    /// One slice of one batch: what a job takes.
    struct Slice {
        RigId rig = kNoRig;
        u32 first = 0;
        u32 count = 0;
    };

    struct EvaluateJob;

    [[nodiscard]] const Slot* slot_of(ecs::Entity entity) const noexcept;
    [[nodiscard]] Slot* slot_of(ecs::Entity entity) noexcept;
    [[nodiscard]] bool tracks(const Animator& animator, ecs::Entity entity) const noexcept;
    [[nodiscard]] Status create(ecs::Entity entity) noexcept;
    [[nodiscard]] Status destroy(u32 index) noexcept;
    void configure(Slot& slot, const Animator& animator) noexcept;
    [[nodiscard]] Status consume_root_motion(f32 tick_seconds) noexcept;
    void schedule_evaluation(f32 seconds) noexcept;
    [[nodiscard]] Status step(u32 ticks, f32 tick_seconds) noexcept;
    [[nodiscard]] Status publish_reference(Slot& slot) noexcept;
    [[nodiscard]] Status evaluate_slice(const Slice& slice, Span<Transform> scratch) noexcept;
    void refuse(const char* reason) noexcept;

    Allocator* allocator_;
    ecs::World* world_;
    scene::SceneTree* tree_;
    ecs::ComponentTypeId animator_;
    AnimationSystemConfig config_;
    Array<Batch> batches_;
    Array<Slot> slots_;
    Array<u32> free_slots_;
    HashMap<u64, u32> index_;
    Array<Slice> slices_;
    Array<Transform> serial_scratch_;
    PoseWorld poses_;
    EventBuffer events_;
    AnimationSystemStats stats_;
    Status last_error_ = ok();
    const determinism::SimulationClock* clock_ = nullptr;
    ecs::SystemId tick_system_ = ecs::kInvalidSystem;
    /// Set by a scheduled evaluation: the frame has read its events, and the next tick starts a
    /// new set.
    bool events_read_ = false;
};

}  // namespace cy::animation
