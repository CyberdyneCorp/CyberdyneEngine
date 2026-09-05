// The evaluated camera's vocabulary, and the two things derived from it.
// See cy/servers/camera/camera.h.

#include <cy/servers/camera/camera.h>

#include <cy/core/math/scalar.h>

namespace cy::camera {

const char* target_kind_name(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::None:
            return "none";
        case TargetKind::Entity:
            return "entity";
        case TargetKind::Group:
            return "group";
        case TargetKind::Position:
            return "position";
        case TargetKind::Bounds:
            return "bounds";
        case TargetKind::Spline:
            return "spline";
        case TargetKind::Provider:
            return "provider";
        case TargetKind::Count:
            break;
    }
    return "unknown";
}

const char* cut_reason_name(CutReason reason) noexcept {
    switch (reason) {
        case CutReason::None:
            return "none";
        case CutReason::PossessionChange:
            return "possession-change";
        case CutReason::VehicleEntry:
            return "vehicle-entry";
        case CutReason::CinematicStart:
            return "cinematic-start";
        case CutReason::Death:
            return "death";
        case CutReason::PhotoMode:
            return "photo-mode";
        case CutReason::Teleport:
            return "teleport";
        case CutReason::Scripted:
            return "scripted";
        case CutReason::Count:
            break;
    }
    return "unknown";
}

const char* listener_policy_name(ListenerPolicy policy) noexcept {
    switch (policy) {
        case ListenerPolicy::AtCamera:
            return "at-camera";
        case ListenerPolicy::AtControlledEntity:
            return "at-controlled-entity";
        case ListenerPolicy::Interpolated:
            return "interpolated";
        case ListenerPolicy::Count:
            break;
    }
    return "unknown";
}

void accumulate(CameraIntent& intent, const CameraIntent& addition) noexcept {
    // Look deltas ADD. A frame that delivered two look events must turn the camera by their sum:
    // taking the last one would silently drop input exactly when the frame rate is uneven, which is
    // the same defect `input-and-actions` names for fixed-tick sampling.
    intent.look = intent.look + addition.look;
    if (addition.zoom_set) {
        intent.zoom = addition.zoom;
        intent.zoom_set = true;
    }
    // Flags latch: a recentre requested by any source in a frame happens in that frame.
    intent.recentre = intent.recentre || addition.recentre;
    if (addition.control_aim_set) {
        intent.control_aim = addition.control_aim;
        intent.control_aim_set = true;
    }
}

ListenerAnchor derive_listener(const EvaluatedCamera& camera, const Transform& controlled,
                               Vec3 controlled_velocity, ListenerPolicy policy,
                               f32 blend) noexcept {
    ListenerAnchor anchor;
    anchor.policy = policy;
    anchor.blend = math::clamp(blend, 0.0F, 1.0F);

    switch (policy) {
        case ListenerPolicy::AtCamera:
            anchor.transform = camera.pose;
            anchor.velocity = camera.velocity;
            break;
        case ListenerPolicy::AtControlledEntity:
            anchor.transform = controlled;
            anchor.velocity = controlled_velocity;
            break;
        case ListenerPolicy::Interpolated:
        case ListenerPolicy::Count:
            // Position interpolates; ORIENTATION TAKES THE CAMERA'S. A listener facing half way
            // between where the player is looking and where the character is facing would pan every
            // sound to a direction neither of them is looking at, which is worse than either end of
            // the blend. The requirement only asks that the listener be "placed sensibly".
            anchor.transform.translation =
                lerp(camera.pose.translation, controlled.translation, anchor.blend);
            anchor.transform.rotation = camera.pose.rotation;
            anchor.transform.scale = Vec3{1.0F, 1.0F, 1.0F};
            anchor.velocity = lerp(camera.velocity, controlled_velocity, anchor.blend);
            break;
    }
    return anchor;
}

StreamingSource derive_streaming_source(const EvaluatedCamera& camera, f32 aspect, f32 importance,
                                        f32 prediction_seconds) noexcept {
    StreamingSource source;
    source.position = camera.pose.translation;
    source.velocity = camera.velocity;
    source.orientation = camera.pose.rotation;
    source.vertical_fov_radians = camera.lens.vertical_fov_radians();
    source.aspect = (aspect > 0.0F) ? aspect : 1.0F;
    source.importance = importance;
    // The horizon is a time, not a distance, so a camera that accelerates asks for content further
    // ahead without anybody changing a radius: "**WHEN** the camera accelerates toward a region
    // **THEN** its streaming source SHALL publish predicted motion and content SHALL be requested
    // ahead."
    source.prediction_horizon_seconds = math::max(prediction_seconds, 0.0F);
    source.cut_pending = camera.last_cut.anticipated;
    source.cut_lead_seconds = camera.last_cut.lead_seconds;
    return source;
}

}  // namespace cy::camera
