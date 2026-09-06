//! Gizmo legibility: the mode is readable from the shape, and the handles are grabbable.
//!
//! `editor-visual-language` states two things about transform gizmos that are appearance rather than
//! behaviour, and both are checkable here without a viewport:
//!
//! > Transform gizmos SHALL be **acquirable without precision** — handles sized for confident
//! > grabbing rather than for minimal footprint.
//!
//! > The three modes SHALL be visually distinct **by shape, not only by colour** ... A user SHALL be
//! > able to identify the active mode from the gizmo alone, with no reference to a toolbar.
//!
//! The second is enforced by [`GizmoMode::forms`] being the only description of a gizmo's shape and
//! by [`mode_of_forms`] being able to invert it: if two modes ever came to share a form set, the
//! inverse stops being a function and [`the_mode_is_readable_from_the_shape_alone`] fails. That is
//! the screenshot-with-the-toolbar-cropped-out scenario, expressed as data.
//!
//! Behaviour — picking, dragging, snapping, one transaction per drag — belongs to
//! `editor-viewport-and-gizmos` and is not here. This module says what a gizmo *looks* like.

use crate::axis::Axis;
use crate::colour::{Rgb, Semantic, Theme};

/// A shape an interactive overlay is drawn from.
///
/// One vocabulary shared by the transform gizmo and the orientation widget, so that "the widget
/// SHALL be distinguishable from the transform gizmo **by form**, not only by position" is a
/// statement about two sets of these rather than about two pictures.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Form {
    /// A directional arrow along an axis. Translation.
    Arrow,
    /// A quad at the intersection of two axes. Translation within a plane.
    PlaneQuad,
    /// A ring or arc around an axis. Rotation.
    Ring,
    /// A box handle at the end of an axis. Scale.
    Box,
    /// A box at the origin. Uniform scale.
    CentreBox,
    /// A short axis stub with no head. The orientation widget, and nothing else.
    AxisStub,
    /// A text label — `X`, `Y`, `Z`. The encoding that survives without colour.
    Label,
}

impl Form {
    /// Whether this form manipulates the object it is drawn on.
    ///
    /// The orientation widget's forms answer `false`, which is what makes "the orientation widget is
    /// not a manipulator" checkable rather than a matter of taste. See [`crate::orientation`].
    #[must_use]
    pub const fn manipulates(self) -> bool {
        match self {
            Form::Arrow | Form::PlaneQuad | Form::Ring | Form::Box | Form::CentreBox => true,
            Form::AxisStub | Form::Label => false,
        }
    }
}

/// Which transform a gizmo performs.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum GizmoMode {
    /// Directional arrows along each axis, with plane handles at the intersections.
    Translate,
    /// Axis-coloured arcs or rings surrounding the object.
    Rotate,
    /// Axis-aligned box handles, with a centre handle for uniform scale.
    Scale,
    /// All three at once. Permitted, constrained: readable or it degrades. See [`presentation`].
    Universal,
}

impl GizmoMode {
    /// Every mode, for a toolbar and for the checks below.
    pub const ALL: [GizmoMode; 4] = [
        GizmoMode::Translate,
        GizmoMode::Rotate,
        GizmoMode::Scale,
        GizmoMode::Universal,
    ];

    /// The three explicit modes, which `editor-visual-language` requires to "always remain
    /// available" whatever the universal mode does.
    pub const EXPLICIT: [GizmoMode; 3] =
        [GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale];

    /// The engine's own word for the mode, for a tooltip and a command label.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            GizmoMode::Translate => "Move",
            GizmoMode::Rotate => "Rotate",
            GizmoMode::Scale => "Scale",
            GizmoMode::Universal => "Universal",
        }
    }

    /// The shapes this mode is drawn from, in a stable order.
    #[must_use]
    pub fn forms(self) -> Vec<Form> {
        match self {
            GizmoMode::Translate => vec![Form::Arrow, Form::PlaneQuad],
            GizmoMode::Rotate => vec![Form::Ring],
            GizmoMode::Scale => vec![Form::Box, Form::CentreBox],
            GizmoMode::Universal => vec![
                Form::Arrow,
                Form::PlaneQuad,
                Form::Ring,
                Form::Box,
                Form::CentreBox,
            ],
        }
    }

    /// The handles this mode presents, one per axis where the form is axial.
    #[must_use]
    pub fn handles(self) -> Vec<Handle> {
        let mut handles = Vec::new();
        for form in self.forms() {
            match form {
                Form::CentreBox => handles.push(Handle {
                    form,
                    axis: None,
                    emphasis: Emphasis::Resting,
                }),
                _ => handles.extend(Axis::ALL.map(|axis| Handle {
                    form,
                    axis: Some(axis),
                    emphasis: Emphasis::Resting,
                })),
            }
        }
        handles
    }
}

/// The mode a set of shapes identifies, or `None` when the set is not a gizmo's.
///
/// The inverse of [`GizmoMode::forms`], and the reason it can exist is the requirement: a user must
/// be able to identify the active mode from the gizmo alone. A mode set that stopped being
/// invertible would be one a screenshot could not answer.
#[must_use]
pub fn mode_of_forms(forms: &[Form]) -> Option<GizmoMode> {
    let mut sorted: Vec<Form> = forms.to_vec();
    sorted.sort_unstable();
    sorted.dedup();
    GizmoMode::ALL.into_iter().find(|mode| {
        let mut candidate = mode.forms();
        candidate.sort_unstable();
        candidate.dedup();
        candidate == sorted
    })
}

/// How prominent one handle is.
///
/// "Hover and active states SHALL emphasise the handle under manipulation and de-emphasise the
/// rest." Four states rather than two, because de-emphasising the rest is a state of its own: a
/// handle that is merely not hovered looks different from one that is being ignored because its
/// neighbour is being dragged.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Emphasis {
    /// Nothing is happening to the gizmo.
    Resting,
    /// The pointer is over this handle.
    Hovered,
    /// This handle is being dragged.
    Active,
    /// Another handle is being dragged, so this one gets out of the way.
    Receded,
}

impl Emphasis {
    /// The opacity this state is drawn at, from 0 to 1.
    #[must_use]
    pub const fn opacity(self) -> f32 {
        match self {
            Emphasis::Resting => 0.85,
            Emphasis::Hovered | Emphasis::Active => 1.0,
            Emphasis::Receded => 0.25,
        }
    }
}

/// One handle of a gizmo.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Handle {
    /// Its shape.
    pub form: Form,
    /// The axis it acts on, or `None` for a handle that acts on all of them at once.
    pub axis: Option<Axis>,
    /// How prominent it is right now.
    pub emphasis: Emphasis,
}

impl Handle {
    /// The colour to draw it in.
    ///
    /// Axial handles take the axis language; the uniform handle takes the attention hue, because it
    /// belongs to no axis and painting it a fourth colour would be a colour introduced for variety.
    #[must_use]
    pub fn colour(self, theme: Theme) -> Rgb {
        match self.axis {
            Some(axis) => crate::axis::colour(axis, theme),
            None => theme.colour(Semantic::Selection),
        }
    }
}

/// Emphasise the handle under the pointer and recede the rest.
///
/// Takes the whole set and returns the whole set, rather than mutating one handle, because
/// "de-emphasise the rest" is a property of the set: a function that only knew about the hovered
/// handle could not express it.
#[must_use]
pub fn emphasise(handles: &[Handle], under_pointer: Option<usize>, dragging: bool) -> Vec<Handle> {
    handles
        .iter()
        .enumerate()
        .map(|(index, handle)| {
            let emphasis = match (under_pointer, dragging) {
                (Some(hot), true) if hot == index => Emphasis::Active,
                (Some(_), true) => Emphasis::Receded,
                (Some(hot), false) if hot == index => Emphasis::Hovered,
                _ => Emphasis::Resting,
            };
            Handle {
                emphasis,
                ..*handle
            }
        })
        .collect()
}

/// The smallest a handle may be and still be "acquirable without precision", in logical pixels.
///
/// Twelve is the number a pointing device reaches without aiming; it is deliberately larger than the
/// handle would need to be to be *visible*, which is the CAD-application trap the requirement names.
/// The interface scale multiplies it, because a scaled interface is a physically larger display and
/// not a more precise pointer.
pub const MINIMUM_ACQUISITION: f32 = 12.0;

/// How much of the screen a gizmo needs before the universal mode stays individually acquirable.
///
/// A universal gizmo carries eleven handles. Below this extent they overlap, which
/// `editor-visual-language` calls "a defect" and offers one alternative to: degrade to a simpler
/// presentation. That is what [`presentation`] does.
pub const UNIVERSAL_MINIMUM_EXTENT: f32 = 96.0;

/// What is actually drawn for a mode at a given on-screen size.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Presentation {
    /// The mode being drawn, which is not the mode that was asked for when it degraded.
    pub mode: GizmoMode,
    /// The handles.
    pub handles: Vec<Handle>,
    /// Set when the requested mode could not be drawn readably and a simpler one was used.
    ///
    /// Surfaced rather than silent: the viewport says so in its overlay, because a user who asked
    /// for the universal gizmo and got the translate one needs to know why the other handles are
    /// missing.
    pub degraded_from: Option<GizmoMode>,
}

/// The presentation of a gizmo at a given on-screen extent, degrading the universal mode when it
/// would stop being readable.
///
/// "WHEN the universal gizmo is displayed on a small object THEN its handles SHALL remain
/// individually acquirable, **or the mode SHALL degrade to a simpler presentation** rather than
/// overlapping."
#[must_use]
pub fn presentation(mode: GizmoMode, extent: f32) -> Presentation {
    if mode == GizmoMode::Universal && extent < UNIVERSAL_MINIMUM_EXTENT {
        return Presentation {
            mode: GizmoMode::Translate,
            handles: GizmoMode::Translate.handles(),
            degraded_from: Some(GizmoMode::Universal),
        };
    }
    Presentation {
        mode,
        handles: mode.handles(),
        degraded_from: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_mode_is_readable_from_the_shape_alone() {
        // "WHEN a screenshot of the viewport is shown with the toolbar cropped out THEN the active
        // transform mode SHALL be identifiable." The toolbar is cropped by not being passed.
        for mode in GizmoMode::ALL {
            assert_eq!(
                mode_of_forms(&mode.forms()),
                Some(mode),
                "{mode:?} cannot be told apart from another mode by its shapes"
            );
        }
    }

    #[test]
    fn the_three_explicit_modes_share_no_shape() {
        // Colour cannot be doing the work: every mode uses the same three axis hues, so if two
        // modes shared a shape set they would be indistinguishable in a monochrome capture.
        for (index, first) in GizmoMode::EXPLICIT.iter().enumerate() {
            for second in &GizmoMode::EXPLICIT[index + 1..] {
                let overlap: Vec<Form> = first
                    .forms()
                    .into_iter()
                    .filter(|form| second.forms().contains(form))
                    .collect();
                assert!(
                    overlap.is_empty(),
                    "{first:?} and {second:?} share {overlap:?}"
                );
            }
        }
    }

    #[test]
    fn a_universal_gizmo_on_a_small_object_degrades_rather_than_overlapping() {
        let small = presentation(GizmoMode::Universal, UNIVERSAL_MINIMUM_EXTENT - 1.0);
        assert_eq!(small.mode, GizmoMode::Translate);
        assert_eq!(small.degraded_from, Some(GizmoMode::Universal));

        let large = presentation(GizmoMode::Universal, UNIVERSAL_MINIMUM_EXTENT);
        assert_eq!(large.mode, GizmoMode::Universal);
        assert_eq!(large.degraded_from, None);
        assert_eq!(
            large.handles.len(),
            13,
            "four forms over three axes, plus the centre"
        );
    }

    #[test]
    fn the_explicit_modes_never_degrade() {
        // "explicit Move, Rotate and Scale modes SHALL always remain available."
        for mode in GizmoMode::EXPLICIT {
            let presentation = presentation(mode, 1.0);
            assert_eq!(presentation.mode, mode);
            assert_eq!(presentation.degraded_from, None);
        }
    }

    #[test]
    fn dragging_one_handle_recedes_the_others() {
        let handles = GizmoMode::Translate.handles();
        let dragging = emphasise(&handles, Some(0), true);
        assert_eq!(dragging[0].emphasis, Emphasis::Active);
        for handle in &dragging[1..] {
            assert_eq!(handle.emphasis, Emphasis::Receded);
            assert!(handle.emphasis.opacity() < Emphasis::Active.opacity());
        }

        let hovering = emphasise(&handles, Some(1), false);
        assert_eq!(hovering[1].emphasis, Emphasis::Hovered);
        assert_eq!(hovering[0].emphasis, Emphasis::Resting);
    }

    #[test]
    fn every_axial_handle_takes_the_axis_language_and_the_uniform_one_takes_attention() {
        let theme = Theme::default();
        for handle in GizmoMode::Scale.handles() {
            match handle.axis {
                Some(axis) => assert_eq!(handle.colour(theme), crate::axis::colour(axis, theme)),
                None => assert_eq!(handle.colour(theme), theme.colour(Semantic::Selection)),
            }
        }
    }

    #[test]
    fn every_gizmo_form_manipulates_something() {
        // The inverse of the orientation widget's rule, and the reason `manipulates` is on `Form`
        // rather than on the widget: one predicate, two users, no way for them to disagree.
        for mode in GizmoMode::ALL {
            for form in mode.forms() {
                assert!(form.manipulates(), "{form:?} is not a manipulation shape");
            }
        }
    }
}
