// Views, projections and the depth convention on the CPU side. Tasks 4.1.1 and 4.1.3.
//
// `rendering-architecture` requires views to be produced from evaluated cameras, with the renderer
// constructing the projection matrix from the camera's *semantic* projection. `Projection::matrix`
// is the one place that construction happens, and design.md §3 requires reversed Z to be a number
// rather than a habit — so these cases assert the numbers.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/model.h>
#include <cy/test/test.h>

using cy::f32;
using namespace cy::render;

namespace {

/// Project a view-space point and return its depth. The camera looks down its local −Z, so a point
/// in front of it has a negative z.
[[nodiscard]] f32 depth_of(const cy::Mat4& projection, f32 view_space_z) noexcept {
    const cy::Vec4 clip = projection * cy::Vec4{0.0F, 0.0F, view_space_z, 1.0F};
    return clip.z / clip.w;
}

}  // namespace

CY_TEST_CASE("a zeroed projection is the engine's default: perspective, reversed Z, infinite far") {
    Projection projection;
    const cy::Mat4 matrix = projection.matrix(16.0F / 9.0F);
    // Near maps to 1 and distance approaches 0 without reaching it. A conventional-depth matrix
    // would produce the opposite, and a scene rendered with one looks correct until something
    // intersects — which is exactly why this is a number and not a comment.
    CY_CHECK_NEAR(depth_of(matrix, -projection.near_plane), 1.0F, 1e-5F);
    CY_CHECK_LT(depth_of(matrix, -10000.0F), 0.001F);
    CY_CHECK_GT(depth_of(matrix, -10000.0F), 0.0F);
}

CY_TEST_CASE("a stated far plane maps to exactly zero") {
    Projection projection;
    projection.near_plane = 0.5F;
    projection.far_plane = 500.0F;
    const cy::Mat4 matrix = projection.matrix(1.0F);
    CY_CHECK_NEAR(depth_of(matrix, -0.5F), 1.0F, 1e-5F);
    CY_CHECK_NEAR(depth_of(matrix, -500.0F), 0.0F, 1e-5F);
}

CY_TEST_CASE("an orthographic projection reverses depth the same way") {
    Projection projection;
    projection.kind = ProjectionKind::Orthographic;
    projection.ortho_height = 20.0F;
    projection.near_plane = 1.0F;
    projection.far_plane = 100.0F;
    const cy::Mat4 matrix = projection.matrix(1.0F);
    CY_CHECK_NEAR(depth_of(matrix, -1.0F), 1.0F, 1e-5F);
    CY_CHECK_NEAR(depth_of(matrix, -100.0F), 0.0F, 1e-5F);
}

CY_TEST_CASE("the default projection's frustum keeps what is in front of the camera") {
    // REGRESSION, and a defect this milestone found in `core-math` rather than in the renderer.
    //
    // `Frustum::from_view_projection` guards against a plane with a zero-length normal, and the
    // engine's DEFAULT projection produces exactly one: an infinite far plane makes the clip-space
    // inequality `z >= 0` vacuous, so its row is (0, 0, 0, near) and its normal is zero. The guard
    // substituted `Plane{up, -infinity}`, whose signed distance is −infinity everywhere, so the
    // frustum rejected every sphere and every box — the whole scene, silently, for the projection
    // the engine uses by default. It is `+infinity` now (src/core/math/src/shapes.cpp).
    //
    // The case is here rather than in src/core/math/tests/ because it is the renderer that made the
    // defect reachable: nothing before M3 built a frustum from a projection at all.
    View view;
    view.desc.viewport = ViewportRect{0, 0, 1920, 1080};
    view.desc.camera = cy::Transform::identity();
    view.refresh();

    // Five metres down −Z: dead centre of the view.
    CY_CHECK(view.frustum.intersects(cy::Sphere{cy::Vec3{0.0F, 0.0F, -5.0F}, 1.0F}));
    CY_CHECK(view.frustum.intersects(
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, -5.0F}, cy::Vec3{1.0F, 1.0F, 1.0F})));
    // And a very distant one, which is the point of an infinite far plane.
    CY_CHECK(view.frustum.intersects(cy::Sphere{cy::Vec3{0.0F, 0.0F, -100000.0F}, 1.0F}));
    // Behind the camera is still rejected: the guard must not turn culling off.
    CY_CHECK_FALSE(view.frustum.intersects(cy::Sphere{cy::Vec3{0.0F, 0.0F, 50.0F}, 1.0F}));
    // And so is something far off to the side.
    CY_CHECK_FALSE(view.frustum.intersects(cy::Sphere{cy::Vec3{10000.0F, 0.0F, -5.0F}, 1.0F}));
}

CY_TEST_CASE("a view derives its matrices and its camera-relative origin from one pose") {
    // design.md §3: positions reach the GPU relative to the camera, and the origin is captured when
    // the matrices are built so the two cannot disagree by a frame.
    View view;
    view.desc.viewport = ViewportRect{0, 0, 800, 600};
    view.desc.camera = cy::Transform::from_translation(cy::Vec3{100.0F, 20.0F, -30.0F});
    view.refresh();
    CY_CHECK_NEAR(view.camera_relative_origin.x, 100.0F, 1e-5F);
    CY_CHECK_NEAR(view.camera_relative_origin.z, -30.0F, 1e-5F);
    // The view matrix is the inverse of the pose, so the camera's own position maps to the origin.
    const cy::Vec4 eye = view.view_matrix * cy::Vec4{100.0F, 20.0F, -30.0F, 1.0F};
    CY_CHECK_NEAR(eye.x, 0.0F, 1e-3F);
    CY_CHECK_NEAR(eye.y, 0.0F, 1e-3F);
    CY_CHECK_NEAR(eye.z, 0.0F, 1e-3F);
}

CY_TEST_CASE("world bounds of a rotated instance enclose it") {
    // The cheap "transform the centre and scale the extents" is wrong under rotation, and wrong
    // bounds are culled-away geometry nobody can explain.
    const cy::Aabb local =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{1.0F, 0.1F, 0.1F});
    const cy::Transform rotated =
        cy::Transform::from_rotation(cy::Quat::from_axis_angle(cy::kAxisUp, cy::math::kHalfPi));
    const cy::Aabb world = world_bounds_of(rotated, local);
    // A long thin box turned a quarter turn about Y is long in Z and thin in X.
    CY_CHECK_NEAR(world.size().x, 0.2F, 1e-4F);
    CY_CHECK_NEAR(world.size().z, 2.0F, 1e-4F);
}
