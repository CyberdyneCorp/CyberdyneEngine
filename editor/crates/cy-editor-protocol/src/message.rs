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
}

impl Message {
    /// Encode a message.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
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
            Message::Ping { frame } => {
                writer.u8(6);
                writer.u64(frame.as_u64());
            }
            Message::Pong { frame } => {
                writer.u8(7);
                writer.u64(frame.as_u64());
            }
        }
        writer.finish()
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
            Message::Applied { request, .. } | Message::Rejected { request, .. } => Some(*request),
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
        ];
        for message in &messages {
            assert_eq!(&Message::decode(&message.encode()).unwrap(), message);
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
        assert_eq!(
            Message::Ping {
                frame: FrameId::default()
            }
            .request(),
            None
        );
    }
}
