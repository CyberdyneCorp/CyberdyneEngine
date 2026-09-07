//! The other end of the gizmo wire: the engine's bytes, read by the editor's own decoder.
//!
//! M7 task 5b.3. `cy::render::encode_gizmo_layout` (src/servers/render/src/gizmo.cpp) writes what
//! `cy_editor_viewport::layout::GizmoLayout::decode` reads, and the two are in two languages with
//! no shared declaration between them — no header, no generated binding, nothing a compiler can
//! compare. A comment saying they agree is worth nothing, and a restatement in each is two things
//! that drift apart at the first change.
//!
//! So there is ONE artefact and two readers of it: `src/servers/render/tests/data/
//! gizmo_layout_v1.wire`, produced by the engine's encoder under `CY_UPDATE_GIZMO_WIRE=1` and
//! compared against by `unit.render_server`. This suite reads the same file with the editor's
//! decoder and asserts the values back out. A change to either side fails one of the two, and the
//! file is what names which.
//!
//! **The handle codes are the part that would fail silently.** A handle's code is its index in
//! `Handle::ALL`; inserting one in the middle would move every handle after it by one, and a click
//! on the X arrow would drag the Y one — correct code on both sides, disagreeing about what a
//! number means. That is why the case below checks every handle in the fixture by name rather than
//! only counting them.

use std::path::PathBuf;

use cy_editor_viewport::gizmo::{GizmoMode, Handle};
use cy_editor_viewport::layout::GizmoLayout;

/// Where the engine's fixture lives, from this crate's own directory.
///
/// Computed rather than passed in an environment variable: the engine's suite and this one are run
/// by different tools — CTest and Cargo — and a variable only one of them sets would make this
/// case's absence look like its success.
fn fixture() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../src/servers/render/tests/data/gizmo_layout_v1.wire")
}

/// Whether two pixel coordinates are the same number.
///
/// A tolerance rather than `==`, and not because the values are inexact — every one in the fixture
/// is exact in `f32`, chosen that way so a failure here means the wire changed rather than that a
/// rounding differed. The tolerance is what `clippy::float_cmp` asks for, and a thousandth of a
/// pixel is far below anything this format can express a difference in.
fn same(measured: f32, expected: f32) -> bool {
    (measured - expected).abs() < 1e-3
}

#[test]
fn the_engines_encoded_layout_is_what_this_editor_decodes() {
    let path = fixture();
    let bytes = std::fs::read(&path).unwrap_or_else(|error| {
        panic!(
            "the engine's gizmo fixture at {} could not be read ({error}). It is written by \
             `CY_UPDATE_GIZMO_WIRE=1 cy_test_unit_render_server`",
            path.display()
        )
    });

    let layout = GizmoLayout::decode(&bytes).expect("the engine's bytes decode");
    assert_eq!(layout.frame.as_u64(), 1016);
    assert_eq!(layout.mode, GizmoMode::Universal);
    assert!(
        same(layout.centre.0, 640.0) && same(layout.centre.1, 360.0),
        "{:?}",
        layout.centre
    );
    assert!(same(layout.extent, 88.0), "{}", layout.extent);
    assert_eq!(layout.spots.len(), 8, "one of every kind of handle");

    // Each handle by NAME, because the code is an index and an index is what drifts.
    let x = layout.spot(Handle::AxisX).expect("the X arrow");
    assert!(same(x.x, 728.0) && same(x.y, 360.0), "{x:?}");
    assert!(same(x.radius, 7.0), "{x:?}");
    assert!(same(
        layout.spot(Handle::AxisY).expect("the Y arrow").y,
        272.0
    ));
    assert!(same(
        layout.spot(Handle::PlaneXY).expect("the XY plane").radius,
        9.0
    ));
    assert!(same(
        layout.spot(Handle::RingZ).expect("the Z ring").y,
        448.0
    ));
    assert!(same(
        layout.spot(Handle::BoxX).expect("the X box").radius,
        8.0
    ));
    assert!(layout.spot(Handle::ScreenRing).is_some());
    assert!(layout.spot(Handle::Screen).is_some());
    assert!(layout.spot(Handle::Uniform).is_some());
    // Not in the fixture, and therefore not in the layout — a decoder that invented one would pass
    // every check above.
    assert!(layout.spot(Handle::PlaneYZ).is_none());
}

#[test]
fn a_click_on_the_engines_x_arrow_takes_the_x_arrow() {
    // The whole point of publishing the geometry: the editor hit-tests what the engine DREW. M6's
    // artefact dragged from a hard-coded (47%, 38%) hoping a handle was there; this is the same
    // click resolved against the engine's own answer.
    let bytes = std::fs::read(fixture()).expect("the engine's fixture");
    let layout = GizmoLayout::decode(&bytes).expect("it decodes");
    let arrow = layout.spot(Handle::AxisX).expect("the X arrow");
    assert_eq!(layout.hit(arrow.x, arrow.y), Some(Handle::AxisX));
    // And the centre resolves to the cube rather than to whichever concentric affordance is first
    // in the list, which is a property of the DEPTHS the engine published.
    assert_eq!(
        layout.hit(layout.centre.0, layout.centre.1),
        Some(Handle::Uniform),
        "the engine biases the three centre depths so the cube is nearest"
    );
}

#[test]
fn the_engine_sizes_the_gizmo_in_pixels_and_the_editors_own_check_agrees() {
    // `screen_constant` is the editor's refusal of a runtime that sized a gizmo in world units. It
    // is run here against the engine's own layout so that the two halves of task 5b.3 — the engine
    // producing a screen-constant gizmo and the editor refusing one that is not — are checked
    // against each other rather than each against its own fixture.
    let bytes = std::fs::read(fixture()).expect("the engine's fixture");
    let layout = GizmoLayout::decode(&bytes).expect("it decodes");
    let run = vec![layout.clone(), layout.clone(), layout];
    cy_editor_viewport::layout::screen_constant(&run).expect("the engine's extent is constant");
}
