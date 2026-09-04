// Projection matrices, view derivation, world bounds and the light vocabulary. See
// cy/servers/render/model.h.

#include <cy/servers/render/model.h>

#include <cy/core/math/projection.h>

namespace cy::render {
namespace {

constexpr const char* kViewPurposeNames[] = {
    "Primary", "Shadow", "ReflectionProbe", "SceneCapture", "EditorViewport", "Thumbnail", "XrEye",
};
static_assert(sizeof(kViewPurposeNames) / sizeof(kViewPurposeNames[0]) ==
              static_cast<usize>(ViewPurpose::Count));

}  // namespace

const char* view_purpose_name(ViewPurpose purpose) noexcept {
    const auto index = static_cast<usize>(purpose);
    return (index < static_cast<usize>(ViewPurpose::Count)) ? kViewPurposeNames[index]
                                                            : "<invalid>";
}

Mat4 Projection::matrix(f32 aspect) const noexcept {
    if (kind == ProjectionKind::Orthographic) {
        const f32 half_height = ortho_height * 0.5F;
        const f32 half_width = half_height * aspect;
        // An orthographic projection has no meaningful infinite far plane: parallel rays never
        // converge, so the depth range has to be bounded. A zero far plane is taken as "a thousand
        // times the near distance", which is a working default rather than a silent zero-depth
        // matrix.
        const f32 far_distance = (far_plane > near_plane) ? far_plane : (near_plane * 1000.0F);
        return orthographic_reversed_z(-half_width, half_width, -half_height, half_height,
                                       near_plane, far_distance);
    }
    if (far_plane <= near_plane) {
        // Zero means infinite (see the field's comment), and reversed Z is what makes that a
        // reasonable default rather than a trick — `core-math`, "Infinite far plane".
        return perspective_reversed_z_infinite(fov_y_radians, aspect, near_plane);
    }
    return perspective_reversed_z(fov_y_radians, aspect, near_plane, far_plane);
}

void View::refresh() noexcept {
    camera_relative_origin = desc.camera.translation;
    view_matrix = view_from_transform(desc.camera.rotation, desc.camera.translation);
    projection_matrix = desc.projection.matrix(desc.viewport.aspect());
    view_projection = projection_matrix * view_matrix;
    frustum = Frustum::from_view_projection(view_projection);
}

const char* light_kind_name(LightKind kind) noexcept {
    switch (kind) {
        case LightKind::Directional:
            return "Directional";
        case LightKind::Point:
            return "Point";
        case LightKind::Spot:
            return "Spot";
        case LightKind::Count:
            break;
    }
    return "<invalid>";
}

Aabb world_bounds_of(const Transform& transform, const Aabb& local) noexcept {
    if (local.is_empty()) {
        return Aabb::from_point(transform.translation);
    }
    // The eight corners through the transform, re-bounded. Conservative and correct under rotation;
    // the cheaper "transform the centre and scale the extents" is wrong for anything rotated, and
    // wrong bounds are culled-away geometry that nobody can explain.
    Aabb world = Aabb::empty();
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec3 point{((corner & 1U) != 0U) ? local.max.x : local.min.x,
                         ((corner & 2U) != 0U) ? local.max.y : local.min.y,
                         ((corner & 4U) != 0U) ? local.max.z : local.min.z};
        const Vec3 transformed = transform.transform_point(point);
        world.min =
            Vec3{math::min(world.min.x, transformed.x), math::min(world.min.y, transformed.y),
                 math::min(world.min.z, transformed.z)};
        world.max =
            Vec3{math::max(world.max.x, transformed.x), math::max(world.max.y, transformed.y),
                 math::max(world.max.z, transformed.z)};
    }
    return world;
}

}  // namespace cy::render
