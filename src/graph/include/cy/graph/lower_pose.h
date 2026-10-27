#pragma once
// The animation IR: poses as values, a lazily-evaluated state machine, and pose dependency
// analysis. M8.b task 2.4.
//
// ================================================================================================
// WHY ANIMATION DOES NOT LOWER THROUGH THE SHARED EXPRESSION CORE
// ================================================================================================
//
// `animation-and-skinning` asks for "graph -> typed IR -> optimisation -> compact program", which
// sounds exactly like expr.h until three of its requirements are read together:
//
//   * VALUES ARE POSES. "sample active clips into poses, blend poses per the compiled animation
//     program into a final pose". A pose is an array of joint transforms, not a number.
//   * A `StateMachine` NODE — "states with transitions, conditions, durations, and interruption
//     rules" — is a back edge, and expr.h has none by construction (design.md §1.3, probe P3).
//   * POSE DEPENDENCY ANALYSIS, whose scenario is explicit: "lower-body joints of that layer's
//     clips are never read, and they SHALL NOT be sampled". That is a LAZY branch, and the shared
//     core's conditional is a value whose arms both evaluate (probe P10).
//
// The third is the one that settles it. An expression DAG that evaluates both arms of a select
// samples both clips, and this specification says in so many words that it must not.
//
// ================================================================================================
// WHAT MAKES THE PROGRAM COMPACT, AND WHAT MAKES IT LAZY
// ================================================================================================
//
// COMPACT: one flat instruction array shared by every character, one `PoseInstance` per character
// holding the state machine's position and its blend clocks, and no virtual dispatch — the same
// shape `visual-scripting` permits and `animation-and-skinning` requires ("the runtime SHALL
// contain no graph compiler").
//
// LAZY: evaluation begins at the ACTIVE state's root instruction and walks only what that root
// reaches. A state nobody is in costs nothing, and a masked-out joint is never sampled, because
// `PoseInstruction::required` says which joints the consumer of that value will read and the
// sampler is handed the mask.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>

namespace cy::graph::pose {

/// The joints a pose value carries.
///
/// A FIXED 256-BIT MASK. `animation-and-skinning`'s bone LOD and layer masks are per-skeleton and a
/// skeleton is authored, so this is a real cap rather than an implementation detail: a rig with
/// more than 256 joints needs a mask that allocates, and that decision belongs to whoever brings
/// one rather than being anticipated here.
inline constexpr u32 kMaxJoints = 256;

class JointMask {
public:
    [[nodiscard]] static JointMask all(u32 joints) noexcept;
    [[nodiscard]] static JointMask range(u32 first, u32 count) noexcept;

    void set(u32 joint) noexcept;
    [[nodiscard]] bool test(u32 joint) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] u32 count() const noexcept;
    [[nodiscard]] JointMask merged(const JointMask& other) const noexcept;
    [[nodiscard]] JointMask intersected(const JointMask& other) const noexcept;
    [[nodiscard]] JointMask without(const JointMask& other) const noexcept;
    [[nodiscard]] u64 word(u32 index) const noexcept { return words_[index]; }

    friend bool operator==(const JointMask& a, const JointMask& b) noexcept;

private:
    u64 words_[kMaxJoints / 64] = {};
};

enum class PoseOp : u16 {
    /// The reference pose. A leaf that samples nothing.
    RefPose = 0,
    /// `clip` at the time in parameter `time_param`. THE ONLY OP THAT SAMPLES.
    SampleClip,
    /// `(a, b, weight_param)`.
    Blend,
    /// `(a, b)` where `mask` says which joints come from `b`.
    BlendMask,
    /// `(base, additive, weight_param)`.
    Additive,
    /// `(base, layer)` under `mask` at `weight_param`.
    Layer,
    /// Two-bone inverse kinematics over the chain `chain`.
    IK,
    /// A BLEND SPACE: the poses of `blend_spaces()[clip]`'s samples, weighted by where the
    /// space's parameters put the character among them. One dimension or two. Appended, so the
    /// operations above keep the numbers a cooked program stores.
    BlendSpace,
    /// Foot placement over the pose `a`: `foot_placements()[chain]`, faded by `weight_param`.
    FootPlacement,
    /// An aim chain over the pose `a`: `aims()[chain]`, faded by `weight_param`.
    Aim,
    Count,
};

[[nodiscard]] const char* pose_op_name(PoseOp op) noexcept;

using PoseValue = u16;
inline constexpr PoseValue kNoPoseValue = 0xFFFFU;

struct PoseInstruction {
    PoseOp op = PoseOp::RefPose;
    PoseValue a = kNoPoseValue;
    PoseValue b = kNoPoseValue;
    u16 clip = 0;
    u16 mask = 0;
    u16 time_param = 0;
    u16 weight_param = 0;
    u16 chain = 0;
    /// POSE DEPENDENCY ANALYSIS' answer: the joints anything downstream will actually read. Filled
    /// by `analyse_dependencies` and handed to the sampler, which is how "they SHALL NOT be
    /// sampled" becomes a property of the program rather than an intention.
    JointMask required;
    NodeKey origin = kInvalidNodeKey;
};

/// How a transition may be interrupted. `animation-and-skinning` requires interruption rules to be
/// authored rather than implied.
enum class Interruption : u8 {
    /// The transition runs to completion.
    None = 0,
    /// A higher-priority transition may take over mid-blend.
    HigherPriority,
    /// Any transition may take over.
    Any,
};

struct Transition {
    u16 target_state = 0;
    /// The parameter that must be non-zero.
    u16 condition_param = 0;
    /// In seconds. Zero is a cut.
    f32 duration = 0.0F;
    u16 priority = 0;
    Interruption interruption = Interruption::None;
    NodeKey origin = kInvalidNodeKey;
};

struct PoseState {
    Name name;
    /// The instruction this state's pose tree is rooted at.
    PoseValue root = kNoPoseValue;
    u32 first_transition = 0;
    u32 transition_count = 0;
    /// The sync group this state belongs to, or `0xFFFF`.
    u16 sync_group = 0xFFFFU;
    NodeKey origin = kInvalidNodeKey;
};

/// `animation-and-skinning`: sync groups align by MARKER CORRESPONDENCE rather than by normalised
/// time, "because two clips of different length at the same normalised time are not at the same
/// point in the stride".
struct SyncMarker {
    Name name;
    f32 time = 0.0F;
};

struct SyncGroup {
    Name name;
    u32 first_marker = 0;
    u32 marker_count = 0;
};

struct ClipRef {
    Name name;
    f32 duration = 1.0F;
    bool looping = true;
    u32 first_marker = 0;
    u32 marker_count = 0;
};

// --- Blend spaces ------------------------------------------------------------------------------
//
// `animation-and-skinning`'s node table: "Blend1D / Blend2D | Blend spaces driven by parameters
// (speed, direction)". A blend space is a set of SAMPLES — a pose each, at a position in a one- or
// two-dimensional parameter space — and the weights of a point are the linear interpolation
// between the two samples around it (1D) or the barycentric coordinates of the triangle it lies in
// (2D). The triangulation is the compiler's: a Delaunay triangulation of the sample positions,
// computed once at cook time and stored, so the runtime finds a triangle and never builds one. A
// point outside the samples is clamped to the nearest point of the space — the hull's edge — rather
// than extrapolated, which would hand a pose a negative weight.
//
// SYNC. A synchronised space plays its clip samples on ONE clock: a phase shared by every sample,
// expressed in MARKER SPACE — the integer part is which stride segment (from one sync marker to the
// next) the character is in and the fraction is how far through it. Each clip maps that phase onto
// its own timeline segment by segment, so a walk and a run of different lengths put a foot down at
// the same moment however they are weighted. The phase advances through a segment at the weighted
// mean of the samples' segment lengths, which is what makes the blended stride's cadence the blend
// of the cadences. Clips whose marker sequences do not correspond fall back to normalised time, one
// segment the length of the clip. The phase is a program parameter (`phase_param`), so it is
// per-instance state like any clock.

/// The most samples one blend space holds: a 3x5 locomotion grid with room to spare.
inline constexpr u32 kMaxBlendSamples = 16;

/// One sample of a blend space: a pose, and where in the space it is the whole pose.
struct BlendSample {
    /// The position in the space. `y` is zero in one dimension.
    f32 x = 0.0F;
    f32 y = 0.0F;
    /// The pose at that position.
    PoseValue value = kNoPoseValue;
};

/// One triangle of a two-dimensional space's cook-time triangulation, as sample indices.
struct BlendTriangle {
    u16 sample[3] = {0, 0, 0};
};

/// A blend space as the program carries it: its parameters, its samples and its triangles.
struct BlendSpace {
    /// One or two.
    u8 dimensions = 1;
    /// The samples' clips share one marker-space clock.
    bool sync = false;
    u16 x_param = 0;
    /// Read only in two dimensions.
    u16 y_param = 0;
    /// The shared phase of a synchronised space; unused otherwise.
    u16 phase_param = 0;
    /// In ascending `x` in one dimension.
    u32 first_sample = 0;
    u32 sample_count = 0;
    /// Two dimensions only.
    u32 first_triangle = 0;
    u32 triangle_count = 0;
    NodeKey origin = kInvalidNodeKey;
};

// --- Foot placement and aim --------------------------------------------------------------------
//
// The program names joints by NAME here, not by index, because a compiled program is shared by
// every skeleton it is bound to and joint numbering is the skeleton's. `AnimationRig::bind`
// resolves the names once; the runtime never compares a string in a frame.

/// A `pose.foot_ik` node as the program carries it: the legs by joint name and the limits.
struct FootPlacementRef {
    /// The joint the hips are lowered through.
    Name pelvis;
    /// Per leg: hip, knee, ankle. Two legs.
    Name leg[2][3];
    /// The ankle joint's height above the sole in the bind pose, in metres.
    f32 ankle_height = 0.08F;
    /// How far above the animated ankle the ground is searched from, and how far below.
    f32 probe_above = 0.5F;
    f32 probe_below = 0.6F;
    /// The most the hips may drop so the lower foot reaches, and the most a foot may be raised.
    f32 max_pelvis_drop = 0.35F;
    f32 max_foot_raise = 0.45F;
    /// The time constant the offsets and the foot tilt follow the ground with, in seconds. Zero
    /// snaps.
    f32 smoothing = 0.08F;
    /// The steepest ground a foot is turned to, in radians.
    f32 max_tilt = 0.6F;
    /// The direction the knees bend towards, in model space.
    f32 pole[3] = {0.0F, 0.0F, 1.0F};
    NodeKey origin = kInvalidNodeKey;
};

/// The most joints one aim chain distributes its rotation over.
inline constexpr u32 kMaxAimJoints = 4;

/// A `pose.aim` node as the program carries it: the chain by joint name, the axis, the target.
struct AimRef {
    /// Root of the chain first; the last joint is the one that points.
    Name joints[kMaxAimJoints];
    u8 joint_count = 0;
    /// The pointing joint's forward axis, in its own space.
    f32 axis[3] = {0.0F, 0.0F, 1.0F};
    /// The most the chain turns, in radians.
    f32 max_angle = 1.4F;
    /// The parameters carrying the target point in model space.
    u16 target_param[3] = {0, 0, 0};
    NodeKey origin = kInvalidNodeKey;
};

/// A compiled animation program. IMMUTABLE AND SHARED; there is no graph in it.
class PoseProgram {
public:
    explicit PoseProgram(Allocator& allocator) noexcept;

    PoseProgram(const PoseProgram&) = delete;
    PoseProgram& operator=(const PoseProgram&) = delete;
    PoseProgram(PoseProgram&&) noexcept = default;
    PoseProgram& operator=(PoseProgram&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const PoseInstruction> code() const noexcept { return code_.span(); }
    [[nodiscard]] Span<const PoseState> states() const noexcept { return states_.span(); }
    [[nodiscard]] Span<const Transition> transitions() const noexcept {
        return transitions_.span();
    }
    [[nodiscard]] Span<const ClipRef> clips() const noexcept { return clips_.span(); }
    [[nodiscard]] Span<const JointMask> masks() const noexcept { return masks_.span(); }
    [[nodiscard]] Span<const Name> parameters() const noexcept { return parameters_.span(); }
    [[nodiscard]] Span<const SyncGroup> sync_groups() const noexcept { return sync_.span(); }
    [[nodiscard]] Span<const SyncMarker> markers() const noexcept { return markers_.span(); }
    [[nodiscard]] Span<const BlendSpace> blend_spaces() const noexcept {
        return blend_spaces_.span();
    }
    [[nodiscard]] Span<const BlendSample> blend_samples() const noexcept {
        return blend_samples_.span();
    }
    [[nodiscard]] Span<const BlendTriangle> blend_triangles() const noexcept {
        return blend_triangles_.span();
    }
    [[nodiscard]] Span<const FootPlacementRef> foot_placements() const noexcept {
        return foot_placements_.span();
    }
    [[nodiscard]] Span<const AimRef> aims() const noexcept { return aims_.span(); }
    [[nodiscard]] u16 entry_state() const noexcept { return entry_state_; }
    [[nodiscard]] u32 joint_count() const noexcept { return joints_; }
    [[nodiscard]] u64 digest() const noexcept { return digest_; }
    [[nodiscard]] const DebugMap& debug() const noexcept { return debug_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return code_.allocator(); }

private:
    friend class PoseProgramAccess;

    Name name_;
    Array<PoseInstruction> code_;
    Array<PoseState> states_;
    Array<Transition> transitions_;
    Array<ClipRef> clips_;
    Array<JointMask> masks_;
    Array<Name> parameters_;
    Array<SyncGroup> sync_;
    Array<SyncMarker> markers_;
    Array<BlendSpace> blend_spaces_;
    Array<BlendSample> blend_samples_;
    Array<BlendTriangle> blend_triangles_;
    Array<FootPlacementRef> foot_placements_;
    Array<AimRef> aims_;
    DebugMap debug_;
    u16 entry_state_ = 0;
    u32 joints_ = 0;
    u64 digest_ = 0;
};

/// The weight of each of `space`'s samples at the point its parameters name, into `out` (one entry
/// per sample, in sample order). The weights are non-negative and sum to one; at most two are
/// non-zero in one dimension and at most three in two. Deterministic: a point on an edge two
/// triangles share is weighted by the first of them, and the same parameters always give the same
/// weights. False when `out` is smaller than the space or the space is out of range.
[[nodiscard]] bool blend_space_weights(const PoseProgram& program, u32 space,
                                       Span<const f32> parameters, Span<f32> out) noexcept;

/// The weights of the point (`x`, `y`) over explicit samples and triangles: what
/// `blend_space_weights` runs once it has read the parameters, exposed so a tool can draw a space
/// it has not compiled.
[[nodiscard]] bool blend_weights_at(u8 dimensions, Span<const BlendSample> samples,
                                    Span<const BlendTriangle> triangles, f32 x, f32 y,
                                    Span<f32> out) noexcept;

/// `PoseInstance::transition` while a blend a HOST requested is in flight — `request_state` —
/// rather than one of the program's own transitions. Its duration is `requested_duration`.
inline constexpr u16 kRequestedTransition = 0xFFFEU;

/// One character's animation state. Small, flat and copyable: this is what an entity carries.
struct PoseInstance {
    u16 state = 0;
    /// The state being blended towards, or `0xFFFF`.
    u16 target = 0xFFFFU;
    /// The transition in flight, `kRequestedTransition`, or `0xFFFF`.
    u16 transition = 0xFFFFU;
    f32 blend_elapsed = 0.0F;
    f32 state_time = 0.0F;
    /// The length of a requested blend, in seconds. Read only while `transition` is
    /// `kRequestedTransition`. Appended, so an instance brace-initialised before it keeps its
    /// meaning.
    f32 requested_duration = 0.0F;
};

/// The length of the blend `instance` is in, in seconds: the program transition's, or a requested
/// one's. Zero when no blend is in flight.
[[nodiscard]] f32 transition_duration(const PoseProgram& program,
                                      const PoseInstance& instance) noexcept;

/// The state named `name`, or `0xFFFF` when the program has none.
[[nodiscard]] u16 find_state(const PoseProgram& program, Name name) noexcept;

/// A HOST's request: blend from the current state to `state` over `seconds`, whatever the
/// program's own transitions say — the "play" and "crossfade" a gameplay script asks for.
///
/// Zero seconds is a cut, as a zero-duration transition is. A request for the state the instance
/// is already in, with no blend in flight, changes nothing; a request for the state a blend is
/// already heading to changes nothing either. A request made while a blend is in flight starts
/// the new blend from the blend's SOURCE state, so the pose may step by what the abandoned blend
/// had reached. A requested blend runs to completion: the program's transitions are considered
/// again from the state it lands in. False when `state` is not a state of the program.
[[nodiscard]] bool request_state(const PoseProgram& program, PoseInstance& instance, u16 state,
                                 f32 seconds) noexcept;

/// What the runtime asks the host for. The sampler is handed the JOINT MASK, which is the whole
/// mechanism behind "they SHALL NOT be sampled".
class PoseSampler {
public:
    PoseSampler() = default;
    virtual ~PoseSampler() = default;
    PoseSampler(const PoseSampler&) = delete;
    PoseSampler& operator=(const PoseSampler&) = delete;
    PoseSampler(PoseSampler&&) = delete;
    PoseSampler& operator=(PoseSampler&&) = delete;

    /// Sample `clip` at `time` into `out`, writing ONLY the joints in `mask`.
    virtual void sample(const ClipRef& clip, f32 time, const JointMask& mask, Span<f32> out) = 0;
    /// The reference pose, for the joints in `mask`.
    virtual void reference(const JointMask& mask, Span<f32> out) = 0;
    /// Solve the inverse-kinematics chain in place.
    virtual void solve_ik(u16 chain, const JointMask& mask, Span<f32> pose) = 0;
};

/// What one evaluation did. The counters are the criterion: a test asserts that an unselected
/// layer's clip contributes zero samples.
struct EvaluationReport {
    u32 instructions_evaluated = 0;
    u32 clips_sampled = 0;
    u32 joints_sampled = 0;
};

/// Advance the state machine by `dt`, evaluating transitions FROM THE CURRENT STATE ONLY.
///
/// `animation-and-skinning`'s state machine is not re-entered from a root each frame; a state
/// nobody is in has no transitions evaluated and no clips sampled.
void advance(const PoseProgram& program, PoseInstance& instance, Span<const f32> parameters,
             f32 dt) noexcept;

/// Evaluate the active pose into `out`, which is `joint_count() * kChannelsPerJoint` floats.
///
/// LAZY BY CONSTRUCTION: only the active state's tree — and, during a blend, the target's — is
/// walked. Nothing else in the program is touched.
inline constexpr u32 kChannelsPerJoint = 8;

[[nodiscard]] Status evaluate(const PoseProgram& program, const PoseInstance& instance,
                              Span<const f32> parameters, PoseSampler& sampler, Span<f32> out,
                              EvaluationReport& report) noexcept;

// --- Loading a cooked program -----------------------------------------------------------------

/// Everything a compiled program holds, as a loader reads it back out of a cooked asset. Spans,
/// because the loader owns the decoded arrays only until `assemble_pose_program` copies them.
struct PoseProgramParts {
    Name name;
    Span<const PoseInstruction> code;
    Span<const PoseState> states;
    Span<const Transition> transitions;
    Span<const ClipRef> clips;
    Span<const JointMask> masks;
    Span<const Name> parameters;
    Span<const SyncGroup> sync_groups;
    Span<const SyncMarker> markers;
    u16 entry_state = 0;
    u32 joint_count = 0;
    u64 digest = 0;
    /// Appended: a program cooked before blend spaces and modifiers existed leaves them empty.
    Span<const BlendSpace> blend_spaces;
    Span<const BlendSample> blend_samples;
    Span<const BlendTriangle> blend_triangles;
    Span<const FootPlacementRef> foot_placements;
    Span<const AimRef> aims;
};

/// Rebuild a program a cook compiled. RUNTIME, AND NOT A COMPILER: it lowers nothing and analyses
/// nothing. It checks that every index the parts hold is in range — every operand, clip, mask,
/// parameter, state, transition and the entry state — so a corrupt asset is refused rather than
/// read out of bounds a frame later, and then copies them, trusting the dependency masks and the
/// digest the compiler wrote. The debug map, which names authored nodes, is not part of a cooked
/// program and stays empty.
///
/// Defined in pose_program.cpp beside `advance`, with nothing of the compiler: a runtime that loads
/// programs links no `compile_pose`.
[[nodiscard]] Expected<PoseProgram, Error> assemble_pose_program(
    Allocator& allocator, const PoseProgramParts& parts) noexcept;

// --- Compilation ------------------------------------------------------------------------------

/// Compile an authored animation graph. Cook time; nothing here runs in a frame.
[[nodiscard]] Expected<PoseProgram, Error> compile_pose(const Graph& graph,
                                                        const NodeRegistry& registry,
                                                        u32 joint_count,
                                                        DiagnosticSink& sink) noexcept;

/// Register the animation node types.
[[nodiscard]] Status register_pose_nodes(NodeRegistry& registry) noexcept;

}  // namespace cy::graph::pose
