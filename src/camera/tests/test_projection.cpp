// Screen and world projection, in a split-screen viewport. M8.b task 7.3.

#include <cy/camera/projection.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::camera;

namespace {

[[nodiscard]] EvaluatedCamera camera_at(Vec3 position) noexcept {
    EvaluatedCamera camera;
    camera.pose = Transform::from_translation(position);     // identity rotation: looking down −Z
    camera.lens.gameplay.vertical_fov_radians = 1.5707963F;  // 90°, so tan(fov/2) is exactly 1
    return camera;
}

[[nodiscard]] render::ViewDescription view_over(const render::ViewportRect& viewport,
                                                const EvaluatedCamera& camera) noexcept {
    render::ViewDescription view;
    view.viewport = viewport;
    view.projection = camera.lens.to_projection();
    return view;
}

}  // namespace

CY_TEST_CASE("camera_projection: the right half of a window is projected in its own coordinates") {
    // "Results SHALL account for the viewport rectangle, so split-screen and editor viewports are
    // correct without special cases." A viewport at x = 1000 in a 2000-pixel window is exactly the
    // case a full-screen assumption gets wrong.
    const EvaluatedCamera camera = camera_at(Vec3{});
    const render::ViewDescription view =
        view_over(render::ViewportRect{1000, 0, 1000, 1000}, camera);

    const Ray centre = screen_point_to_ray(camera, view, Vec2{1500.0F, 500.0F});
    CY_CHECK_NEAR(centre.direction.z, -1.0F, 1e-5F);
    CY_CHECK_NEAR(centre.direction.x, 0.0F, 1e-5F);

    const ScreenPoint point = world_to_screen(camera, view, Vec3{1.0F, 0.0F, -10.0F});
    CY_CHECK(point.on_screen);
    CY_CHECK_FALSE(point.behind);
    CY_CHECK_NEAR(point.depth, 10.0F, 1e-5F);
    CY_CHECK_GT(point.position.x, 1500.0F);
    CY_CHECK_LT(point.position.x, 2000.0F);

    // Round trip: the ray through the projected point looks back at the point.
    const Ray back = screen_point_to_ray(camera, view, point.position);
    const Vec3 expected = normalize(Vec3{1.0F, 0.0F, -10.0F});
    CY_CHECK_NEAR(back.direction.x, expected.x, 1e-4F);
    CY_CHECK_NEAR(back.direction.z, expected.z, 1e-4F);
}

CY_TEST_CASE("camera_projection: a point behind the camera says so") {
    const EvaluatedCamera camera = camera_at(Vec3{});
    const render::ViewDescription view = view_over(render::ViewportRect{0, 0, 1000, 1000}, camera);
    const ScreenPoint behind = world_to_screen(camera, view, Vec3{1.0F, 0.0F, 10.0F});
    CY_CHECK(behind.behind);
    CY_CHECK_FALSE(behind.on_screen);
    CY_CHECK_LT(behind.depth, 0.0F);
}

CY_TEST_CASE(
    "camera_projection: a point outside the viewport is projected and reported off screen") {
    const EvaluatedCamera camera = camera_at(Vec3{});
    const render::ViewDescription view = view_over(render::ViewportRect{0, 0, 1000, 1000}, camera);
    const ScreenPoint far_right = world_to_screen(camera, view, Vec3{40.0F, 0.0F, -10.0F});
    CY_CHECK_FALSE(far_right.on_screen);
    CY_CHECK_FALSE(far_right.behind);
    // Still projected, so a caller can draw an off-screen indicator toward it.
    CY_CHECK_GT(far_right.position.x, 1000.0F);
}

CY_TEST_CASE("camera_projection: a dragged rectangle becomes a frustum selection can query") {
    // "WHEN a player drags a selection rectangle THEN the camera SHALL produce a frustum for the
    // rectangle, and selection SHALL query it."
    const EvaluatedCamera camera = camera_at(Vec3{});
    const render::ViewDescription view = view_over(render::ViewportRect{0, 0, 1000, 1000}, camera);

    // The left half of the view, vertically centred, dragged bottom-right to top-left to prove the
    // corners may arrive in any order.
    const Frustum frustum =
        screen_rect_to_frustum(camera, view, Vec2{450.0F, 600.0F}, Vec2{100.0F, 400.0F});
    CY_CHECK(frustum.contains(Vec3{-2.0F, 0.0F, -10.0F}));
    CY_CHECK_FALSE(frustum.contains(Vec3{6.0F, 0.0F, -10.0F}));
    // A box behind the camera is rejected, and the corner masks were refreshed — an `intersects`
    // that skipped that step is right most of the time, which is the worst kind of wrong.
    CY_CHECK_FALSE(frustum.intersects(
        Aabb::from_center_extents(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.5F, 0.5F, 0.5F})));
    CY_CHECK(frustum.intersects(
        Aabb::from_center_extents(Vec3{-2.0F, 0.0F, -10.0F}, Vec3{0.5F, 0.5F, 0.5F})));
}

CY_TEST_CASE("camera_projection: an orthographic view projects without a special case") {
    // "Orthographic SHALL be first-class, since strategy, two-dimensional, and editor views require
    // it" — so it goes through the same three functions rather than a fork.
    EvaluatedCamera camera = camera_at(Vec3{});
    camera.lens.projection = render::ProjectionKind::Orthographic;
    camera.lens.ortho_height = 20.0F;
    const render::ViewDescription view = view_over(render::ViewportRect{0, 0, 1000, 1000}, camera);

    const Ray edge = screen_point_to_ray(camera, view, Vec2{1000.0F, 500.0F});
    // An orthographic ray starts at the edge of the extent and still points along the view.
    CY_CHECK_NEAR(edge.origin.x, 10.0F, 1e-4F);
    CY_CHECK_NEAR(edge.direction.z, -1.0F, 1e-5F);

    const ScreenPoint point = world_to_screen(camera, view, Vec3{5.0F, 0.0F, -50.0F});
    CY_CHECK(point.on_screen);
    // Depth does not change the projected position under an orthographic lens, which is the whole
    // difference between the two.
    const ScreenPoint nearer = world_to_screen(camera, view, Vec3{5.0F, 0.0F, -5.0F});
    CY_CHECK_NEAR(point.position.x, nearer.position.x, 1e-4F);
}
