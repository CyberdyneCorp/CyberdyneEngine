#pragma once
// Pose evaluation: the compiled animation program run over poses, batched by program, with root
// motion on a deterministic CPU path beside it. M8.b task 5.2 and the root-motion half of 5.3.
//
// ================================================================================================
// WHY THE PROGRAM IS `cy::graph::pose`'s AND THE EVALUATOR IS THIS MODULE'S
// ================================================================================================
//
// The compiled form — the flat instruction array, the state machine, the joint masks pose
// dependency analysis produced — is M8.b task 2.4's, in `cy/graph/lower_pose.h`, and this module
// does not duplicate it: `compile_pose()` is the compiler and `graph::pose::advance()` is the state
// machine.
//
// Its `evaluate()` is not what a frame runs, for two reasons that are both properties of where it
// lives rather than defects in it:
//
//   * `cy::graph` depends on `cy::core-base`, `cy::core-memory` and `cy::core-values` and NOT on
//     `cy::core-math`. Its pose value is eight floats per joint blended componentwise, so it cannot
//     slerp. A rotation blended componentwise is not a rotation, and a skeleton evaluated that way
//     shortens every bone at the middle of a blend.
//   * it allocates a scratch array per call. At the crowd sizes this specification is written for —
//     "50,000 characters visible" — a heap allocation per instance per frame is the cost.
//
// So the runtime evaluator is here, over `Transform`, with `interpolate()` doing the rotation
// properly and one caller-owned `PoseScratch` reused across a whole batch. It walks the same
// program, honours the same `PoseInstruction::required` masks, and is lazy in the same way.
//
// ================================================================================================
// ROOT MOTION IS COMPUTED IN advance(), NOT IN evaluate(), AND THAT IS THE DETERMINISM CONTRACT
// ================================================================================================
//
// `animation-and-skinning` — "Animation determinism": root motion "SHALL be computed on a
// deterministic CPU path from clips' root tracks with blend weights composed on the CPU, regardless
// of where the rest of the pose is evaluated and regardless of the instance's animation LOD tier".
//
// `evaluate()` is the part that a tier may skip, a pose cache may satisfy and a GPU may one day
// perform. `advance()` is the part that may not. So root motion is accumulated in `advance()`, from
// the clips' root tracks over the whole interval, weighted by the same blend weights the pose tree
// uses — and an instance at `Baked` tier, whose pose is never evaluated at all, still travels
// exactly as far. `test_evaluate.cpp` asserts that in both directions.

#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/graph/lower_pose.h>

#include <cy/animation/clip.h>
#include <cy/animation/ik.h>
#include <cy/animation/lod.h>
#include <cy/animation/skeleton.h>

namespace cy::animation {

using PoseProgram = graph::pose::PoseProgram;
using PoseInstruction = graph::pose::PoseInstruction;
using PoseOp = graph::pose::PoseOp;
using PoseValue = graph::pose::PoseValue;

/// A skeleton, a compiled program and the clips the program's clip table names. Shared by every
/// instance using them: "one compiled program SHALL exist and per-instance memory SHALL be the
/// state block and pose only".
class AnimationRig {
public:
    explicit AnimationRig(Allocator& allocator) noexcept;

    /// Bind a program to a skeleton and to the clips its clip table names. `clips` is parallel to
    /// `program.clips()`; a null entry is a clip that failed to load, and sampling it yields the
    /// pose it was handed rather than failing the frame.
    [[nodiscard]] Status bind(const Skeleton& skeleton, const PoseProgram& program,
                              Span<const Clip* const> clips) noexcept;

    [[nodiscard]] const Skeleton& skeleton() const noexcept { return *skeleton_; }
    [[nodiscard]] const PoseProgram& program() const noexcept { return *program_; }
    [[nodiscard]] Span<const Clip* const> clips() const noexcept { return clips_.span(); }
    [[nodiscard]] bool bound() const noexcept { return skeleton_ != nullptr; }

    /// The parameters the runtime advances rather than the game: one per clip instruction, paired
    /// with the clip whose duration wraps it. Collected once at bind, so `advance()` is a walk over
    /// a small table rather than over the program.
    struct TimeParameter {
        u16 parameter = 0;
        u16 clip = 0;
        f32 duration = 1.0F;
    };
    [[nodiscard]] Span<const TimeParameter> time_parameters() const noexcept {
        return times_.span();
    }

    /// The per-joint weight of one of the program's masks.
    ///
    /// The compiled program's masks are a BIT PER JOINT — a joint is in a layer or it is not — and
    /// `animation-and-skinning` requires more than that: "WHEN a mask assigns partial weight to the
    /// spine THEN that joint SHALL blend proportionally rather than being fully overridden or
    /// excluded." The bit stays the membership test the compiler's pose dependency analysis needs;
    /// this table is the proportion, and it defaults to one so a program with no weights authored
    /// behaves exactly as its masks say.
    [[nodiscard]] Status set_mask_weight(u16 mask, u16 joint, f32 weight) noexcept;
    [[nodiscard]] f32 mask_weight(u16 mask, u16 joint) const noexcept;

    /// The chain an `IK` instruction solves. A chain that was never set is skipped and counted.
    [[nodiscard]] Status set_ik_chain(u16 index, const IkChain& chain) noexcept;
    [[nodiscard]] const IkChain* ik_chain(u16 index) const noexcept;

private:
    const Skeleton* skeleton_ = nullptr;
    const PoseProgram* program_ = nullptr;
    Array<const Clip*> clips_;
    Array<TimeParameter> times_;
    Array<f32> mask_weights_;
    Array<IkChain> chains_;
    u32 joints_ = 0;
};

/// The per-instance state block: where the state machine is, what the parameters are, and where
/// each clip's cursor left off. Small and flat.
class AnimationInstance {
public:
    explicit AnimationInstance(Allocator& allocator) noexcept;

    [[nodiscard]] Status prepare(const AnimationRig& rig) noexcept;

    [[nodiscard]] graph::pose::PoseInstance& machine() noexcept { return machine_; }
    [[nodiscard]] const graph::pose::PoseInstance& machine() const noexcept { return machine_; }
    [[nodiscard]] Span<f32> parameters() noexcept { return parameters_.span(); }
    [[nodiscard]] Span<const f32> parameters() const noexcept { return parameters_.span(); }

    [[nodiscard]] Status set_parameter(const AnimationRig& rig, Name parameter, f32 value) noexcept;
    [[nodiscard]] f32 parameter(const AnimationRig& rig, Name parameter) const noexcept;

    /// The playback rate. One is authored speed; zero freezes the instance without changing its
    /// state machine.
    void set_play_rate(f32 rate) noexcept { play_rate_ = rate; }
    [[nodiscard]] f32 play_rate() const noexcept { return play_rate_; }

    void set_tier(LodTier tier) noexcept { tier_ = tier; }
    [[nodiscard]] LodTier tier() const noexcept { return tier_; }

    void set_event_policy(EventPolicy policy) noexcept { events_ = policy; }
    [[nodiscard]] EventPolicy event_policy() const noexcept { return events_; }

    /// A cursor per clip of the program, so forward playback never searches.
    [[nodiscard]] ClipCursor& cursor(u16 clip) noexcept;

    /// The clip times as they were before the last `advance()`. Root motion and events are both
    /// integrals over the interval between these and the current ones, which is what makes a tick
    /// at 10 Hz travel exactly as far as six at 60 Hz.
    [[nodiscard]] Span<const f32> previous_times() const noexcept { return previous_.span(); }
    [[nodiscard]] Span<f32> previous_times() noexcept { return previous_.span(); }

    /// What the instance has travelled this tick, and the total since it was prepared. The total is
    /// what a re-simulated tick is compared against.
    [[nodiscard]] const RootDelta& root_motion() const noexcept { return root_; }
    [[nodiscard]] Vec3 travelled() const noexcept { return travelled_; }
    void clear_root_motion() noexcept;
    /// Record what `advance()` integrated. Public because `advance()` is a free function and a
    /// friend declaration for one caller is a wider hole than a named method.
    void accept_root_motion(const RootDelta& delta) noexcept;

    [[nodiscard]] u32 identifier() const noexcept { return identifier_; }
    void set_identifier(u32 value) noexcept { identifier_ = value; }

private:
    graph::pose::PoseInstance machine_;
    Array<f32> parameters_;
    Array<ClipCursor> cursors_;
    Array<f32> previous_;
    RootDelta root_;
    Vec3 travelled_{0.0F, 0.0F, 0.0F};
    f32 play_rate_ = 1.0F;
    u32 identifier_ = 0;
    LodTier tier_ = LodTier::Full;
    EventPolicy events_ = EventPolicy::Emit;
};

/// The pose buffers one evaluation needs, owned by the caller and reused across a batch.
///
/// "per-instance pose buffers allocated from the frame arena" — this is the shape that allows it:
/// the caller decides where the memory comes from and how long it lives, and evaluation allocates
/// nothing.
class PoseScratch {
public:
    explicit PoseScratch(Allocator& allocator) noexcept;

    [[nodiscard]] Status prepare(const AnimationRig& rig) noexcept;
    [[nodiscard]] Span<Transform> slot(PoseValue value) noexcept;
    [[nodiscard]] u32 joints() const noexcept { return joints_; }
    [[nodiscard]] bool ready() const noexcept { return joints_ != 0; }

private:
    Array<Transform> storage_;
    u32 joints_ = 0;
    u32 slots_ = 0;
};

/// What one evaluation did. Every counter is a measurement a test asserts on.
struct EvaluationStats {
    u32 instructions = 0;
    u32 instructions_skipped = 0;
    u32 clips_sampled = 0;
    /// The joints the sampler was ASKED for, summed over every clip sampled. This is what pose
    /// dependency analysis reduces: a masked layer asks for its mask, and a reduced bone level of
    /// detail asks for fewer still.
    u32 joints_sampled = 0;
    /// The joints a track actually wrote. Smaller than `joints_sampled` for any clip that does not
    /// key every joint, which is every clip.
    u32 joints_written = 0;
    u32 tracks_skipped = 0;
    u32 ik_solved = 0;
};

/// Advance the state machine, the clip times, root motion and events by `dt`.
///
/// THE DETERMINISTIC HALF. It samples no pose and touches no joint; it reads the clips' root tracks
/// and their event tracks. It runs at every level of detail tier, including `Baked`.
[[nodiscard]] Status advance(const AnimationRig& rig, AnimationInstance& instance, f32 dt,
                             EventBuffer* events) noexcept;

/// Evaluate the instance's pose into `out_local`, in the skeleton's local space.
///
/// `bone_lod` is the bone level of detail: only joints retained at that level are sampled, blended
/// or written, which is `Skeleton::retained()` intersected with each instruction's own required
/// mask. `out_local` must be seeded — `Skeleton::reference_pose()` is the usual seed — because a
/// joint no track drives keeps what it was handed.
[[nodiscard]] Status evaluate(const AnimationRig& rig, AnimationInstance& instance, u8 bone_lod,
                              PoseScratch& scratch, Span<Transform> out_local,
                              EvaluationStats& stats) noexcept;

/// One batch: the instances sharing a rig. "instances using the same compiled graph and skeleton
/// SHALL be evaluated together, so that iteration is over packed per-instance state rather than
/// scattered objects."
class AnimationBatch {
public:
    AnimationBatch(Allocator& allocator, const AnimationRig& rig) noexcept;

    [[nodiscard]] Expected<u32, Error> add() noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(instances_.size()); }
    [[nodiscard]] AnimationInstance& instance(u32 slot) noexcept { return instances_[slot]; }
    [[nodiscard]] const AnimationRig& rig() const noexcept { return *rig_; }

    /// Advance every instance. One pass over packed state.
    [[nodiscard]] Status advance_all(f32 dt, EventBuffer* events) noexcept;

    /// Evaluate the half-open range `[first, first + count)` into `poses`, which is
    /// `count * joint_count()` transforms. THE RANGE IS THE UNIT OF WORK a job takes: the batch is
    /// the unit of parallelism and a worker takes a slice of it.
    [[nodiscard]] Status evaluate_range(u32 first, u32 count, u8 bone_lod, PoseScratch& scratch,
                                        Span<Transform> poses, EvaluationStats& stats) noexcept;

private:
    const AnimationRig* rig_ = nullptr;
    Allocator* allocator_ = nullptr;
    Array<AnimationInstance> instances_;
};

}  // namespace cy::animation
