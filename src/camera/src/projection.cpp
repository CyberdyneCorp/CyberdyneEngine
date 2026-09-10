// Screen and world projection. M8.b task 7.3.

#include <cy/camera/projection.h>

#include <algorithm>
#include <cmath>

namespace cy::camera {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

/// The view's basis and its half-extents at unit depth. Every utility below needs the same four
/// values, and computing them once is what keeps split-screen correct: the aspect comes from the
/// VIEWPORT, never from the back buffer.
struct ViewBasis {
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    f32 tan_half_x = 1.0F;
    f32 tan_half_y = 1.0F;
    bool orthographic = false;
    f32 ortho_half_height = 1.0F;
};

[[nodiscard]] ViewBasis basis_of(const EvaluatedCamera& camera,
                                 const render::ViewDescription& view) noexcept {
    ViewBasis basis;
    basis.forward = camera.pose.rotation * Vec3{0.0F, 0.0F, -1.0F};
    basis.right = camera.pose.rotation * Vec3{1.0F, 0.0F, 0.0F};
    basis.up = camera.pose.rotation * Vec3{0.0F, 1.0F, 0.0F};
    const f32 aspect = view.viewport.aspect();
    if (view.projection.kind == render::ProjectionKind::Orthographic) {
        basis.orthographic = true;
        basis.ortho_half_height = view.projection.ortho_height * 0.5F;
        basis.tan_half_y = 1.0F;
        basis.tan_half_x = aspect;
        return basis;
    }
    basis.tan_half_y = std::tan(clampf(view.projection.fov_y_radians, 0.02F, 3.1F) * 0.5F);
    basis.tan_half_x = basis.tan_half_y * aspect;
    return basis;
}

/// A screen point in the viewport's pixels to normalised device coordinates in [-1, 1], +Y up.
/// THE VIEWPORT RECTANGLE IS SUBTRACTED HERE, and nowhere else.
[[nodiscard]] Vec2 to_ndc(const render::ViewportRect& viewport, Vec2 screen) noexcept {
    const f32 width = (viewport.width == 0) ? 1.0F : static_cast<f32>(viewport.width);
    const f32 height = (viewport.height == 0) ? 1.0F : static_cast<f32>(viewport.height);
    const f32 x = (screen.x - static_cast<f32>(viewport.x)) / width;
    const f32 y = (screen.y - static_cast<f32>(viewport.y)) / height;
    return Vec2{(x * 2.0F) - 1.0F, 1.0F - (y * 2.0F)};
}

[[nodiscard]] Vec2 from_ndc(const render::ViewportRect& viewport, Vec2 ndc) noexcept {
    const f32 width = static_cast<f32>(viewport.width);
    const f32 height = static_cast<f32>(viewport.height);
    return Vec2{static_cast<f32>(viewport.x) + (((ndc.x + 1.0F) * 0.5F) * width),
                static_cast<f32>(viewport.y) + (((1.0F - ndc.y) * 0.5F) * height)};
}

}  // namespace

Ray screen_point_to_ray(const EvaluatedCamera& camera, const render::ViewDescription& view,
                        Vec2 screen) noexcept {
    const ViewBasis basis = basis_of(camera, view);
    const Vec2 ndc = to_ndc(view.viewport, screen);
    Ray ray;
    if (basis.orthographic) {
        const f32 half_height = basis.ortho_half_height;
        const f32 half_width = half_height * view.viewport.aspect();
        ray.origin = camera.pose.translation + (basis.right * (ndc.x * half_width)) +
                     (basis.up * (ndc.y * half_height));
        ray.direction = basis.forward;
        return ray;
    }
    ray.origin = camera.pose.translation;
    ray.direction = normalize(basis.forward + (basis.right * (ndc.x * basis.tan_half_x)) +
                              (basis.up * (ndc.y * basis.tan_half_y)));
    return ray;
}

ScreenPoint world_to_screen(const EvaluatedCamera& camera, const render::ViewDescription& view,
                            Vec3 world) noexcept {
    const ViewBasis basis = basis_of(camera, view);
    const Vec3 offset = world - camera.pose.translation;
    ScreenPoint out;
    out.depth = dot(offset, basis.forward);
    const f32 x = dot(offset, basis.right);
    const f32 y = dot(offset, basis.up);

    if (basis.orthographic) {
        const f32 half_height = basis.ortho_half_height;
        const f32 half_width = half_height * view.viewport.aspect();
        const Vec2 ndc{x / ((half_width > 0.0F) ? half_width : 1.0F),
                       y / ((half_height > 0.0F) ? half_height : 1.0F)};
        out.position = from_ndc(view.viewport, ndc);
        out.behind = out.depth < 0.0F;
        out.on_screen = !out.behind && std::fabs(ndc.x) <= 1.0F && std::fabs(ndc.y) <= 1.0F;
        return out;
    }

    if (out.depth <= 1e-5F) {
        // BEHIND. The projection would mirror the point across the centre and a caller drawing a
        // marker there would draw it in the wrong half of the screen, so the flag is set and the
        // position is the one the mirrored projection would give — reported, not hidden.
        out.behind = true;
        const f32 denominator = (std::fabs(out.depth) < 1e-5F) ? 1e-5F : std::fabs(out.depth);
        const Vec2 ndc{x / (denominator * basis.tan_half_x), y / (denominator * basis.tan_half_y)};
        out.position = from_ndc(view.viewport, ndc);
        out.on_screen = false;
        return out;
    }
    const Vec2 ndc{x / (out.depth * basis.tan_half_x), y / (out.depth * basis.tan_half_y)};
    out.position = from_ndc(view.viewport, ndc);
    out.on_screen = std::fabs(ndc.x) <= 1.0F && std::fabs(ndc.y) <= 1.0F;
    return out;
}

Frustum screen_rect_to_frustum(const EvaluatedCamera& camera, const render::ViewDescription& view,
                               Vec2 corner_a, Vec2 corner_b) noexcept {
    const ViewBasis basis = basis_of(camera, view);
    const Vec2 ndc_a = to_ndc(view.viewport, corner_a);
    const Vec2 ndc_b = to_ndc(view.viewport, corner_b);
    const f32 left = (ndc_a.x < ndc_b.x) ? ndc_a.x : ndc_b.x;
    const f32 right = (ndc_a.x < ndc_b.x) ? ndc_b.x : ndc_a.x;
    const f32 bottom = (ndc_a.y < ndc_b.y) ? ndc_a.y : ndc_b.y;
    const f32 top = (ndc_a.y < ndc_b.y) ? ndc_b.y : ndc_a.y;

    const Vec3 origin = camera.pose.translation;
    const f32 near_plane = view.projection.near_plane;
    const f32 far_plane = (view.projection.far_plane > 0.0F) ? view.projection.far_plane : 1.0e6F;

    Frustum frustum;
    if (basis.orthographic) {
        const f32 half_height = basis.ortho_half_height;
        const f32 half_width = half_height * view.viewport.aspect();
        const Vec3 centre_left = origin + (basis.right * (left * half_width));
        const Vec3 centre_right = origin + (basis.right * (right * half_width));
        const Vec3 centre_bottom = origin + (basis.up * (bottom * half_height));
        const Vec3 centre_top = origin + (basis.up * (top * half_height));
        frustum.planes[Frustum::Left] = Plane::from_point_normal(centre_left, basis.right);
        frustum.planes[Frustum::Right] = Plane::from_point_normal(centre_right, -basis.right);
        frustum.planes[Frustum::Bottom] = Plane::from_point_normal(centre_bottom, basis.up);
        frustum.planes[Frustum::Top] = Plane::from_point_normal(centre_top, -basis.up);
    } else {
        // Each side plane passes through the camera and contains the ray through that edge, so a
        // dragged rectangle becomes exactly the volume the player selected.
        const Vec3 ray_left = basis.forward + (basis.right * (left * basis.tan_half_x));
        const Vec3 ray_right = basis.forward + (basis.right * (right * basis.tan_half_x));
        const Vec3 ray_bottom = basis.forward + (basis.up * (bottom * basis.tan_half_y));
        const Vec3 ray_top = basis.forward + (basis.up * (top * basis.tan_half_y));
        // THE NORMALS POINT INWARD, which is what `Frustum::contains` and `intersects` test
        // against. Each cross product is ordered so that the result points into the volume: get one
        // of the four backwards and the frustum selects the complement of the rectangle, which
        // looks like a selection bug and is a sign error.
        frustum.planes[Frustum::Left] =
            Plane::from_point_normal(origin, normalize(cross(ray_left, basis.up)));
        frustum.planes[Frustum::Right] =
            Plane::from_point_normal(origin, normalize(cross(basis.up, ray_right)));
        frustum.planes[Frustum::Bottom] =
            Plane::from_point_normal(origin, normalize(cross(basis.right, ray_bottom)));
        frustum.planes[Frustum::Top] =
            Plane::from_point_normal(origin, normalize(cross(ray_top, basis.right)));
    }
    frustum.planes[Frustum::Near] =
        Plane::from_point_normal(origin + (basis.forward * near_plane), basis.forward);
    frustum.planes[Frustum::Far] =
        Plane::from_point_normal(origin + (basis.forward * far_plane), -basis.forward);
    // The planes were written directly rather than extracted from a matrix, so the corner masks
    // `intersects(Aabb)` selects with have to be recomputed — an omission that shows up as a box
    // test that is right most of the time.
    frustum.refresh_corner_masks();
    return frustum;
}

}  // namespace cy::camera
