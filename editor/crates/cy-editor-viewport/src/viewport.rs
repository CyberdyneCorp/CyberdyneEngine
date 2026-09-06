//! A viewport, and several of them with one focused.
//!
//! `editor-viewport-and-gizmos` — "Multiple viewports and view states":
//!
//! > The editor SHALL support **multiple simultaneous viewports** — perspective and orthographic,
//! > split views, and secondary cameras — each with its own view state, view mode, and visualisation
//! > settings. Each viewport SHALL declare its cost, and the editor SHALL be able to limit rendering
//! > of unfocused or hidden viewports. A viewport SHALL be able to render **through a game camera**
//! > so that composition can be judged with the shipping camera's settings.
//!
//! --- THE ONE THING THIS TYPE ADDS BEYOND HOLDING FIELDS ------------------------------------------------
//!
//! [`Viewport::interaction_view`]. Every interaction — a pick, a gizmo drag, an overlay's alignment —
//! resolves against **the view state of the frame that is on screen**, not against the camera the
//! editor has since moved to. That is the specification's scenario ("the hit SHALL be resolved
//! against the view state of the frame shown, not a newer one"), and it is one function so that
//! there is one place to get it right rather than one per interaction.
//!
//! It matters more than it looks. The editor's camera is free-running and the runtime's frames arrive
//! whenever they arrive; on a remote transport the two are tens of milliseconds apart, and a drag
//! computed against the newer camera moves the object by the camera's motion as well as the cursor's.
//! The symptom is a gizmo that "slides" while the user orbits, and it is unattributable.
//!
//! --- WHAT A VIEWPORT DOES NOT OWN ----------------------------------------------------------------------
//!
//! The selection. `editor-documents-and-transactions` makes selection a service that panels observe,
//! and a viewport that held its own copy would be the second source of truth that requirement exists
//! to prevent. [`crate::picking::apply`] takes the selection as an argument, and the caller is
//! whoever owns the service.
//!
//! The document, likewise. A drag is given one for the length of a call.

use std::collections::BTreeMap;

use crate::budget::{Cadence, ViewportBudget, ViewportCost};
use crate::gizmo::{GizmoMode, GizmoSpace, Pivot};
use crate::navigation::Navigator;
use crate::overlay::{Capture, Overlays};
use crate::picking::{ClickCycle, PickFilter, PickIntent, PickRequest};
use crate::play::{CameraAttachment, Persistence, PlayIndication, PlayState, indication};
use crate::snapping::SnapSettings;
use crate::state::ViewState;
use crate::transport::{Degradation, FrameStream, Transport, TransportKind};

/// A viewport's identity, unique within an editor session.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Default)]
pub struct ViewportId(u64);

impl ViewportId {
    /// The viewport with this number.
    #[must_use]
    pub const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    /// The number, for a message and for a persisted layout.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }
}

impl std::fmt::Display for ViewportId {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "viewport-{}", self.0)
    }
}

/// One viewport: its camera, its image, its overlays and its tools.
pub struct Viewport {
    /// Which one.
    pub id: ViewportId,
    /// What the user calls it.
    pub name: String,
    /// What the editor is asking to see. Its own, per viewport, as the requirement states.
    pub state: ViewState,
    /// How the camera moves.
    pub navigator: Navigator,
    /// What has arrived and how old it is.
    pub stream: FrameStream,
    /// What is drawn over the image.
    pub overlays: Overlays,
    /// The increments in force.
    pub snap: SnapSettings,
    /// Which gizmo is shown.
    pub gizmo_mode: GizmoMode,
    /// The frame its axes are in.
    pub gizmo_space: GizmoSpace,
    /// What manipulation happens about.
    pub gizmo_pivot: Pivot,
    /// How often it asks to be rendered.
    pub cadence: Cadence,
    /// What it may cost.
    pub budget: ViewportBudget,
    /// Where the runtime is.
    pub play: PlayState,
    /// What happens to edits made while it is playing.
    pub persistence: Persistence,
    /// Whether it is looking through a game camera.
    pub attachment: CameraAttachment,
    /// How many times the same spot has been clicked, for cycling.
    pub cycle: ClickCycle,
}

impl Viewport {
    /// A perspective viewport with the default everything.
    #[must_use]
    pub fn new(id: ViewportId, name: impl Into<String>, kind: TransportKind) -> Self {
        Self {
            id,
            name: name.into(),
            state: ViewState::new(),
            navigator: Navigator::new(),
            stream: FrameStream::new(kind),
            overlays: Overlays::default(),
            snap: SnapSettings::default(),
            gizmo_mode: GizmoMode::default(),
            gizmo_space: GizmoSpace::default(),
            gizmo_pivot: Pivot::default(),
            cadence: Cadence::Focused,
            budget: ViewportBudget::default(),
            play: PlayState::default(),
            persistence: Persistence::default(),
            attachment: CameraAttachment::Detached {
                state: Box::new(ViewState::new()),
            },
            cycle: ClickCycle::new(),
        }
    }

    /// Consume whatever the transport has. One call per interface frame; never blocks.
    pub fn pump(&mut self, transport: &mut dyn Transport, now_micros: u64) {
        let _ = self.stream.pump(transport, now_micros);
    }

    /// **The view state every interaction resolves against.**
    ///
    /// The presented frame's, when there is one; the editor's current camera before the first frame
    /// arrives, because a viewport that refused to interact until the runtime answered would be one
    /// that appears frozen while the runtime starts. See the module note for why this is one
    /// function.
    #[must_use]
    pub fn interaction_view(&self) -> &ViewState {
        self.stream
            .latest()
            .map_or(&self.state, |frame| &frame.state)
    }

    /// A pick against the frame the user is looking at.
    ///
    /// `None` before the first frame: there is nothing on screen to have clicked, and a request
    /// naming no frame is one the runtime would have to resolve against its current state — which is
    /// the defect this whole path exists to prevent.
    #[must_use]
    pub fn pick(&self, intent: PickIntent) -> Option<PickRequest> {
        let frame = self.stream.latest()?;
        Some(
            PickRequest::for_frame(self.id, frame.frame, intent)
                .with_filter(PickFilter::from_visibility(&self.state.filter)),
        )
    }

    /// Record a click and return which overlapping candidate it should take.
    pub fn click(&mut self, x: f32, y: f32) -> u32 {
        self.cycle.advance(x, y)
    }

    /// What this viewport costs, for the arbiter and for the statistics overlay.
    #[must_use]
    pub fn cost(&self) -> ViewportCost {
        ViewportCost {
            pixels: self.state.viewport.pixel_count(),
            measured_micros: self.stream.pacing().last_age_micros,
            cadence: self.cadence,
        }
    }

    /// What the viewport should give up to stay inside its budget, and at what resolution.
    #[must_use]
    pub fn degradation(&self) -> (Degradation, f32) {
        self.budget.degrade(self.cost())
    }

    /// What the user is told about the image: staleness, then quality. `None` when there is nothing
    /// to say.
    #[must_use]
    pub fn advisory(&self, now_micros: u64) -> Option<String> {
        self.stream.advisory(now_micros)
    }

    /// What the viewport says about the runtime's state.
    #[must_use]
    pub fn play_indication(&self) -> PlayIndication {
        indication(self.play, self.persistence, &self.attachment)
    }

    /// Look through a game camera, so composition is judged with the shipping camera's settings.
    pub fn attach_to_game_camera(&mut self, camera: u64) {
        self.attachment = CameraAttachment::attached(camera);
    }

    /// Take the editor's own camera, starting from where the view is now. Inspecting a running world
    /// without altering it.
    pub fn detach_camera(&mut self) {
        self.attachment = self.attachment.detach(&self.state);
    }

    /// Capture this observation. Overlays excluded unless asked for — see [`Capture`].
    #[must_use]
    pub fn capture(&self, include_overlays: bool) -> Capture {
        let frame = self.stream.latest().map_or(0, |frame| frame.frame.as_u64());
        if include_overlays {
            Capture::with_overlays(&self.state, &self.overlays, frame)
        } else {
            Capture::clean(&self.state, frame)
        }
    }

    /// Restore a captured observation into this viewport.
    pub fn restore(&mut self, capture: &Capture) -> cy_editor_core::problem::Result<()> {
        self.state = capture.restore()?;
        Ok(())
    }
}

/// Every viewport the editor has open, with one focused.
#[derive(Default)]
pub struct Viewports {
    by_id: BTreeMap<ViewportId, Viewport>,
    focused: Option<ViewportId>,
    next: u64,
}

impl Viewports {
    /// No viewports.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Open one, focusing it when it is the first.
    pub fn open(&mut self, name: impl Into<String>, kind: TransportKind) -> ViewportId {
        self.next += 1;
        let id = ViewportId::from_raw(self.next);
        self.by_id.insert(id, Viewport::new(id, name, kind));
        if self.focused.is_none() {
            self.focus(id);
        } else {
            self.apply_cadences();
        }
        id
    }

    /// Close one. The focus moves to whichever remains, or nowhere.
    pub fn close(&mut self, id: ViewportId) -> bool {
        if self.by_id.remove(&id).is_none() {
            return false;
        }
        if self.focused == Some(id) {
            self.focused = self.by_id.keys().next().copied();
        }
        self.apply_cadences();
        true
    }

    /// The viewport with this identity.
    #[must_use]
    pub fn get(&self, id: ViewportId) -> Option<&Viewport> {
        self.by_id.get(&id)
    }

    /// The viewport with this identity, mutably.
    pub fn get_mut(&mut self, id: ViewportId) -> Option<&mut Viewport> {
        self.by_id.get_mut(&id)
    }

    /// Which one the user is working in.
    #[must_use]
    pub const fn focused(&self) -> Option<ViewportId> {
        self.focused
    }

    /// Work in this one. Every other visible viewport drops to the unfocused cadence, which is where
    /// "the editor SHALL be able to limit rendering of unfocused viewports" actually happens.
    pub fn focus(&mut self, id: ViewportId) {
        if self.by_id.contains_key(&id) {
            self.focused = Some(id);
            self.apply_cadences();
        }
    }

    /// Hide or show one. A hidden viewport is not rendered at all.
    pub fn set_visible(&mut self, id: ViewportId, visible: bool) {
        let focused = self.focused;
        if let Some(viewport) = self.by_id.get_mut(&id) {
            viewport.cadence = cadence_for(visible, focused == Some(id));
        }
    }

    /// Every viewport, in identity order.
    pub fn iter(&self) -> impl Iterator<Item = &Viewport> {
        self.by_id.values()
    }

    /// Every viewport, mutably, in identity order.
    pub fn iter_mut(&mut self) -> impl Iterator<Item = &mut Viewport> {
        self.by_id.values_mut()
    }

    /// How many are open.
    #[must_use]
    pub fn len(&self) -> usize {
        self.by_id.len()
    }

    /// Whether none are open.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.by_id.is_empty()
    }

    /// The viewports the runtime should render this frame, in identity order.
    ///
    /// A hidden viewport is absent, which is the requirement's scenario as a property of the list
    /// rather than of a caller remembering to check.
    #[must_use]
    pub fn render_set(&self) -> Vec<ViewportId> {
        self.by_id
            .values()
            .filter(|viewport| viewport.cadence.should_render())
            .map(|viewport| viewport.id)
            .collect()
    }

    /// What the whole editor is asking the runtime for, as a fraction of a second's rendering.
    #[must_use]
    pub fn total_duty_cycle(&self) -> f32 {
        self.by_id
            .values()
            .map(|viewport| viewport.cost().duty_cycle())
            .sum()
    }

    /// Give the focused viewport the focused cadence and every other visible one the unfocused
    /// cadence, leaving hidden ones hidden.
    fn apply_cadences(&mut self) {
        let focused = self.focused;
        for viewport in self.by_id.values_mut() {
            if viewport.cadence == Cadence::Hidden {
                continue;
            }
            viewport.cadence = cadence_for(true, focused == Some(viewport.id));
        }
    }
}

const fn cadence_for(visible: bool, focused: bool) -> Cadence {
    match (visible, focused) {
        (false, _) => Cadence::Hidden,
        (true, true) => Cadence::Focused,
        (true, false) => Cadence::Unfocused,
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_protocol::FrameId;

    use super::*;
    use crate::math::Vec3;
    use crate::transport::{FrameImage, PresentedFrame};

    fn presented(frame: u64, camera_x: f32, produced_micros: u64) -> PresentedFrame {
        let mut state = ViewState::new();
        state.camera.position = Vec3::new(camera_x, 0.0, 0.0);
        PresentedFrame::new(
            FrameId::from_raw(frame),
            state,
            FrameImage::SharedTexture {
                handle: 1,
                bytes: 0,
            },
            produced_micros,
        )
    }

    #[test]
    fn an_interaction_resolves_against_the_frame_on_screen_not_the_camera_since_moved_to() {
        // THE SCENARIO. The frame was rendered from x = 0; the editor's camera is at x = 500 by the
        // time the user clicks. The pick must be resolved against the first.
        let mut viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::EncodedStream,
        );
        viewport.stream.accept(presented(9, 0.0, 1_000), 2_000);
        viewport.state.camera.position = Vec3::new(500.0, 0.0, 0.0);

        assert!(
            viewport
                .interaction_view()
                .camera
                .position
                .nearly_equals(Vec3::ZERO, 1e-6),
            "the interaction used the editor's newer camera"
        );
        let request = viewport
            .pick(PickIntent::Click { x: 10.0, y: 10.0 })
            .expect("a frame is on screen");
        assert_eq!(request.frame, FrameId::from_raw(9));
    }

    #[test]
    fn before_the_first_frame_there_is_nothing_to_pick_but_the_editor_still_works() {
        let viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::LocalSurface,
        );
        assert!(
            viewport
                .pick(PickIntent::Click { x: 0.0, y: 0.0 })
                .is_none()
        );
        // And the interaction view falls back to the editor's own, so nothing is frozen waiting.
        assert_eq!(
            viewport.interaction_view().camera.position,
            viewport.state.camera.position
        );
    }

    #[test]
    fn focusing_one_viewport_drops_the_others_to_the_unfocused_cadence() {
        let mut viewports = Viewports::new();
        let first = viewports.open("Perspective", TransportKind::SharedTexture);
        let second = viewports.open("Top", TransportKind::SharedTexture);

        assert_eq!(viewports.focused(), Some(first));
        assert_eq!(viewports.get(first).unwrap().cadence, Cadence::Focused);
        assert_eq!(viewports.get(second).unwrap().cadence, Cadence::Unfocused);

        viewports.focus(second);
        assert_eq!(viewports.get(first).unwrap().cadence, Cadence::Unfocused);
        assert_eq!(viewports.get(second).unwrap().cadence, Cadence::Focused);
    }

    #[test]
    fn a_hidden_viewport_is_not_in_the_render_set_and_stays_hidden_through_a_focus_change() {
        let mut viewports = Viewports::new();
        let first = viewports.open("Perspective", TransportKind::SharedTexture);
        let second = viewports.open("Top", TransportKind::SharedTexture);

        viewports.set_visible(second, false);
        assert_eq!(viewports.render_set(), vec![first]);

        // Focusing the other one must not quietly un-hide it.
        viewports.focus(first);
        assert_eq!(viewports.render_set(), vec![first]);
        assert_eq!(viewports.get(second).unwrap().cadence, Cadence::Hidden);

        viewports.set_visible(second, true);
        assert_eq!(viewports.render_set(), vec![first, second]);
    }

    #[test]
    fn several_viewports_each_keep_their_own_view_state() {
        let mut viewports = Viewports::new();
        let perspective = viewports.open("Perspective", TransportKind::SharedTexture);
        let top = viewports.open("Top", TransportKind::SharedTexture);

        viewports.get_mut(top).unwrap().state.projection =
            crate::state::Projection::Orthographic { height: 20.0 };
        viewports.get_mut(top).unwrap().state.view_mode = crate::viewmode::ViewMode::Wireframe;

        assert!(
            viewports
                .get(top)
                .unwrap()
                .state
                .projection
                .is_orthographic()
        );
        assert!(
            !viewports
                .get(perspective)
                .unwrap()
                .state
                .projection
                .is_orthographic(),
            "one viewport's settings reached another"
        );
        assert_eq!(
            viewports.get(perspective).unwrap().state.view_mode,
            crate::viewmode::ViewMode::Off
        );
    }

    #[test]
    fn closing_the_focused_viewport_moves_the_focus_rather_than_losing_it() {
        let mut viewports = Viewports::new();
        let first = viewports.open("Perspective", TransportKind::LocalSurface);
        let second = viewports.open("Top", TransportKind::LocalSurface);

        assert!(viewports.close(first));
        assert_eq!(viewports.focused(), Some(second));
        assert_eq!(viewports.len(), 1);
        assert!(!viewports.close(first), "closing it twice is not an error");

        assert!(viewports.close(second));
        assert_eq!(viewports.focused(), None);
        assert!(viewports.is_empty());
    }

    #[test]
    fn a_viewport_can_look_through_a_game_camera_and_be_taken_off_it() {
        let mut viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::LocalSurface,
        );
        viewport.attach_to_game_camera(77);
        assert!(viewport.play_indication().through_game_camera);

        viewport.detach_camera();
        assert!(!viewport.play_indication().through_game_camera);
        assert!(viewport.attachment.is_detached());
    }

    #[test]
    fn a_capture_from_a_viewport_names_the_frame_it_came_from() {
        let mut viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::EncodedStream,
        );
        viewport.stream.accept(presented(12, 0.0, 0), 500);

        let clean = viewport.capture(false);
        assert_eq!(clean.frame, 12);
        assert!(clean.represents_shipping_image());

        let annotated = viewport.capture(true);
        assert_eq!(annotated.frame, 12);
        assert!(!annotated.represents_shipping_image());
    }

    #[test]
    fn restoring_a_capture_puts_the_camera_back() {
        let mut viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::LocalSurface,
        );
        viewport.state.camera.position = Vec3::new(4.0, 5.0, 6.0);
        let capture = viewport.capture(false);

        viewport.state.camera.position = Vec3::ZERO;
        viewport.restore(&capture).expect("our own capture");
        assert!(
            viewport
                .state
                .camera
                .position
                .nearly_equals(Vec3::new(4.0, 5.0, 6.0), 1e-6)
        );
    }

    #[test]
    fn the_editors_total_demand_is_the_sum_of_what_its_viewports_ask_for() {
        let mut viewports = Viewports::new();
        let first = viewports.open("Perspective", TransportKind::SharedTexture);
        let second = viewports.open("Top", TransportKind::SharedTexture);
        for id in [first, second] {
            viewports
                .get_mut(id)
                .unwrap()
                .stream
                .accept(presented(1, 0.0, 0), 8_000);
        }
        let total = viewports.total_duty_cycle();
        assert!(total > 0.0, "{total}");

        viewports.set_visible(second, false);
        let reduced = viewports.total_duty_cycle();
        assert!(reduced < total, "a hidden viewport still costs: {reduced}");
    }
}
