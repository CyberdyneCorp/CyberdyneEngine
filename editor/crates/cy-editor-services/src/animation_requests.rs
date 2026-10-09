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
//!
//! THE CHARACTER is the engine's too: the editor sends which one a graph plays on
//! (`animation.character.set`) before the preview that needs it, and only when it differs from the
//! one it last sent. Its answer changes the clips a clip node can name, so the vocabulary is asked
//! for again and every kept compile is dropped. A BAKE's answer is kept per graph and handed to the
//! editor once ([`AnimationRequests::take_baked`]), which writes its files into the project.

use std::collections::{BTreeMap, VecDeque};
use std::time::{Duration, Instant};

use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::animation_graph::{
    BAKE, BakeReport, CATALOGUE, CHARACTER_SET, COMPILE, CharacterChoice, CompileReport,
    PREVIEW_GET, PREVIEW_SET, PREVIEW_STOP, PlayedCharacter, PreviewSettings, PreviewState,
    bake_payload, character_payload, compile_payload, preview_payload,
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
    /// The character the editor last asked the engine to play.
    character_sent: Option<CharacterChoice>,
    /// The engine's answer: the character its preview plays.
    character: Versioned<Option<PlayedCharacter>>,
    /// The engine's last bake of each graph.
    bakes: Versioned<BTreeMap<String, BakeReport>>,
    /// Bakes answered and not yet written into the project.
    baked: Vec<(String, BakeReport)>,
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
            character_sent: None,
            character: Versioned::new(None),
            bakes: Versioned::new(BTreeMap::new()),
            baked: Vec::new(),
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

    /// Whether the engine has not been asked to play `choice`: the last character sent differs, or
    /// nothing was sent on this connection.
    #[must_use]
    pub fn wants_character(&self, choice: &CharacterChoice) -> bool {
        self.character_sent.as_ref() != Some(choice)
    }

    /// Ask the engine's preview to play `choice`. Replaces a character request still queued; a
    /// preview queued after it is shown on it.
    ///
    /// # Errors
    ///
    /// When no runtime is attached.
    pub fn character(
        &mut self,
        runtime: &RuntimeSession,
        choice: CharacterChoice,
    ) -> Result<Option<RequestId>> {
        let queued = Queued {
            operation: CHARACTER_SET,
            payload: character_payload(&choice),
            reference: String::new(),
            source: String::new(),
        };
        self.queue
            .retain(|waiting| waiting.operation != CHARACTER_SET);
        let sent = self.enqueue(runtime, queued)?;
        self.character_sent = Some(choice);
        Ok(sent)
    }

    /// Cook `source`, the text of `reference`, for `choice` into the rig `rig`.
    ///
    /// # Errors
    ///
    /// When no runtime is attached: the engine cooks.
    pub fn bake(
        &mut self,
        runtime: &RuntimeSession,
        reference: &str,
        source: &str,
        choice: &CharacterChoice,
    ) -> Result<Option<RequestId>> {
        let rig = crate::animation_graph::rig_name(reference);
        self.enqueue(
            runtime,
            Queued {
                operation: BAKE,
                payload: bake_payload(&rig, source, choice),
                reference: reference.to_owned(),
                source: source.to_owned(),
            },
        )
    }

    /// The character the engine's preview plays, once it has answered.
    #[must_use]
    pub fn played_character(&self) -> Option<&PlayedCharacter> {
        self.character.get().as_ref()
    }

    /// Moves when the engine answers a character.
    #[must_use]
    pub const fn character_revision(&self) -> Revision {
        self.character.revision()
    }

    /// The engine's last bake of `reference`.
    #[must_use]
    pub fn bake_report(&self, reference: &str) -> Option<&BakeReport> {
        self.bakes.get().get(reference)
    }

    /// Moves when a bake is answered.
    #[must_use]
    pub const fn bake_revision(&self) -> Revision {
        self.bakes.revision()
    }

    /// The bakes answered since the last call, each to be written into the project once.
    pub fn take_baked(&mut self) -> Vec<(String, BakeReport)> {
        std::mem::take(&mut self.baked)
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
        // A runtime that starts again plays the mannequin until it is told otherwise.
        self.character_sent = None;
        if self.character.get().is_some() {
            self.character.set(None);
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
        if result.is_err() && queued.operation == CHARACTER_SET {
            // The engine kept the character it had, which is not the one asked for: the next
            // preview asks again rather than being shown on the wrong skeleton.
            self.character_sent = None;
        }
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
                CHARACTER_SET => {
                    self.character.set(Some(PlayedCharacter::decode(payload)?));
                    // Another character, other clips: the palette and every compile were the old
                    // one's. The engine stopped its preview.
                    self.catalogue_requested = false;
                    self.reports.set(BTreeMap::new());
                    self.preview.set(None);
                    Ok(())
                }
                BAKE => {
                    let report = BakeReport::decode(payload)?;
                    let mut bakes = self.bakes.get().clone();
                    bakes.insert(queued.reference.clone(), report.clone());
                    self.bakes.set(bakes);
                    self.baked.push((queued.reference.clone(), report));
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

    fn hero() -> CharacterChoice {
        CharacterChoice {
            model: "characters/hero.fbx".into(),
            skeleton: "0000000000005e1e0000000000000001".into(),
            mesh: "0000000000005e1e0000000000000003".into(),
            clips: vec![("hero".into(), "0000000000005e1e0000000000000002".into())],
        }
    }

    fn answer(requests: &mut AnimationRequests, sent: RequestId, payload: Vec<u8>) {
        let answered = requests.accept(&Message::ServiceEvent {
            request: sent,
            kind: ServiceEventKind::Completed,
            schema_version: SCHEMA,
            payload,
        });
        assert!(matches!(answered, Some(None)), "{answered:?}");
    }

    #[test]
    fn a_character_is_sent_once_and_its_answer_asks_for_the_vocabulary_again() {
        let (runtime, _reader, _writer) = silent_runtime();
        let mut requests = AnimationRequests {
            catalogue_requested: true,
            catalogue: Versioned::new(Some(vec![1])),
            ..AnimationRequests::default()
        };
        assert!(requests.wants_character(&CharacterChoice::default()));
        let sent = requests
            .character(&runtime, hero())
            .unwrap()
            .expect("the character goes out at once");
        assert!(!requests.wants_character(&hero()));
        assert!(requests.wants_character(&CharacterChoice::default()));
        // A compile kept from the last character is not this one's.
        let mut reports = BTreeMap::new();
        reports.insert("g".to_owned(), (String::new(), CompileReport::default()));
        requests.reports.set(reports);
        answer(
            &mut requests,
            sent,
            engine_fixture("animation_character_v1.wire"),
        );
        assert_eq!(
            requests
                .played_character()
                .map(|played| played.model.as_str()),
            Some("characters/hero.fbx")
        );
        assert!(requests.report("g").is_none());
        assert!(requests.maintain(&runtime).is_none());
        assert!(
            requests
                .in_flight
                .as_ref()
                .is_some_and(|(_, queued)| queued.operation == CATALOGUE),
            "another character, other clips: the vocabulary is asked for again"
        );
    }

    #[test]
    fn a_preview_after_a_character_is_shown_on_it() {
        let (runtime, _reader, _writer) = silent_runtime();
        let mut requests = AnimationRequests {
            catalogue_requested: true,
            ..AnimationRequests::default()
        };
        // Something in flight, so both wait in the queue in the order they were asked.
        assert!(requests.refresh(&runtime).unwrap().is_some());
        requests.character(&runtime, hero()).unwrap();
        requests.preview(&runtime, settings(0.5), "graph").unwrap();
        let order: Vec<&str> = requests
            .queue
            .iter()
            .map(|queued| queued.operation)
            .collect();
        assert_eq!(order, [CHARACTER_SET, PREVIEW_SET]);
        // The editor always sends a character with the preview that needs it: a newer pair
        // replaces the queued one, character first.
        requests
            .character(&runtime, CharacterChoice::default())
            .unwrap();
        requests.preview(&runtime, settings(0.6), "graph").unwrap();
        let order: Vec<&str> = requests
            .queue
            .iter()
            .map(|queued| queued.operation)
            .collect();
        assert_eq!(order, [CHARACTER_SET, PREVIEW_SET]);
        assert_eq!(
            requests.queue[0].payload,
            character_payload(&CharacterChoice::default())
        );
    }

    #[test]
    fn a_bake_is_handed_over_once_and_kept_for_its_graph() {
        let (runtime, _reader, _writer) = silent_runtime();
        let mut requests = AnimationRequests {
            catalogue_requested: true,
            ..AnimationRequests::default()
        };
        let sent = requests
            .bake(
                &runtime,
                "game/animation/hero.cyanimgraph",
                "graph",
                &hero(),
            )
            .unwrap()
            .expect("the bake goes out at once");
        answer(
            &mut requests,
            sent,
            engine_fixture("animation_bake_v1.wire"),
        );
        let baked = requests.take_baked();
        assert_eq!(baked.len(), 1);
        assert_eq!(baked[0].0, "game/animation/hero.cyanimgraph");
        assert!(baked[0].1.baked);
        assert!(requests.take_baked().is_empty(), "written once");
        assert!(
            requests
                .bake_report("game/animation/hero.cyanimgraph")
                .is_some_and(|report| report.files.len() == 3)
        );
    }

    #[test]
    fn a_refused_character_is_asked_for_again() {
        // The engine keeps the character it had when it refuses one (a model not cooked yet): the
        // editor must not believe it plays the refused one.
        let (runtime, _reader, _writer) = silent_runtime();
        let mut requests = AnimationRequests {
            catalogue_requested: true,
            ..AnimationRequests::default()
        };
        let sent = requests.character(&runtime, hero()).unwrap().unwrap();
        let mut failure = cy_editor_core::codec::Writer::new();
        failure.u32(1);
        failure.text("animation.character.failed");
        failure.text("the character's cooked skeleton could not be read");
        let answered = requests.accept(&Message::ServiceEvent {
            request: sent,
            kind: ServiceEventKind::Failed,
            schema_version: SCHEMA,
            payload: failure.finish(),
        });
        assert!(matches!(answered, Some(Some(_))), "{answered:?}");
        assert!(requests.wants_character(&hero()));
        assert!(
            requests
                .problem()
                .is_some_and(|problem| problem.contains("animation.character.failed"))
        );
    }
}
