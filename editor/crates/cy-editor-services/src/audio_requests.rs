// SPDX-License-Identifier: MIT
//! The editor's side of the `audio.*` backend operations: one request in flight, the rest queued,
//! and the engine's last answer kept. Issue #29.
//!
//! Queued rather than refused while one is pending, because two authoring actions in one frame —
//! an undo and the mixer apply it implies, or a preview straight after a save — are ordinary, and
//! refusing the second would leave the engine one edit behind the document with nothing saying so.
//! The queue is bounded; past its bound the oldest state read is dropped first, never an edit.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

use cy_editor_core::codec::Writer;
use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::audio::{AudioState, AudioVocabulary};
use crate::runtime::RuntimeSession;

const SCHEMA: u32 = 1;
const CAPABILITIES: &str = "audio.capabilities.get";
const STATE: &str = "audio.state.get";
const MAX_QUEUED: usize = 32;
/// How often a visible mixer asks for levels. Four times a second reads as live on a meter and
/// costs the bridge nothing measurable.
const POLL_INTERVAL: Duration = Duration::from_millis(250);

/// The `audio.*` request state [`crate::backend::BackendServices`] holds.
#[derive(Debug)]
#[expect(
    clippy::struct_excessive_bools,
    reason = "four independent latches of one connection's audio lifecycle"
)]
pub struct AudioRequests {
    wanted: bool,
    polling: bool,
    last_poll: Option<Instant>,
    vocabulary: Versioned<Option<AudioVocabulary>>,
    vocabulary_requested: bool,
    in_flight: Option<(RequestId, String)>,
    queue: VecDeque<(String, Vec<u8>)>,
    state: Versioned<Option<AudioState>>,
    problem: Option<String>,
    needs_mixer: bool,
}

impl Default for AudioRequests {
    fn default() -> Self {
        Self {
            wanted: false,
            polling: false,
            last_poll: None,
            vocabulary: Versioned::new(None),
            vocabulary_requested: false,
            in_flight: None,
            queue: VecDeque::new(),
            state: Versioned::new(None),
            problem: None,
            needs_mixer: false,
        }
    }
}

impl AudioRequests {
    /// Ask for the engine's vocabulary and keep asking for levels while a mixer is on screen.
    /// Nothing audio is sent to a runtime until something wants it.
    pub fn set_polling(&mut self, polling: bool) {
        self.polling = polling;
        if polling {
            self.want();
        }
    }

    fn want(&mut self) {
        if !self.wanted {
            self.wanted = true;
            self.needs_mixer = true;
        }
    }

    /// Queue one operation, sending it now when nothing else is in flight.
    ///
    /// Answers the request's identity when it was sent and `None` when it was queued.
    pub fn request(
        &mut self,
        runtime: &RuntimeSession,
        operation: &str,
        payload: Vec<u8>,
    ) -> Result<Option<RequestId>> {
        if !runtime.is_connected() {
            return Err(Problem::new(
                format!("send {operation} to the engine"),
                "no runtime is attached, and the editor does not mix audio itself",
            )
            .with_remedy("start the editor with a hosted runtime (`just run-editor-live`)"));
        }
        self.want();
        if operation == crate::audio::MIXER_APPLY {
            self.needs_mixer = false;
        }
        if self.queue.len() >= MAX_QUEUED
            && let Some(index) = self.queue.iter().position(|(queued, _)| queued == STATE)
        {
            self.queue.remove(index);
        }
        self.queue.push_back((operation.to_owned(), payload));
        self.send_next(runtime)
    }

    fn send_next(&mut self, runtime: &RuntimeSession) -> Result<Option<RequestId>> {
        if self.in_flight.is_some() {
            return Ok(None);
        }
        let Some((operation, payload)) = self.queue.pop_front() else {
            return Ok(None);
        };
        let request = runtime.service_request(SCHEMA, &operation, payload)?;
        self.in_flight = Some((request, operation));
        Ok(Some(request))
    }

    /// Once a frame: the vocabulary, the queue, and the meters.
    pub fn maintain(&mut self, runtime: &RuntimeSession) -> Option<Problem> {
        if !runtime.is_connected() {
            self.disconnect();
            return None;
        }
        if !self.wanted {
            return None;
        }
        if !self.vocabulary_requested {
            self.vocabulary_requested = true;
            self.queue.push_front((CAPABILITIES.into(), Vec::new()));
        }
        let due = self
            .last_poll
            .is_none_or(|last| last.elapsed() >= POLL_INTERVAL);
        if self.polling
            && due
            && self.in_flight.is_none()
            && self.queue.is_empty()
            && self.vocabulary.get().is_some()
        {
            self.last_poll = Some(Instant::now());
            self.queue.push_back((STATE.into(), Vec::new()));
        }
        self.send_next(runtime).err()
    }

    fn disconnect(&mut self) {
        if self.in_flight.take().is_some() || !self.queue.is_empty() {
            self.problem =
                Some("the runtime disconnected before answering the audio request".into());
        }
        self.queue.clear();
        self.vocabulary_requested = false;
        self.needs_mixer = self.wanted;
    }

    /// Take the engine's answer to the request in flight. `None` for a message that is not one;
    /// the next queued request goes out on the next [`Self::maintain`].
    pub fn accept(&mut self, message: &Message) -> Option<Option<Problem>> {
        let Message::ServiceEvent {
            request,
            kind,
            schema_version,
            payload,
        } = message
        else {
            return None;
        };
        let (pending, operation) = self.in_flight.as_ref()?;
        if pending != request {
            return None;
        }
        if matches!(
            kind,
            ServiceEventKind::Accepted | ServiceEventKind::Progress
        ) {
            return Some(None);
        }
        let operation = operation.clone();
        self.in_flight = None;
        Some(
            self.settle(&operation, *kind, *schema_version, payload)
                .err(),
        )
    }

    fn settle(
        &mut self,
        operation: &str,
        kind: ServiceEventKind,
        schema_version: u32,
        payload: &[u8],
    ) -> Result<()> {
        let result = match (kind, schema_version) {
            (ServiceEventKind::Completed, SCHEMA) if operation == CAPABILITIES => {
                AudioVocabulary::decode(payload).map(|vocabulary| {
                    self.vocabulary.set(Some(vocabulary));
                })
            }
            (ServiceEventKind::Completed, SCHEMA) => AudioState::decode(payload).map(|state| {
                self.state.set(Some(state));
            }),
            (ServiceEventKind::Failed, _) => Err(refusal(operation, payload)),
            (ServiceEventKind::Cancelled, _) => Err(Problem::new(
                format!("run {operation}"),
                "the engine cancelled the audio request",
            )),
            _ => Err(Problem::new(
                format!("run {operation}"),
                "the engine answered with an incompatible audio schema",
            )),
        };
        match &result {
            Ok(()) => self.problem = None,
            Err(problem) => self.problem = Some(problem.to_string()),
        }
        result
    }

    /// The engine's vocabulary, once it has answered.
    #[must_use]
    pub fn vocabulary(&self) -> Option<&AudioVocabulary> {
        self.vocabulary.get().as_ref()
    }

    /// Moves when the vocabulary arrives.
    #[must_use]
    pub const fn vocabulary_revision(&self) -> Revision {
        self.vocabulary.revision()
    }

    /// The engine's last state.
    #[must_use]
    pub fn state(&self) -> Option<&AudioState> {
        self.state.get().as_ref()
    }

    /// Moves when a state arrives.
    #[must_use]
    pub const fn state_revision(&self) -> Revision {
        self.state.revision()
    }

    /// The last refusal, in the engine's words.
    #[must_use]
    pub fn problem(&self) -> Option<&str> {
        self.problem.as_deref()
    }

    /// Whether a request is in flight or queued.
    #[must_use]
    pub fn pending(&self) -> bool {
        self.in_flight.is_some() || !self.queue.is_empty()
    }

    /// Whether the project's mixer has not been sent since the engine connected.
    #[must_use]
    pub const fn needs_mixer(&self) -> bool {
        self.needs_mixer
    }

    /// Record that there is no mixer to send, so the engine keeps its Master-only graph.
    pub fn mixer_absent(&mut self) {
        self.needs_mixer = false;
    }
}

/// The engine's refusal: `u32 1`, its code, and its words.
fn refusal(operation: &str, payload: &[u8]) -> Problem {
    let mut reader = cy_editor_core::codec::Reader::new(payload);
    let decoded = (|| -> Result<(String, String)> {
        let _count = reader.u32()?;
        Ok((reader.text()?, reader.text()?))
    })();
    match decoded {
        Ok((code, detail)) => Problem::new(
            format!("run {operation} in the engine"),
            format!("{code}: {detail}"),
        ),
        Err(_) => Problem::new(
            format!("run {operation} in the engine"),
            "the engine refused with an unreadable diagnostic",
        ),
    }
}

/// The `audio.state.get` payload that advances a null-backend mix by `seconds` before answering.
#[must_use]
pub fn advance_payload(seconds: f32) -> Vec<u8> {
    let mut out = Writer::new();
    out.f32(seconds);
    out.finish()
}
