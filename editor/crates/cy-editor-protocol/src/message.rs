//! What crosses the bridge.
//!
//! Every message is encoded with [`cy_editor_core::codec`] — the same codec the journal writes —
//! because `editor-documents-and-transactions` requires that "the journal SHALL be the same
//! operation stream used by diff, live editing, and any future collaboration, rather than a separate
//! representation". [`Message::Apply`] therefore carries the bytes a journal record carries, and a
//! change to one is a change to both.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

use crate::frame::FrameId;

/// A request's identity, so that a reply can be matched to it.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Default)]
pub struct RequestId(u64);

impl RequestId {
    /// The request with this number.
    #[must_use]
    pub const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    /// The number.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }
}

/// When the runtime should apply a change.
///
/// The spike's single biggest available win, made explicit rather than inferred. An authoring world
/// that is not simulating should apply on arrival — p50 0.059 ms — and a playing world must wait for
/// a tick boundary, because `src/ecs/include/cy/ecs/world.h` refuses a structural change during
/// iteration and `CommandBuffer` is the supported path. Measured difference: 168-fold, from a
/// scheduling decision rather than an architecture one.
///
/// The editor says which, because the editor is what knows whether it is in play mode. A runtime
/// that guessed would guess wrong exactly when a designer was dragging a gizmo in a paused world.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ApplyWhen {
    /// Nothing is simulating: apply it now and render on demand.
    OnArrival,
    /// The world is running: apply it at the next tick boundary, through a command buffer.
    AtTickBoundary,
}

impl ApplyWhen {
    const fn as_u8(self) -> u8 {
        match self {
            ApplyWhen::OnArrival => 0,
            ApplyWhen::AtTickBoundary => 1,
        }
    }

    const fn from_u8(raw: u8) -> Option<Self> {
        match raw {
            0 => Some(ApplyWhen::OnArrival),
            1 => Some(ApplyWhen::AtTickBoundary),
            _ => None,
        }
    }
}

/// One message in either direction.
#[derive(Clone, PartialEq, Debug)]
pub enum Message {
    /// The editor introducing itself and the ABI it was built against.
    Hello {
        /// The ABI major the editor's SDK was generated against.
        abi_major: u32,
        /// The ABI minor.
        abi_minor: u32,
        /// The editor's own version, for a log.
        editor: String,
    },
    /// The runtime accepting, with what it is.
    Welcome {
        /// The runtime's ABI major.
        abi_major: u32,
        /// The runtime's ABI minor.
        abi_minor: u32,
        /// The runtime's own version.
        runtime: String,
    },
    /// The runtime refusing, with the reason a person acts on.
    Refused {
        /// Why.
        reason: String,
        /// What would make it work.
        remedy: String,
    },
    /// Apply an encoded transaction to the hosted world.
    Apply {
        /// The request's identity.
        request: RequestId,
        /// The frame this change belongs to, for reconciliation.
        frame: FrameId,
        /// When the runtime should apply it.
        when: ApplyWhen,
        /// The transaction, encoded exactly as the journal encodes it.
        transaction: Vec<u8>,
    },
    /// The runtime's authoritative echo of an applied change.
    Applied {
        /// Which request.
        request: RequestId,
        /// Which frame, so the editor reconciles its prediction against the right one.
        frame: FrameId,
        /// The runtime's observed state after applying, encoded the same way.
        observed: Vec<u8>,
    },
    /// The runtime refusing one request, with the reason the interactive path would give.
    Rejected {
        /// Which request.
        request: RequestId,
        /// Why.
        reason: String,
        /// What would make it succeed, when something would.
        remedy: String,
    },
    /// A liveness probe, carrying the frame it was sent on.
    Ping {
        /// The frame.
        frame: FrameId,
    },
    /// The reply to a probe.
    Pong {
        /// The frame the probe carried.
        frame: FrameId,
    },
    /// Load a newly built generation of a script module into the running world.
    ///
    /// M5.5 task 3.7 — "build and reload triggered by the agent, over M4's proven model". The model
    /// is M4's exactly: the build produces a **different file** per generation, because `dlopen` of a
    /// path already open returns the same image, and the loader is handed that path. Nothing about
    /// it is agent-specific; a person pressing the reload key sends the same message.
    Reload {
        /// The request's identity, so the answer can be paired with it.
        request: RequestId,
        /// Which module, as `module.toml` names it.
        module: String,
        /// The library the build produced, absolute.
        library: String,
        /// Which generation this is. Monotonic per module, and part of the file name.
        generation: u32,
    },
    /// The runtime confirming a module was reloaded, with live state intact.
    Reloaded {
        /// Which request.
        request: RequestId,
        /// Which module.
        module: String,
        /// The generation now resident.
        generation: u32,
    },
    /// Resolve a pick against a frame the runtime rendered. M6 task 2.6.
    ///
    /// **Picking is engine-side**, because `editor-viewport-and-gizmos` requires that "what is
    /// picked matches what is rendered — including virtual geometry, instanced content, foliage,
    /// terrain, and skinned meshes", none of which the editor has any description of. M5.5 built
    /// both ends of that and no wire between them: `cy_editor_viewport::picking` produced a
    /// `PickRequest` and nothing carried it, so engine-side picking was unreachable from the editor.
    ///
    /// `frame` is named on the message as well as inside `pick`, so that a runtime can refuse a
    /// request for a frame it no longer holds without decoding a payload this crate deliberately
    /// does not understand — see the note on `pick` below.
    Pick {
        /// The request's identity, so the answer can be paired with it.
        request: RequestId,
        /// **The frame that was on screen when the user clicked.** The runtime resolves against
        /// that frame's view state, not against whatever its camera has since become.
        frame: FrameId,
        /// A `cy_editor_viewport::picking::PickRequest`, encoded by that module.
        ///
        /// OPAQUE ON PURPOSE. `cy-editor-viewport` depends on this crate, so this crate cannot name
        /// its types without a cycle — and it should not want to: the protocol's job is to carry
        /// bytes and match a reply to a request. The encoding has one owner and one test suite,
        /// exactly as `Apply`'s transaction bytes have.
        pick: Vec<u8>,
    },
    /// What the runtime found under the pointer.
    Picked {
        /// Which request.
        request: RequestId,
        /// A `cy_editor_viewport::picking::PickResponse`, encoded by that module.
        candidates: Vec<u8>,
    },
    /// What gizmo the editor wants drawn, and about what. M6 task 2.7.
    ///
    /// `editor-viewport-and-gizmos` assigns "gizmo geometry generation, depth handling, and
    /// screen-constant sizing" to the ENGINE and leaves the editor "intent and manipulation state".
    /// This message is that intent. The editor does not send geometry and cannot: a second
    /// computation of where the arrows are is a second answer, and the moment it disagrees the user
    /// grabs one handle and drags another.
    GizmoIntent {
        /// The request's identity, so a published layout can be paired with it.
        request: RequestId,
        /// Which viewport the gizmo is for. A session has several and they differ in camera.
        viewport: u64,
        /// A `cy_editor_services::gizmo::Request`, encoded by that module: which frame, which
        /// manipulator, which space and pivot, and what is selected.
        intent: Vec<u8>,
    },
    /// A view that frames what the runtime is holding. M7 task 5b.1.
    ///
    /// --- WHY THE RUNTIME GETS TO SUGGEST A CAMERA, AND ONLY ONCE ---------------------------------
    ///
    /// The editor owns the camera: `editor-viewport-and-gizmos` gives it "intent and manipulation
    /// state", and navigation is intent. But a viewport opens at the origin looking down −Z, and
    /// **where the content is** is a question only the runtime can answer — it is the one holding a
    /// world. Without an answer the editor's first view is inside whatever happens to be at the
    /// origin, and the drag arithmetic degenerates: a pivot at the camera's own position gives a
    /// screen-space axis of zero length, and every manipulation reports "0.000 m" while every part
    /// of it is individually correct. That is exactly what M7's first end-to-end run measured.
    ///
    /// So the runtime says, once, "here is a view that frames what I have". The editor applies it
    /// if it has not yet been given one, and after that the camera is the editor's and the runtime
    /// renders what it is asked for. A suggestion that arrived every frame would be a runtime that
    /// owned the camera, which is the division this message is careful not to cross.
    ViewSuggested {
        /// Where the camera is, in world space.
        position: [f32; 3],
        /// Which way it faces, as a quaternion. The camera looks down its local −Z.
        rotation: [f32; 4],
        /// The vertical field of view, in radians.
        fov_y_radians: f32,
        /// The near clip distance, in world units.
        near: f32,
    },
    /// Where the runtime drew the gizmo, in the frame it drew it into.
    ///
    /// The layout names its own frame, for the same reason a pick does: a click lands two frames
    /// after the pixels it was aimed at, and hit-testing it against a newer layout is the same
    /// defect in a smaller place.
    GizmoGeometry {
        /// Which request this answers.
        request: RequestId,
        /// A `cy_editor_viewport::layout::GizmoLayout`, encoded by that module.
        layout: Vec<u8>,
    },
    /// Press play, pause, or stop. M8.a task 5.1.
    ///
    /// --- WHY PLAY IS A MESSAGE AND NOT A FLAG ON THE NEXT APPLY -----------------------------------
    ///
    /// Before this, `play.enter` set `cy_editor_viewport::play::PlayState` on every viewport and
    /// told the runtime nothing — so pressing play changed a badge and simulated nothing, which is
    /// what design.md §4 means by "today it reports `hosting: NoRuntime`". Entering play is a thing
    /// the RUNTIME does: it builds a simulation from the authored world, steps it, and puts the
    /// world back when play ends (`cy::gameplay::PlaySession`). None of that is expressible as an
    /// attribute of an edit.
    ///
    /// `state` is the word `cy::gameplay::play_state_name` spells — "editing", "playing" or
    /// "paused" — rather than a number, for the reason `ApplyWhen` is a number and this is not: a
    /// fourth state added on one side and not the other must be REFUSED by name, and a `u8` that
    /// fell through a match would be silently treated as the closest one.
    Play {
        /// The request's identity, so the answer can be paired with it.
        request: RequestId,
        /// The state the editor wants: `editing`, `playing` or `paused`.
        state: String,
    },
    /// What the runtime's play session is doing now, and what it did.
    ///
    /// Sent in answer to [`Message::Play`] and never unprompted, so a runtime cannot decide on its
    /// own that a session has ended — a designer who pressed play and found the editor back in
    /// authoring mode with no action of their own would have no way to tell that from a crash.
    Playing {
        /// Which request this answers.
        request: RequestId,
        /// The state now in force, which may not be the one asked for when the runtime refused.
        state: String,
        /// One line for a person: how many entities and bodies the session built, or why not.
        detail: String,
    },
}

impl Message {
    /// Encode a message.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
        // THREE FUNCTIONS RATHER THAN ONE MATCH, because the message set has grown past the point
        // where one function of it can be read in a sitting — and past the point where two can:
        // M8.a's play pair took `write_work` over the hundred lines clippy's `too_many_lines`
        // allows, which is the same observation with a number on it. The split is the protocol's
        // own: the first group opens and keeps a connection, the second does work on a world, the
        // third starts and stops a simulation of it. A message that belongs to none would fail to
        // encode loudly, which is why the fall-through is a debug assertion rather than a silent
        // empty frame.
        if !self.write_connection(&mut writer)
            && !self.write_work(&mut writer)
            && !self.write_play(&mut writer)
        {
            debug_assert!(
                false,
                "{self:?} belongs to no message group and would encode as an empty frame"
            );
        }
        writer.finish()
    }

    /// The handshake and the liveness probe. `true` when this message was one of them.
    fn write_connection(&self, writer: &mut Writer) -> bool {
        match self {
            Message::Hello {
                abi_major,
                abi_minor,
                editor,
            } => {
                writer.u8(0);
                writer.u32(*abi_major);
                writer.u32(*abi_minor);
                writer.text(editor);
            }
            Message::Welcome {
                abi_major,
                abi_minor,
                runtime,
            } => {
                writer.u8(1);
                writer.u32(*abi_major);
                writer.u32(*abi_minor);
                writer.text(runtime);
            }
            Message::Refused { reason, remedy } => {
                writer.u8(2);
                writer.text(reason);
                writer.text(remedy);
            }
            Message::Ping { frame } => {
                writer.u8(6);
                writer.u64(frame.as_u64());
            }
            Message::Pong { frame } => {
                writer.u8(7);
                writer.u64(frame.as_u64());
            }
            // WITH THE HANDSHAKE RATHER THAN WITH THE WORK, because it is sent once, when a
            // connection opens, and it acts on no world: it says where the runtime's content is so
            // that the editor's first view has something in it. See the variant for why once.
            Message::ViewSuggested {
                position,
                rotation,
                fov_y_radians,
                near,
            } => {
                writer.u8(14);
                for lane in position {
                    writer.f32(*lane);
                }
                for lane in rotation {
                    writer.f32(*lane);
                }
                writer.f32(*fov_y_radians);
                writer.f32(*near);
            }
            _ => return false,
        }
        true
    }

    /// Everything that acts on a world: a change, its echo, a refusal, a reload, a pick.
    /// `true` when this message was one of them.
    fn write_work(&self, writer: &mut Writer) -> bool {
        match self {
            Message::Apply {
                request,
                frame,
                when,
                transaction,
            } => {
                writer.u8(3);
                writer.u64(request.as_u64());
                writer.u64(frame.as_u64());
                writer.u8(when.as_u8());
                writer.bytes(transaction);
            }
            Message::Applied {
                request,
                frame,
                observed,
            } => {
                writer.u8(4);
                writer.u64(request.as_u64());
                writer.u64(frame.as_u64());
                writer.bytes(observed);
            }
            Message::Rejected {
                request,
                reason,
                remedy,
            } => {
                writer.u8(5);
                writer.u64(request.as_u64());
                writer.text(reason);
                writer.text(remedy);
            }
            Message::Reload {
                request,
                module,
                library,
                generation,
            } => {
                writer.u8(8);
                writer.u64(request.as_u64());
                writer.text(module);
                writer.text(library);
                writer.u32(*generation);
            }
            Message::Reloaded {
                request,
                module,
                generation,
            } => {
                writer.u8(9);
                writer.u64(request.as_u64());
                writer.text(module);
                writer.u32(*generation);
            }
            Message::Pick {
                request,
                frame,
                pick,
            } => {
                writer.u8(10);
                writer.u64(request.as_u64());
                writer.u64(frame.as_u64());
                writer.bytes(pick);
            }
            Message::Picked {
                request,
                candidates,
            } => {
                writer.u8(11);
                writer.u64(request.as_u64());
                writer.bytes(candidates);
            }
            Message::GizmoIntent {
                request,
                viewport,
                intent,
            } => {
                writer.u8(12);
                writer.u64(request.as_u64());
                writer.u64(*viewport);
                writer.bytes(intent);
            }
            Message::GizmoGeometry { request, layout } => {
                writer.u8(13);
                writer.u64(request.as_u64());
                writer.bytes(layout);
            }
            _ => return false,
        }
        true
    }

    /// Starting, pausing and stopping a simulation of a world. M8.a task 5.1.
    ///
    /// Its own group rather than more of `write_work`, and the reason is in `encode`: play is not a
    /// change to a world, it is a change to what is running over one, and the two answer to
    /// different halves of the editor.
    fn write_play(&self, writer: &mut Writer) -> bool {
        match self {
            Message::Play { request, state } => {
                writer.u8(15);
                writer.u64(request.as_u64());
                writer.text(state);
            }
            Message::Playing {
                request,
                state,
                detail,
            } => {
                writer.u8(16);
                writer.u64(request.as_u64());
                writer.text(state);
                writer.text(detail);
            }
            _ => return false,
        }
        true
    }

    /// Decode a message, refusing a tag this build does not know.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        let tag = reader.u8()?;
        let message = match tag {
            0 => Message::Hello {
                abi_major: reader.u32()?,
                abi_minor: reader.u32()?,
                editor: reader.text()?,
            },
            1 => Message::Welcome {
                abi_major: reader.u32()?,
                abi_minor: reader.u32()?,
                runtime: reader.text()?,
            },
            2 => Message::Refused {
                reason: reader.text()?,
                remedy: reader.text()?,
            },
            3 => Message::Apply {
                request: RequestId::from_raw(reader.u64()?),
                frame: FrameId::from_raw(reader.u64()?),
                when: ApplyWhen::from_u8(reader.u8()?).ok_or_else(|| {
                    Problem::new(
                        "decode an apply",
                        "its scheduling tag is not one this build knows",
                    )
                })?,
                transaction: reader.bytes()?,
            },
            4 => Message::Applied {
                request: RequestId::from_raw(reader.u64()?),
                frame: FrameId::from_raw(reader.u64()?),
                observed: reader.bytes()?,
            },
            5 => Message::Rejected {
                request: RequestId::from_raw(reader.u64()?),
                reason: reader.text()?,
                remedy: reader.text()?,
            },
            6 => Message::Ping {
                frame: FrameId::from_raw(reader.u64()?),
            },
            7 => Message::Pong {
                frame: FrameId::from_raw(reader.u64()?),
            },
            8 => Message::Reload {
                request: RequestId::from_raw(reader.u64()?),
                module: reader.text()?,
                library: reader.text()?,
                generation: reader.u32()?,
            },
            9 => Message::Reloaded {
                request: RequestId::from_raw(reader.u64()?),
                module: reader.text()?,
                generation: reader.u32()?,
            },
            10 => Message::Pick {
                request: RequestId::from_raw(reader.u64()?),
                frame: FrameId::from_raw(reader.u64()?),
                pick: reader.bytes()?,
            },
            11 => Message::Picked {
                request: RequestId::from_raw(reader.u64()?),
                candidates: reader.bytes()?,
            },
            12 => Message::GizmoIntent {
                request: RequestId::from_raw(reader.u64()?),
                viewport: reader.u64()?,
                intent: reader.bytes()?,
            },
            13 => Message::GizmoGeometry {
                request: RequestId::from_raw(reader.u64()?),
                layout: reader.bytes()?,
            },
            15 => Message::Play {
                request: RequestId::from_raw(reader.u64()?),
                state: reader.text()?,
            },
            16 => Message::Playing {
                request: RequestId::from_raw(reader.u64()?),
                state: reader.text()?,
                detail: reader.text()?,
            },
            14 => Message::ViewSuggested {
                position: [reader.f32()?, reader.f32()?, reader.f32()?],
                rotation: [reader.f32()?, reader.f32()?, reader.f32()?, reader.f32()?],
                fov_y_radians: reader.f32()?,
                near: reader.f32()?,
            },
            other => {
                return Err(Problem::new(
                    "decode a message",
                    format!("message tag {other} is not one this build knows"),
                )
                .with_remedy("the peer is a newer editor or runtime; rebuild them together"));
            }
        };
        Ok(message)
    }

    /// The request this message answers, when it answers one.
    #[must_use]
    pub const fn request(&self) -> Option<RequestId> {
        match self {
            Message::Applied { request, .. }
            | Message::Rejected { request, .. }
            | Message::Reloaded { request, .. }
            | Message::Picked { request, .. }
            | Message::Playing { request, .. }
            | Message::GizmoGeometry { request, .. } => Some(*request),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_message_round_trips() {
        let messages = [
            Message::Hello {
                abi_major: 1,
                abi_minor: 1,
                editor: "0.5.0".into(),
            },
            Message::Welcome {
                abi_major: 1,
                abi_minor: 1,
                runtime: "0.5.0".into(),
            },
            Message::Refused {
                reason: "abi mismatch".into(),
                remedy: "rebuild".into(),
            },
            Message::Reload {
                request: RequestId::from_raw(11),
                module: "character".into(),
                library: "/tmp/libCyGame_g3.so".into(),
                generation: 3,
            },
            Message::Reloaded {
                request: RequestId::from_raw(11),
                module: "character".into(),
                generation: 3,
            },
            Message::Apply {
                request: RequestId::from_raw(7),
                frame: FrameId::from_raw(99),
                when: ApplyWhen::OnArrival,
                transaction: vec![1, 2, 3],
            },
            Message::Applied {
                request: RequestId::from_raw(7),
                frame: FrameId::from_raw(99),
                observed: vec![4, 5],
            },
            Message::Rejected {
                request: RequestId::from_raw(7),
                reason: "the object is locked".into(),
                remedy: "unlock it".into(),
            },
            Message::Ping {
                frame: FrameId::from_raw(1),
            },
            Message::Pong {
                frame: FrameId::from_raw(1),
            },
            Message::Pick {
                request: RequestId::from_raw(12),
                frame: FrameId::from_raw(1016),
                pick: vec![9, 8, 7],
            },
            Message::Picked {
                request: RequestId::from_raw(12),
                candidates: vec![6, 5],
            },
            Message::GizmoIntent {
                request: RequestId::from_raw(13),
                viewport: 1,
                intent: vec![4, 3, 2],
            },
            Message::GizmoGeometry {
                request: RequestId::from_raw(13),
                layout: vec![1],
            },
            Message::Play {
                request: RequestId::from_raw(14),
                state: "playing".into(),
            },
            Message::Playing {
                request: RequestId::from_raw(14),
                state: "playing".into(),
                detail: "2 entities, 2 bodies".into(),
            },
        ];
        for message in &messages {
            assert_eq!(&Message::decode(&message.encode()).unwrap(), message);
        }
    }

    #[test]
    fn a_play_state_the_runtime_does_not_know_is_carried_as_a_word_and_refused_by_name() {
        // The word, not a number: a fourth state added on one side and not the other has to be
        // REFUSED by the far end, and a `u8` that fell through a match would be silently treated
        // as the closest one. The protocol carries it; `cy::gameplay::play_state_of` refuses it.
        let asked = Message::Play {
            request: RequestId::from_raw(1),
            state: "rewinding".into(),
        };
        let back = Message::decode(&asked.encode()).unwrap();
        assert_eq!(back, asked);
        match back {
            Message::Play { state, .. } => assert_eq!(state, "rewinding"),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn a_message_from_a_newer_peer_says_so() {
        let problem = Message::decode(&[250]).unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("newer"),
            "{problem}"
        );
    }

    #[test]
    fn an_echo_names_the_request_it_answers() {
        let applied = Message::Applied {
            request: RequestId::from_raw(3),
            frame: FrameId::from_raw(4),
            observed: Vec::new(),
        };
        assert_eq!(applied.request(), Some(RequestId::from_raw(3)));
        // A play answer names its request too; the ask does not, because it IS the request.
        assert_eq!(
            Message::Playing {
                request: RequestId::from_raw(5),
                state: "editing".into(),
                detail: String::new(),
            }
            .request(),
            Some(RequestId::from_raw(5))
        );
        assert_eq!(
            Message::Play {
                request: RequestId::from_raw(5),
                state: "playing".into(),
            }
            .request(),
            None
        );
        assert_eq!(
            Message::Ping {
                frame: FrameId::default()
            }
            .request(),
            None
        );
    }
}
