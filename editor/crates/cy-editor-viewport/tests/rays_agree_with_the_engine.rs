//! The editor's camera model and the engine's produce the same rays. Task 4.2's premise.
//!
//! --- WHY THIS TEST EXISTS AT ALL ------------------------------------------------------------------
//!
//! The editor does not resolve picks: it sends a pixel and a frame identifier, and
//! `cy::render::ray_through_pixel` builds the ray from that frame's own view state. So the two
//! camera models never have to agree for picking to be correct — which is the point of doing it that
//! way.
//!
//! But the editor **does** build a ray for manipulation, because turning a cursor's motion into a
//! translation along an axis is editor-side intent (`crate::math`'s module note has the table). If
//! the editor's ray and the engine's disagree, a gizmo drag moves the object to a position that does
//! not correspond to where the cursor is over the image — the classic "the arrow does not follow the
//! mouse" defect, which is invisible at the centre of the screen and worst at the edges, where it
//! would be blamed on the projection.
//!
//! --- HOW THE AGREEMENT IS PINNED --------------------------------------------------------------------
//!
//! Both sides assert the **same six constants**, computed once from the definition of a perspective
//! frustum and written into both files. `src/servers/render/tests/test_picking.cpp` carries the case
//! "the editor's camera model and this one agree about where a pixel points" with these numbers in
//! it. Two independent implementations checked against a third thing is the only arrangement that
//! catches the case where both drift the same way.
//!
//! The configuration is deliberately not symmetric about the centre: a transposed matrix, a missed
//! aspect ratio or an unflipped Y all survive a centre-pixel test and none of them survives a corner.

use cy_editor_viewport::math::Vec3;
use cy_editor_viewport::state::{Projection, ViewState, ViewportRect};

/// The view both sides use: a 1920 x 1080 viewport, the engine's default 60-degree vertical field,
/// and a camera at the origin looking down −Z.
fn agreed_view() -> ViewState {
    let mut state = ViewState::new();
    state.viewport = ViewportRect {
        x: 0,
        y: 0,
        width: 1920,
        height: 1080,
    };
    // Sixty degrees: the engine's default, and the same number
    // `src/servers/render/tests/test_picking.cpp` writes as 1.0471975512F.
    state.projection = Projection::Perspective {
        fov_y: std::f32::consts::FRAC_PI_3,
    };
    state
}

/// The tolerance. Both sides normalise, so the answers agree to about a part in a million; a
/// thousandth is loose enough that a different order of operations does not fail it and tight enough
/// that a wrong aspect ratio (a 78% error at the edge) cannot pass.
const TOLERANCE: f32 = 1e-3;

#[test]
fn the_centre_pixel_points_along_the_view_axis() {
    let view = agreed_view();
    let ray = view.ray_through_pixel(960.0, 540.0);
    assert!(
        ray.direction
            .nearly_equals(Vec3::new(0.0, 0.0, -1.0), TOLERANCE),
        "{:?}",
        ray.direction
    );
    assert_eq!(
        ray.origin,
        Vec3::ZERO,
        "a perspective ray starts at the eye"
    );
}

#[test]
fn the_right_edge_points_where_the_field_of_view_and_the_aspect_ratio_say() {
    // ndc (1, 0). tan(30°) x 16/9 sideways, one forward, normalised. A missed aspect ratio gives
    // 0.5 here instead of 0.716 — a 40% error that a centre-pixel test cannot see.
    let ray = agreed_view().ray_through_pixel(1920.0, 540.0);
    assert!(
        ray.direction
            .nearly_equals(Vec3::new(0.716_258, 0.0, -0.697_835), TOLERANCE),
        "{:?}",
        ray.direction
    );
}

#[test]
fn the_top_left_corner_points_up_and_left() {
    // ndc (−1, 1). The sign of the Y lane is the flip between a top-left pixel origin and a
    // bottom-left device origin; getting it wrong is a gizmo that drags the wrong way vertically.
    let ray = agreed_view().ray_through_pixel(0.0, 0.0);
    assert!(
        ray.direction
            .nearly_equals(Vec3::new(-0.664_364, 0.373_705, -0.647_275), TOLERANCE),
        "{:?}",
        ray.direction
    );
    assert!(ray.direction.y > 0.0, "the top of the image looks upward");
}

#[test]
fn the_bottom_centre_points_down_by_half_the_field_of_view() {
    // ndc (0, −1): thirty degrees below the axis, so the direction is exactly (0, −sin 30, −cos 30).
    let ray = agreed_view().ray_through_pixel(960.0, 1080.0);
    assert!(
        ray.direction
            .nearly_equals(Vec3::new(0.0, -0.5, -0.866_025), TOLERANCE),
        "{:?}",
        ray.direction
    );
}

#[test]
fn a_camera_that_has_moved_and_turned_carries_the_ray_with_it() {
    // The engine applies the camera's pose to the view-space direction, and so does the editor. A
    // rotated camera is where a transposed rotation shows up.
    let mut view = agreed_view();
    view.camera.position = Vec3::new(10.0, 2.0, -5.0);
    view.camera.rotation =
        cy_editor_viewport::math::Quat::from_axis_angle(Vec3::Y, std::f32::consts::FRAC_PI_2);

    let ray = view.ray_through_pixel(960.0, 540.0);
    assert_eq!(ray.origin, view.camera.position);
    // A quarter turn about +Y takes the camera's −Z onto −X.
    assert!(
        ray.direction
            .nearly_equals(Vec3::new(-1.0, 0.0, 0.0), TOLERANCE),
        "{:?}",
        ray.direction
    );
}
