//! What a pointer and a keyboard do to a viewport. Tasks 2.2, 2.3, 2.4.
//!
//! --- WHY THIS EXISTS RATHER THAN LIVING IN THE PANEL ------------------------------------------------
//!
//! The window's viewport panel is thirty lines of toolkit calls and this is everything they mean.
//! That split is not tidiness: `editor-rust-application` requires every model to be **testable
//! headlessly**, and viewport interaction is the part of an editor where the interesting failures
//! live — a click resolved against the wrong frame, a drag that accumulates, a modifier that changes
//! meaning mid-gesture, a camera that moves during a manipulation. Every one of those is a test in
//! this file, and none of them would be reachable if the logic were inside an `egui` closure.
//!
//! The panel's whole job is to translate the toolkit's events into [`Event`] and to draw what
//! [`Interaction`] reports. There is no decision left in it.
//!
//! --- THE PRECEDENCE, WHICH IS THE ONLY REAL DESIGN DECISION HERE ------------------------------------
//!
//! A press has to mean one of three things, and the order is:
//!
//! 1. **A gizmo handle**, when the runtime's published layout says one is under the cursor. This wins
//!    over navigation deliberately: `Alt`+drag is "orbit" in the Maya preset and "duplicate and
//!    transform" on a handle, and a user pressing on an arrow means the arrow.
//! 2. **A navigation gesture**, when the button and modifiers are bound to one.
//! 3. **A selection**, otherwise — resolved on *release*, so that a press-and-drag is a rectangle
//!    rather than a click that happens to have moved.
//!
//! --- THE VIEW STATE EVERY GESTURE RESOLVES AGAINST --------------------------------------------------
//!
//! [`crate::viewport::Viewport::interaction_view`], never `Viewport::state`. The editor's camera and
//! the frame on screen are tens of milliseconds apart on any transport worth having, and a drag
//! computed against the newer camera moves the object by the camera's motion as well as the
//! cursor's. There is one call to it, in [`Interaction::begin_drag`], and the drag keeps the copy it
//! captured for its whole life.
//!
//! --- WHAT HAPPENS WITH NO RUNTIME -------------------------------------------------------------------
//!
//! Everything that is the editor's own still works: the camera moves, the orientation widget works,
//! the numeric fields work, the overlays work. What needs the engine reports that it needs the
//! engine — a click with no presented frame is [`Outcome::NothingToPick`], and a press where a gizmo
//! would be is a press on nothing, because the gizmo is drawn by the runtime and the runtime is not
//! there. That is the same rule the viewport image follows and for the same reason: an editor that
//! invented a plausible answer here would be an editor whose picking disagrees with its picture.

use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::Problem;
use cy_editor_documents::Document;

use crate::gizmo::{
    AxisLock, Drag, DragInput, DragRequest, Feedback, GizmoRegistry, Handle, TransformBinding,
};
use crate::layout::GizmoLayout;
use crate::navigation::{Bindings, Button, Gesture, Modifiers};
use crate::picking::{PickIntent, PickRequest, SelectionMode};
use crate::viewport::Viewport;

/// How far the pointer may move between press and release and still be a click, in pixels.
///
/// Beyond it the gesture is a rectangle selection. Three is the number a hand holding a mouse
/// produces; zero would make a click a thing only a trackball can perform.
pub const CLICK_SLOP: f32 = 3.0;

/// Something the toolkit reports.
///
/// Deliberately not an `egui` type, a `winit` type or anything else with a version number: this
/// crate is layer 2 and names no toolkit, and the translation is six lines in the panel that does.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Event {
    /// The pointer moved to a pixel inside the viewport.
    PointerMoved {
        /// Pixels from the viewport's left edge.
        x: f32,
        /// From its top edge.
        y: f32,
    },
    /// A button went down.
    PointerDown {
        /// Which button.
        button: Button,
        /// Where.
        x: f32,
        /// Where.
        y: f32,
        /// What was held.
        modifiers: Modifiers,
    },
    /// A button came up.
    PointerUp {
        /// Which button.
        button: Button,
        /// Where.
        x: f32,
        /// Where.
        y: f32,
        /// What was held.
        modifiers: Modifiers,
    },
    /// The wheel turned.
    Wheel {
        /// Notches, positive toward the subject.
        notches: f32,
        /// What was held.
        modifiers: Modifiers,
    },
    /// A key that means something to a viewport went down.
    Key(Key),
    /// The pointer left the viewport, or the window lost focus.
    PointerLeft,
}

/// The keys a viewport interprets itself.
///
/// `W`/`E`/`R`/`T` are **not** here: they are commands in the registry, bound in the keymap, and
/// reachable from the palette and from an agent. A viewport that handled them privately would be the
/// "action reachable only through a specific widget" `editor-rust-application` calls a defect.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Key {
    /// `X` — lock the manipulation to the world X axis, or release it.
    LockX,
    /// `Y`.
    LockY,
    /// `Z`.
    LockZ,
    /// `Escape` — abandon the manipulation and put everything back.
    Cancel,
}

/// What one event did.
#[derive(Clone, PartialEq, Debug)]
pub enum Outcome {
    /// Nothing that concerns the caller.
    Nothing,
    /// The camera moved. No document changed and nothing needs recording.
    CameraMoved,
    /// The handle under the pointer changed, so the runtime's gizmo highlight should follow.
    HoverChanged(Option<Handle>),
    /// A manipulation began on a handle.
    DragBegan(Handle),
    /// A manipulation advanced, with the numbers to show while it does.
    DragAdvanced(Feedback),
    /// A manipulation finished. `true` when it recorded a history entry — a drag that ended where it
    /// began records nothing, which is not a failure.
    DragFinished {
        /// Whether an entry was recorded.
        recorded: bool,
    },
    /// A manipulation was abandoned and the document is as it was.
    DragCancelled,
    /// Send this to the runtime; its answer resolves into a selection.
    Pick(Box<PickRequest>, SelectionMode),
    /// A click arrived before any frame did, so there is nothing on screen to have clicked.
    NothingToPick,
    /// Something could not be done, with the reason and the remedy.
    Refused(Box<Problem>),
}

/// The things an interaction needs that it does not own.
///
/// Passed per event rather than held, because every one of them belongs to somebody else: the
/// document is the document service's, the selection is the selection service's, and a viewport
/// interaction that held a `&mut Document` across frames would be holding the editor's state
/// hostage to the pointer.
pub struct Context<'a> {
    /// The viewport being interacted with.
    pub viewport: &'a mut Viewport,
    /// The document the manipulated objects live in.
    pub document: &'a mut Document,
    /// The manipulators available, including any a plugin registered.
    pub registry: &'a GizmoRegistry,
    /// Which fields hold a transform, when the document describes one.
    pub binding: Option<TransformBinding>,
    /// What is selected, in selection order.
    pub nodes: &'a [NodeId],
    /// Who is acting.
    pub actor: Actor,
}

/// The state a pointer gesture needs between events.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Pressed {
    /// A navigation gesture is in progress.
    Navigating { gesture: Gesture, button: Button },
    /// A manipulation is in progress. The drag itself is in [`Interaction::drag`].
    Manipulating,
    /// A button is down over nothing in particular; on release it becomes a click or a rectangle.
    Selecting {
        origin: (f32, f32),
        modifiers: Modifiers,
    },
}

/// One viewport's pointer and keyboard state.
pub struct Interaction {
    bindings: Bindings,
    layout: Option<GizmoLayout>,
    hovered: Option<Handle>,
    pointer: (f32, f32),
    pressed: Option<Pressed>,
    drag: Option<Drag>,
    input: DragInput,
    feedback: Option<Feedback>,
}

impl Default for Interaction {
    fn default() -> Self {
        Self::new()
    }
}

impl Interaction {
    /// A viewport nobody has touched yet.
    #[must_use]
    pub fn new() -> Self {
        Self {
            bindings: Bindings::default(),
            layout: None,
            hovered: None,
            pointer: (0.0, 0.0),
            pressed: None,
            drag: None,
            input: DragInput::NONE,
            feedback: None,
        }
    }

    /// The navigation bindings in force.
    #[must_use]
    pub const fn bindings(&self) -> &Bindings {
        &self.bindings
    }

    /// Rebind navigation, which a settings panel and a preset both do.
    pub fn set_bindings(&mut self, bindings: Bindings) {
        self.bindings = bindings;
    }

    /// Take the gizmo geometry the runtime published with the frame now on screen.
    ///
    /// Called once a frame, with whatever the transport delivered. `None` means the runtime drew no
    /// gizmo, which is the state an editor with no runtime is permanently in.
    pub fn set_layout(&mut self, layout: Option<GizmoLayout>) {
        self.layout = layout;
        if self.drag.is_none() && self.layout.is_none() {
            self.hovered = None;
        }
    }

    /// Which handle the pointer is over, for the interface to say so and for the runtime's highlight.
    #[must_use]
    pub const fn hovered(&self) -> Option<Handle> {
        self.hovered
    }

    /// Whether a manipulation is in progress.
    #[must_use]
    pub const fn is_manipulating(&self) -> bool {
        self.drag.is_some()
    }

    /// The numbers to show while a manipulation is in progress: the delta and the resulting value.
    #[must_use]
    pub const fn feedback(&self) -> Option<&Feedback> {
        self.feedback.as_ref()
    }

    /// The axes the manipulation is confined to.
    #[must_use]
    pub const fn lock(&self) -> AxisLock {
        self.input.lock
    }

    /// The pointer's last known position inside the viewport.
    #[must_use]
    pub const fn pointer(&self) -> (f32, f32) {
        self.pointer
    }

    /// Feed one event in and find out what it did.
    pub fn handle(&mut self, context: &mut Context<'_>, event: Event) -> Outcome {
        match event {
            Event::PointerMoved { x, y } => self.moved(context, x, y),
            Event::PointerDown {
                button,
                x,
                y,
                modifiers,
            } => self.down(context, button, x, y, modifiers),
            Event::PointerUp {
                button,
                x,
                y,
                modifiers,
            } => self.up(context, button, x, y, modifiers),
            Event::Wheel { notches, modifiers } => {
                let _ = modifiers;
                context
                    .viewport
                    .navigator
                    .zoom(&mut context.viewport.state, notches);
                Outcome::CameraMoved
            }
            Event::Key(key) => self.key(context, key),
            Event::PointerLeft => {
                // A pointer that left is not a pointer that released: a drag continues, because the
                // window still has the button. Only the hover goes.
                if self.drag.is_none() && self.hovered.take().is_some() {
                    return Outcome::HoverChanged(None);
                }
                Outcome::Nothing
            }
        }
    }

    fn moved(&mut self, context: &mut Context<'_>, x: f32, y: f32) -> Outcome {
        let (dx, dy) = (x - self.pointer.0, y - self.pointer.1);
        self.pointer = (x, y);

        match self.pressed {
            Some(Pressed::Navigating { gesture, .. }) => {
                Self::navigate(context, gesture, dx, dy);
                Outcome::CameraMoved
            }
            Some(Pressed::Manipulating) => self.advance(context),
            Some(Pressed::Selecting { .. }) | None => {
                let hovered = self.layout.as_ref().and_then(|layout| layout.hit(x, y));
                if hovered == self.hovered {
                    return Outcome::Nothing;
                }
                self.hovered = hovered;
                // The hover is reported even mid-rectangle, because the runtime's highlight is what
                // tells a user which handle they are about to grab — before the press, which is the
                // half of the three states the reference gets right.
                Outcome::HoverChanged(hovered)
            }
        }
    }

    fn down(
        &mut self,
        context: &mut Context<'_>,
        button: Button,
        x: f32,
        y: f32,
        modifiers: Modifiers,
    ) -> Outcome {
        self.pointer = (x, y);
        if self.drag.is_some() {
            // A second button during a manipulation is ignored rather than starting a second one.
            return Outcome::Nothing;
        }

        // 1. A handle wins over everything, including a navigation binding that uses the same
        //    button and modifiers. See the module note.
        if button == Button::Left
            && let Some(handle) = self.layout.as_ref().and_then(|layout| layout.hit(x, y))
        {
            return self.begin_drag(context, handle, modifiers);
        }

        // 2. A bound navigation gesture.
        if let Some(gesture) = self.bindings.gesture(button, modifiers) {
            self.pressed = Some(Pressed::Navigating { gesture, button });
            return Outcome::Nothing;
        }

        // 3. Otherwise the press is the beginning of a selection, resolved on release.
        if button == Button::Left {
            self.pressed = Some(Pressed::Selecting {
                origin: (x, y),
                modifiers,
            });
        }
        Outcome::Nothing
    }

    fn up(
        &mut self,
        context: &mut Context<'_>,
        button: Button,
        x: f32,
        y: f32,
        _released_with: Modifiers,
    ) -> Outcome {
        self.pointer = (x, y);
        let Some(pressed) = self.pressed else {
            return Outcome::Nothing;
        };
        match pressed {
            Pressed::Navigating {
                button: held,
                gesture: _,
            } => {
                if held == button {
                    self.pressed = None;
                }
                Outcome::Nothing
            }
            Pressed::Manipulating => {
                if button != Button::Left {
                    return Outcome::Nothing;
                }
                self.finish(context)
            }
            Pressed::Selecting { origin, modifiers } => {
                if button != Button::Left {
                    return Outcome::Nothing;
                }
                self.pressed = None;
                Self::select(context, origin, (x, y), modifiers)
            }
        }
    }

    fn key(&mut self, context: &mut Context<'_>, key: Key) -> Outcome {
        match key {
            Key::Cancel => {
                let Some(mut drag) = self.drag.take() else {
                    return Outcome::Nothing;
                };
                self.pressed = None;
                self.feedback = None;
                self.input.lock = AxisLock::NONE;
                match drag.cancel(context.document) {
                    Ok(()) => Outcome::DragCancelled,
                    Err(problem) => Outcome::Refused(Box::new(problem)),
                }
            }
            axis => {
                let index = match axis {
                    Key::LockX => 0,
                    Key::LockY => 1,
                    _ => 2,
                };
                self.input.lock = self.input.lock.toggled(index);
                // Applied immediately rather than at the next pointer move: a lock that only took
                // effect once the hand moved would read as a key that did nothing.
                if self.drag.is_some() {
                    return self.advance(context);
                }
                Outcome::Nothing
            }
        }
    }

    /// Begin a manipulation on a handle.
    fn begin_drag(
        &mut self,
        context: &mut Context<'_>,
        handle: Handle,
        modifiers: Modifiers,
    ) -> Outcome {
        let Some(binding) = context.binding else {
            return Outcome::Refused(Box::new(
                Problem::new(
                    "manipulate the selection",
                    "the document does not describe which fields hold a transform",
                )
                .with_remedy("open a world whose schema declares a transform component"),
            ));
        };
        let mode = context.viewport.gizmo_mode;
        let Some(manipulator) = context.registry.for_handle(mode, handle) else {
            return Outcome::Refused(Box::new(Problem::new(
                "manipulate the selection",
                format!(
                    "no manipulator is registered for the {} handle",
                    handle.name()
                ),
            )));
        };
        let manipulator = manipulator.id().to_string();

        // THE VIEW STATE THE WHOLE DRAG RESOLVES AGAINST — the presented frame's, copied here and
        // never read again. See the module note.
        let view = context.viewport.interaction_view().clone();
        let request = DragRequest {
            manipulator: &manipulator,
            handle,
            space: context.viewport.gizmo_space,
            pivot: context.viewport.gizmo_pivot,
            nodes: context.nodes,
            binding,
            view: &view,
            pixel: self.pointer,
            actor: context.actor.clone(),
            duplicate: modifiers.alt,
            bounds: None,
        };
        match Drag::begin(context.registry, context.document, &request) {
            Ok(drag) => {
                self.drag = Some(drag);
                self.pressed = Some(Pressed::Manipulating);
                self.input = DragInput {
                    snap_modifier: modifiers.control,
                    precision: modifiers.shift,
                    lock: AxisLock::NONE,
                };
                self.hovered = Some(handle);
                Outcome::DragBegan(handle)
            }
            Err(problem) => Outcome::Refused(Box::new(problem)),
        }
    }

    /// Advance the manipulation to the pointer's current position.
    fn advance(&mut self, context: &mut Context<'_>) -> Outcome {
        let Some(drag) = self.drag.as_mut() else {
            return Outcome::Nothing;
        };
        let view = context.viewport.interaction_view().clone();
        match drag.update(
            context.registry,
            context.document,
            &view,
            self.pointer,
            &context.viewport.snap,
            self.input,
        ) {
            Ok(feedback) => {
                self.feedback = Some(feedback.clone());
                Outcome::DragAdvanced(feedback)
            }
            Err(problem) => Outcome::Refused(Box::new(problem)),
        }
    }

    /// Finish the manipulation. One transaction, or none if nothing moved.
    fn finish(&mut self, context: &mut Context<'_>) -> Outcome {
        let Some(mut drag) = self.drag.take() else {
            return Outcome::Nothing;
        };
        self.pressed = None;
        self.feedback = None;
        self.input.lock = AxisLock::NONE;
        match drag.commit(context.document) {
            Ok(recorded) => Outcome::DragFinished { recorded },
            Err(problem) => Outcome::Refused(Box::new(problem)),
        }
    }

    /// Turn a press and a release into a pick request against the frame that was on screen.
    fn select(
        context: &mut Context<'_>,
        origin: (f32, f32),
        release: (f32, f32),
        modifiers: Modifiers,
    ) -> Outcome {
        let travelled = ((release.0 - origin.0).powi(2) + (release.1 - origin.1).powi(2)).sqrt();
        let intent = if travelled > CLICK_SLOP {
            PickIntent::rectangle(origin, release)
        } else {
            // Clicking the same spot again takes the next candidate under it, which is how an object
            // behind another is reached without moving the camera. The count is advanced here rather
            // than in the runtime, because it is a property of this pointer.
            let _cycle = context.viewport.click(release.0, release.1);
            PickIntent::Click {
                x: release.0,
                y: release.1,
            }
        };
        let mode = if modifiers.control {
            SelectionMode::Toggle
        } else if modifiers.shift {
            SelectionMode::Add
        } else {
            SelectionMode::Replace
        };
        context.viewport.pick(intent).map_or(
            // No frame has arrived, so there is nothing on screen to have clicked. The editor says
            // so rather than resolving the click against its own camera, which would be the
            // forbidden editor-side pick that disagrees with the picture.
            Outcome::NothingToPick,
            |request| Outcome::Pick(Box::new(request), mode),
        )
    }

    fn navigate(context: &mut Context<'_>, gesture: Gesture, dx: f32, dy: f32) {
        let viewport = &mut *context.viewport;
        match gesture {
            Gesture::Orbit => viewport.navigator.orbit(&mut viewport.state, dx, dy),
            Gesture::Pan => viewport.navigator.pan(&mut viewport.state, dx, dy),
            Gesture::Zoom => viewport.navigator.zoom(&mut viewport.state, -dy * 0.05),
            Gesture::Fly => {
                // A fly drag turns the camera in place; the keys that move it are commands, so that
                // the same movement is available to a script and to an agent.
                viewport.navigator.orbit(&mut viewport.state, dx, dy);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::value::{Value, ValueKind};

    use super::*;
    use crate::gizmo::{GizmoMode, Transform3};
    use crate::layout::HandleSpot;
    use crate::math::{Quat, Vec3};
    use crate::picking::PickIntent;
    use crate::transport::{FrameImage, PresentedFrame, TransportKind};
    use crate::viewport::{Viewport, ViewportId};
    use cy_editor_protocol::FrameId;

    struct World {
        document: Document,
        binding: TransformBinding,
        nodes: Vec<NodeId>,
        registry: GizmoRegistry,
        viewport: Viewport,
    }

    fn world() -> World {
        let mut document = Document::new("worlds/city.cyworld");
        let component = document.schema_mut().declare_type("Transform", false);
        let translation = document
            .schema_mut()
            .declare_field(component, "translation", ValueKind::Vec3, "where it is")
            .expect("a fresh schema");
        let rotation = document
            .schema_mut()
            .declare_field(component, "rotation", ValueKind::Quat, "which way it faces")
            .expect("a fresh schema");
        let scale = document
            .schema_mut()
            .declare_field(component, "scale", ValueKind::Vec3, "how big it is")
            .expect("a fresh schema");
        let binding = TransformBinding {
            component,
            translation,
            rotation,
            scale,
        };
        let nodes = document
            .with_transaction("Populate", Actor::human("designer"), |document| {
                let node = document.create_node(None)?;
                document.add_component(
                    node,
                    component,
                    vec![
                        (translation, Value::Vec3([0.0, 0.0, 0.0])),
                        (rotation, Value::Quat(Quat::IDENTITY.to_array())),
                        (scale, Value::Vec3([1.0, 1.0, 1.0])),
                    ],
                )?;
                Ok(vec![node])
            })
            .expect("a populated document");

        let mut viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::LocalSurface,
        );
        viewport.state.camera.position = Vec3::new(0.0, 0.0, 20.0);
        viewport.snap.modes.grid = false;
        viewport.snap.modes.angle = false;
        viewport.snap.modes.scale = false;
        World {
            document,
            binding,
            nodes,
            registry: GizmoRegistry::with_builtins(),
            viewport,
        }
    }

    /// A frame on screen, so that a pick has something to be resolved against.
    fn present(viewport: &mut Viewport, frame: u64) {
        let state = viewport.state.clone();
        viewport.stream.accept(
            PresentedFrame::new(FrameId::from_raw(frame), state, FrameImage::Surface(1), 0),
            0,
        );
    }

    fn layout(frame: u64) -> GizmoLayout {
        GizmoLayout {
            frame: FrameId::from_raw(frame),
            mode: GizmoMode::Translate,
            centre: (960.0, 540.0),
            extent: 120.0,
            spots: vec![HandleSpot {
                handle: Handle::AxisX,
                x: 1060.0,
                y: 540.0,
                radius: 6.0,
                depth: 10.0,
            }],
        }
    }

    fn session(world: &mut World) -> Context<'_> {
        Context {
            viewport: &mut world.viewport,
            document: &mut world.document,
            registry: &world.registry,
            binding: Some(world.binding),
            nodes: &world.nodes,
            actor: Actor::human("designer"),
        }
    }

    fn translation(world: &World) -> [f32; 3] {
        lanes(&world.document, world.binding, world.nodes[0])
    }

    /// The same reading, through a document a context is already holding.
    fn lanes(document: &Document, binding: TransformBinding, node: NodeId) -> [f32; 3] {
        match document
            .content()
            .field(node, binding.component, binding.translation)
            .expect("a translation")
        {
            Value::Vec3(lanes) => *lanes,
            other => panic!("expected a Vec3, got {other:?}"),
        }
    }

    #[test]
    fn a_middle_drag_orbits_the_camera_and_touches_no_document() {
        let mut world = world();
        let revision = world.document.revision();
        let before = world.viewport.state.camera.position;
        let mut interaction = Interaction::new();
        let mut context = session(&mut world);

        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Middle,
                x: 900.0,
                y: 500.0,
                modifiers: Modifiers::NONE,
            },
        );
        let outcome = interaction.handle(&mut context, Event::PointerMoved { x: 980.0, y: 520.0 });
        assert_eq!(outcome, Outcome::CameraMoved);
        interaction.handle(
            &mut context,
            Event::PointerUp {
                button: Button::Middle,
                x: 980.0,
                y: 520.0,
                modifiers: Modifiers::NONE,
            },
        );

        assert_ne!(
            world
                .viewport
                .state
                .camera
                .position
                .to_array()
                .map(f32::to_bits),
            before.to_array().map(f32::to_bits),
            "the camera did not move"
        );
        assert_eq!(
            world.document.revision(),
            revision,
            "navigation wrote to the document"
        );
        assert!(!world.document.is_transaction_open());
    }

    #[test]
    fn the_wheel_zooms_and_a_zoom_cannot_pass_through_what_it_is_looking_at() {
        let mut world = world();
        let mut interaction = Interaction::new();
        let mut context = session(&mut world);
        for _ in 0..200 {
            interaction.handle(
                &mut context,
                Event::Wheel {
                    notches: 1.0,
                    modifiers: Modifiers::NONE,
                },
            );
        }
        assert!(
            world.viewport.navigator.pivot_distance() > 0.0,
            "the camera passed through the pivot"
        );
    }

    #[test]
    fn a_click_produces_a_pick_against_the_frame_on_screen_and_never_against_the_camera_since() {
        let mut world = world();
        present(&mut world.viewport, 77);
        // The camera moves after the frame was presented, exactly as it does between a click and the
        // frame the user was looking at when they clicked.
        world.viewport.state.camera.position = Vec3::new(50.0, 50.0, 50.0);

        let mut interaction = Interaction::new();
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 400.0,
                y: 300.0,
                modifiers: Modifiers::NONE,
            },
        );
        let outcome = interaction.handle(
            &mut context,
            Event::PointerUp {
                button: Button::Left,
                x: 401.0,
                y: 300.0,
                modifiers: Modifiers::NONE,
            },
        );
        match outcome {
            Outcome::Pick(request, mode) => {
                assert_eq!(request.frame, FrameId::from_raw(77));
                assert_eq!(mode, SelectionMode::Replace);
                assert!(matches!(request.intent, PickIntent::Click { .. }));
            }
            other => panic!("expected a pick, got {other:?}"),
        }
    }

    #[test]
    fn a_press_and_drag_is_a_rectangle_rather_than_a_click_that_moved() {
        let mut world = world();
        present(&mut world.viewport, 3);
        let mut interaction = Interaction::new();
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 100.0,
                y: 100.0,
                modifiers: Modifiers::SHIFT,
            },
        );
        let outcome = interaction.handle(
            &mut context,
            Event::PointerUp {
                button: Button::Left,
                x: 400.0,
                y: 380.0,
                modifiers: Modifiers::SHIFT,
            },
        );
        match outcome {
            Outcome::Pick(request, mode) => {
                assert!(request.intent.is_area(), "{:?}", request.intent);
                assert_eq!(mode, SelectionMode::Add, "shift adds to the selection");
            }
            other => panic!("expected a rectangle pick, got {other:?}"),
        }
    }

    #[test]
    fn a_click_before_the_first_frame_says_there_is_nothing_to_pick() {
        // Not a pick resolved against the editor's own camera, which is the forbidden pattern this
        // whole path exists to prevent.
        let mut world = world();
        let mut interaction = Interaction::new();
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 10.0,
                y: 10.0,
                modifiers: Modifiers::NONE,
            },
        );
        let outcome = interaction.handle(
            &mut context,
            Event::PointerUp {
                button: Button::Left,
                x: 10.0,
                y: 10.0,
                modifiers: Modifiers::NONE,
            },
        );
        assert_eq!(outcome, Outcome::NothingToPick);
    }

    #[test]
    fn the_hovered_handle_is_reported_before_the_press() {
        // "Three states: normal, hover, active — the hovered handle emphasised *before* the press."
        // The editor's half of that is knowing which handle is under the pointer without one.
        let mut world = world();
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(layout(1)));
        let mut context = session(&mut world);

        assert_eq!(
            interaction.handle(
                &mut context,
                Event::PointerMoved {
                    x: 1060.0,
                    y: 540.0
                }
            ),
            Outcome::HoverChanged(Some(Handle::AxisX))
        );
        assert_eq!(interaction.hovered(), Some(Handle::AxisX));
        assert!(!interaction.is_manipulating(), "hovering is not dragging");

        // Moving within the same handle reports nothing: a hover event per pixel would be a
        // repaint per pixel.
        assert_eq!(
            interaction.handle(
                &mut context,
                Event::PointerMoved {
                    x: 1061.0,
                    y: 540.0
                }
            ),
            Outcome::Nothing
        );
        assert_eq!(
            interaction.handle(&mut context, Event::PointerMoved { x: 200.0, y: 200.0 }),
            Outcome::HoverChanged(None)
        );
    }

    #[test]
    fn with_no_layout_there_is_no_hover_and_a_press_selects_instead() {
        // An editor with no runtime: the gizmo is drawn by the engine, so there is none, and a press
        // where one would be is a press on the scene.
        let mut world = world();
        present(&mut world.viewport, 5);
        let mut interaction = Interaction::new();
        let mut context = session(&mut world);
        assert_eq!(
            interaction.handle(
                &mut context,
                Event::PointerMoved {
                    x: 1060.0,
                    y: 540.0
                }
            ),
            Outcome::Nothing
        );
        assert_eq!(interaction.hovered(), None);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        assert!(!interaction.is_manipulating());
        let outcome = interaction.handle(
            &mut context,
            Event::PointerUp {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        assert!(matches!(outcome, Outcome::Pick(_, _)), "{outcome:?}");
    }

    #[test]
    fn a_drag_on_a_handle_moves_the_object_and_a_drag_back_restores_the_exact_bits() {
        let mut world = world();
        present(&mut world.viewport, 9);
        let before = translation(&world);
        let entries = world.document.history().entries().len();
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(layout(9)));
        let mut context = session(&mut world);

        assert_eq!(
            interaction.handle(
                &mut context,
                Event::PointerDown {
                    button: Button::Left,
                    x: 1060.0,
                    y: 540.0,
                    modifiers: Modifiers::NONE,
                },
            ),
            Outcome::DragBegan(Handle::AxisX)
        );
        assert!(interaction.is_manipulating());

        for step in 1..40 {
            #[allow(clippy::cast_precision_loss, reason = "a loop counter below 2^24")]
            let x = 1060.0 + step as f32 * 4.0;
            let outcome = interaction.handle(&mut context, Event::PointerMoved { x, y: 540.0 });
            assert!(matches!(outcome, Outcome::DragAdvanced(_)), "{outcome:?}");
        }
        assert_ne!(
            translation(&world).map(f32::to_bits),
            before.map(f32::to_bits)
        );
        assert!(interaction.feedback().is_some(), "no numbers to show");

        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerMoved {
                x: 1060.0,
                y: 540.0,
            },
        );
        let outcome = interaction.handle(
            &mut context,
            Event::PointerUp {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        assert_eq!(outcome, Outcome::DragFinished { recorded: false });
        assert_eq!(
            translation(&world).map(f32::to_bits),
            before.map(f32::to_bits),
            "a drag out and back did not restore the original bits"
        );
        assert_eq!(
            world.document.history().entries().len(),
            entries,
            "a drag that changed nothing entered the history"
        );
        assert!(!interaction.is_manipulating());
        assert!(interaction.feedback().is_none());
    }

    #[test]
    fn one_manipulation_is_one_history_entry() {
        let mut world = world();
        present(&mut world.viewport, 9);
        let entries = world.document.history().entries().len();
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(layout(9)));
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        for step in 1..50 {
            #[allow(clippy::cast_precision_loss, reason = "a loop counter below 2^24")]
            let x = 1060.0 + step as f32 * 6.0;
            interaction.handle(&mut context, Event::PointerMoved { x, y: 540.0 });
        }
        assert_eq!(
            interaction.handle(
                &mut context,
                Event::PointerUp {
                    button: Button::Left,
                    x: 1354.0,
                    y: 540.0,
                    modifiers: Modifiers::NONE,
                },
            ),
            Outcome::DragFinished { recorded: true }
        );
        assert_eq!(world.document.history().entries().len(), entries + 1);
    }

    #[test]
    fn escape_abandons_a_manipulation_and_puts_everything_back() {
        let mut world = world();
        present(&mut world.viewport, 9);
        let before = translation(&world);
        let entries = world.document.history().entries().len();
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(layout(9)));
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        interaction.handle(
            &mut context,
            Event::PointerMoved {
                x: 1300.0,
                y: 540.0,
            },
        );
        assert_ne!(
            translation(&world).map(f32::to_bits),
            before.map(f32::to_bits)
        );

        let mut context = session(&mut world);
        assert_eq!(
            interaction.handle(&mut context, Event::Key(Key::Cancel)),
            Outcome::DragCancelled
        );
        assert_eq!(
            translation(&world).map(f32::to_bits),
            before.map(f32::to_bits)
        );
        assert_eq!(world.document.history().entries().len(), entries);
        assert!(!interaction.is_manipulating());
        assert!(!world.document.is_transaction_open());
    }

    #[test]
    fn pressing_x_mid_drag_confines_it_and_takes_effect_without_moving_the_hand() {
        let mut world = world();
        present(&mut world.viewport, 9);
        let (binding, node) = (world.binding, world.nodes[0]);
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(GizmoLayout {
            spots: vec![HandleSpot {
                handle: Handle::Screen,
                x: 960.0,
                y: 540.0,
                radius: 18.0,
                depth: 10.0,
            }],
            ..layout(9)
        }));
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 960.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        interaction.handle(
            &mut context,
            Event::PointerMoved {
                x: 1200.0,
                y: 300.0,
            },
        );
        let free = lanes(context.document, binding, node);
        assert_ne!(free[1].to_bits(), 0.0_f32.to_bits(), "it moved in Y");

        let outcome = interaction.handle(&mut context, Event::Key(Key::LockX));
        assert!(matches!(outcome, Outcome::DragAdvanced(_)), "{outcome:?}");
        assert_eq!(interaction.lock().label(), "X");
        let locked = lanes(context.document, binding, node);
        assert_eq!(
            locked[1].to_bits(),
            0.0_f32.to_bits(),
            "Y moved under an X lock"
        );
        assert_eq!(
            locked[0].to_bits(),
            free[0].to_bits(),
            "X changed with the lock"
        );

        // And pressing it again releases the lock, which is what a toggle means.
        interaction.handle(&mut context, Event::Key(Key::LockX));
        assert_eq!(interaction.lock().label(), "");
        interaction.handle(&mut context, Event::Key(Key::Cancel));
    }

    #[test]
    fn a_handle_press_manipulates_even_where_the_preset_binds_that_button_to_navigation() {
        // The precedence in the module note. Maya binds Alt+Left to orbit; Alt on a handle is
        // duplicate-and-transform, and the handle wins.
        let mut world = world();
        present(&mut world.viewport, 9);
        let count = world.document.content().node_count();
        let mut interaction = Interaction::new();
        interaction.set_bindings(Bindings::preset(crate::navigation::NavigationPreset::Maya));
        interaction.set_layout(Some(layout(9)));
        let camera = world.viewport.state.camera.position;
        let mut context = session(&mut world);

        let outcome = interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::ALT,
            },
        );
        assert_eq!(outcome, Outcome::DragBegan(Handle::AxisX));
        interaction.handle(
            &mut context,
            Event::PointerMoved {
                x: 1200.0,
                y: 540.0,
            },
        );
        interaction.handle(
            &mut context,
            Event::PointerUp {
                button: Button::Left,
                x: 1200.0,
                y: 540.0,
                modifiers: Modifiers::ALT,
            },
        );
        assert_eq!(
            world
                .viewport
                .state
                .camera
                .position
                .to_array()
                .map(f32::to_bits),
            camera.to_array().map(f32::to_bits),
            "the camera orbited during a manipulation"
        );
        assert_eq!(
            world.document.content().node_count(),
            count + 1,
            "alt on a handle did not duplicate"
        );
    }

    #[test]
    fn a_press_away_from_a_handle_still_navigates_under_the_same_modifier() {
        let mut world = world();
        let mut interaction = Interaction::new();
        interaction.set_bindings(Bindings::preset(crate::navigation::NavigationPreset::Maya));
        interaction.set_layout(Some(layout(9)));
        let before = world.viewport.state.camera.position;
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 200.0,
                y: 200.0,
                modifiers: Modifiers::ALT,
            },
        );
        interaction.handle(&mut context, Event::PointerMoved { x: 260.0, y: 210.0 });
        assert_ne!(
            world
                .viewport
                .state
                .camera
                .position
                .to_array()
                .map(f32::to_bits),
            before.to_array().map(f32::to_bits)
        );
        assert!(!interaction.is_manipulating());
    }

    #[test]
    fn the_camera_cannot_move_during_a_manipulation() {
        // A drag resolves against a captured view; letting the camera move under it would move the
        // object by the camera's motion as well as the cursor's.
        let mut world = world();
        present(&mut world.viewport, 9);
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(layout(9)));
        let camera = world.viewport.state.camera.position;
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        // A second button, and a move: the manipulation continues and the camera stays put.
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Middle,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        interaction.handle(
            &mut context,
            Event::PointerMoved {
                x: 1200.0,
                y: 400.0,
            },
        );
        assert!(interaction.is_manipulating());
        assert_eq!(
            context
                .viewport
                .state
                .camera
                .position
                .to_array()
                .map(f32::to_bits),
            camera.to_array().map(f32::to_bits)
        );
        interaction.handle(&mut context, Event::Key(Key::Cancel));
    }

    #[test]
    fn a_manipulation_survives_the_pointer_leaving_the_viewport() {
        let mut world = world();
        present(&mut world.viewport, 9);
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(layout(9)));
        let mut context = session(&mut world);
        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        assert_eq!(
            interaction.handle(&mut context, Event::PointerLeft),
            Outcome::Nothing
        );
        assert!(
            interaction.is_manipulating(),
            "a drag ended because the pointer crossed a panel edge"
        );
        interaction.handle(&mut context, Event::Key(Key::Cancel));
    }

    #[test]
    fn manipulating_without_a_transform_binding_is_refused_with_a_remedy() {
        let mut world = world();
        present(&mut world.viewport, 9);
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(layout(9)));
        let mut context = Context {
            binding: None,
            ..session(&mut world)
        };
        let outcome = interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 1060.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        match outcome {
            Outcome::Refused(problem) => assert!(problem.remedy.is_some(), "{problem}"),
            other => panic!("expected a refusal, got {other:?}"),
        }
        assert!(!world.document.is_transaction_open());
    }

    #[test]
    fn the_universal_mode_manipulates_from_whichever_handle_was_grabbed() {
        let mut world = world();
        world.viewport.gizmo_mode = GizmoMode::Universal;
        present(&mut world.viewport, 9);
        let mut interaction = Interaction::new();
        interaction.set_layout(Some(GizmoLayout {
            spots: vec![
                HandleSpot {
                    handle: Handle::RingY,
                    x: 1000.0,
                    y: 540.0,
                    radius: 6.0,
                    depth: 10.0,
                },
                HandleSpot {
                    handle: Handle::BoxZ,
                    x: 800.0,
                    y: 540.0,
                    radius: 6.0,
                    depth: 10.0,
                },
            ],
            ..layout(9)
        }));
        let mut context = session(&mut world);

        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 1000.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        let Outcome::DragAdvanced(feedback) = interaction.handle(
            &mut context,
            Event::PointerMoved {
                x: 1000.0,
                y: 400.0,
            },
        ) else {
            panic!("the ring did not turn anything");
        };
        assert!(feedback.delta.contains('°'), "{feedback:?}");
        interaction.handle(&mut context, Event::Key(Key::Cancel));

        interaction.handle(
            &mut context,
            Event::PointerDown {
                button: Button::Left,
                x: 800.0,
                y: 540.0,
                modifiers: Modifiers::NONE,
            },
        );
        let Outcome::DragAdvanced(feedback) =
            interaction.handle(&mut context, Event::PointerMoved { x: 700.0, y: 540.0 })
        else {
            panic!("the box did not resize anything");
        };
        assert!(feedback.delta.starts_with('×'), "{feedback:?}");
        interaction.handle(&mut context, Event::Key(Key::Cancel));
    }

    #[test]
    fn a_transform_is_read_back_from_the_document_rather_than_kept_beside_it() {
        // The interaction holds no copy of the transform: everything it knows comes from the drag,
        // and the drag reads the document once. This is the property that keeps a script, an agent
        // and a gizmo from disagreeing about where an object is.
        let world = world();
        let interaction = Interaction::new();
        assert!(interaction.feedback().is_none());
        assert_eq!(interaction.hovered(), None);
        assert_eq!(
            Transform3::default().scale.to_array().map(f32::to_bits),
            [1.0_f32, 1.0, 1.0].map(f32::to_bits),
            "the default transform is the identity"
        );
        drop(world);
    }
}
