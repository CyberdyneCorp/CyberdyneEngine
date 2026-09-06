//! The view-orientation widget, which is **not** a manipulator.
//!
//! `editor-visual-language`: "The viewport's view-orientation widget SHALL show **only the three
//! principal axes** and their labels. It SHALL NOT display rotation rings, scale boxes, translation
//! arrows, or any other form that resembles a transform gizmo, because its purpose is camera and
//! world orientation and confusing it with object manipulation is a direct cost to the user."
//!
//! It has its own module for the same reason it has its own requirement: it sits a few hundred
//! pixels from a transform gizmo, it is drawn in the same three colours, and the obvious way to
//! build one is to reuse the gizmo's handle code — at which point it acquires arrows, and a user
//! drags it expecting the object to move. Here it shares the [`crate::gizmo::Form`] vocabulary and
//! nothing else, and [`check`] refuses a manipulation shape.

use cy_editor_core::problem::{Problem, Result};

use crate::axis::Axis;
use crate::colour::{Rgb, Theme};
use crate::gizmo::Form;

/// One of the widget's stubs: an axis, its label, and whether it points the negative way.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Stub {
    /// Which axis.
    pub axis: Axis,
    /// Whether this is the negative direction, which is drawn hollow and unlabelled.
    pub negative: bool,
}

impl Stub {
    /// The label — `X`, `Y`, `Z` — or nothing for a negative stub, which is identified by position.
    #[must_use]
    pub const fn label(self) -> &'static str {
        if self.negative { "" } else { self.axis.label() }
    }

    /// The colour, from the one axis function in the workspace.
    #[must_use]
    pub const fn colour(self, theme: Theme) -> Rgb {
        crate::axis::colour(self.axis, theme)
    }
}

/// The widget in the viewport's corner.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct OrientationWidget {
    /// Whether the three negative directions are shown as hollow stubs.
    ///
    /// A preference rather than a constant because six stubs are more useful on a large viewport
    /// and more cluttered on a small one. Neither setting adds a form: a negative stub is the same
    /// [`Form::AxisStub`] drawn hollow.
    pub show_negative: bool,
}

impl OrientationWidget {
    /// The widget as it ships: three axes, three labels, nothing else.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            show_negative: false,
        }
    }

    /// The stubs it draws.
    #[must_use]
    pub fn stubs(&self) -> Vec<Stub> {
        let mut stubs: Vec<Stub> = Axis::ALL
            .into_iter()
            .map(|axis| Stub {
                axis,
                negative: false,
            })
            .collect();
        if self.show_negative {
            stubs.extend(Axis::ALL.map(|axis| Stub {
                axis,
                negative: true,
            }));
        }
        stubs
    }

    /// The shapes it is drawn from. Two, and neither of them manipulates anything.
    #[must_use]
    pub fn forms(&self) -> Vec<Form> {
        if self.show_negative {
            vec![Form::AxisStub]
        } else {
            vec![Form::AxisStub, Form::Label]
        }
    }

    /// How prominent the widget is.
    ///
    /// "It SHALL be visually **quieter** than the transform gizmo." Quieter is a number here so that
    /// a test can hold it: the widget rests below the gizmo's resting opacity, and it has no active
    /// state at all because it is not dragged.
    #[must_use]
    pub const fn opacity(&self) -> f32 {
        0.6
    }

    /// The camera direction clicking a stub aligns to.
    ///
    /// "SHALL support clicking an axis to align the camera." The widget answers with a direction and
    /// an up vector; moving the camera is the viewport's business, which is why this returns a value
    /// rather than doing anything.
    #[must_use]
    pub fn alignment(&self, stub: Stub) -> Alignment {
        let sign = if stub.negative { 1.0 } else { -1.0 };
        let forward = match stub.axis {
            Axis::X => [sign, 0.0, 0.0],
            Axis::Y => [0.0, sign, 0.0],
            Axis::Z => [0.0, 0.0, sign],
        };
        // Looking down an axis needs an up vector that is not parallel to it, which is the one case
        // a caller always forgets and then wonders why the camera rolled.
        let up = if matches!(stub.axis, Axis::Y) {
            [0.0, 0.0, 1.0]
        } else {
            [0.0, 1.0, 0.0]
        };
        Alignment { forward, up }
    }
}

/// Where clicking a stub points the camera.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Alignment {
    /// The direction the camera looks along.
    pub forward: [f32; 3],
    /// The camera's up vector, chosen so that looking down an axis does not roll.
    pub up: [f32; 3],
}

/// Refuse a widget that has acquired a manipulation shape.
///
/// The scenario, checkable: "WHEN an orientation widget with rotation rings is proposed THEN it
/// SHALL be flagged against this requirement." A proposal is a set of forms, and this is what flags
/// it — used by [`crate::rules::check_viewport_overlays`] and callable directly by a plugin's own
/// test.
pub fn check(forms: &[Form]) -> Result<()> {
    if let Some(offender) = forms.iter().find(|form| form.manipulates()) {
        return Err(Problem::new(
            "add this shape to the view-orientation widget",
            format!(
                "{offender:?} is a transform gizmo's shape, and the widget's purpose is camera and \
                 world orientation"
            ),
        )
        .with_remedy(
            "show the three axes and their labels; a user who wants to manipulate the object has \
             the transform gizmo a few hundred pixels away, and confusing the two costs them every \
             time they glance at it",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gizmo::GizmoMode;

    #[test]
    fn the_widget_is_not_mistaken_for_a_gizmo_by_form() {
        // "WHEN a user sees the widget in the viewport corner THEN it SHALL be distinguishable from
        // the transform gizmo by form, not only by position."
        let widget = OrientationWidget::new();
        check(&widget.forms()).unwrap();
        for mode in GizmoMode::ALL {
            for form in mode.forms() {
                assert!(
                    !widget.forms().contains(&form),
                    "the widget shares {form:?} with {mode:?}"
                );
            }
        }
    }

    #[test]
    fn rings_are_rejected_with_a_reason_a_reviewer_can_act_on() {
        let problem = check(&[Form::AxisStub, Form::Ring]).unwrap_err();
        assert!(problem.because.contains("Ring"), "{problem}");
        assert!(problem.remedy.is_some(), "{problem}");
    }

    #[test]
    fn the_widget_is_quieter_than_the_gizmo() {
        assert!(
            OrientationWidget::new().opacity() < crate::gizmo::Emphasis::Resting.opacity(),
            "a widget as loud as the thing it must not be confused with is not quieter"
        );
    }

    #[test]
    fn the_shipped_widget_shows_three_axes_and_their_labels() {
        let widget = OrientationWidget::new();
        assert_eq!(widget.stubs().len(), 3);
        let labels: Vec<&str> = widget.stubs().iter().map(|stub| stub.label()).collect();
        assert_eq!(labels, ["X", "Y", "Z"]);
    }

    #[test]
    fn clicking_an_axis_aligns_the_camera_without_rolling_it() {
        let widget = OrientationWidget::new();
        for stub in widget.stubs() {
            let alignment = widget.alignment(stub);
            let parallel = alignment
                .forward
                .iter()
                .zip(alignment.up)
                .map(|(forward, up)| forward * up)
                .sum::<f32>()
                .abs();
            assert!(parallel < 1.0, "{stub:?} looks along its own up vector");
        }
    }
}
