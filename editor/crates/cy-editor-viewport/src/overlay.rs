//! Overlays, viewport controls, the orientation widget, and what a capture contains.
//!
//! `editor-viewport-and-gizmos` — "Overlays and in-viewport interfaces":
//!
//! > The viewport SHALL support overlays ... which SHALL be individually toggleable and SHALL not be
//! > baked into the rendered image used for judging appearance. Overlays SHALL be **excluded from any
//! > capture intended to represent the shipping image**, and a capture SHALL state whether overlays
//! > were included.
//!
//! > Viewport **controls** ... SHALL likewise be presented as overlays within the viewport rather
//! > than as an additional toolbar occupying vertical space above it.
//!
//! > The **view-orientation widget** is an overlay, not a manipulator: it presents camera and world
//! > orientation, SHALL show only the three principal axes, and SHALL NOT carry rotation rings, scale
//! > handles, or translation arrows that would make it read as a transform gizmo.
//!
//! --- THREE REQUIREMENTS, THREE THINGS MADE STRUCTURAL ------------------------------------------------
//!
//! **A capture says what it contains.** [`Capture`] has no constructor that omits the answer, and
//! [`Capture::represents_shipping_image`] is what a reader asks rather than inferring it. A capture
//! that included overlays and did not say so is the one that ends up in a bug report as evidence of a
//! rendering defect that is a statistics overlay.
//!
//! **Controls do not take height.** [`Overlays::content_rect`] returns the viewport rect it was given,
//! unchanged, whatever is enabled. There is no code path that subtracts a toolbar's height, so
//! "WHEN viewport controls are added or extended THEN the rendered viewport area SHALL NOT shrink" is
//! a property of there being nothing to subtract with.
//!
//! **The orientation widget cannot become a gizmo.** [`OrientationPart`] has three variants, all of
//! them axes, and [`OrientationWidget::activate`] takes a [`ViewState`] and returns nothing else — it
//! has no document, no node and no transform, so a drag on it can only move the camera. The
//! specification says this is stated as a prohibition because it is violated by accident, and the
//! accident is exactly the one where somebody adds a ring "so it can be turned freely".

use std::collections::BTreeSet;

use crate::navigation::{Navigator, ViewAxis};
use crate::state::ViewState;
use crate::state::ViewportRect;

/// Something drawn over the image rather than in it.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Overlay {
    /// Distances and dimensions the user measured.
    Measurement,
    /// Frame time, draw calls, triangle counts.
    Statistics,
    /// Title and action safe areas.
    SafeFrames,
    /// Thirds, golden ratio, centre lines.
    CompositionGuides,
    /// Focal length, exposure, aperture of the camera being looked through.
    CameraInformation,
    /// The cursor's world position and the camera's.
    CoordinateReadout,
    /// Names floating over objects.
    ObjectLabels,
    /// Projection, show flags, camera speed, snapping, transform mode, maximise, debug view —
    /// **an overlay, not a toolbar**. See the module note.
    Controls,
    /// The three principal axes and where the camera is relative to them.
    Orientation,
}

impl Overlay {
    /// Every overlay, for a settings panel and for the test that checks each can be toggled.
    #[must_use]
    pub const fn all() -> [Self; 9] {
        [
            Overlay::Measurement,
            Overlay::Statistics,
            Overlay::SafeFrames,
            Overlay::CompositionGuides,
            Overlay::CameraInformation,
            Overlay::CoordinateReadout,
            Overlay::ObjectLabels,
            Overlay::Controls,
            Overlay::Orientation,
        ]
    }

    /// A name for a menu.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Overlay::Measurement => "Measurement",
            Overlay::Statistics => "Statistics",
            Overlay::SafeFrames => "Safe Frames",
            Overlay::CompositionGuides => "Composition Guides",
            Overlay::CameraInformation => "Camera Information",
            Overlay::CoordinateReadout => "Coordinate Readout",
            Overlay::ObjectLabels => "Object Labels",
            Overlay::Controls => "Viewport Controls",
            Overlay::Orientation => "Orientation Widget",
        }
    }

    /// Whether this overlay may sit over the middle of the image.
    ///
    /// "In-viewport controls SHALL not obstruct the content being judged, and SHALL be dismissible."
    /// The ones that must not are the ones a person would drag over the subject; the composition
    /// guides and safe frames are drawn across the whole image by definition and are what somebody
    /// enabled them for.
    #[must_use]
    pub const fn obstructs_content(self) -> bool {
        matches!(
            self,
            Overlay::Controls | Overlay::Orientation | Overlay::Statistics
        )
    }
}

/// Which overlays are on.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Overlays {
    enabled: BTreeSet<Overlay>,
}

impl Default for Overlays {
    fn default() -> Self {
        let mut enabled = BTreeSet::new();
        enabled.insert(Overlay::Controls);
        enabled.insert(Overlay::Orientation);
        Self { enabled }
    }
}

impl Overlays {
    /// Nothing enabled at all, which is what a capture uses.
    #[must_use]
    pub fn none() -> Self {
        Self {
            enabled: BTreeSet::new(),
        }
    }

    /// Whether one is on.
    #[must_use]
    pub fn is_enabled(&self, overlay: Overlay) -> bool {
        self.enabled.contains(&overlay)
    }

    /// Turn one on or off. Individually, which is the requirement.
    pub fn set(&mut self, overlay: Overlay, enabled: bool) {
        if enabled {
            self.enabled.insert(overlay);
        } else {
            self.enabled.remove(&overlay);
        }
    }

    /// Turn one from on to off or back.
    pub fn toggle(&mut self, overlay: Overlay) {
        let enabled = self.is_enabled(overlay);
        self.set(overlay, !enabled);
    }

    /// Dismiss everything drawn over the middle of the image, leaving the rest.
    ///
    /// What "dismissible" means as one action: a user judging a shot wants the controls and the
    /// widget gone without losing the safe frames they are judging against.
    pub fn dismiss_obstructions(&mut self) {
        self.enabled.retain(|overlay| !overlay.obstructs_content());
    }

    /// Everything on, in a stable order.
    #[must_use]
    pub fn active(&self) -> Vec<Overlay> {
        self.enabled.iter().copied().collect()
    }

    /// The area the image is drawn in.
    ///
    /// **The whole viewport, always.** Overlays are drawn over it and controls are overlays, so
    /// there is nothing to subtract — which is how "the rendered viewport area SHALL NOT shrink" is
    /// enforced rather than remembered. The function exists so that a panel asks it rather than
    /// computing a rect of its own with a toolbar height in it.
    #[must_use]
    pub const fn content_rect(&self, viewport: ViewportRect) -> ViewportRect {
        viewport
    }
}

/// A part of the view-orientation widget.
///
/// Three variants, all axes. There is deliberately no `RotationRing`, no `ScaleHandle` and no
/// `TranslationArrow`: adding one is the change the requirement forbids, and it is a change to this
/// enum rather than a change to a drawing routine somebody has to notice.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum OrientationPart {
    /// The first principal axis, positive and negative.
    AxisX,
    /// The second.
    AxisY,
    /// The third.
    AxisZ,
}

impl OrientationPart {
    /// The three parts. Three, and the test asserts it is three.
    #[must_use]
    pub const fn all() -> [Self; 3] {
        [
            OrientationPart::AxisX,
            OrientationPart::AxisY,
            OrientationPart::AxisZ,
        ]
    }

    /// The two views this axis's ends correspond to: positive first.
    #[must_use]
    pub const fn views(self) -> (ViewAxis, ViewAxis) {
        match self {
            OrientationPart::AxisX => (ViewAxis::Left, ViewAxis::Right),
            OrientationPart::AxisY => (ViewAxis::Top, ViewAxis::Bottom),
            OrientationPart::AxisZ => (ViewAxis::Back, ViewAxis::Front),
        }
    }
}

/// The view-orientation widget: an overlay that presents orientation and moves the camera.
///
/// It holds no state of its own. That is not minimalism — it is what makes the prohibition
/// enforceable: a widget with no document, no selection and no transform binding has nothing it
/// could edit even if somebody added a drag handler to it.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct OrientationWidget;

impl OrientationWidget {
    /// The parts it draws. Only the three principal axes.
    #[must_use]
    pub const fn parts(self) -> [OrientationPart; 3] {
        OrientationPart::all()
    }

    /// Look along one of the widget's axis ends.
    ///
    /// **Moves the camera and nothing else.** The signature is the proof: a [`ViewState`] and a
    /// [`Navigator`], no document, no node, no transform. "WHEN a user drags the view-orientation
    /// widget THEN the camera SHALL orient, and no object transform SHALL change" is not a
    /// behaviour that is tested here so much as one that cannot be written otherwise.
    pub fn activate(self, navigator: &Navigator, state: &mut ViewState, axis: ViewAxis) {
        navigator.snap_to_axis(state, axis);
    }

    /// Perform one of the widget's gestures.
    ///
    /// **The signature is the requirement.** A [`Navigator`], a [`ViewState`] and nothing else: no
    /// document, no selection, no transform binding, no actor. `docs/design/images/scene-orientation-
    /// gizmo.png` puts four interactions on this widget — click an axis, drag to orbit, scroll to
    /// zoom, modifier-drag to pan — and every one of them is a camera move, so *none* of them can
    /// open a transaction. That is why this returns a [`ViewPreset`] rather than a `Result`: there is
    /// no failure a camera move can have, and nothing for a caller to record.
    pub fn perform(
        self,
        navigator: &mut Navigator,
        state: &mut ViewState,
        gesture: WidgetGesture,
    ) -> ViewPreset {
        match gesture {
            WidgetGesture::Choose(preset) => preset.apply(navigator, state),
            WidgetGesture::ClickAxis(axis) => {
                navigator.snap_to_axis(state, axis);
            }
            WidgetGesture::Cycle { forward } => {
                let next = if forward {
                    ViewPreset::of_view(state).next()
                } else {
                    ViewPreset::of_view(state).previous()
                };
                next.apply(navigator, state);
            }
            WidgetGesture::Orbit { dx, dy } => navigator.orbit(state, dx, dy),
            WidgetGesture::Pan { dx, dy } => navigator.pan(state, dx, dy),
            WidgetGesture::Zoom { notches } => navigator.zoom(state, notches),
        }
        ViewPreset::of_view(state)
    }
}

/// One of the seven views the widget offers.
///
/// Seven, from the reference: perspective, top, bottom, front, back, left and right. Six of them are
/// [`ViewAxis`] under another name and the seventh is the absence of one, which is why this is an
/// enumeration of its own rather than an `Option<ViewAxis>` — the widget shows *the current view as
/// cycleable text*, and "Persp" has to be one of the values that text can take.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum ViewPreset {
    /// A three-quarter perspective view. The one a scene is authored from.
    #[default]
    Perspective,
    /// Looking down.
    Top,
    /// Looking up.
    Bottom,
    /// Looking along −Z.
    Front,
    /// Looking along +Z.
    Back,
    /// Looking along +X.
    Left,
    /// Looking along −X.
    Right,
}

impl ViewPreset {
    /// The seven, in the order the widget's list shows them.
    pub const ALL: [ViewPreset; 7] = [
        ViewPreset::Perspective,
        ViewPreset::Top,
        ViewPreset::Bottom,
        ViewPreset::Front,
        ViewPreset::Back,
        ViewPreset::Left,
        ViewPreset::Right,
    ];

    /// The short text the widget draws under the cube — "Persp", "Top".
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            ViewPreset::Perspective => "Persp",
            ViewPreset::Top => "Top",
            ViewPreset::Bottom => "Bottom",
            ViewPreset::Front => "Front",
            ViewPreset::Back => "Back",
            ViewPreset::Left => "Left",
            ViewPreset::Right => "Right",
        }
    }

    /// The identifier a command and a keymap use.
    #[must_use]
    pub const fn id(self) -> &'static str {
        match self {
            ViewPreset::Perspective => "perspective",
            ViewPreset::Top => "top",
            ViewPreset::Bottom => "bottom",
            ViewPreset::Front => "front",
            ViewPreset::Back => "back",
            ViewPreset::Left => "left",
            ViewPreset::Right => "right",
        }
    }

    /// The preset with this identifier.
    #[must_use]
    pub fn of_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|preset| preset.id() == id)
    }

    /// The axis this preset looks along, or `None` for the perspective view.
    #[must_use]
    pub const fn axis(self) -> Option<ViewAxis> {
        match self {
            ViewPreset::Perspective => None,
            ViewPreset::Top => Some(ViewAxis::Top),
            ViewPreset::Bottom => Some(ViewAxis::Bottom),
            ViewPreset::Front => Some(ViewAxis::Front),
            ViewPreset::Back => Some(ViewAxis::Back),
            ViewPreset::Left => Some(ViewAxis::Left),
            ViewPreset::Right => Some(ViewAxis::Right),
        }
    }

    /// The next in the list, wrapping. What the `›` beside the view text does.
    #[must_use]
    pub fn next(self) -> Self {
        let index = Self::ALL
            .iter()
            .position(|preset| *preset == self)
            .unwrap_or(0);
        Self::ALL[(index + 1) % Self::ALL.len()]
    }

    /// The previous, wrapping.
    #[must_use]
    pub fn previous(self) -> Self {
        let index = Self::ALL
            .iter()
            .position(|preset| *preset == self)
            .unwrap_or(0);
        Self::ALL[(index + Self::ALL.len() - 1) % Self::ALL.len()]
    }

    /// Point the camera at this view.
    ///
    /// **The projection is not changed.** Every other editor switches to orthographic on an axis view
    /// and it is the one thing users complain about afterwards, because the switch is invisible until
    /// something looks wrong — and because judging a scene's appearance through a projection the game
    /// will never use is exactly what the viewport exists not to do. Projection is its own control,
    /// in the viewport's own chrome, and it stays where the user put it.
    pub fn apply(self, navigator: &Navigator, state: &mut ViewState) {
        if let Some(axis) = self.axis() {
            navigator.snap_to_axis(state, axis);
        } else {
            // The three-quarter view a scene is authored from, at the distance the camera is already
            // at, so that "back to perspective" does not also mean "somewhere else".
            let pivot = navigator.pivot(state);
            let distance = navigator.pivot_distance();
            let direction = crate::math::Vec3::new(0.559, 0.408, 0.722);
            state.camera.position = pivot + direction * distance;
            state.camera.rotation = crate::navigation::look_along(-direction);
        }
    }

    /// Which preset the camera is currently in, for the widget's text.
    ///
    /// A view is one of the six axis views only when it is *exactly* one — within a degree — because
    /// text that said "Top" for a view a degree off top would be a widget that lies about the one
    /// thing it exists to report.
    #[must_use]
    pub fn of_view(state: &ViewState) -> Self {
        let forward = state.camera.forward();
        for preset in Self::ALL {
            let Some(axis) = preset.axis() else {
                continue;
            };
            if forward.dot(axis.forward()) > AXIS_VIEW_COSINE {
                return preset;
            }
        }
        ViewPreset::Perspective
    }
}

/// How near an axis a view must be to be reported as that axis view: one degree.
const AXIS_VIEW_COSINE: f32 = 0.999_847_7;

/// Something a person did to the orientation widget.
///
/// A closed set of five, all of them camera moves. There is no variant that names an object, and a
/// sixth that did would have to explain why the widget the specification calls "not a manipulator" is
/// manipulating something.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum WidgetGesture {
    /// A click on one of the axis stubs: snap the camera to that view.
    ClickAxis(ViewAxis),
    /// A choice from the preset list, or a click on the view text's arrows.
    Choose(ViewPreset),
    /// The `‹` and `›` beside the current view text.
    Cycle {
        /// Forward through [`ViewPreset::ALL`], or backward.
        forward: bool,
    },
    /// A drag anywhere on the widget: orbit.
    Orbit {
        /// Pixels moved horizontally.
        dx: f32,
        /// And vertically.
        dy: f32,
    },
    /// A modifier-drag: pan.
    Pan {
        /// Pixels moved horizontally.
        dx: f32,
        /// And vertically.
        dy: f32,
    },
    /// The wheel over the widget: zoom.
    Zoom {
        /// Wheel notches, positive toward the subject.
        notches: f32,
    },
}

/// How prominent the widget is right now.
///
/// Four, from the reference's "Visual States" row. `Disabled` is the one an implementation forgets,
/// and it is the one that matters most: a viewport looking through a game camera during play has an
/// orientation widget that cannot move the camera, and a widget that looked live and did nothing
/// would be worse than one that says so.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum WidgetState {
    /// Nothing is happening to it.
    #[default]
    Normal,
    /// The pointer is over it, or over one of its stubs.
    Hovered,
    /// It is being dragged, or a stub is being pressed.
    Active,
    /// The camera it would move is not the editor's to move.
    Disabled,
}

impl WidgetState {
    /// Every state, for a theme check that draws all four.
    pub const ALL: [WidgetState; 4] = [
        WidgetState::Normal,
        WidgetState::Hovered,
        WidgetState::Active,
        WidgetState::Disabled,
    ];

    /// Whether a gesture is accepted in this state.
    #[must_use]
    pub const fn accepts_input(self) -> bool {
        !matches!(self, WidgetState::Disabled)
    }
}

/// The smallest the widget may be drawn, in logical pixels.
pub const WIDGET_MINIMUM_SIZE: f32 = 56.0;

/// The largest.
pub const WIDGET_MAXIMUM_SIZE: f32 = 96.0;

/// The size it is drawn at unless somebody has changed it.
pub const WIDGET_DEFAULT_SIZE: f32 = 72.0;

/// The range is a range, checked where it is declared rather than in a test that could be deleted.
const _: () = assert!(WIDGET_MINIMUM_SIZE <= WIDGET_DEFAULT_SIZE);
const _: () = assert!(WIDGET_DEFAULT_SIZE <= WIDGET_MAXIMUM_SIZE);

/// A widget size inside the range the visual language fixes.
///
/// **Constant screen size**, so this is a number of logical pixels rather than a factor of anything:
/// a widget that scaled with the camera would be hardest to read exactly when orientation is hardest
/// to judge, which is the same failure the transform gizmo's screen-constant sizing prevents.
#[must_use]
pub fn widget_size(requested: f32) -> f32 {
    requested.clamp(WIDGET_MINIMUM_SIZE, WIDGET_MAXIMUM_SIZE)
}

/// A viewport image captured for reference or for a defect report.
///
/// The view state is carried as bytes so that a capture can go into a trace, a report or a file
/// without anything in the path having to know what a view state is — see
/// [`crate::state::ViewState::encode`].
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Capture {
    /// The view state, encoded. Restoring it reproduces the observation exactly.
    pub view_state: Vec<u8>,
    /// **Whether overlays were included.** A capture that did not say would be evidence of a
    /// rendering defect that is a statistics overlay.
    pub overlays_included: bool,
    /// Which overlays were on, when they were included. Empty otherwise, and the emptiness is a
    /// consequence of the flag rather than a second thing to keep in step.
    pub overlays: Vec<Overlay>,
    /// The frame identifier the image came from, so the capture and the runtime's own trace can be
    /// lined up.
    pub frame: u64,
}

impl Capture {
    /// A capture of the shipping image: no overlays, whatever is on screen.
    ///
    /// The default, and the one with the shorter name, because it is the one that is almost always
    /// wanted and the other one has to be asked for.
    #[must_use]
    pub fn clean(state: &ViewState, frame: u64) -> Self {
        Self {
            view_state: state.encode(),
            overlays_included: false,
            overlays: Vec::new(),
            frame,
        }
    }

    /// A capture with the overlays in it, for showing somebody what the editor looked like.
    #[must_use]
    pub fn with_overlays(state: &ViewState, overlays: &Overlays, frame: u64) -> Self {
        Self {
            view_state: state.encode(),
            overlays_included: true,
            overlays: overlays.active(),
            frame,
        }
    }

    /// Whether this image may be presented as what the project looks like.
    #[must_use]
    pub const fn represents_shipping_image(&self) -> bool {
        !self.overlays_included
    }

    /// One line saying what this is, for the caption a report shows.
    #[must_use]
    pub fn caption(&self) -> String {
        if self.overlays_included {
            format!(
                "Frame {} with {} overlay(s); not representative of the shipping image.",
                self.frame,
                self.overlays.len()
            )
        } else {
            format!("Frame {}, no overlays.", self.frame)
        }
    }

    /// Restore the view this capture was taken from.
    pub fn restore(&self) -> cy_editor_core::problem::Result<ViewState> {
        ViewState::decode(&self.view_state)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::math::Vec3;

    /// A document with one node, to prove a widget gesture cannot touch one.
    fn a_document_with_a_node() -> (cy_editor_documents::Document, cy_editor_core::ids::NodeId) {
        use cy_editor_core::Actor;
        let mut document = cy_editor_documents::Document::new("worlds/city.cyworld");
        let node = document
            .with_transaction("Populate", Actor::human("designer"), |document| {
                document.create_node(None)
            })
            .expect("a node");
        (document, node)
    }

    #[test]
    fn the_widget_offers_seven_presets_and_cycles_through_all_of_them() {
        // `docs/design/images/scene-orientation-gizmo.png`: "Seven presets — perspective, top,
        // bottom, front, back, left, right — and the current view shown as cycleable text."
        assert_eq!(ViewPreset::ALL.len(), 7);
        let mut seen = std::collections::BTreeSet::new();
        let mut preset = ViewPreset::default();
        for _ in 0..ViewPreset::ALL.len() {
            seen.insert(preset.label());
            preset = preset.next();
        }
        assert_eq!(seen.len(), 7, "cycling forward does not reach every view");
        assert_eq!(preset, ViewPreset::default(), "cycling does not wrap");
        for preset in ViewPreset::ALL {
            assert_eq!(preset.next().previous(), preset);
            assert_eq!(ViewPreset::of_id(preset.id()), Some(preset));
        }
    }

    #[test]
    fn clicking_a_preset_points_the_camera_and_the_text_then_reads_that_preset() {
        let navigator = Navigator::new();
        let mut state = ViewState::new();
        for preset in ViewPreset::ALL {
            preset.apply(&navigator, &mut state);
            assert_eq!(
                ViewPreset::of_view(&state),
                preset,
                "after choosing {} the widget reads {}",
                preset.label(),
                ViewPreset::of_view(&state).label()
            );
        }
        // And a view a degree off an axis is honestly reported as perspective rather than as the
        // axis it nearly is.
        ViewPreset::Top.apply(&navigator, &mut state);
        // Vertically: orbiting horizontally from a top view turns about the axis the camera is
        // already looking down, which changes nothing — the one case a test would pass by accident.
        navigator.orbit(&mut state, 0.0, 40.0);
        assert_eq!(ViewPreset::of_view(&state), ViewPreset::Perspective);
    }

    #[test]
    fn every_widget_gesture_moves_the_camera_and_none_of_them_touches_a_document() {
        // "Dragging the widget orbits the camera, never the selection, and produces no transaction."
        // The proof is structural — `perform` has no document — so the test drives every gesture and
        // then asserts the document is untouched and no transaction was left open.
        let (document, _node) = a_document_with_a_node();
        let revision = document.revision();
        let entries = document.history().entries().len();

        let widget = OrientationWidget;
        let mut navigator = Navigator::new();
        let mut state = ViewState::new();
        let before = state.camera;
        for gesture in [
            WidgetGesture::ClickAxis(ViewAxis::Top),
            WidgetGesture::Choose(ViewPreset::Front),
            WidgetGesture::Cycle { forward: true },
            WidgetGesture::Cycle { forward: false },
            WidgetGesture::Orbit {
                dx: 30.0,
                dy: -12.0,
            },
            WidgetGesture::Pan { dx: 8.0, dy: 4.0 },
            WidgetGesture::Zoom { notches: 2.0 },
        ] {
            let reported = widget.perform(&mut navigator, &mut state, gesture);
            assert_eq!(reported, ViewPreset::of_view(&state));
        }
        assert_ne!(
            before.position.to_array().map(f32::to_bits),
            state.camera.position.to_array().map(f32::to_bits),
            "the camera did not move"
        );
        assert_eq!(
            document.revision(),
            revision,
            "a gesture changed a document"
        );
        assert_eq!(document.history().entries().len(), entries);
        assert!(!document.is_transaction_open());
    }

    #[test]
    fn the_widget_is_drawn_between_fifty_six_and_ninety_six_pixels_and_defaults_to_seventy_two() {
        assert_eq!(
            widget_size(WIDGET_DEFAULT_SIZE).to_bits(),
            72.0_f32.to_bits()
        );
        assert_eq!(widget_size(10.0).to_bits(), WIDGET_MINIMUM_SIZE.to_bits());
        assert_eq!(widget_size(400.0).to_bits(), WIDGET_MAXIMUM_SIZE.to_bits());
    }

    #[test]
    fn a_disabled_widget_refuses_input_and_the_other_three_states_accept_it() {
        assert_eq!(WidgetState::ALL.len(), 4);
        assert!(!WidgetState::Disabled.accepts_input());
        for state in [
            WidgetState::Normal,
            WidgetState::Hovered,
            WidgetState::Active,
        ] {
            assert!(state.accepts_input(), "{state:?}");
        }
    }

    #[test]
    fn every_overlay_is_individually_toggleable() {
        let mut overlays = Overlays::none();
        for overlay in Overlay::all() {
            assert!(!overlays.is_enabled(overlay));
            overlays.toggle(overlay);
            assert!(overlays.is_enabled(overlay), "{}", overlay.name());
            overlays.toggle(overlay);
            assert!(!overlays.is_enabled(overlay));
        }
    }

    #[test]
    fn controls_do_not_consume_viewport_height() {
        // "WHEN viewport controls are added or extended THEN they SHALL be placed as overlays, and
        // the rendered viewport area SHALL NOT shrink."
        let rect = ViewportRect {
            x: 0,
            y: 0,
            width: 1600,
            height: 900,
        };
        let mut overlays = Overlays::none();
        let bare = overlays.content_rect(rect);
        for overlay in Overlay::all() {
            overlays.set(overlay, true);
        }
        assert_eq!(overlays.content_rect(rect), bare);
        assert_eq!(overlays.content_rect(rect).height, 900);
    }

    #[test]
    fn dismissing_obstructions_leaves_what_is_being_judged_against() {
        let mut overlays = Overlays::default();
        overlays.set(Overlay::SafeFrames, true);
        overlays.set(Overlay::Statistics, true);
        overlays.dismiss_obstructions();

        assert!(overlays.is_enabled(Overlay::SafeFrames));
        assert!(!overlays.is_enabled(Overlay::Controls));
        assert!(!overlays.is_enabled(Overlay::Statistics));
        assert!(!overlays.is_enabled(Overlay::Orientation));
    }

    #[test]
    fn the_orientation_widget_shows_three_axes_and_nothing_that_reads_as_a_gizmo() {
        // The prohibition, as a property of the type. A rotation ring, a scale handle or a
        // translation arrow would have to be a fourth variant of `OrientationPart`, which is a
        // change to this assertion rather than a change nobody notices.
        let widget = OrientationWidget;
        assert_eq!(widget.parts().len(), 3);
        let views: Vec<ViewAxis> = widget
            .parts()
            .iter()
            .flat_map(|part| {
                let (positive, negative) = part.views();
                [positive, negative]
            })
            .collect();
        assert_eq!(views.len(), 6, "two ends per axis, and no other parts");
    }

    #[test]
    fn dragging_the_orientation_widget_orients_the_camera_and_nothing_else() {
        // The signature is the argument; this checks the behaviour it permits.
        let navigator = Navigator::new();
        let mut state = ViewState::new();
        state.camera.position = Vec3::new(0.0, 0.0, 10.0);
        let before = state.camera.position;

        OrientationWidget.activate(&navigator, &mut state, ViewAxis::Top);
        assert!(state.camera.forward().nearly_equals(-Vec3::Y, 1e-4));
        assert_ne!(
            state.camera.position, before,
            "the camera moved to look down"
        );
    }

    #[test]
    fn a_capture_says_whether_overlays_were_in_it() {
        let state = ViewState::new();
        let overlays = Overlays::default();

        let clean = Capture::clean(&state, 42);
        assert!(clean.represents_shipping_image());
        assert!(clean.overlays.is_empty());
        assert!(
            clean.caption().contains("no overlays"),
            "{}",
            clean.caption()
        );

        let annotated = Capture::with_overlays(&state, &overlays, 42);
        assert!(!annotated.represents_shipping_image());
        assert!(!annotated.overlays.is_empty());
        assert!(
            annotated.caption().contains("not representative"),
            "{}",
            annotated.caption()
        );
    }

    #[test]
    fn a_capture_restores_the_view_it_was_taken_from() {
        let mut state = ViewState::new();
        state.camera.position = Vec3::new(3.0, 4.0, 5.0);
        let capture = Capture::clean(&state, 7);
        assert_eq!(capture.restore().expect("our own capture"), state);
    }
}
