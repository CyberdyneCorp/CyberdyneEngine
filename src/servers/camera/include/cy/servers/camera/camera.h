#pragma once
// The evaluated camera, and the four vocabularies that feed it: targets, intent, impulses and cuts.
// Tasks 4.3.1 and 4.3.3.
//
// `camera-system` — "Four separated concepts". This file owns the third of them. The definition and
// the rig are rig.h's, the render view is view.h's, and keeping the evaluated camera in its own
// header is the cheap way to make the separation visible: nothing here names a rig node, and a
// consumer that only needs a pose and a lens includes only this.
//
// ================================================================================================
// WHAT AN EVALUATED CAMERA IS FOR, WHICH IS MORE THAN RENDERING
// ================================================================================================
//
// `camera-system` calls the camera "the producer nine other capabilities were waiting for": render
// views, streaming sources, cut events, view importance, the listener anchor, and the screen rays
// selection needs. Three of those are published from `EvaluatedCamera` directly — the listener
// anchor, the streaming source and the cut — and the requirement says why they live here rather
// than being configured beside the camera:
//
//   "The listener and the streaming source SHALL be **derived from the evaluated camera**, not
//    configured independently, so they cannot disagree about where the player is."
//
// A field is therefore the wrong shape for either of them to be *set*: both are computed by
// `derive_listener()` and `derive_streaming_source()` from the pose, the velocity and one policy
// value, and the policy is the only thing a project chooses.
//
// ================================================================================================
// AIM IS THREE THINGS, AND THE CAMERA IS NOT AUTHORITY
// ================================================================================================
//
// `camera-system` separates **view aim** (where the camera looks), **control aim** (where the
// player is aiming — "the value carried in commands and validated by the authority") and **weapon
// aim** (where the weapon points after offsets and recoil). `AimState` below holds all three, and
// the two rules that make the separation worth having are enforced by shape rather than by comment:
//
//   * `view_aim` is DERIVED from the pose and has no setter. Nothing can write a view aim that
//     disagrees with where the camera is looking.
//   * `control_aim` is carried IN through `CameraIntent` and echoed back unchanged. The camera
//     never computes it, because "A server SHALL validate against control aim carried in a command
//     and the player's simulated state, never against a client's camera transform" — a camera that
//     produced the control aim would make the camera the authority by the back door.
//
// ================================================================================================
// WHAT THIS HEADER MAY NOT NAME
// ================================================================================================
//
// `src/servers/` is layer 2: no entity, no node, no world, no script. A camera target is therefore
// a **stable identifier plus a sampled pose** (`TargetBinding` and `TargetSample`), never a pointer
// into component storage — which is also `camera-system`'s own rule: "rig nodes SHALL NOT retain
// raw pointers to component data across frames". Whoever owns a world resolves the binding into a
// sample once per evaluation and hands the samples in. That the camera server *cannot* do the
// resolution itself is the layering doing the remembering.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/values/name.h>
#include <cy/servers/camera/lens.h>

namespace cy::camera {

// --- Targets ----------------------------------------------------------------------------------

/// What a camera is framing. `camera-system`: "**The target SHALL NOT be implicitly the controlled
/// entity.** Following what the player controls is one policy among several."
enum class TargetKind : u8 {
    /// No target. A free-fly debug camera and a photo-mode camera are this.
    None = 0,
    Entity,
    /// Several entities framed together: the group's bounds are the target.
    Group,
    Position,
    Bounds,
    Spline,
    /// A project-supplied provider, resolved by whoever owns the world into a `TargetSample`.
    Provider,
    Count,
};

[[nodiscard]] const char* target_kind_name(TargetKind kind) noexcept;

/// A camera's binding to what it frames — a *policy*, resolved once per evaluation into a sample.
struct TargetBinding {
    TargetKind kind = TargetKind::None;
    /// The stable identity of whatever is being framed: an entity id, a group id, a spline id.
    /// Stable across streaming and structural change, which is why it is a number the world assigns
    /// and not a handle this module issues.
    u64 stable_id = 0;
    /// Used when `kind` is `Position` or `Bounds`, and as the fallback when a sample is missing.
    Vec3 position{0.0F, 0.0F, 0.0F};
    Aabb bounds = Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.5F, 0.5F, 0.5F});
    /// Weight, minimum screen size and priority, for multi-target framing.
    f32 weight = 1.0F;
    f32 min_screen_size = 0.0F;
    u8 priority = 0;
};

/// One target resolved for one evaluation. Supplied by the caller; the camera server has no world
/// to resolve it from and that is the point (see the header comment).
struct TargetSample {
    u64 stable_id = 0;
    Transform transform;
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    Aabb bounds = Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.5F, 0.5F, 0.5F});
    /// False when the world could not resolve the binding this frame — a streamed-out entity, a
    /// destroyed one. The rig holds its last anchor rather than snapping to the origin.
    bool valid = false;
};

// --- Intent -----------------------------------------------------------------------------------

/// What gameplay and players are allowed to say to a camera.
///
/// `camera-system`: "Gameplay systems SHALL NOT set a camera's position or orientation directly."
/// Every field here is an *influence*; none of them is a pose. The one exception the requirement
/// allows — "Direct pose control SHALL be available only to low-level debug and custom node code" —
/// is `CameraServer::override_pose()`, which says so in its own comment and is reported by the
/// diagnostics as an override rather than as a contribution.
struct CameraIntent {
    /// Look delta in radians, already through the input layer's sensitivity, inversion and
    /// assistance. `camera-system` requires settings to be applied "in **one place**", and that
    /// place is the input pipeline plus `CameraSettings` — never a second multiply in a rig node.
    Vec2 look{0.0F, 0.0F};
    /// Normalised zoom in [0, 1]. One parameter, mapped through curves to height, distance, tilt
    /// and lens, because "zooming out raises and tilts the camera coherently rather than through
    /// independent controls".
    f32 zoom = 0.0F;
    bool zoom_set = false;
    /// Recentre behind the target on the next evaluation.
    bool recentre = false;
    /// Where the PLAYER is aiming. Carried through, never computed here. See the header comment.
    Vec3 control_aim{0.0F, 0.0F, -1.0F};
    bool control_aim_set = false;
};

/// Accumulate `addition` into `intent`. Look deltas add, flags latch, the last zoom wins.
///
/// A free function rather than `operator+=`: several sources contribute intent in one frame — a
/// stick, a mouse, a scripted source — and "accumulate" is the operation, with a name that says a
/// second contribution does not replace the first.
void accumulate(CameraIntent& intent, const CameraIntent& addition) noexcept;

// --- Impulses ---------------------------------------------------------------------------------

/// A disturbance emitted by gameplay. `camera-system`: gameplay "SHALL emit **impulses** — a
/// source, a tag, a strength, and a world position where applicable — rather than modifying the
/// camera".
struct CameraImpulse {
    Name source;
    Name tag;
    f32 strength = 1.0F;
    f32 frequency_hz = 12.0F;
    f32 duration_seconds = 0.25F;
    /// Zero means "not spatial": a weapon's recoil shakes the camera that fired it whatever the
    /// distance. A non-zero radius attenuates by distance from `world_position`.
    f32 radius = 0.0F;
    Vec3 world_position{0.0F, 0.0F, 0.0F};
    /// Occlusion between the impulse and the camera, in [0, 1], supplied by whoever can answer the
    /// query. One means fully occluded. The camera attenuates by it; it does not compute it, for
    /// the same layering reason a target is sampled rather than resolved.
    f32 occlusion = 0.0F;
};

// --- Cuts -------------------------------------------------------------------------------------

/// Why the camera cut. `camera-system` requires the cause to be carried: "A **camera cut** SHALL be
/// a typed event carrying its cause".
enum class CutReason : u8 {
    None = 0,
    PossessionChange,
    VehicleEntry,
    CinematicStart,
    Death,
    PhotoMode,
    Teleport,
    Scripted,
    Count,
};

[[nodiscard]] const char* cut_reason_name(CutReason reason) noexcept;

/// A cut, as published to the systems whose assumptions it breaks.
///
/// `anticipated` and `lead_seconds` are what make a cut a *deadline* rather than a surprise:
/// "**Anticipated cuts** — a cinematic's cut list, a scripted teleport — SHALL be announcable in
/// advance and SHALL become deadlines through the residency layer, so the destination is prepared
/// rather than discovered."
struct CameraCut {
    CutReason reason = CutReason::None;
    bool anticipated = false;
    f32 lead_seconds = 0.0F;
    /// The rig the cut applies to, and the evaluation tick it was raised on.
    u64 tick = 0;
};

// --- Listener and streaming source -------------------------------------------------------------

/// Where the ears are. `camera-system`: "at the camera, at the controlled entity, or interpolated
/// between them — because a distant third-person camera and a character's ears are not the same
/// place."
enum class ListenerPolicy : u8 {
    AtCamera = 0,
    AtControlledEntity,
    Interpolated,
    Count,
};

[[nodiscard]] const char* listener_policy_name(ListenerPolicy policy) noexcept;

struct ListenerAnchor {
    Transform transform;
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    ListenerPolicy policy = ListenerPolicy::AtCamera;
    /// Only read for `Interpolated`: 0 is at the camera, 1 is at the controlled entity.
    f32 blend = 0.0F;
};

/// What `world-partition-and-streaming` needs of a camera. Derived, never configured.
struct StreamingSource {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    Quat orientation = Quat::identity();
    f32 vertical_fov_radians = 1.0471975512F;
    f32 aspect = 1.0F;
    /// How far ahead of itself this source is asking for content, in seconds. Scaled by speed, so a
    /// camera accelerating toward a region asks earlier: "the camera SHALL publish predicted motion
    /// and content SHALL be requested ahead".
    f32 prediction_horizon_seconds = 0.0F;
    f32 importance = 1.0F;
    /// A cut is a deadline rather than a prediction, so it travels beside the prediction rather
    /// than inside it.
    bool cut_pending = false;
    f32 cut_lead_seconds = 0.0F;
};

// --- Aim --------------------------------------------------------------------------------------

/// The three aims, kept apart. See the header comment for why `view_aim` has no setter.
struct AimState {
    Vec3 view_aim{0.0F, 0.0F, -1.0F};
    Vec3 control_aim{0.0F, 0.0F, -1.0F};
    Vec3 weapon_aim{0.0F, 0.0F, -1.0F};
};

// --- The evaluated camera ----------------------------------------------------------------------

/// The pose, lens and derived data resolved for one frame. `camera-system`'s third concept.
///
/// A VALUE, NOT AN OBJECT. It has no handle (handles.h says why), it owns nothing, and it is
/// recomputed every evaluation. A consumer that kept one across a frame would be reading a pose
/// that no longer holds, and there is nothing here to make that look supported.
struct EvaluatedCamera {
    Transform pose;
    Lens lens;
    /// World-space velocity of the camera itself, from the previous evaluation. Feeds the streaming
    /// prediction and, at M7, motion blur.
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    AimState aim;

    /// The identity temporal history follows. `rendering-architecture` requires it to be supplied
    /// by the camera and never derived from a view index. Stable across target changes; changed by
    /// a cut, which is how a cut invalidates history — see view.h.
    u64 history_id = 0;
    /// Incremented by every cut. `history_id` mixes it in, so one number expresses both halves of
    /// "stable across target changes, invalidated on cuts".
    u32 cut_epoch = 0;
    CameraCut last_cut;

    ListenerAnchor listener;
    StreamingSource streaming;

    /// True when the pose came from `override_pose()` rather than from the rig. Reported by the
    /// diagnostics so "the camera is not where I expect" has an answer before anybody reads a rig.
    bool pose_overridden = false;
};

/// Derive the listener anchor from an evaluated camera and the controlled entity's pose.
///
/// `controlled` is ignored for `AtCamera`, which is why it is passed by value as a `Transform`
/// rather than as an optional: a caller with no controlled entity passes the camera's own pose and
/// gets the same answer for every policy, which is the correct degenerate case.
[[nodiscard]] ListenerAnchor derive_listener(const EvaluatedCamera& camera,
                                             const Transform& controlled, Vec3 controlled_velocity,
                                             ListenerPolicy policy, f32 blend) noexcept;

/// Derive the streaming source. `aspect` is the primary view's, and `importance` the camera's own.
[[nodiscard]] StreamingSource derive_streaming_source(const EvaluatedCamera& camera, f32 aspect,
                                                      f32 importance,
                                                      f32 prediction_seconds) noexcept;

}  // namespace cy::camera
