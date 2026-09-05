// Render view production, and the history identity that a cut invalidates. Task 4.3.3.
//
// `camera-system` — "Render view production" and "Camera cuts": a view's history identity "SHALL be
// stable across target changes and shall be invalidated on cuts", and "changing a camera's target
// SHALL NOT automatically be a cut".
//
// THOSE TWO SENTENCES ARE ONE NUMBER, and the cases below are what keep them from becoming two
// mechanisms that disagree.

#include <cy/servers/camera/view.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::camera;

namespace {

[[nodiscard]] RenderViewRequest request_for(u32 view_key) {
    RenderViewRequest request;
    request.name = cy::Name::intern("main");
    request.viewport = cy::render::ViewportRect{0, 0, 1920, 1080};
    request.view_key = view_key;
    request.importance = 1.0F;
    return request;
}

}  // namespace

CY_TEST_CASE("a view carries everything the requirement lists and nothing a backend would know") {
    EvaluatedCamera camera;
    camera.pose.translation = cy::Vec3{1.0F, 2.0F, 3.0F};
    camera.history_id = 0xABCDEF;
    camera.lens.gameplay.vertical_fov_radians = 1.2F;
    camera.lens.gameplay.near_plane = 0.2F;

    RenderViewRequest request = request_for(0);
    request.purpose = cy::render::ViewPurpose::Primary;
    request.layer_mask = 0x3;
    request.importance = 2.5F;
    request.family = 4;

    cy::render::ViewDescription view;
    CY_REQUIRE(static_cast<bool>(produce_view(camera, request, view)));

    CY_CHECK(view.camera.translation == camera.pose.translation);
    CY_CHECK_NEAR(view.projection.fov_y_radians, 1.2F, 1e-6F);
    CY_CHECK_NEAR(view.projection.near_plane, 0.2F, 1e-6F);
    CY_CHECK_EQ(view.projection.far_plane, 0.0F);  // infinite, which is the engine's default
    CY_CHECK_EQ(view.viewport.width, 1920U);
    CY_CHECK_EQ(view.layer_mask, 0x3U);
    CY_CHECK_NEAR(view.importance, 2.5F, 1e-6F);
    CY_CHECK_EQ(view.family, 4U);
    CY_CHECK_NE(view.history_id, 0U);
}

CY_TEST_CASE("a viewport with no area is refused rather than producing an aspect of infinity") {
    const EvaluatedCamera camera;
    RenderViewRequest request = request_for(0);
    request.viewport = cy::render::ViewportRect{0, 0, 0, 1080};

    cy::render::ViewDescription view;
    const cy::Status produced = produce_view(camera, request, view);
    CY_REQUIRE_FALSE(static_cast<bool>(produced));
    CY_CHECK_EQ(produced.error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("one camera produces several views, each with its own temporal history") {
    // `camera-system`: "**WHEN** a first-person camera renders the world and a separate weapon view
    // **THEN** both SHALL be render views derived from one evaluated camera." They accumulate
    // different history, so they cannot share an identity.
    EvaluatedCamera camera;
    camera.history_id = 12345;

    cy::render::ViewDescription world;
    cy::render::ViewDescription weapon;
    CY_REQUIRE(static_cast<bool>(produce_view(camera, request_for(0), world)));
    CY_REQUIRE(static_cast<bool>(produce_view(camera, request_for(1), weapon)));
    CY_CHECK_NE(world.history_id, weapon.history_id);
    // But both look from the same pose, because they are one camera.
    CY_CHECK(world.camera.translation == weapon.camera.translation);
}

CY_TEST_CASE("changing the target does not change the history identity") {
    // "changing a camera's target SHALL NOT automatically be a cut" — so the accumulated temporal
    // history must survive it. The identity has no target in it, which is how.
    EvaluatedCamera camera;
    camera.history_id = 999;
    const u64 before = history_identity(camera, 0);

    // Everything a target change moves: the pose, the anchor, the aim.
    camera.pose.translation = cy::Vec3{500.0F, 0.0F, 0.0F};
    camera.aim.control_aim = cy::Vec3{0.0F, 1.0F, 0.0F};
    CY_CHECK_EQ(history_identity(camera, 0), before);
}

CY_TEST_CASE("a cut invalidates the history of every view of that camera") {
    // "A cut SHALL invalidate the view's temporal history." One number does both halves, so a
    // consumer cannot forget to act on a separate flag.
    EvaluatedCamera camera;
    camera.history_id = 999;
    const u64 world_before = history_identity(camera, 0);
    const u64 weapon_before = history_identity(camera, 1);

    ++camera.cut_epoch;
    CY_CHECK_NE(history_identity(camera, 0), world_before);
    CY_CHECK_NE(history_identity(camera, 1), weapon_before);
    // And the two views still do not collide with each other after the cut.
    CY_CHECK_NE(history_identity(camera, 0), history_identity(camera, 1));
}

CY_TEST_CASE("two cameras never share a history identity") {
    EvaluatedCamera first;
    first.history_id = 1;
    EvaluatedCamera second;
    second.history_id = 2;
    CY_CHECK_NE(history_identity(first, 0), history_identity(second, 0));
}

CY_TEST_CASE("a view may override the lens but never the pose") {
    // A view that could move the camera would be a second camera pretending to be a view. The
    // request has no pose field, so the only way to check it is that the produced pose is the
    // camera's whatever the request says.
    EvaluatedCamera camera;
    camera.pose.translation = cy::Vec3{7.0F, 0.0F, 0.0F};
    camera.lens.gameplay.vertical_fov_radians = 1.5F;

    RenderViewRequest request = request_for(1);
    request.override_lens = true;
    request.lens.gameplay.vertical_fov_radians = 0.4F;  // a narrow weapon view

    cy::render::ViewDescription view;
    CY_REQUIRE(static_cast<bool>(produce_view(camera, request, view)));
    CY_CHECK_NEAR(view.projection.fov_y_radians, 0.4F, 1e-6F);
    CY_CHECK_NEAR(view.camera.translation.x, 7.0F, 1e-6F);
}

CY_TEST_CASE("an orthographic camera takes the same view path as a perspective one") {
    // `camera-system`: "**WHEN** a strategy or editor view uses orthographic projection **THEN** it
    // SHALL use the same camera, rig, and view path."
    EvaluatedCamera camera;
    camera.lens.projection = cy::render::ProjectionKind::Orthographic;
    camera.lens.ortho_height = 40.0F;

    RenderViewRequest request = request_for(0);
    request.purpose = cy::render::ViewPurpose::EditorViewport;

    cy::render::ViewDescription view;
    CY_REQUIRE(static_cast<bool>(produce_view(camera, request, view)));
    CY_CHECK_EQ(view.projection.kind, cy::render::ProjectionKind::Orthographic);
    CY_CHECK_NEAR(view.projection.ortho_height, 40.0F, 1e-6F);
}
