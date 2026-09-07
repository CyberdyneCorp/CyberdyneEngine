//! What keeps a hosted runtime in step with the document, and what carries the gizmo back.
//! M7 tasks 5b.3 and 5b.4.
//!
//! --- WHAT WAS MISSING, AND WHY IT WAS INVISIBLE --------------------------------------------------
//!
//! Every piece of the gizmo path existed at M6 and no two of them were joined.
//! [`crate::gizmo::request`] built an intent and nothing called it; `cy_editor_protocol` carried
//! `GizmoIntent` and `GizmoGeometry` and nothing sent either; `cy_editor_viewport::layout` hit-tested
//! a published layout and nothing published one; and the viewport panel's `published_gizmo` returned
//! `None` with a paragraph saying what would have to arrive. So a drag landed on nothing, and the
//! artefact reported a gap that read as a defect in the drag.
//!
//! This module is the join. Once a frame it asks the runtime for the gizmo on the current selection,
//! takes the answer, and hands it to the panel — which draws nothing and hit-tests everything,
//! because the geometry is the engine's.
//!
//! --- AND THE OTHER DIRECTION: THE RUNTIME HAS TO SEE THE EDIT ------------------------------------
//!
//! A gizmo the engine draws sits on an object the engine holds. If the editor moves its own copy and
//! the runtime never hears, the arrow drifts away from the box on the screen and the drag looks
//! broken while both halves are individually correct.
//!
//! So this module also FORWARDS. It watches the document's history — the entries and the undo cursor
//! — and sends what changed as [`cy_editor_protocol::Message::Apply`]: new entries forward, undone
//! entries as their own inverse. That is the same operation stream the journal holds, which
//! `editor-documents-and-transactions` requires ("the journal SHALL be the same operation stream
//! used by diff, live editing, and any future collaboration").
//!
//! It watches rather than being called at the commit, because [`cy_editor_documents::Document`] has
//! no hook there and inventing one would put a runtime session inside the document layer. The cost
//! is that a change is forwarded on the next pump rather than inside the commit, which for a viewport
//! at sixty frames a second is under a frame.
//!
//! --- WHY THE GIZMO IS ASKED FOR EVERY FRAME ------------------------------------------------------
//!
//! Because a layout belongs to ONE frame, and the editor refuses one that names another
//! ([`crate::gizmo::accept`]). The camera moves, the runtime's frames arrive on their own schedule,
//! and a layout kept across frames is a click resolved against handles that have moved. Asking again
//! is one message of about forty bytes; keeping a stale one is the defect the frame identifier exists
//! to catch.

use cy_editor_core::observe::Revision;
use cy_editor_documents::Document;
use cy_editor_documents::transaction::Transaction;
use cy_editor_protocol::{ApplyWhen, Message};
use cy_editor_viewport::layout::GizmoLayout;
use cy_editor_viewport::viewport::Viewport;

use crate::runtime::RuntimeSession;

/// The runtime's view of the editor, and the editor's view of the runtime's gizmo.
#[derive(Debug, Default)]
pub struct RuntimeMirror {
    /// The layout the runtime published for the frame the viewport is showing.
    layout: Option<GizmoLayout>,
    /// The gizmo request this is waiting on, and the frame it named — so a late answer to an
    /// older request is discarded rather than used against a camera that has moved.
    pending: Option<crate::gizmo::Asked>,
    /// How many history entries have been forwarded.
    forwarded: usize,
    /// Where the undo cursor was when this was last looked at.
    cursor: usize,
    /// Which document those two numbers are about. A different one starts again.
    document: Option<Revision>,
    /// How many transactions have been sent, for a report and for a test.
    sent: u64,
    /// How many layouts have been taken, ditto.
    accepted: u64,
    /// Whether the runtime's one-time view suggestion has been taken.
    ///
    /// Once, and only once: see `Message::ViewSuggested`. After it, the camera is the editor's and
    /// a second suggestion is ignored, because a runtime that re-aimed the camera every frame would
    /// be a runtime that owned it.
    framed: bool,
    /// The view the runtime suggested and this mirror has not applied yet, because applying it
    /// needs a mutable viewport and `accept` is handed a shared one.
    suggestion: Option<ViewSuggestion>,
    /// Why the last gizmo request or forward could not be made, when there was a reason worth
    /// keeping. Not a notification: "no runtime is attached" is an ordinary state and a toast per
    /// frame would be a wall of them.
    quiet_reason: Option<String>,
}

impl RuntimeMirror {
    /// A mirror that has seen nothing.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The gizmo the runtime published for the frame on screen, or `None`.
    #[must_use]
    pub const fn layout(&self) -> Option<&GizmoLayout> {
        self.layout.as_ref()
    }

    /// How many transactions have been forwarded to the runtime.
    #[must_use]
    pub const fn forwarded_transactions(&self) -> u64 {
        self.sent
    }

    /// How many published layouts have been taken.
    #[must_use]
    pub const fn accepted_layouts(&self) -> u64 {
        self.accepted
    }

    /// Why nothing is being asked for, when there is a reason.
    #[must_use]
    pub fn quiet_reason(&self) -> Option<&str> {
        self.quiet_reason.as_deref()
    }

    /// Take a message the session delivered. Anything this mirror has no use for is left alone.
    pub fn accept(&mut self, message: &Message, viewport: &Viewport) {
        if let Message::ViewSuggested {
            position,
            rotation,
            fov_y_radians,
            near,
        } = message
        {
            if !self.framed {
                self.suggestion = Some(ViewSuggestion {
                    position: *position,
                    rotation: *rotation,
                    fov_y_radians: *fov_y_radians,
                    near: *near,
                });
            }
            return;
        }
        let Message::GizmoGeometry { request, layout } = message else {
            return;
        };
        if self.pending.map(|asked| asked.request) != Some(*request) {
            // An answer to a request two frames old. Discarded rather than used: it describes a
            // frame the viewport has moved past, and `crate::gizmo::accept` would refuse it anyway.
            return;
        }
        let asked = self.pending.take().expect("just matched on it");
        let _ = viewport;
        match crate::gizmo::accept_for(layout, Some(asked.frame)) {
            Ok(taken) => {
                self.accepted += 1;
                self.layout = Some(taken);
            }
            Err(problem) => {
                // The layout named another frame. Keeping the previous one would be worse than
                // showing none: a gizmo drawn where the handles are not is a drag that grabs the
                // wrong axis.
                self.layout = None;
                self.quiet_reason = Some(problem.because);
            }
        }
    }

    /// Apply the runtime's suggested view, if one arrived and none has been applied.
    ///
    /// Separate from [`RuntimeMirror::accept`] because it needs the viewport mutably and `accept`
    /// is handed a shared one — the layout check reads the viewport and this writes it, and doing
    /// both through one reference would make the caller choose between two borrows.
    ///
    /// Returns whether a view was applied, which is what a caller logs rather than what it acts on.
    pub fn frame_the_world(&mut self, viewport: &mut Viewport) -> bool {
        let Some(view) = self.suggestion.take() else {
            return false;
        };
        if self.framed {
            return false;
        }
        self.framed = true;
        viewport.state.camera.position = cy_editor_viewport::math::Vec3::new(
            view.position[0],
            view.position[1],
            view.position[2],
        );
        viewport.state.camera.rotation = cy_editor_viewport::math::Quat::from_array(view.rotation);
        if view.fov_y_radians > 0.0 {
            viewport.state.projection = cy_editor_viewport::state::Projection::Perspective {
                fov_y: view.fov_y_radians,
            };
        }
        if view.near > 0.0 {
            viewport.state.near = view.near;
        }
        true
    }

    /// One frame of keeping the runtime and the editor in step.
    ///
    /// Split into the two directions because they fail for different reasons and a caller reading a
    /// stack trace should be able to tell which was in flight.
    pub fn sync(
        &mut self,
        runtime: &RuntimeSession,
        document: Option<&Document>,
        viewport: &Viewport,
        identities: Vec<u64>,
    ) {
        if !runtime.is_connected() {
            self.layout = None;
            self.pending = None;
            self.quiet_reason = Some("no runtime is attached".to_string());
            return;
        }
        self.quiet_reason = None;
        if let Some(document) = document {
            self.forward(runtime, document);
        }
        self.request_gizmo(runtime, viewport, identities);
    }

    /// Send whatever the document committed or undid since the last look.
    fn forward(&mut self, runtime: &RuntimeSession, document: &Document) {
        let history = document.history();
        let entries = history.entries();
        let cursor = history.cursor();

        // A NEW DOCUMENT STARTS AGAIN. Forwarding entry three of a document the runtime has never
        // seen would move an object that is not there.
        // The document's identity, narrowed to the width a `Revision` holds. A truncation, and a
        // safe one here for the same reason `engine_identity` gives: this is compared for equality
        // against ITSELF a frame later, so a collision would have to be between two documents open
        // in one editor whose identities agree in their low 64 bits.
        #[allow(
            clippy::cast_possible_truncation,
            reason = "a 64-bit window on a 128-bit identity, compared only against another window \
                      on the same identity"
        )]
        let revision = Revision::from_u64(document.id().as_u128() as u64);
        if self.document != Some(revision) {
            self.document = Some(revision);
            self.forwarded = entries.len();
            self.cursor = cursor;
            return;
        }

        // Undo first: the cursor moved back, so the entries between the new cursor and the old one
        // are undone, newest first, each as its own inverse.
        if cursor < self.cursor {
            for index in (cursor..self.cursor.min(entries.len())).rev() {
                self.send(runtime, &entries[index].inverse());
            }
            self.forwarded = self.forwarded.min(cursor);
        }
        // Then everything the cursor now covers that has not been sent. This is both a redo — the
        // cursor moved forward over entries already in the list — and a new commit, because a
        // commit truncates the redoable tail and appends, so the two are the same operation from
        // here.
        if self.forwarded < cursor {
            for entry in &entries[self.forwarded..cursor.min(entries.len())] {
                self.send(runtime, entry);
            }
        }
        self.forwarded = cursor.min(entries.len());
        self.cursor = cursor;
    }

    fn send(&mut self, runtime: &RuntimeSession, transaction: &Transaction) {
        let mut writer = cy_editor_core::codec::Writer::new();
        transaction.encode(&mut writer);
        // `OnArrival`: an authoring world is not simulating, so the runtime applies it now and
        // renders on demand. A playing world would want `AtTickBoundary`, and the editor is what
        // knows which — see `ApplyWhen`.
        if runtime.apply(writer.finish(), ApplyWhen::OnArrival).is_ok() {
            self.sent += 1;
        }
    }

    /// Ask for the gizmo on this selection, in the frame the viewport is showing.
    fn request_gizmo(
        &mut self,
        runtime: &RuntimeSession,
        viewport: &Viewport,
        identities: Vec<u64>,
    ) {
        // One request in flight. A second would arrive after the first and describe an older frame,
        // and the answer that came back last would win — which is a gizmo that flickers between two
        // camera positions.
        if self.pending.is_some() {
            return;
        }
        match crate::gizmo::request(runtime, viewport, identities) {
            Ok(asked) => self.pending = Some(asked),
            Err(problem) => {
                // Before the first frame arrives this is "no frame has arrived from the runtime
                // yet", which is an ordinary state during start-up and not worth a notification.
                self.quiet_reason = Some(problem.because);
            }
        }
    }
}

/// A camera the runtime offered, held until there is a mutable viewport to put it in.
#[derive(Clone, Copy, PartialEq, Debug)]
struct ViewSuggestion {
    position: [f32; 3],
    rotation: [f32; 4],
    fov_y_radians: f32,
    near: f32,
}

/// The identity the engine knows an authored node by.
///
/// The low 64 bits of the [`cy_editor_core::ids::NodeId`], because the protocol's identities are
/// `u64` and a node's is `u128`.
///
/// **A NARROWING, AND THEREFORE A COLLISION THIS NAMES RATHER THAN HIDES.** A `NodeId` is a 128-bit
/// FNV-1a of `(document, ordinal)`, so the low half is a hash rather than the ordinal, and two nodes
/// of one document could in principle share it. At the sizes a document reaches — thousands of
/// nodes — the chance is around one in `2^45`, and the consequence is a gizmo on the wrong object
/// rather than a corrupted edit. Widening the protocol's identity to 128 bits is the fix, and it is
/// a change to `cy_editor_services::gizmo::Request`'s encoding and to the engine's decoder together.
#[must_use]
#[allow(
    clippy::cast_possible_truncation,
    reason = "the narrowing IS this function, and the doc comment above states what it costs"
)]
pub fn engine_identity(node: cy_editor_core::ids::NodeId) -> u64 {
    node.as_u128() as u64
}

#[cfg(test)]
mod tests {
    use cy_editor_core::ids::DocumentId;
    use cy_editor_core::ids::NodeId;

    use super::*;

    use cy_editor_protocol::FrameId;
    use cy_editor_viewport::transport::{
        FrameImage, Mailbox, MailboxTransport, PresentedFrame, TransportKind,
    };
    use cy_editor_viewport::viewport::ViewportId;

    fn showing(frame: u64) -> Viewport {
        let mut viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::SharedTexture,
        );
        let mailbox = Mailbox::new();
        mailbox.publish(PresentedFrame::new(
            FrameId::from_raw(frame),
            viewport.state.clone(),
            FrameImage::Surface(0),
            0,
        ));
        let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox);
        viewport.pump(&mut transport, 1_000);
        viewport
    }

    #[test]
    fn a_mirror_with_no_runtime_shows_no_gizmo_and_says_why() {
        // The ordinary state, and the reason it is a state rather than an error: a large part of
        // what an editor does needs no engine, and a toast per frame would be a wall of them.
        let mut mirror = RuntimeMirror::new();
        let viewport = showing(7);
        mirror.sync(&RuntimeSession::none(), None, &viewport, vec![1]);
        assert!(mirror.layout().is_none());
        assert_eq!(mirror.quiet_reason(), Some("no runtime is attached"));
        assert_eq!(mirror.forwarded_transactions(), 0);
    }

    #[test]
    fn an_answer_to_a_request_this_mirror_did_not_make_is_discarded() {
        // A late answer to a request two frames old describes a frame the viewport has moved past.
        let mut mirror = RuntimeMirror::new();
        let viewport = showing(7);
        mirror.accept(
            &Message::GizmoGeometry {
                request: cy_editor_protocol::RequestId::from_raw(9),
                layout: GizmoLayout::none(FrameId::from_raw(7)).encode(),
            },
            &viewport,
        );
        assert!(mirror.layout().is_none());
        assert_eq!(mirror.accepted_layouts(), 0);
    }

    #[test]
    fn two_nodes_of_one_document_get_two_identities() {
        let document = DocumentId::of_asset("worlds/city.cyworld");
        let first = NodeId::in_document(document, 1);
        let second = NodeId::in_document(document, 2);
        assert_ne!(engine_identity(first), engine_identity(second));
    }
}
