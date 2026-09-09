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
    DebugMap debug_;
    u16 entry_state_ = 0;
    u32 joints_ = 0;
    u64 digest_ = 0;
};

/// One character's animation state. Small, flat and copyable: this is what an entity carries.
struct PoseInstance {
    u16 state = 0;
    /// The state being blended towards, or `0xFFFF`.
    u16 target = 0xFFFFU;
    /// The transition in flight, or `0xFFFF`.
    u16 transition = 0xFFFFU;
    f32 blend_elapsed = 0.0F;
    f32 state_time = 0.0F;
};

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

// --- Compilation ------------------------------------------------------------------------------

/// Compile an authored animation graph. Cook time; nothing here runs in a frame.
[[nodiscard]] Expected<PoseProgram, Error> compile_pose(const Graph& graph,
                                                        const NodeRegistry& registry,
                                                        u32 joint_count,
                                                        DiagnosticSink& sink) noexcept;

/// Register the animation node types.
[[nodiscard]] Status register_pose_nodes(NodeRegistry& registry) noexcept;

}  // namespace cy::graph::pose
