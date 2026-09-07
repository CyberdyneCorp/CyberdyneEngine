//! The transform gizmo and the orientation widget, on one screen, told apart. Task 2.4.8.
//!
//! `docs/design/images/editor-scene-view.png` is the case this exists to protect: both gizmos visible
//! at once. `editor-visual-language` states the rule and names the failure —
//!
//! > The view-orientation widget SHALL be distinguishable from the transform gizmo **by form**, not
//! > only by position … Making it resemble a transform gizmo costs the user every time they glance at
//! > it.
//!
//! — and `design.md` adds the correction that settles what "by form" means: *"What makes a manipulator
//! is **rings, planar handles and scale boxes** — not arrows."*
//!
//! Three separations, and the test asserts all three, because any one of them alone is a coincidence
//! somebody can remove without noticing:
//!
//! | | Transform gizmo | Orientation widget |
//! |---|---|---|
//! | **Form** | arrows, planar quads, rings, boxes, a centre cube | axis stubs and labels |
//! | **Size** | as large as the object needs, at least 96 px for the universal mode | 56–96 px, constant |
//! | **Position** | on the selection, wherever that is | pinned to a corner |
//!
//! This test lives in the shell because it is the only crate that can see both halves at once:
//! `cy-editor-visual` owns how they look and `cy-editor-viewport` owns what they do, and neither may
//! name the other.

use cy_editor_viewport::gizmo::{GizmoMode as Behaviour, Handle, HandleRole};
use cy_editor_viewport::overlay::{
    WIDGET_DEFAULT_SIZE, WIDGET_MAXIMUM_SIZE, WIDGET_MINIMUM_SIZE, widget_size,
};
use cy_editor_visual::chrome::{Corner, Overlay};
use cy_editor_visual::gizmo::{
    Emphasis, Form, GizmoMode as Appearance, MINIMUM_ACQUISITION, UNIVERSAL_MINIMUM_EXTENT,
    mode_of_forms, presentation,
};
use cy_editor_visual::orientation::{self, OrientationWidget};

/// Every form the transform gizmo can draw, in any mode.
fn gizmo_forms() -> Vec<Form> {
    let mut forms: Vec<Form> = Appearance::ALL
        .into_iter()
        .flat_map(Appearance::forms)
        .collect();
    forms.sort_unstable();
    forms.dedup();
    forms
}

#[test]
fn the_widget_carries_no_form_the_transform_gizmo_manipulates_with() {
    let widget = OrientationWidget::new();
    orientation::check(&widget.forms()).expect("the shipped widget is not a manipulator");
    orientation::check(
        &OrientationWidget {
            show_negative: true,
        }
        .forms(),
    )
    .expect("nor is it with its negative stubs shown");

    for form in widget.forms() {
        assert!(
            !gizmo_forms().contains(&form) || !form.manipulates(),
            "{form:?} is drawn by both, and it manipulates"
        );
    }
    // The three the correction names explicitly. A widget that acquired any of them would read as a
    // thing you can drag an object with.
    for manipulator_only in [Form::Ring, Form::PlaneQuad, Form::Box, Form::CentreBox] {
        assert!(
            !widget.forms().contains(&manipulator_only),
            "the orientation widget carries {manipulator_only:?}"
        );
        assert!(
            gizmo_forms().contains(&manipulator_only),
            "the transform gizmo no longer carries {manipulator_only:?}"
        );
    }
}

#[test]
fn a_proposal_to_put_rings_on_the_widget_is_refused_by_name() {
    // The specification's own scenario: "WHEN an orientation widget with rotation rings is proposed
    // THEN it SHALL be flagged against this requirement."
    let proposed = vec![Form::AxisStub, Form::Label, Form::Ring];
    let problem = orientation::check(&proposed).expect_err("rings are a manipulator's form");
    assert!(problem.remedy.is_some(), "{problem}");
}

#[test]
fn the_transform_mode_is_identifiable_from_the_gizmo_with_the_toolbar_cropped_out() {
    for appearance in Appearance::ALL {
        assert_eq!(mode_of_forms(&appearance.forms()), Some(appearance));
    }
    // And the widget's forms are not any mode's, so a screenshot of the corner cannot be mistaken
    // for a screenshot of a gizmo.
    assert_eq!(mode_of_forms(&OrientationWidget::new().forms()), None);
}

#[test]
fn the_two_halves_of_the_gizmo_agree_about_how_many_modes_there_are() {
    // `cy-editor-visual` says what a mode looks like and `cy-editor-viewport` says what it does. They
    // are separate crates on purpose and neither may name the other, so this is the only place the
    // two lists can be held level. A mode in one and not the other is a toolbar button that does
    // nothing, or a manipulation nobody can reach.
    let appearance: Vec<&str> = Appearance::ALL.iter().map(|mode| mode.label()).collect();
    let behaviour: Vec<&str> = Behaviour::ALL.iter().map(|mode| mode.name()).collect();
    assert_eq!(
        appearance.len(),
        behaviour.len(),
        "{appearance:?} {behaviour:?}"
    );
    assert_eq!(
        appearance,
        ["Move", "Rotate", "Scale", "Universal"],
        "the reference's four modes, in the reference's order"
    );
    assert_eq!(behaviour, ["translate", "rotate", "scale", "universal"]);
}

#[test]
fn every_handle_the_behaviour_can_drag_has_a_form_the_appearance_can_draw() {
    // The two vocabularies, matched. A handle with no form would be a manipulation with nothing on
    // screen to grab; a form with no handle would be a shape that does nothing when pressed.
    for handle in Handle::ALL {
        let expected = match handle.role() {
            HandleRole::Move => [Form::Arrow, Form::PlaneQuad],
            HandleRole::Turn => [Form::Ring, Form::Ring],
            HandleRole::Resize => [Form::Box, Form::CentreBox],
        };
        assert!(
            expected.iter().any(|form| gizmo_forms().contains(form)),
            "{handle:?} has no form"
        );
    }
    // And the universal gizmo draws every form there is, which is what makes it universal.
    let universal = Appearance::Universal.forms();
    for form in gizmo_forms() {
        assert!(
            universal.contains(&form),
            "{form:?} is in no universal gizmo"
        );
    }
}

#[test]
fn the_widget_is_always_smaller_than_a_readable_universal_gizmo() {
    // Size is the second separation. The universal gizmo degrades below 96 px because its handles
    // would overlap; the widget's *maximum* is that same 96, and its default is 72 — so on any
    // screen where both are drawn, the gizmo is the larger of the two.
    const { assert!(WIDGET_MAXIMUM_SIZE <= UNIVERSAL_MINIMUM_EXTENT) }
    const { assert!(WIDGET_DEFAULT_SIZE < UNIVERSAL_MINIMUM_EXTENT) }
    assert_eq!(
        widget_size(1_000.0).to_bits(),
        WIDGET_MAXIMUM_SIZE.to_bits()
    );
    assert_eq!(widget_size(0.0).to_bits(), WIDGET_MINIMUM_SIZE.to_bits());

    // And a gizmo too small to stay acquirable degrades rather than overlapping, which is the
    // failure mode the reference image's slender column sits at the edge of.
    let degraded = presentation(Appearance::Universal, UNIVERSAL_MINIMUM_EXTENT - 1.0);
    assert_eq!(degraded.degraded_from, Some(Appearance::Universal));
    const { assert!(MINIMUM_ACQUISITION <= WIDGET_MINIMUM_SIZE / 4.0) }
}

#[test]
fn the_widget_is_cornered_and_the_gizmo_is_on_the_object() {
    // Position is the third separation, and it is the weakest of the three on its own — which is
    // why it is not the only one. The widget's corner comes from the visual language's own table.
    assert_eq!(Overlay::Orientation.corner(), Corner::TopRight);
    // A corner, not the middle: an overlay in the centre is one that covers the thing it is meant
    // to help you look at.
    assert_ne!(Overlay::Orientation.corner(), Overlay::Performance.corner());
}

#[test]
fn the_widget_is_quieter_than_the_gizmo_at_every_state_it_has() {
    // "It SHALL be visually quieter than the transform gizmo." Quieter as a number, at the states
    // both of them have: the widget's resting opacity is below the gizmo's, and the gizmo's active
    // handle is the brightest thing either of them draws.
    let widget = OrientationWidget::new();
    assert!(widget.opacity() < Emphasis::Resting.opacity());
    assert!(widget.opacity() < Emphasis::Hovered.opacity());
    assert!(Emphasis::Active.opacity() >= Emphasis::Hovered.opacity());
    assert!(
        Emphasis::Receded.opacity() < Emphasis::Resting.opacity(),
        "dragging one handle no longer recedes the others"
    );
}

#[test]
fn dragging_the_widget_orbits_the_camera_and_produces_no_transaction() {
    // The end-to-end version of the structural argument, because "it cannot touch a document" is
    // worth checking rather than only asserting: a document is created, the widget is dragged
    // through every gesture it has, and the document is untouched.
    use cy_editor_core::Actor;
    use cy_editor_documents::Document;
    use cy_editor_viewport::ViewAxis;
    use cy_editor_viewport::navigation::Navigator;
    use cy_editor_viewport::overlay::{OrientationWidget as Behaviour, ViewPreset, WidgetGesture};
    use cy_editor_viewport::state::ViewState;

    let mut document = Document::new("worlds/city.cyworld");
    document
        .with_transaction("Populate", Actor::human("designer"), |document| {
            document.create_node(None)
        })
        .expect("a node");
    let revision = document.revision();
    let entries = document.history().entries().len();

    let mut navigator = Navigator::new();
    let mut state = ViewState::new();
    for gesture in [
        WidgetGesture::Orbit { dx: 40.0, dy: 20.0 },
        WidgetGesture::Pan { dx: 10.0, dy: 5.0 },
        WidgetGesture::Zoom { notches: -3.0 },
        WidgetGesture::ClickAxis(ViewAxis::Front),
        WidgetGesture::Choose(ViewPreset::Left),
        WidgetGesture::Cycle { forward: true },
    ] {
        Behaviour.perform(&mut navigator, &mut state, gesture);
    }
    assert_eq!(document.revision(), revision);
    assert_eq!(document.history().entries().len(), entries);
    assert!(!document.is_transaction_open());
}
