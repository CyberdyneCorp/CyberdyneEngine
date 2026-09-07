// The vocabulary: the screen-error projection, the selection test, and the normal cone. M7
// task 7.1.
//
// These are the four lines of arithmetic every other part of CyberGeometry is built on, and they
// are asserted here on hand-built clusters rather than on a cooked asset — so that a failure names
// the arithmetic instead of the cook.

#include <cy/test/test.h>

#include <cy/rendering/virtual_geometry/cluster.h>

namespace {

using namespace cy;             // NOLINT(google-build-using-namespace) — the suite's own subject
using namespace cy::rendering;  // NOLINT(google-build-using-namespace)

vg::ProjectionView unit_view() noexcept {
    vg::ProjectionView view;
    view.viewport_height = 1000.0F;
    view.fov_y_radians = 1.5707963268F;  // 90 degrees: tan(half) is exactly 1
    view.camera_position = Vec3{0.0F, 0.0F, 0.0F};
    return view;
}

/// A cluster whose lower test passes below `parent` and whose upper test passes above `own`.
vg::Cluster bracketed(f32 own, f32 parent, f32 distance) noexcept {
    vg::Cluster cluster;
    cluster.lod_error = own;
    cluster.lod_sphere.center = Vec3{0.0F, 0.0F, -distance};
    cluster.lod_sphere.radius = 0.0F;
    cluster.parent_error = parent;
    cluster.parent_sphere.center = Vec3{0.0F, 0.0F, -distance};
    cluster.parent_sphere.radius = 0.0F;
    return cluster;
}

}  // namespace

CY_TEST_CASE("a world error projects to the pixels the projection says it does") {
    // At 90 degrees and 1000 pixels of height, one world unit at one unit of distance is 500
    // pixels: h/2 / (d * tan(fov/2)) = 500 / (1 * 1).
    const vg::ProjectionView view = unit_view();
    vg::ErrorSphere sphere;
    sphere.center = Vec3{0.0F, 0.0F, -1.0F};
    sphere.radius = 0.0F;
    // The comparison is spelled in DOUBLE on purpose: `CY_CHECK_NEAR` wraps `doctest::Approx`,
    // which takes a double, so an f32 argument is promoted — and clang's `-Wdouble-promotion`,
    // which this tree builds with as an error, reports the promotion the macro caused. Casting at
    // the call site says what is happening rather than leaving a warning for the next compiler.
    CY_CHECK_NEAR(static_cast<f64>(vg::project_error(view, 1.0F, sphere)), 500.0, 0.01);
    CY_CHECK_NEAR(static_cast<f64>(vg::project_error(view, 0.5F, sphere)), 250.0, 0.01);

    // Twice the distance is half the pixels.
    sphere.center = Vec3{0.0F, 0.0F, -2.0F};
    CY_CHECK_NEAR(static_cast<f64>(vg::project_error(view, 1.0F, sphere)), 250.0, 0.01);

    // The DISTANCE IS TO THE SPHERE'S NEAR POINT, so a large sphere at the same centre projects a
    // larger error. That is what makes the parent's projection at least its children's when its
    // sphere contains theirs, which is the monotonicity the selection test needs.
    sphere.radius = 1.0F;
    CY_CHECK_NEAR(static_cast<f64>(vg::project_error(view, 1.0F, sphere)), 500.0, 0.01);
}

CY_TEST_CASE("a camera inside the error sphere saturates rather than inverting") {
    // The near-point distance goes negative here. Without the clamp the projection would come back
    // small and NEGATIVE, which compares as within threshold — so the coarsest representation would
    // be chosen at exactly the moment the viewer is closest to it.
    const vg::ProjectionView view = unit_view();
    vg::ErrorSphere sphere;
    sphere.center = Vec3{0.0F, 0.0F, -1.0F};
    sphere.radius = 5.0F;
    const f32 projected = vg::project_error(view, 1.0F, sphere);
    CY_CHECK_GT(projected, 1000.0F);
}

CY_TEST_CASE("the root's upper test is true at every threshold") {
    const vg::ProjectionView view = unit_view();
    vg::Cluster root = bracketed(0.001F, vg::kRootError, 1.0F);
    CY_CHECK(vg::cluster_selected(root, view, 0.5F));
    CY_CHECK(vg::cluster_selected(root, view, 1000000.0F));
}

CY_TEST_CASE("selection brackets the threshold, and the bracket is half-open") {
    const vg::ProjectionView view = unit_view();
    // own error 0.001 projects to 0.5 px at distance 1; parent 0.01 projects to 5 px.
    const vg::Cluster cluster = bracketed(0.001F, 0.01F, 1.0F);
    CY_CHECK_FALSE(vg::cluster_selected(cluster, view, 0.25F));  // too coarse; descend
    CY_CHECK(vg::cluster_selected(cluster, view, 0.5F));         // exactly at the lower bound
    CY_CHECK(vg::cluster_selected(cluster, view, 4.9F));
    CY_CHECK_FALSE(vg::cluster_selected(cluster, view, 5.0F));  // the parent covers it from here
    CY_CHECK_FALSE(vg::cluster_selected(cluster, view, 50.0F));

    CY_CHECK(vg::cluster_too_coarse(cluster, view, 0.25F));
    CY_CHECK_FALSE(vg::cluster_too_coarse(cluster, view, 0.5F));
}

CY_TEST_CASE("equal errors would leave a hole, which is why the builder refuses them") {
    // The failure this test names is the reason `strictly_above()` exists in build.cpp. With
    // lod_error == parent_error there is exactly one threshold — the projection of that error — at
    // which neither the cluster nor anything above it is selected.
    const vg::ProjectionView view = unit_view();
    const vg::Cluster degenerate = bracketed(0.01F, 0.01F, 1.0F);
    CY_CHECK_FALSE(vg::cluster_selected(degenerate, view, 5.0F));
    CY_CHECK_FALSE(vg::cluster_too_coarse(degenerate, view, 5.0F));
}

CY_TEST_CASE("a normal cone rejects only what faces entirely away") {
    vg::NormalCone cone;
    cone.axis = Vec3{0.0F, 0.0F, 1.0F};
    cone.cos_angle = 0.9F;  // a narrow cone about +Z

    // The camera on the +Z side: front-facing, never rejected.
    CY_CHECK_FALSE(vg::cone_backfacing(cone, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 10.0F}));
    // Directly behind: entirely backfacing.
    CY_CHECK(vg::cone_backfacing(cone, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -10.0F}));
    // Edge on: some of the cone still faces the camera, so it must NOT be rejected.
    CY_CHECK_FALSE(vg::cone_backfacing(cone, Vec3{0.0F, 0.0F, 0.0F}, Vec3{10.0F, 0.0F, 0.0F}));
}

CY_TEST_CASE("a cone spanning a hemisphere never rejects") {
    // A cluster whose normals span more than a hemisphere can never be culled by its cone, and it
    // says so with a degenerate cosine rather than being culled wrongly.
    vg::NormalCone cone;
    cone.axis = Vec3{0.0F, 0.0F, 1.0F};
    cone.cos_angle = -1.0F;
    CY_CHECK(cone.degenerate());
    CY_CHECK_FALSE(vg::cone_backfacing(cone, Vec3{}, Vec3{0.0F, 0.0F, -10.0F}));
}

CY_TEST_CASE("importance scales the threshold in the direction the requirement gives") {
    // "background geometry SHALL coarsen before gameplay-critical geometry does": a larger scale is
    // a larger tolerated error, so background is above one and critical below it.
    CY_CHECK_LT(vg::importance_threshold_scale(vg::Importance::Critical),
                vg::importance_threshold_scale(vg::Importance::Gameplay));
    CY_CHECK_LT(vg::importance_threshold_scale(vg::Importance::Gameplay),
                vg::importance_threshold_scale(vg::Importance::Normal));
    CY_CHECK_LT(vg::importance_threshold_scale(vg::Importance::Normal),
                vg::importance_threshold_scale(vg::Importance::Background));
    CY_CHECK_EQ(vg::importance_threshold_scale(vg::Importance::Normal), 1.0F);
}

CY_TEST_CASE("an orthographic view has no distance term") {
    vg::ProjectionView view = unit_view();
    view.orthographic = true;
    view.ortho_height = 10.0F;
    vg::ErrorSphere near_sphere{Vec3{0.0F, 0.0F, -1.0F}, 0.0F};
    vg::ErrorSphere far_sphere{Vec3{0.0F, 0.0F, -1000.0F}, 0.0F};
    CY_CHECK_EQ(vg::project_error(view, 1.0F, near_sphere),
                vg::project_error(view, 1.0F, far_sphere));
    CY_CHECK_NEAR(static_cast<f64>(vg::project_error(view, 1.0F, near_sphere)), 100.0, 0.01);
}

CY_TEST_CASE("a sphere contains another only when it really does") {
    const vg::ErrorSphere big{Vec3{0.0F, 0.0F, 0.0F}, 10.0F};
    CY_CHECK(big.contains(vg::ErrorSphere{Vec3{0.0F, 0.0F, 0.0F}, 10.0F}));
    CY_CHECK(big.contains(vg::ErrorSphere{Vec3{5.0F, 0.0F, 0.0F}, 5.0F}));
    CY_CHECK_FALSE(big.contains(vg::ErrorSphere{Vec3{6.0F, 0.0F, 0.0F}, 5.0F}));
    CY_CHECK_FALSE(big.contains(vg::ErrorSphere{Vec3{0.0F, 0.0F, 0.0F}, 11.0F}));
}
