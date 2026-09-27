// SPDX-License-Identifier: MIT
// The `camera` adapter: `cy::abi::game::CameraBackend` over `cy::camera::CameraServer` and the
// `cy::camera` projections. `add-swift-game-api`.
//
// OWNER: implementer A. The header says what each answer is read from and why; this file is the
// mapping. Every domain failure is reported through `cy::abi::report` and returned — the thunk in
// front of it has already checked the engine, the phase, the pointers and the resimulation flag.

#include <cy/abi/errors.h>
#include <cy/camera/projection.h>
#include <cy/game_backend/camera_backend.h>

#include <cmath>

namespace cy::game_backend {
namespace {

using cy::camera::EvaluatedCamera;
using cy::camera::RigHandle;

/// The far distance a ray is given when the projection's far plane is infinite. The same stand-in
/// `cy::camera::screen_rect_to_frustum` uses, so a pick ray and a selection box reach equally far.
constexpr f32 kInfiniteFar = 1.0e6F;

[[nodiscard]] bool finite(const f32* values, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        if (!std::isfinite(values[index])) {
            return false;
        }
    }
    return true;
}

/// A `CyPose` as a transform. The all-zero quaternion is the identity (cy_abi.h) — and so is one
/// too short to have a direction — and any other is normalised so a caller's rounding does not
/// scale the camera.
[[nodiscard]] Transform to_transform(const CyPose& pose) noexcept {
    const Quat raw{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
    Transform transform;
    transform.rotation =
        length_squared(raw) > cy::math::kSmallLength ? normalize(raw) : Quat::identity();
    transform.translation = Vec3{pose.position[0], pose.position[1], pose.position[2]};
    return transform;
}

void write_pose(const Transform& transform, CyPose& out) noexcept {
    out.position[0] = transform.translation.x;
    out.position[1] = transform.translation.y;
    out.position[2] = transform.translation.z;
    out.rotation[0] = transform.rotation.x;
    out.rotation[1] = transform.rotation.y;
    out.rotation[2] = transform.rotation.z;
    out.rotation[3] = transform.rotation.w;
}

[[nodiscard]] f32 far_distance(const cy::render::Projection& projection) noexcept {
    return projection.far_plane > 0.0F ? projection.far_plane : kInfiniteFar;
}

}  // namespace

void CameraAdapter::set_primary_view(RigHandle rig,
                                     const cy::camera::RenderViewRequest& request) noexcept {
    primary_ = rig;
    request_ = request;
}

const CameraAdapter::Framing* CameraAdapter::framing(RigHandle rig) const noexcept {
    for (const FramingRecord& record : framings_) {
        if (record.rig == rig.bits()) {
            return &record.framing;
        }
    }
    return nullptr;
}

CyResult CameraAdapter::live_rig(CyCamera camera, RigHandle& out) const noexcept {
    out = RigHandle::from_bits(camera);
    if (out.is_null() || !server_->alive(out)) {
        return cy::abi::report(CY_RESULT_NOT_FOUND, "camera: no live camera rig has that handle");
    }
    return CY_RESULT_OK;
}

const EvaluatedCamera* CameraAdapter::primary_view(CyCamera camera,
                                                   cy::render::ViewDescription& out_view,
                                                   CyResult& result) const noexcept {
    RigHandle rig;
    result = live_rig(camera, rig);
    if (result != CY_RESULT_OK) {
        return nullptr;
    }
    if (rig != primary_) {
        result = cy::abi::report(CY_RESULT_NOT_FOUND,
                                 "camera: that camera has no view; only the primary view projects");
        return nullptr;
    }
    const EvaluatedCamera* evaluated = server_->evaluated(rig);
    if (evaluated == nullptr) {
        result = cy::abi::report(CY_RESULT_NOT_FOUND, "camera: the rig has no evaluated camera");
        return nullptr;
    }
    if (auto produced = cy::camera::produce_view(*evaluated, request_, out_view); !produced) {
        result = cy::abi::report(produced.error());
        return nullptr;
    }
    return evaluated;
}

CyResult CameraAdapter::active(CyCamera& out_camera) noexcept {
    if (primary_.is_null() || !server_->alive(primary_)) {
        return cy::abi::report(CY_RESULT_UNAVAILABLE, "camera: there is no primary view");
    }
    out_camera = primary_.bits();
    return CY_RESULT_OK;
}

CyResult CameraAdapter::view(CyCamera camera, CyCameraView& out_view) noexcept {
    cy::render::ViewDescription view;
    CyResult found = CY_RESULT_OK;
    const EvaluatedCamera* evaluated = primary_view(camera, view, found);
    if (evaluated == nullptr) {
        return found;
    }
    const bool orthographic = view.projection.kind == cy::render::ProjectionKind::Orthographic;
    out_view.flags = orthographic ? CY_CAMERA_VIEW_ORTHOGRAPHIC : 0U;
    write_pose(evaluated->pose, out_view.pose);
    out_view.vertical_fov = orthographic ? 0.0F : view.projection.fov_y_radians;
    out_view.ortho_height = orthographic ? view.projection.ortho_height : 0.0F;
    out_view.near_plane = view.projection.near_plane;
    out_view.far_plane = view.projection.far_plane;
    out_view.viewport[0] = static_cast<f32>(view.viewport.x);
    out_view.viewport[1] = static_cast<f32>(view.viewport.y);
    out_view.viewport[2] = static_cast<f32>(view.viewport.width);
    out_view.viewport[3] = static_cast<f32>(view.viewport.height);
    return CY_RESULT_OK;
}

CyResult CameraAdapter::screen_to_ray(CyCamera camera, f32 screen_x, f32 screen_y,
                                      CyRay& out_ray) noexcept {
    cy::render::ViewDescription view;
    CyResult found = CY_RESULT_OK;
    const EvaluatedCamera* evaluated = primary_view(camera, view, found);
    if (evaluated == nullptr) {
        return found;
    }
    const Ray ray = cy::camera::screen_point_to_ray(*evaluated, view, Vec2{screen_x, screen_y});
    // `screen_point_to_ray` starts at the eye; the entry's ray runs from the near plane to the far
    // plane. Along a ray at angle θ to the view axis both planes are 1/cos θ further away.
    const Vec3 forward = evaluated->pose.rotation * Vec3{0.0F, 0.0F, -1.0F};
    const f32 alignment = dot(ray.direction, forward);
    const f32 stretch = alignment > 1e-6F ? 1.0F / alignment : 1.0F;
    const f32 near_plane = view.projection.near_plane;
    const Vec3 origin = ray.at(near_plane * stretch);
    out_ray.origin[0] = origin.x;
    out_ray.origin[1] = origin.y;
    out_ray.origin[2] = origin.z;
    out_ray.direction[0] = ray.direction.x;
    out_ray.direction[1] = ray.direction.y;
    out_ray.direction[2] = ray.direction.z;
    out_ray.max_distance = (far_distance(view.projection) - near_plane) * stretch;
    return CY_RESULT_OK;
}

CyResult CameraAdapter::world_to_screen(CyCamera camera, Span<const f32> points_xyz,
                                        Span<CyScreenPoint> out_points) noexcept {
    cy::render::ViewDescription view;
    CyResult found = CY_RESULT_OK;
    const EvaluatedCamera* evaluated = primary_view(camera, view, found);
    if (evaluated == nullptr) {
        return found;
    }
    if (!finite(points_xyz.data(), points_xyz.size())) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "camera: a world point is not finite");
    }
    const f32 near_plane = view.projection.near_plane;
    for (usize index = 0; index < out_points.size(); ++index) {
        const Vec3 world{points_xyz[index * 3U], points_xyz[(index * 3U) + 1U],
                         points_xyz[(index * 3U) + 2U]};
        const cy::camera::ScreenPoint point = cy::camera::world_to_screen(*evaluated, view, world);
        CyScreenPoint& out = out_points[index];
        out.position[0] = point.position.x;
        out.position[1] = point.position.y;
        out.depth = point.depth;
        out.flags = 0U;
        // "Inside the viewport AND in front of the near plane" (cy_abi.h): the projection's own
        // `on_screen` says only the first.
        if (point.on_screen && point.depth >= near_plane) {
            out.flags |= CY_SCREEN_POINT_ON_SCREEN;
        }
        if (point.behind) {
            out.flags |= CY_SCREEN_POINT_BEHIND;
        }
    }
    return CY_RESULT_OK;
}

CyResult CameraAdapter::record_framing(u64 rig, const CyCameraTarget& target) noexcept {
    Framing* framing = nullptr;
    for (FramingRecord& record : framings_) {
        if (record.rig == rig) {
            framing = &record.framing;
        }
    }
    if (framing == nullptr) {
        auto added = framings_.emplace_back(FramingRecord{rig, Framing{}});
        if (!added) {
            return cy::abi::report(added.error());
        }
        framing = &(*added)->framing;
    }
    framing->yaw = target.yaw;
    framing->pitch = target.pitch;
    framing->distance = target.distance;
    framing->blend_seconds = target.blend_seconds;
    ++framing->revision;
    return CY_RESULT_OK;
}

CyResult CameraAdapter::set_target(CyCamera camera, const CyCameraTarget& target) noexcept {
    RigHandle rig;
    if (const CyResult live = live_rig(camera, rig); live != CY_RESULT_OK) {
        return live;
    }
    const f32 scalars[] = {target.position[0], target.position[1], target.position[2],  target.yaw,
                           target.pitch,       target.distance,    target.blend_seconds};
    if (!finite(scalars, sizeof(scalars) / sizeof(scalars[0])) || target.distance < 0.0F ||
        target.blend_seconds < 0.0F) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT,
                               "camera: a target needs finite values and a non-negative distance "
                               "and blend");
    }
    cy::camera::TargetBinding binding;
    if ((target.flags & CY_CAMERA_TARGET_FOLLOW_ENTITY) != 0U) {
        binding.kind = cy::camera::TargetKind::Entity;
        binding.stable_id = target.entity;
    } else {
        binding.kind = cy::camera::TargetKind::Position;
    }
    binding.position = Vec3{target.position[0], target.position[1], target.position[2]};
    if (auto bound = server_->set_target(rig, binding); !bound) {
        return cy::abi::report(bound.error());
    }
    if (target.blend_seconds == 0.0F) {
        if (auto cut = server_->cut(rig, cy::camera::CutReason::Scripted); !cut) {
            return cy::abi::report(cut.error());
        }
    }
    return record_framing(rig.bits(), target);
}

CyResult CameraAdapter::set_pose(CyCamera camera, const CyPose& pose) noexcept {
    RigHandle rig;
    if (const CyResult live = live_rig(camera, rig); live != CY_RESULT_OK) {
        return live;
    }
    if (!finite(pose.position, 3U) || !finite(pose.rotation, 4U)) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "camera: the pose is not finite");
    }
    if (auto overridden = server_->override_pose(rig, to_transform(pose)); !overridden) {
        return cy::abi::report(overridden.error());
    }
    return CY_RESULT_OK;
}

CyResult CameraAdapter::clear_pose(CyCamera camera) noexcept {
    RigHandle rig;
    if (const CyResult live = live_rig(camera, rig); live != CY_RESULT_OK) {
        return live;
    }
    if (auto cleared = server_->clear_pose_override(rig); !cleared) {
        return cy::abi::report(cleared.error());
    }
    return CY_RESULT_OK;
}

void bind(cy::abi::Host& host, CameraAdapter* adapter) noexcept {
    host.game.camera = adapter;
}

}  // namespace cy::game_backend
