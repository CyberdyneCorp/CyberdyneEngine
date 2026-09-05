#pragma once
// Rig graphs, the program they compile to, and the state one instance keeps. Tasks 4.3.1 and 4.3.2.
//
// `camera-system` — "Rig graphs compile to programs": a camera definition "SHALL be a **graph of
// rig nodes** — target, position, orientation, constraint, collision, lens, noise, blend, and
// output — composed rather than inherited"; graphs "SHALL be **compiled** at cook time into a
// compact rig program, following the same pattern as material, VFX, animation, and behaviour
// graphs"; runtime evaluation "SHALL execute the compiled program", and "Per-node heap-allocated
// virtual objects SHALL NOT be used in the evaluation path".
//
// ================================================================================================
// COMPOSITION, NOT INHERITANCE — AND WHAT THAT COSTS THE OBVIOUS DESIGN
// ================================================================================================
//
// The obvious design is `class ThirdPersonCamera : public Camera`. `camera-system` forbids it twice
// — once under "A camera is not a scene object" ("There SHALL NOT be a camera base class intended
// for subclassing") and again under "Forbidden camera patterns". So there is no base class here and
// no virtual function anywhere in this module's evaluation path. A third-person camera is a
// *composition*: follow, orbit, shoulder offset, collision, aim and noise nodes, in that order.
//
// The price is that a node cannot hold state of its own shape. The state a rig keeps is `RigState`,
// below — one struct for the whole rig, because the smoothers and the orbit angles are the only
// things that persist and putting them in one place is what lets a cut reset all of them with one
// call. `camera-system` requires exactly that: "Smoothing state SHALL be part of the rig instance,
// and SHALL be resettable on cuts and teleports."
//
// ================================================================================================
// WHAT "COMPILED" MEANS HERE, STATED PLAINLY SO NOBODY OVERSELLS IT
// ================================================================================================
//
// `RigDefinition` is the authored **graph**: nodes with identities, each naming the node it reads.
// `compile()` topologically orders it from the output backwards, rejects a cycle, rejects a node
// that reaches nothing, resolves each custom node's `Name` to an opcode, and emits `RigProgram` — a
// contiguous `Array<RigOp>` in evaluation order. Evaluation is a `switch` over that array writing
// into a `RigFrame` register file: no allocation, no virtual dispatch, no pointer chasing.
//
// What it does NOT yet do is fold constants or fuse nodes. That is honest rather than apologetic:
// the two optimisations need a graph editor to produce the patterns worth folding, which is M5's,
// and a `RigProgram` that is already the runtime form is what makes adding them a change to one
// function rather than to a representation.
//
// ================================================================================================
// COLLISION AND OCCLUSION DO NOT CAST RAYS FROM HERE
// ================================================================================================
//
// `camera-system`: queries "SHALL be **batched** through the physics interface, and SHALL NOT be
// issued as scattered synchronous casts from individual rig nodes". A `Collision` node therefore
// APPENDS a `CameraQuery` to an output array and consumes the answer the caller left in
// `RigEvaluationInput::query_results` from the previous evaluation. This is not a workaround for
// layer 2 being unable to reach a physics server — it is the requirement, and the layer number is
// what stops it being written the other way by accident.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/servers/camera/camera.h>
#include <cy/servers/camera/lens.h>
#include <cy/servers/camera/smoothing.h>

namespace cy::camera {

// --- Node kinds --------------------------------------------------------------------------------

/// The rig node kinds `camera-system` enumerates. `Custom` is the extension point
/// `project-and-plugins` requires: a project registers a `Name` and an evaluation function, and
/// gets an opcode.
enum class RigNodeKind : u8 {
    /// Resolves the target binding into the frame's anchor. A rig with no target node frames
    /// nothing, which is what a free-fly debug camera and a photo-mode camera are.
    Target = 0,
    /// Places the camera relative to the anchor, smoothed.
    Follow,
    /// Yaw, pitch and distance around the anchor, driven by look intent, with limits.
    Orbit,
    /// A fixed offset in camera space. The shoulder offset of a third-person camera.
    Offset,
    /// Orients the camera toward the anchor, smoothed.
    LookAt,
    /// Sets or scales the lens, optionally driven by the zoom parameter through a curve.
    Lens,
    /// Additive shake from the impulse bus.
    Noise,
    /// Clamps position: to a distance from the anchor, and to a world-space region.
    Constraint,
    /// Emits collision and occlusion queries and applies the previous evaluation's answers.
    Collision,
    /// The terminator. Exactly one, and it is the node every path must reach.
    Output,
    /// A project- or plugin-supplied node kind, dispatched through the registry.
    Custom,
    Count,
};

[[nodiscard]] const char* rig_node_kind_name(RigNodeKind kind) noexcept;

/// Which space a follow offset is expressed in.
enum class FollowSpace : u8 {
    /// The offset is a world-space vector: the camera keeps a compass bearing as the target turns.
    World = 0,
    /// The offset rotates with the target: the camera stays behind a car as it corners.
    Target,
    /// The offset rotates with the rig's own current orientation, which is what an orbit produces.
    Rig,
    Count,
};

// --- Node parameters ---------------------------------------------------------------------------
//
// One struct per kind, named fields, no shared scratch. `RigOp` below carries all of them rather
// than a union: an op is about two hundred bytes, a definition is a handful of ops, and definitions
// are shared by every rig instantiated from them — so the whole saving a union would buy is under a
// kilobyte per definition, against parameters that a reader can name. The evaluation path reads
// contiguous memory either way, which is the property the requirement is actually about.

struct TargetParams {
    /// Frame the bounds' centre rather than the transform's origin. What a group target wants.
    bool use_bounds_center = false;
    /// Added to the resolved anchor, in the target's space.
    Vec3 anchor_offset{0.0F, 0.0F, 0.0F};
    /// How hard the anchor itself is smoothed, in seconds. Smoothing the anchor rather than the
    /// camera is what produces a dead zone that does not also lag the orbit.
    f32 anchor_half_life = 0.0F;
};

struct FollowParams {
    Vec3 offset{0.0F, 2.0F, 6.0F};
    FollowSpace space = FollowSpace::Rig;
    f32 position_half_life = 0.12F;
    /// A dead zone in metres: target movement smaller than this produces no camera movement.
    /// `camera-system`'s "A dead zone avoids jitter" scenario.
    f32 dead_zone = 0.0F;
};

struct OrbitParams {
    /// Radians of yaw and pitch per unit of look intent. The intent has already been through
    /// sensitivity and inversion — see `CameraIntent::look`.
    f32 yaw_scale = 1.0F;
    f32 pitch_scale = 1.0F;
    f32 min_pitch_radians = -1.4F;
    f32 max_pitch_radians = 1.4F;
    /// Distance at zoom 0 and at zoom 1. One normalised parameter mapped through the rig, which is
    /// the strategy camera's "Zoom is one parameter" requirement generalised.
    f32 near_distance = 3.0F;
    f32 far_distance = 12.0F;
    /// Pitch at zoom 0 and at zoom 1, so zooming out tilts down coherently rather than through a
    /// second control.
    f32 near_pitch_radians = 0.0F;
    f32 far_pitch_radians = 0.0F;
    /// Applies the zoom curve to pitch as well as distance.
    bool zoom_drives_pitch = false;
    f32 distance_half_life = 0.1F;
    /// Yaw the camera back behind the target when the recentre intent is set.
    f32 recentre_half_life = 0.2F;
};

struct OffsetParams {
    /// In camera space: +x is right, +y is up, −z is forward.
    Vec3 offset{0.0F, 0.0F, 0.0F};
    /// Mirrors `offset.x` — a shoulder swap, which the collision response may request.
    bool mirrored = false;
};

struct LookAtParams {
    /// Look at the anchor plus this world-space offset, so a camera can aim above a character's
    /// feet without moving the anchor every other node reads.
    Vec3 aim_offset{0.0F, 0.0F, 0.0F};
    f32 rotation_half_life = 0.08F;
    /// Keep the horizon level: the roll of the produced orientation is discarded.
    bool level_horizon = true;
};

struct LensParams {
    LensKind kind = LensKind::Gameplay;
    render::ProjectionKind projection = render::ProjectionKind::Perspective;
    /// The lens at zoom 0 and at zoom 1, in the authored quantity of `kind`: radians for a gameplay
    /// lens, millimetres for a physical one. Equal values mean the zoom does not drive the lens.
    f32 near_value = 1.0471975512F;
    f32 far_value = 1.0471975512F;
    f32 near_plane = 0.1F;
    f32 far_plane = 0.0F;
    f32 ortho_height = 10.0F;
    f32 sensor_height_mm = 24.0F;
    /// Seconds. The lens is smoothed like everything else, so a zoom does not step.
    f32 half_life = 0.08F;
};

struct NoiseParams {
    /// Scales every impulse this node responds to. The player's shake setting is applied once, in
    /// `CameraSettings`, and never a second time here.
    f32 amplitude = 1.0F;
    /// Metres of positional shake and radians of rotational shake at unit strength.
    f32 position_scale = 0.05F;
    f32 rotation_scale = 0.01F;
    /// Only impulses carrying this tag are answered. An empty tag answers all of them.
    Name tag;
};

struct ConstraintParams {
    f32 min_distance = 0.0F;
    /// Zero means unbounded.
    f32 max_distance = 0.0F;
    /// A world-space region the camera position is clamped into. `region_extents` of zero on an
    /// axis leaves that axis unconstrained, so a side-scroller constrains one axis and a bounded
    /// map two, from one node.
    Vec3 region_center{0.0F, 0.0F, 0.0F};
    Vec3 region_extents{0.0F, 0.0F, 0.0F};
};

/// How the camera answers a collision or an occlusion. `camera-system` requires the responses to be
/// declared rather than implied, and requires collision and occlusion to be different questions
/// with different responses — "glass occludes without colliding".
enum class CollisionResponse : u8 {
    PullIn = 0,
    Slide,
    SwapShoulder,
    /// Fade the obstacle rather than move the camera. The camera does not move; the request is
    /// published for whoever owns the material.
    FadeObstacle,
    Ignore,
    Count,
};

[[nodiscard]] const char* collision_response_name(CollisionResponse response) noexcept;

struct CollisionParams {
    /// The camera's probe radius, so it stops before its near plane enters a wall.
    f32 probe_radius = 0.25F;
    CollisionResponse collision_response = CollisionResponse::PullIn;
    CollisionResponse occlusion_response = CollisionResponse::PullIn;
    /// How fast the camera returns after the obstruction clears, in seconds.
    f32 recovery_half_life = 0.25F;
    /// Sample this many points on the target, so a partly occluded target is distinguishable from a
    /// fully hidden one. One means the anchor only.
    u8 occlusion_samples = 1;
};

// --- The compiled op ----------------------------------------------------------------------------

/// One instruction of a compiled rig.
///
/// A plain struct — no virtual, no pointer, trivially copyable — so a `RigProgram` is a contiguous
/// block that can be cooked to disk unchanged and executed without a fix-up pass.
struct RigOp {
    RigNodeKind kind = RigNodeKind::Output;
    /// The authored node's identity, kept for the diagnostics: "why a rig node moved the camera"
    /// needs a name to answer with.
    Name id;
    /// The registry opcode for `Custom`, and zero otherwise.
    u16 custom_op = 0;

    TargetParams target;
    FollowParams follow;
    OrbitParams orbit;
    OffsetParams offset;
    LookAtParams look_at;
    LensParams lens;
    NoiseParams noise;
    ConstraintParams constraint;
    CollisionParams collision;
};

/// A compiled rig: ops in evaluation order.
struct RigProgram {
    Array<RigOp> ops;
    /// True when a `Target` node is present. Read by `compile()`'s validation and by the server,
    /// which refuses a target binding on a rig that has nowhere to put it.
    bool has_target = false;
    /// True when a `Collision` node is present, so a caller knows whether to expect queries.
    bool emits_queries = false;

    explicit RigProgram(Allocator& allocator) noexcept : ops(allocator) {}
};

// --- The authored graph -------------------------------------------------------------------------

/// One authored node. `id` names it; `input` names the node it reads.
///
/// An empty `input` means "the start of the rig" — the implicit source that supplies an identity
/// frame. Exactly one node may have an empty input, and exactly one node is the `Output`.
struct RigNodeDesc {
    Name id;
    Name input;
    RigNodeKind kind = RigNodeKind::Output;
    /// The registered name of a `Custom` node kind.
    Name custom_kind;

    TargetParams target;
    FollowParams follow;
    OrbitParams orbit;
    OffsetParams offset;
    LookAtParams look_at;
    LensParams lens;
    NoiseParams noise;
    ConstraintParams constraint;
    CollisionParams collision;
};

/// The authored camera definition: a graph, plus what a rig instantiated from it starts with.
///
/// `camera-system`'s first concept — "An authored asset describing a rig composition and its
/// parameters". It is data. Nothing inherits from it and nothing evaluates it directly; `compile()`
/// turns it into the `RigProgram` that is executed.
struct RigDefinition {
    Name name;
    Array<RigNodeDesc> nodes;
    /// Where a fresh rig's smoothers start when it has no target sample yet.
    Transform initial_pose;
    Lens initial_lens;

    explicit RigDefinition(Allocator& allocator) noexcept : nodes(allocator) {}
};

// --- Custom node kinds --------------------------------------------------------------------------

struct RigFrame;
struct RigEvaluationInput;

/// A project-supplied node's evaluation. A function pointer rather than an interface: an interface
/// would be a virtual call in the evaluation path, which the requirement forbids by name.
using RigCustomEvalFn = void (*)(const RigOp& op, const RigEvaluationInput& input, RigFrame& frame,
                                 void* user) noexcept;

/// The extension point `project-and-plugins` requires: "Projects and plugins SHALL be able to
/// register custom node kinds".
class RigNodeRegistry {
public:
    explicit RigNodeRegistry(Allocator& allocator) noexcept : entries_(allocator) {}

    /// Register a kind. Refuses a duplicate name — two evaluations under one name would make which
    /// one ran a function of registration order.
    [[nodiscard]] Status register_kind(Name name, RigCustomEvalFn eval,
                                       void* user = nullptr) noexcept;

    /// The opcode for `name`, or zero when it was never registered. Opcodes start at one so that
    /// zero is "not a custom node" in a zeroed `RigOp`.
    [[nodiscard]] u16 opcode(Name name) const noexcept;
    [[nodiscard]] Name name_of(u16 opcode) const noexcept;
    void evaluate(u16 opcode, const RigOp& op, const RigEvaluationInput& input,
                  RigFrame& frame) const noexcept;
    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        Name name;
        RigCustomEvalFn eval = nullptr;
        void* user = nullptr;
    };
    Array<Entry> entries_;
};

/// Compile a definition. See the header comment for what compilation does and does not do.
///
/// Fails, naming the node, when: a node's input does not exist, the graph contains a cycle, there
/// is not exactly one `Output`, a node cannot reach the output, a `Custom` node names an
/// unregistered kind, or a node that needs an anchor (`Follow`, `Orbit`, `LookAt`, `Constraint`,
/// `Collision`) has no `Target` before it.
[[nodiscard]] Expected<RigProgram, Error> compile(const RigDefinition& definition,
                                                  const RigNodeRegistry& registry,
                                                  Allocator& allocator) noexcept;

// --- Evaluation ---------------------------------------------------------------------------------

/// A collision or occlusion query a `Collision` node wants answered before the next evaluation.
///
/// Published, not issued. `kind` distinguishes the two questions because their responses differ —
/// "Glass occludes without colliding".
struct CameraQuery {
    enum class Kind : u8 { Collision = 0, Occlusion };

    Kind kind = Kind::Collision;
    /// The rig that asked, so a batch resolved for several cameras can be routed back.
    u64 rig_bits = 0;
    Vec3 from{0.0F, 0.0F, 0.0F};
    Vec3 to{0.0F, 0.0F, 0.0F};
    f32 radius = 0.0F;
};

/// One query's answer, supplied by the caller on the next evaluation.
struct CameraQueryResult {
    CameraQuery::Kind kind = CameraQuery::Kind::Collision;
    u64 rig_bits = 0;
    /// Fraction of the segment that was clear, in [0, 1]. One is "nothing in the way".
    f32 fraction = 1.0F;
    /// For an occlusion query with several samples: the fraction of samples that were blocked.
    f32 blocked_fraction = 0.0F;
    /// The surface normal at the hit, for the `Slide` response. Ignored when `fraction` is one.
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// True when the obstruction occludes but does not collide — glass. The camera then applies the
    /// occlusion response and not the collision one.
    bool transparent = false;
};

/// The register file a program writes into. One evaluation's working state.
struct RigFrame {
    /// What the rig is framing, in world space, after the `Target` node.
    Vec3 anchor{0.0F, 0.0F, 0.0F};
    Vec3 anchor_velocity{0.0F, 0.0F, 0.0F};
    bool has_anchor = false;

    Vec3 position{0.0F, 0.0F, 0.0F};
    Quat rotation = Quat::identity();
    Lens lens;

    /// Orbit state, carried from the `Orbit` node to the nodes after it.
    f32 yaw_radians = 0.0F;
    f32 pitch_radians = 0.0F;
    f32 distance = 0.0F;
    f32 zoom = 0.0F;

    /// Additive shake, applied over the evaluated base rather than baked into it, so a diagnostic
    /// can subtract it and a player setting can scale it.
    Vec3 shake_offset{0.0F, 0.0F, 0.0F};
    Quat shake_rotation = Quat::identity();

    /// Set by a `Collision` node that asked for a shoulder swap.
    bool shoulder_swapped = false;
    /// Set by a `Collision` node whose response was `FadeObstacle`: the camera did not move and
    /// whoever owns the obstacle's material is being asked to fade it.
    f32 obstacle_fade = 0.0F;
};

/// Everything one evaluation reads that is not the program or the rig's own state.
struct RigEvaluationInput {
    /// Seconds. In simulation mode this is the fixed tick; in render mode the frame. Passed in
    /// rather than read from a clock, which is what makes `camera-system`'s "Cameras evaluated in
    /// simulation mode SHALL NOT depend on wall-clock time" a property of the interface.
    f32 delta_seconds = 0.0F;
    u64 tick = 0;

    TargetBinding binding;
    /// The binding resolved by whoever owns the world. Invalid when it could not be resolved.
    TargetSample sample;

    CameraIntent intent;
    /// Impulses live this frame, already attenuated by distance and occlusion by the server.
    Span<const CameraImpulse> impulses;
    /// Answers to the queries the previous evaluation published.
    Span<const CameraQueryResult> query_results;

    /// The player's settings, applied in one place. See `CameraSettings`.
    f32 shake_scale = 1.0F;
    f32 fov_override_radians = 0.0F;
    f32 collision_strength = 1.0F;

    /// Where queries are published. Null when the caller does not resolve queries, in which case a
    /// `Collision` node still applies whatever results it was given and asks for nothing.
    Array<CameraQuery>* queries = nullptr;
    u64 rig_bits = 0;
};

/// The state one rig instance keeps between evaluations. Reset by a cut.
struct RigState {
    SmoothVec3 anchor;
    SmoothVec3 position;
    SmoothQuat rotation;
    SmoothScalar distance;
    SmoothScalar lens_value;
    SmoothScalar collision_distance;

    f32 yaw_radians = 0.0F;
    f32 pitch_radians = 0.0F;
    f32 zoom = 0.0F;

    /// Latched by a recentre intent and cleared when the yaw has arrived. A tap therefore completes
    /// the recentre; without the latch a one-frame intent would only nudge, and a designer would
    /// discover that by holding the button.
    bool recentring = false;
    /// Seconds accumulated by evaluation, driving the shake oscillators. Accumulated rather than
    /// read from a clock: a simulation-mode camera must be reproducible from its tick and its
    /// inputs, and a wall-clock phase would make two replays of one frame differ.
    f32 noise_time = 0.0F;

    /// False until the first evaluation. The first evaluation snaps every smoother to its target
    /// rather than easing from wherever a default-constructed one started, which is the difference
    /// between a camera that appears where it belongs and one that flies in from the origin.
    bool primed = false;

    Vec3 previous_position{0.0F, 0.0F, 0.0F};
    bool has_previous = false;

    /// Reset every smoother to `pose`. What a cut and a teleport call.
    void reset(const Transform& pose, const Lens& lens) noexcept;
};

/// One op's contribution, for the inspector. `camera-system`: "**WHEN** the camera is not where
/// expected **THEN** the inspector SHALL show which node or contribution moved it and by how much".
struct RigOpTrace {
    Name id;
    RigNodeKind kind = RigNodeKind::Output;
    Vec3 position_before{0.0F, 0.0F, 0.0F};
    Vec3 position_after{0.0F, 0.0F, 0.0F};
    /// Radians between the orientation before and after.
    f32 rotation_delta_radians = 0.0F;
    f32 fov_before_radians = 0.0F;
    f32 fov_after_radians = 0.0F;
};

/// Execute a compiled program. Allocates nothing except through `trace` and `input.queries`, both
/// of which the caller sized.
[[nodiscard]] Status evaluate(const RigProgram& program, const RigNodeRegistry& registry,
                              RigState& state, const RigEvaluationInput& input, RigFrame& frame,
                              Array<RigOpTrace>* trace) noexcept;

}  // namespace cy::camera
