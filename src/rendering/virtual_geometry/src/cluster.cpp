#include <cy/rendering/virtual_geometry/cluster.h>

#include <cmath>

namespace cy::rendering::vg {

namespace {

/// The smallest distance the projection will divide by. A camera inside a cluster's error sphere
/// would otherwise divide by zero or by a negative number and produce an error that compares as
/// small, which selects the coarsest representation at the moment the viewer is closest to it.
constexpr f32 kMinimumDistance = 1.0e-4F;

}  // namespace

const char* deformation_class_name(DeformationClass value) noexcept {
    switch (value) {
        case DeformationClass::Static:
            return "static";
        case DeformationClass::RigidInstanced:
            return "rigid-instanced";
        case DeformationClass::Terrain:
            return "terrain";
        case DeformationClass::Destructible:
            return "destructible";
        case DeformationClass::Skinned:
            return "skinned";
        case DeformationClass::Count:
            break;
    }
    return "unknown";
}

const char* surface_class_name(SurfaceClass value) noexcept {
    switch (value) {
        case SurfaceClass::Solid:
            return "solid";
        case SurfaceClass::Aggregate:
            return "aggregate";
        case SurfaceClass::Foliage:
            return "foliage";
        case SurfaceClass::Hair:
            return "hair";
        case SurfaceClass::Thin:
            return "thin";
        case SurfaceClass::Count:
            break;
    }
    return "unknown";
}

const char* tangent_policy_name(TangentPolicy value) noexcept {
    switch (value) {
        case TangentPolicy::Stored:
            return "stored";
        case TangentPolicy::Derived:
            return "derived";
        case TangentPolicy::Absent:
            return "absent";
        case TangentPolicy::Count:
            break;
    }
    return "unknown";
}

const char* importance_name(Importance value) noexcept {
    switch (value) {
        case Importance::Critical:
            return "critical";
        case Importance::Gameplay:
            return "gameplay";
        case Importance::Normal:
            return "normal";
        case Importance::Background:
            return "background";
        case Importance::Count:
            break;
    }
    return "unknown";
}

f32 importance_threshold_scale(Importance value) noexcept {
    switch (value) {
        case Importance::Critical:
            return 0.25F;
        case Importance::Gameplay:
            return 0.5F;
        case Importance::Normal:
            return 1.0F;
        case Importance::Background:
            return 2.0F;
        case Importance::Count:
            break;
    }
    return 1.0F;
}

Status ClusterPolicy::validate() const noexcept {
    if (min_triangles == 0 || target_triangles == 0 || max_triangles == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "ClusterPolicy: min, target and max triangle counts must all be positive");
    }
    if (min_triangles > target_triangles || target_triangles > max_triangles) {
        return fail(ErrorCode::InvalidArgument,
                    "ClusterPolicy: min <= target <= max is required of the triangle counts");
    }
    if (max_vertices < 3 || max_vertices > kVertexCeiling) {
        return fail(ErrorCode::InvalidArgument,
                    "ClusterPolicy: max_vertices must be between 3 and 256 — a cluster's indices "
                    "are stored as bytes, and a wider index is a page format change");
    }
    if (max_vertices < max_triangles) {
        // Not a hard geometric bound, but a cluster whose vertex ceiling is below its triangle
        // ceiling can never reach the triangle target on a welded mesh, and the clusters come out
        // at a fraction of the size the policy asked for with no diagnostic.
        return fail(ErrorCode::InvalidArgument,
                    "ClusterPolicy: max_vertices below max_triangles makes the triangle target "
                    "unreachable on a welded mesh");
    }
    if (group_size < 2) {
        return fail(ErrorCode::InvalidArgument,
                    "ClusterPolicy: a group of one cluster cannot be simplified below itself "
                    "without unlocking its boundary; group_size must be at least 2");
    }
    if (!(simplify_ratio > 0.0F) || !(simplify_ratio < 1.0F)) {
        return fail(ErrorCode::InvalidArgument, "ClusterPolicy: simplify_ratio must be in (0, 1)");
    }
    if (root_cluster_limit == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "ClusterPolicy: root_cluster_limit must be positive");
    }
    if (max_levels == 0) {
        return fail(ErrorCode::InvalidArgument, "ClusterPolicy: max_levels must be positive");
    }
    return ok();
}

bool ErrorSphere::contains(const ErrorSphere& inner) const noexcept {
    const Vec3 delta = inner.center - center;
    const f32 distance_squared = dot(delta, delta);
    const f32 slack = radius - inner.radius;
    if (slack < 0.0F) {
        return false;
    }
    return distance_squared <= (slack * slack) + 1.0e-6F;
}

f32 ProjectionView::pixels_per_unit_at(const ErrorSphere& sphere) const noexcept {
    if (orthographic) {
        // No distance term at all: an orthographic view maps a fixed world height onto the whole
        // viewport, so one world unit is the same number of pixels everywhere.
        const f32 height = ortho_height > kMinimumDistance ? ortho_height : kMinimumDistance;
        return viewport_height / height;
    }
    const Vec3 delta = sphere.center - camera_position;
    const f32 near_distance = std::sqrt(dot(delta, delta)) - sphere.radius;
    const f32 distance = near_distance > kMinimumDistance ? near_distance : kMinimumDistance;
    const f32 half_fov = fov_y_radians * 0.5F;
    const f32 tangent = std::tan(half_fov);
    const f32 safe_tangent = tangent > 1.0e-6F ? tangent : 1.0e-6F;
    return (viewport_height * 0.5F) / (distance * safe_tangent);
}

f32 project_error(const ProjectionView& view, f32 error, const ErrorSphere& sphere) noexcept {
    if (error >= kRootError) {
        return kRootError;
    }
    return error * view.pixels_per_unit_at(sphere);
}

bool cluster_selected(const Cluster& cluster, const ProjectionView& view,
                      f32 threshold_pixels) noexcept {
    const f32 own = project_error(view, cluster.lod_error, cluster.lod_sphere);
    if (own > threshold_pixels) {
        return false;
    }
    const f32 parent = project_error(view, cluster.parent_error, cluster.parent_sphere);
    return parent > threshold_pixels;
}

bool cluster_too_coarse(const Cluster& cluster, const ProjectionView& view,
                        f32 threshold_pixels) noexcept {
    return project_error(view, cluster.lod_error, cluster.lod_sphere) > threshold_pixels;
}

bool cone_backfacing(const NormalCone& cone, Vec3 cone_apex, Vec3 camera_position) noexcept {
    if (cone.degenerate()) {
        return false;
    }
    const Vec3 to_camera = camera_position - cone_apex;
    const f32 length_squared = dot(to_camera, to_camera);
    if (length_squared <= 1.0e-12F) {
        return false;
    }
    const Vec3 direction = to_camera * (1.0F / std::sqrt(length_squared));
    // The cone is entirely backfacing when the angle between its axis and the direction to the
    // camera exceeds the half-angle plus a right angle: dot < -sin(half_angle) is the closed form,
    // and sin comes from the stored cosine without a trigonometric call.
    const f32 cos_half = cone.cos_angle;
    const f32 sin_squared = 1.0F - (cos_half * cos_half);
    const f32 sin_half = sin_squared > 0.0F ? std::sqrt(sin_squared) : 0.0F;
    return dot(cone.axis, direction) < -sin_half;
}

}  // namespace cy::rendering::vg
