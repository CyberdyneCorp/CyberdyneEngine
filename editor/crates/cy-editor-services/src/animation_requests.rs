// SPDX-License-Identifier: MIT
//! The editor's side of the `animation.*` backend operations: one request in flight, the rest
//! queued, and the engine's last answers kept. Issue #29, animation.
//!
//! Nothing reaches a runtime until something asks about an animation graph: the panel being
//! drawn, or an `animation.*` command. Then the catalogue is fetched once per connection and
//! compiles go out as they are asked for. A compile's answer is kept per graph reference together
//! with the source it answered, so a report is never shown against a graph that has changed since.
//!
//! THE PREVIEW is the engine's. The editor keeps only what it asked to see — the graph, the focused
//! clip node, the time, playing or not, and the author's parameters ([`PreviewSettings`]) — and
//! sends all of it with the graph's text in one `animation.preview.set` whenever any of it changes.
//! Only the newest request matters, so a queued one is replaced rather than appended: a scrub drag
//! sends one request per frame, and the engine evaluates the last. While the preview plays, its state
//! is polled ten times a second so the playhead — and `animation.status`, for an agent with no panel
//! on screen — follows the engine's clock. A paused or stopped preview is not polled.

use std::collections::{BTreeMap, VecDeque};
use std::time::{Duration, Instant};

use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::animation_graph::{
    CATALOGUE, COMPILE, CompileReport, PREVIEW_GET, PREVIEW_SET, PREVIEW_STOP, PreviewSettings,
    PreviewState, compile_payload, preview_payload,
};
use crate::runtime::RuntimeSession;

const SCHEMA: u32 = 1;
const MAX_QUEUED: usize = 32;
const POLL_INTERVAL: Duration = Duration::from_millis(100);

/// One queued request: its operation, its payload, and the graph a compile is for.
#[derive(Clone, Debug)]
struct Queued {
    operation: &'static str,
    payload: Vec<u8>,
    reference: String,
    source: String,
}

impl Queued {
    fn plain(operation: &'static str) -> Self {
        Self {
            operation,
            payload: Vec::new(),
            reference: String::new(),
            source: String::new(),
        }
    }
}

/// The `animation.*` request state [`crate::backend::BackendServices`] holds.
#[derive(Debug)]
pub struct AnimationRequests {
    wanted: bool,
    last_poll: Option<Instant>,
    catalogue: Versioned<Option<Vec<u8>>>,
    catalogue_requested: bool,
    in_flight: Option<(RequestId, Queued)>,
    queue: VecDeque<Queued>,
    reports: Versioned<BTreeMap<String, (String, CompileReport)>>,
    preview: Versioned<Option<PreviewState>>,
    settings: Option<PreviewSettings>,
    /// The source the last `animation.preview.set` carried, so an edit is resent once.
    previewed: Option<String>,
    problem: Option<String>,
}

impl Default for AnimationRequests {
    fn default() -> Self {
        Self {
            wanted: false,
            last_poll: None,
            catalogue: Versioned::new(None),
            catalogue_requested: false,
            in_flight: None,
            queue: VecDeque::new(),
            reports: Versioned::new(BTreeMap::new()),
            preview: Versioned::new(None),
            settings: None,
            previewed: None,
            problem: None,
        }
    }
}

impl AnimationRequests {
    /// Ask for the engine's vocabulary now: the panel is drawn.
    pub fn set_polling(&mut self, polling: bool) {
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
                payload: compile_payload(source),
                reference: reference.to_owned(),
                source: source.to_owned(),
            },
        )
    }

    /// Show `settings` of `source`. Replaces a preview request still queued.
    ///
    /// # Errors
    ///
    /// When no runtime is attached: the engine evaluates and draws the preview.
    pub fn preview(
        &mut self,
        runtime: &RuntimeSession,
        settings: PreviewSettings,
        source: &str,
    ) -> Result<Option<RequestId>> {
        let queued = Queued {
            operation: PREVIEW_SET,
            payload: preview_payload(source, &settings),
            reference: settings.reference.clone(),
            source: source.to_owned(),
        };
        self.settings = Some(settings);
        self.previewed = Some(source.to_owned());
        self.queue
            .retain(|waiting| waiting.operation != PREVIEW_SET && waiting.operation != PREVIEW_GET);
        self.enqueue(runtime, queued)
    }

    /// Stop previewing.
    ///
    /// # Errors
    ///
    /// When no runtime is attached.
    pub fn stop(&mut self, runtime: &RuntimeSession) -> Result<Option<RequestId>> {
        self.settings = None;
        self.previewed = None;
        self.queue
            .retain(|waiting| waiting.operation != PREVIEW_SET && waiting.operation != PREVIEW_GET);
        self.enqueue(runtime, Queued::plain(PREVIEW_STOP))
    }

    /// Ask for the preview's state now.
    ///
    /// # Errors
    ///
    /// When no runtime is attached.
    pub fn refresh(&mut self, runtime: &RuntimeSession) -> Result<Option<RequestId>> {
        self.enqueue(runtime, Queued::plain(PREVIEW_GET))
    }

    /// What the editor last asked the preview to show, if it is previewing.
    #[must_use]
    pub const fn settings(&self) -> Option<&PreviewSettings> {
        self.settings.as_ref()
    }

    /// The source the last preview request carried.
    #[must_use]
    pub fn previewed_source(&self) -> Option<&str> {
        self.previewed.as_deref()
    }

    fn enqueue(&mut self, runtime: &RuntimeSession, queued: Queued) -> Result<Option<RequestId>> {
        if !runtime.is_connected() {
            return Err(Problem::new(
                format!("send {} to the engine", queued.operation),
                "no runtime is attached, and the editor neither compiles nor evaluates animation \
                 itself",
            )
            .with_remedy("start the editor with a hosted runtime (`just run-editor-live`)"));
        }
        self.wanted = true;
        if self.queue.len() >= MAX_QUEUED
            && let Some(index) = self.queue.iter().position(|q| q.operation == PREVIEW_GET)
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

    fn playing(&self) -> bool {
        self.preview
            .get()
            .as_ref()
            .is_some_and(|state| state.active && state.playing)
    }

    /// Once a frame: the catalogue, the queue, and a playing preview's clock.
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
            self.queue.push_front(Queued::plain(CATALOGUE));
        }
        let due = self
            .last_poll
            .is_none_or(|last| last.elapsed() >= POLL_INTERVAL);
        if self.playing() && due && self.in_flight.is_none() && self.queue.is_empty() {
            self.last_poll = Some(Instant::now());
            self.queue.push_back(Queued::plain(PREVIEW_GET));
        }
        self.send_next(runtime).err()
    }

    fn disconnect(&mut self) {
        if self.in_flight.take().is_some() || !self.queue.is_empty() {
            self.problem =
                Some("the runtime disconnected before answering the animation request".into());
        }
        self.queue.clear();
        self.catalogue_requested = false;
        if self.preview.get().is_some() {
            self.preview.set(None);
        }
        self.previewed = None;
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
                _ => {
                    self.preview.set(Some(PreviewState::decode(payload)?));
                    Ok(())
                }
            },
            (ServiceEventKind::Failed, _) => Err(refusal(queued.operation, payload)),
            (ServiceEventKind::Cancelled, _) => Err(Problem::new(
                format!("run {}", queued.operation),
                "the engine cancelled the animation request",
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

    /// The preview as the engine last reported it.
    #[must_use]
    pub fn preview_state(&self) -> Option<&PreviewState> {
        self.preview.get().as_ref()
    }

    /// Moves when a preview state arrives.
    #[must_use]
    pub const fn preview_revision(&self) -> Revision {
        self.preview.revision()
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

/// The engine's refusal: `u32 1`, its code, and its words.
fn refusal(operation: &str, payload: &[u8]) -> Problem {
    let mut reader = cy_editor_core::codec::Reader::new(payload);
    let decoded = (|| -> Result<(String, String)> {
        let _format = reader.u32()?;
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

#[cfg(test)]
mod tests {
    use super::*;

    /// A session to a runtime that never answers: the first request stays in flight.
    fn silent_runtime() -> (RuntimeSession, std::io::PipeReader, std::io::PipeWriter) {
        let (editor_reader, runtime_writer) = std::io::pipe().unwrap();
        let (runtime_reader, editor_writer) = std::io::pipe().unwrap();
        let runtime = RuntimeSession::over(cy_editor_protocol::Session::over(
            editor_reader,
            editor_writer,
        ));
        (runtime, runtime_reader, runtime_writer)
    }

    fn settings(time: f32) -> PreviewSettings {
        PreviewSettings {
            reference: "game/animation/locomotion.cyanimgraph".into(),
            focus: 3,
            time,
            playing: false,
            parameters: BTreeMap::new(),
        }
    }

    fn engine_fixture(name: &str) -> Vec<u8> {
        let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../src/editor_backend/tests/data")
            .join(name);
        std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
    }

    #[test]
    fn a_playing_preview_is_followed_without_a_panel_on_screen() {
        // An agent reads `animation.status` with no panel drawn; the engine's clock must reach it.
        let (runtime, _reader, _writer) = silent_runtime();
        // The vocabulary arrived long ago.
        let mut requests = AnimationRequests {
            catalogue_requested: true,
            ..AnimationRequests::default()
        };
        let sent = requests
            .preview(&runtime, settings(0.4), "graph")
            .unwrap()
            .expect("the preview goes out at once");
        let mut playing = engine_fixture("animation_preview_state_v1.wire");
        playing[5] = 1;
        let answered = requests.accept(&Message::ServiceEvent {
            request: sent,
            kind: ServiceEventKind::Completed,
            schema_version: SCHEMA,
            payload: playing,
        });
        assert!(matches!(answered, Some(None)), "{answered:?}");
        assert!(requests.preview_state().is_some_and(|state| state.playing));
        assert!(!requests.pending());
        assert!(requests.maintain(&runtime).is_none());
        assert!(
            requests
                .in_flight
                .as_ref()
                .is_some_and(|(_, queued)| queued.operation == PREVIEW_GET),
            "a playing preview's state is asked for again"
        );
    }

    #[test]
    fn a_newer_preview_replaces_one_still_queued() {
        // A scrub drag asks for a pose every frame; only the last one the engine has not started
        // is worth evaluating, so a queued preview is replaced, never appended behind.
        let (runtime, _reader, _writer) = silent_runtime();
        let mut requests = AnimationRequests::default();
        let first = requests.preview(&runtime, settings(0.1), "graph").unwrap();
        assert!(first.is_some(), "the first preview goes out at once");
        assert!(
            requests
                .preview(&runtime, settings(0.2), "graph")
                .unwrap()
                .is_none()
        );
        assert!(
            requests
                .preview(&runtime, settings(0.3), "graph")
                .unwrap()
                .is_none()
        );
        let queued: Vec<&Queued> = requests
            .queue
            .iter()
            .filter(|queued| queued.operation == PREVIEW_SET)
            .collect();
        assert_eq!(
            queued.len(),
            1,
            "one preview waits behind the one in flight"
        );
        assert_eq!(queued[0].payload, preview_payload("graph", &settings(0.3)));
        assert_eq!(requests.settings().map(|kept| kept.time), Some(0.3));
    }
}
