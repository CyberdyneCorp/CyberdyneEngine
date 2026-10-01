// SPDX-License-Identifier: MIT
//! The editor's side of the `script.*` backend operations: one request in flight, the rest queued,
//! and the engine's last answers kept. Issue #29, visual scripting.
//!
//! Nothing reaches a runtime until something asks for a gameplay graph: the panel being drawn, or
//! a `script.*` command. Then the catalogue is fetched once per connection, compiles go out as
//! they are asked for, and while the panel is on screen during Play the state is polled four times
//! a second. A compile's answer is kept per graph reference together with the source it answered,
//! so a report is never shown against a graph that has changed since.

use std::collections::{BTreeMap, VecDeque};
use std::time::{Duration, Instant};

use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::runtime::RuntimeSession;
use crate::script_graph::{CATALOGUE, COMPILE, CompileReport, PlayState, RAISE, STATE};

const SCHEMA: u32 = 1;
const MAX_QUEUED: usize = 32;
const POLL_INTERVAL: Duration = Duration::from_millis(250);

/// One queued request: its operation, its payload, and the graph a compile is for.
#[derive(Clone, Debug)]
struct Queued {
    operation: &'static str,
    payload: Vec<u8>,
    reference: String,
    source: String,
}

/// The `script.*` request state [`crate::backend::BackendServices`] holds.
#[derive(Debug)]
pub struct ScriptRequests {
    wanted: bool,
    polling: bool,
    last_poll: Option<Instant>,
    catalogue: Versioned<Option<Vec<u8>>>,
    catalogue_requested: bool,
    in_flight: Option<(RequestId, Queued)>,
    queue: VecDeque<Queued>,
    reports: Versioned<BTreeMap<String, (String, CompileReport)>>,
    state: Versioned<Option<PlayState>>,
    started: Option<u32>,
    problem: Option<String>,
}

impl Default for ScriptRequests {
    fn default() -> Self {
        Self {
            wanted: false,
            polling: false,
            last_poll: None,
            catalogue: Versioned::new(None),
            catalogue_requested: false,
            in_flight: None,
            queue: VecDeque::new(),
            reports: Versioned::new(BTreeMap::new()),
            state: Versioned::new(None),
            started: None,
            problem: None,
        }
    }
}

impl ScriptRequests {
    /// Ask for the engine's vocabulary now, and keep Play's state fresh while `polling`.
    pub fn set_polling(&mut self, polling: bool) {
        self.polling = polling;
        if polling {
            self.wanted = true;
        }
    }

    /// Ask for the vocabulary without polling: what a command does before it needs it.
    pub fn want(&mut self) {
        self.wanted = true;
    }

    /// Queue a compile of `source`, the text of `reference`.
    ///
    /// # Errors
    ///
    /// When no runtime is attached: the editor does not compile graphs itself.
    pub fn compile(
        &mut self,
        runtime: &RuntimeSession,
        reference: &str,
        source: &str,
    ) -> Result<Option<RequestId>> {
        self.enqueue(
            runtime,
            Queued {
                operation: COMPILE,
                payload: crate::script_graph::compile_payload(source),
                reference: reference.to_owned(),
                source: source.to_owned(),
            },
        )
    }

    /// Queue an event raise.
    ///
    /// # Errors
    ///
    /// When no runtime is attached.
    pub fn raise(&mut self, runtime: &RuntimeSession, payload: Vec<u8>) -> Result<Option<RequestId>> {
        self.enqueue(
            runtime,
            Queued {
                operation: RAISE,
                payload,
                reference: String::new(),
                source: String::new(),
            },
        )
    }

    /// Queue a state read.
    ///
    /// # Errors
    ///
    /// When no runtime is attached.
    pub fn refresh(&mut self, runtime: &RuntimeSession) -> Result<Option<RequestId>> {
        self.enqueue(runtime, state_request())
    }

    fn enqueue(&mut self, runtime: &RuntimeSession, queued: Queued) -> Result<Option<RequestId>> {
        if !runtime.is_connected() {
            return Err(Problem::new(
                format!("send {} to the engine", queued.operation),
                "no runtime is attached, and the editor neither compiles nor runs graphs itself",
            )
            .with_remedy("start the editor with a hosted runtime (`just run-editor-live`)"));
        }
        self.wanted = true;
        if self.queue.len() >= MAX_QUEUED
            && let Some(index) = self.queue.iter().position(|q| q.operation == STATE)
        {
            self.queue.remove(index);
        }
        self.queue.push_back(queued);
        self.send_next(runtime)
    }

    fn send_next(&mut self, runtime: &RuntimeSession) -> Result<Option<RequestId>> {
        if self.in_flight.is_some() {
            return Ok(None);
        }
        let Some(queued) = self.queue.pop_front() else {
            return Ok(None);
        };
        let request = runtime.service_request(SCHEMA, queued.operation, queued.payload.clone())?;
        self.in_flight = Some((request, queued));
        Ok(Some(request))
    }

    /// Once a frame: the catalogue, the queue, and Play's state while the panel is drawn.
    pub fn maintain(&mut self, runtime: &RuntimeSession) -> Option<Problem> {
        if !runtime.is_connected() {
            self.disconnect();
            return None;
        }
        if !self.wanted {
            return None;
        }
        if !self.catalogue_requested {
            self.catalogue_requested = true;
            self.queue.push_front(Queued {
                operation: CATALOGUE,
                payload: Vec::new(),
                reference: String::new(),
                source: String::new(),
            });
        }
        let due = self
            .last_poll
            .is_none_or(|last| last.elapsed() >= POLL_INTERVAL);
        if self.polling && due && self.in_flight.is_none() && self.queue.is_empty() {
            self.last_poll = Some(Instant::now());
            self.queue.push_back(state_request());
        }
        self.send_next(runtime).err()
    }

    fn disconnect(&mut self) {
        if self.in_flight.take().is_some() || !self.queue.is_empty() {
            self.problem =
                Some("the runtime disconnected before answering the gameplay graph request".into());
        }
        self.queue.clear();
        self.catalogue_requested = false;
        if self.state.get().is_some() {
            self.state.set(None);
        }
    }

    /// Take the engine's answer to the request in flight; `None` for a message that is not one.
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
        let (pending, _) = self.in_flight.as_ref()?;
        if pending != request {
            return None;
        }
        if matches!(
            kind,
            ServiceEventKind::Accepted | ServiceEventKind::Progress
        ) {
            return Some(None);
        }
        let (_, queued) = self.in_flight.take().expect("checked above");
        let result = self.settle(&queued, *kind, *schema_version, payload);
        match &result {
            Ok(()) => self.problem = None,
            Err(problem) => self.problem = Some(problem.to_string()),
        }
        Some(result.err())
    }

    fn settle(
        &mut self,
        queued: &Queued,
        kind: ServiceEventKind,
        schema_version: u32,
        payload: &[u8],
    ) -> Result<()> {
        match (kind, schema_version) {
            (ServiceEventKind::Completed, SCHEMA) => match queued.operation {
                CATALOGUE => {
                    self.catalogue.set(Some(payload.to_vec()));
                    Ok(())
                }
                COMPILE => {
                    let report = CompileReport::decode(payload)?;
                    let mut reports = self.reports.get().clone();
                    reports.insert(queued.reference.clone(), (queued.source.clone(), report));
                    self.reports.set(reports);
                    Ok(())
                }
                RAISE => {
                    let (started, state) = PlayState::decode_raise(payload)?;
                    self.started = Some(started);
                    self.state.set(Some(state));
                    Ok(())
                }
                _ => {
                    self.state.set(Some(PlayState::decode(payload)?));
                    Ok(())
                }
            },
            (ServiceEventKind::Failed, _) => Err(refusal(queued.operation, payload)),
            (ServiceEventKind::Cancelled, _) => Err(Problem::new(
                format!("run {}", queued.operation),
                "the engine cancelled the gameplay graph request",
            )),
            _ => Err(Problem::new(
                format!("run {}", queued.operation),
                "the engine answered with an incompatible schema",
            )),
        }
    }

    /// The engine's node vocabulary, once it has answered.
    #[must_use]
    pub fn catalogue(&self) -> Option<&[u8]> {
        self.catalogue.get().as_deref()
    }

    /// Moves when the vocabulary arrives.
    #[must_use]
    pub const fn catalogue_revision(&self) -> Revision {
        self.catalogue.revision()
    }

    /// The engine's last compile of `reference`, with the source it compiled.
    #[must_use]
    pub fn report(&self, reference: &str) -> Option<&(String, CompileReport)> {
        self.reports.get().get(reference)
    }

    /// Moves when any compile answer arrives.
    #[must_use]
    pub const fn reports_revision(&self) -> Revision {
        self.reports.revision()
    }

    /// Play's last reported graphs.
    #[must_use]
    pub fn state(&self) -> Option<&PlayState> {
        self.state.get().as_ref()
    }

    /// How many handlers the last raise started.
    #[must_use]
    pub const fn started(&self) -> Option<u32> {
        self.started
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
}

fn state_request() -> Queued {
    Queued {
        operation: STATE,
        payload: Vec::new(),
        reference: String::new(),
        source: String::new(),
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
