#pragma once
// The camera rig program: a transfer-function DAG on the shared expression core, with declared
// roots and one declared phase boundary. M8.b task 2.4.
//
// ================================================================================================
// THE ONE CONSUMER IN THIS MODULE THAT DOES LOWER THROUGH expr.h
// ================================================================================================
//
// `camera-system` asks for "a graph of rig nodes — target, position, orientation, constraint,
// collision, lens, noise, blend, and output — composed rather than inherited", "compiled at cook
// time into a compact rig program" with "no per-node allocation or virtual dispatch". Every rig
// node is a pure function of its inputs, which is exactly what a hash-consed expression DAG is good
// at, and the M8.b spike encoded a real camera smoothing rig in the material IR unchanged (probe
// P12, digest 7dd08230a50f73ce).
//
// What it needs beyond that is E1, E3 and E4 of design.md §1.4, and this file is what those three
// extensions were specified for:
//
//   * E1, AN OPEN TYPE LATTICE — a quaternion and a transform are not in a material's seven types.
//   * E3, DECLARED ROOTS — a camera's roots are a pose, a lens, and THE SMOOTHING STATE ON ITS WAY
//     OUT. "Smoothing state SHALL be part of the rig instance" and resettable on a cut, so it is
//     carried IN as an input and OUT as a root. Two fixed roots called surface and opacity cannot
//     express that.
//   * E4, A DECLARED PHASE BOUNDARY — "collision and occlusion queries SHALL be batched through the
//     physics interface" rather than "scattered synchronous casts from individual rig nodes". A
//     `collide` node ends one dispatch and begins the next; everything above it belongs to the
//     second, and `CameraRigProgram::phase_count()` says how many there are.
//
// ================================================================================================
// SMOOTHING IS IN PHYSICALLY MEANINGFUL TERMS, AND THAT IS A REQUIREMENT
// ================================================================================================
//
// `camera-system`: smoothing must be "frame-rate independent in physically meaningful terms —
// half-life, or frequency and damping ratio". A rig authored with a per-frame lerp factor behaves
// differently at 30 and 144 frames a second, and a designer cannot tell from the number which one
// they tuned. `camera.smooth_half_life` lowers to `lerp(previous, desired, 1 - 0.5^(dt / half))`,
// which is the transfer function the spike measured.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/expr.h>
#include <cy/graph/passes.h>

namespace cy::graph::camera {

/// The rig domain's type lattice (E1). Identity is TEXT here, not pinned: this is a new domain and
/// has no cooked data to stay compatible with.
enum RigType : TypeId { Scalar = 0, Vector, Quaternion, Transform, Lens, RigTypeCount };

/// The rig domain's operation table (E2).
enum RigOp : OpId {
    Constant = 0,
    /// A rig parameter, by name: a distance, a damping ratio, a field of view.
    Parameter,
    /// A per-frame input the host supplies: the target's position, the previous pose, the delta
    /// time, and the SMOOTHING STATE ON ITS WAY IN.
    Input,
    Add,
    Sub,
    Mul,
    Scale,
    Lerp,
    /// Frame-rate independent smoothing by half-life: `(previous, desired, half_life, dt)`.
    SmoothHalfLife,
    /// The same by frequency and damping ratio: `(previous, desired, frequency, dt)`.
    SmoothDamped,
    /// An additive impulse from the shake bus.
    Shake,
    /// THE PHASE BOUNDARY (E4). `(origin, target)`; its value is where the batched query landed.
    Collide,
    /// A lens: `(focal_length)`.
    LensOf,
    /// Compose a pose from a position and an orientation.
    Compose,
    RigOpCount,
};

/// The declared roots (E3).
inline constexpr u32 kPoseRoot = 0;
inline constexpr u32 kLensRoot = 1;
inline constexpr u32 kStateRoot = 2;

[[nodiscard]] const Domain& rig_domain() noexcept;

/// One step of the compiled program: an operation over slots, with no pointer to follow and no
/// virtual call to make. `camera-system`: "no per-node allocation or virtual dispatch".
struct RigStep {
    OpId op = Constant;
    u16 a = 0xFFFFU;
    u16 b = 0xFFFFU;
    u16 c = 0xFFFFU;
    u16 dst = 0;
    /// Which dispatch this step belongs to (E4).
    u8 phase = 0;
    Immediate value;
    /// A parameter's or input's name.
    Name symbol;
    NodeKey origin = kInvalidNodeKey;
};

/// A query the rig asked for, to be answered in one batch between the phases.
struct RigQuery {
    f32 origin[3] = {};
    f32 target[3] = {};
    /// The step that asked, so the answer can be written back into its slot.
    u16 step = 0;
};

/// What the host answers a batch of queries with: the point the cast reached.
struct RigQueryResult {
    f32 hit[3] = {};
};

/// The batched query interface. ONE CALL PER RIG EVALUATION, whatever the rig's node count.
class RigQueryBatch {
public:
    RigQueryBatch() = default;
    virtual ~RigQueryBatch() = default;
    RigQueryBatch(const RigQueryBatch&) = delete;
    RigQueryBatch& operator=(const RigQueryBatch&) = delete;
    RigQueryBatch(RigQueryBatch&&) = delete;
    RigQueryBatch& operator=(RigQueryBatch&&) = delete;

    virtual void resolve(Span<const RigQuery> queries, Span<RigQueryResult> results) = 0;
};

/// A compiled rig. IMMUTABLE AND SHARED by every camera using this rig.
class CameraRigProgram {
public:
    explicit CameraRigProgram(Allocator& allocator) noexcept;

    CameraRigProgram(const CameraRigProgram&) = delete;
    CameraRigProgram& operator=(const CameraRigProgram&) = delete;
    CameraRigProgram(CameraRigProgram&&) noexcept = default;
    CameraRigProgram& operator=(CameraRigProgram&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const RigStep> steps() const noexcept { return steps_.span(); }
    [[nodiscard]] u32 slot_count() const noexcept { return slots_; }
    [[nodiscard]] u32 phase_count() const noexcept { return phases_; }
    [[nodiscard]] u16 pose_slot() const noexcept { return pose_slot_; }
    [[nodiscard]] u16 lens_slot() const noexcept { return lens_slot_; }
    [[nodiscard]] u16 state_slot() const noexcept { return state_slot_; }
    [[nodiscard]] u64 digest() const noexcept { return digest_; }
    /// The IR digest the shared core produced. Recorded so a rig's cook key is the core's answer
    /// rather than a second hash of the same thing.
    [[nodiscard]] u64 ir_digest() const noexcept { return ir_digest_; }
    [[nodiscard]] const DebugMap& debug() const noexcept { return debug_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return steps_.allocator(); }

private:
    friend class RigProgramAccess;

    Name name_;
    Array<RigStep> steps_;
    DebugMap debug_;
    u32 slots_ = 0;
    u32 phases_ = 1;
    u16 pose_slot_ = 0xFFFFU;
    u16 lens_slot_ = 0xFFFFU;
    u16 state_slot_ = 0xFFFFU;
    u64 digest_ = 0;
    u64 ir_digest_ = 0;
};

/// One camera's state. `camera-system`: "smoothing state [...] part of the rig instance", and
/// resettable on a cut — which is what `reset()` is for and why it is not the program's business.
struct RigInstance {
    f32 state[4] = {};
    bool primed = false;

    /// A CUT. The next evaluation starts from the desired value rather than smoothing towards it
    /// from wherever the camera was before the cut.
    void reset() noexcept {
        for (f32& channel : state) {
            channel = 0.0F;
        }
        primed = false;
    }
};

struct RigInputs {
    /// Named per-frame inputs the host supplies, in the order `CameraRigProgram` declared them.
    Span<const Name> input_names;
    Span<const f32> input_values;
    Span<const Name> parameter_names;
    Span<const f32> parameter_values;
    f32 dt = 1.0F / 60.0F;
};

struct RigOutput {
    f32 position[3] = {};
    f32 focal_length = 0.0F;
    f32 state[4] = {};
};

/// Evaluate one rig. The batch is resolved EXACTLY ONCE, between the phases, whatever the rig's
/// node count — which is the whole content of "queries SHALL be batched".
[[nodiscard]] Status evaluate_rig(const CameraRigProgram& program, const RigInputs& inputs,
                                  RigInstance& instance, RigQueryBatch& batch,
                                  RigOutput& out) noexcept;

/// Compile an authored rig graph through the shared expression core.
[[nodiscard]] Expected<CameraRigProgram, Error> compile_rig(const Graph& graph,
                                                            const NodeRegistry& registry,
                                                            DiagnosticSink& sink) noexcept;

[[nodiscard]] Status register_camera_nodes(NodeRegistry& registry) noexcept;

}  // namespace cy::graph::camera
