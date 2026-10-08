// SPDX-License-Identifier: MIT
//! Animation graphs: the `.cyanimgraph` source the editor writes, and the wire to the engine's
//! `animation.*` operations. Issue #29, animation.
//!
//! **The source is the engine's canonical CyberGraph text** (`cygraph 1`), the same text a gameplay
//! graph is, so it is read and written by [`ScriptGraph`]: nodes by key, properties by name, wires
//! in the engine's order, layout last, floats as C's `%.9g`. The vocabulary is
//! `cy::graph::pose`'s: clips, blends, states and the transitions between them.
//! `src/editor_backend/tests/data/animation_locomotion_v1.cyanimgraph` pins that from both sides:
//! the MCP suite must author it byte for byte, and the engine's suite must read it back to the same
//! bytes and preview it.
//!
//! **A clip's events live on its `pose.clip` node** as the `events` property, `name@seconds` items
//! separated by `; ` ([`format_events`]). The timeline draws them as keys on one event track per
//! name and edits them through `animation.event.*`.
//!
//! The editor computes nothing about what a graph means. The engine declares the vocabulary
//! (`animation.catalogue.get`), compiles (`animation.compile`) and evaluates the preview character
//! (`animation.preview.*`); the payloads are specified in
//! `src/editor_backend/include/cy/editor/animation_service.h` and decoded below.

use std::collections::BTreeMap;

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

use crate::script_graph::{CompileDiagnostic, ScriptGraph, Severity, format_float};

/// The undo record kind for a saved animation graph: this prefix and the project-relative reference.
pub const DOMAIN_PREFIX: &str = "animation_graph:";
/// What an animation graph file is called.
pub const EXTENSION: &str = "cyanimgraph";
/// The node property that holds a clip's events.
pub const EVENTS: &str = "events";
/// The node type a clip is sampled by; only it carries events.
pub const CLIP_NODE: &str = "pose.clip";

/// The engine's `animation.*` operations.
pub const CATALOGUE: &str = "animation.catalogue.get";
/// Compile a source and answer its program or what refused it.
pub const COMPILE: &str = "animation.compile";
/// Show a graph, or one of its clips, on the preview character at a time.
pub const PREVIEW_SET: &str = "animation.preview.set";
/// Read the preview's state.
pub const PREVIEW_GET: &str = "animation.preview.get";
/// Stop previewing.
pub const PREVIEW_STOP: &str = "animation.preview.stop";

/// The wire format every `animation.*` payload begins with.
const WIRE_FORMAT: u32 = 1;
/// The state number the engine answers for "none": no state in a clip preview, no blend.
pub const NO_STATE: u32 = 0xFFFF;

/// Refuse a reference that is not a project-relative `.cyanimgraph` inside the project.
///
/// # Errors
///
/// An empty, absolute or escaping path, or another extension.
pub fn validate_reference(reference: &str) -> Result<()> {
    let path = std::path::Path::new(reference);
    let stem = path
        .file_stem()
        .and_then(|stem| stem.to_str())
        .unwrap_or("");
    // Checked on the text as well as the components, as a gameplay graph's reference is.
    let fine = !reference.is_empty()
        && !reference.starts_with('/')
        && !reference.contains(':')
        && !reference.contains('\\')
        && path
            .components()
            .all(|component| matches!(component, std::path::Component::Normal(_)))
        && path.extension().and_then(|extension| extension.to_str()) == Some(EXTENSION)
        && !stem.is_empty()
        && stem
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-');
    if fine {
        Ok(())
    } else {
        Err(Problem::new(
            format!("use {reference:?} as an animation graph"),
            "an animation graph is a project-relative .cyanimgraph whose name is letters, digits, \
             underscores or hyphens",
        )
        .with_remedy("for example game/animation/locomotion.cyanimgraph"))
    }
}

/// An empty animation graph named after its file. A pose graph is granted nothing: it reads no
/// world and writes none.
#[must_use]
pub fn new_graph(reference: &str) -> ScriptGraph {
    let mut graph = ScriptGraph::new(crate::script_graph::graph_name(reference));
    graph.capabilities.clear();
    graph
}

/// One authored event: its name and where on its clip it sits.
#[derive(Clone, PartialEq, Debug)]
pub struct ClipEvent {
    /// What the event is called: `footstep`.
    pub name: String,
    /// Seconds from the clip's start.
    pub time: f32,
}

fn event_name_is_valid(name: &str) -> bool {
    !name.is_empty()
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'.' | b'-'))
}

/// Refuse an event name the engine would not read.
///
/// # Errors
///
/// An empty name, or one with a character other than letters, digits, `_`, `.` and `-`.
pub fn validate_event_name(name: &str) -> Result<()> {
    if event_name_is_valid(name) {
        Ok(())
    } else {
        Err(Problem::new(
            format!("name an animation event {name:?}"),
            "an event's name is letters, digits, underscores, dots or hyphens",
        )
        .with_remedy("for example footstep or hit.land"))
    }
}

/// Read a clip node's `events` property.
///
/// # Errors
///
/// An item that is not `name@seconds`, which the engine would refuse too.
pub fn parse_events(text: &str) -> Result<Vec<ClipEvent>> {
    let mut events = Vec::new();
    for item in text
        .split(';')
        .map(str::trim)
        .filter(|item| !item.is_empty())
    {
        let refuse = || {
            Problem::new(
                format!("read the animation event {item:?}"),
                "a clip's events are `name@seconds` items separated by `;`",
            )
        };
        let (name, time) = item.split_once('@').ok_or_else(refuse)?;
        let name = name.trim();
        let time: f32 = time.trim().parse().map_err(|_| refuse())?;
        if !event_name_is_valid(name) || !time.is_finite() {
            return Err(refuse());
        }
        events.push(ClipEvent {
            name: name.to_owned(),
            time,
        });
    }
    Ok(events)
}

/// Write events as the `events` property holds them: in time order, then name order, each time
/// as C's `%.9g` so it reads back to the same `f32`.
#[must_use]
pub fn format_events(events: &[ClipEvent]) -> String {
    let mut sorted: Vec<&ClipEvent> = events.iter().collect();
    sorted.sort_by(|a, b| a.time.total_cmp(&b.time).then_with(|| a.name.cmp(&b.name)));
    sorted
        .iter()
        .map(|event| format!("{}@{}", event.name, format_float(event.time)))
        .collect::<Vec<_>>()
        .join("; ")
}

// --- The wire -------------------------------------------------------------------------------------

/// The `animation.compile` request for `source`.
#[must_use]
pub fn compile_payload(source: &str) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.text(source);
    out.finish()
}

/// What the editor asks the preview to show. Kept by the editor between requests, so a scrub sends
/// the parameters the author set and a parameter change keeps the scrubbed time.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct PreviewSettings {
    /// The graph previewed.
    pub reference: String,
    /// The `pose.clip` node previewed alone, or zero for the state machine.
    pub focus: u64,
    /// The time shown, in seconds.
    pub time: f32,
    /// Whether the engine plays it on.
    pub playing: bool,
    /// The author's parameters, by name.
    pub parameters: BTreeMap<String, f32>,
}

/// The `animation.preview.set` request: the graph's text and what to show of it.
#[must_use]
pub fn preview_payload(source: &str, settings: &PreviewSettings) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.text(source);
    out.u64(settings.focus);
    out.f32(settings.time);
    out.u8(u8::from(settings.playing));
    out.u32(u32::try_from(settings.parameters.len()).unwrap_or(u32::MAX));
    for (name, value) in &settings.parameters {
        out.text(name);
        out.f32(*value);
    }
    out.finish()
}

fn expect_format(reader: &mut Reader<'_>, action: &str) -> Result<()> {
    let format = reader.u32()?;
    if format == WIRE_FORMAT {
        Ok(())
    } else {
        Err(Problem::new(
            action,
            format!("the engine answered in animation format {format}; this editor reads 1"),
        ))
    }
}

fn finished(reader: &Reader<'_>, action: &str) -> Result<()> {
    if reader.is_empty() {
        Ok(())
    } else {
        Err(Problem::new(action, "bytes remain after the reply"))
    }
}

/// One state of a compiled program.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct CompiledState {
    /// Its name.
    pub name: String,
    /// The `pose.state` node.
    pub node: u64,
    /// How many transitions leave it.
    pub transitions: u32,
}

/// One transition of a compiled program.
#[derive(Clone, PartialEq, Debug)]
pub struct CompiledTransition {
    /// The `pose.transition` node.
    pub node: u64,
    /// The state it leaves, by number.
    pub from: u32,
    /// The state it enters, by number.
    pub to: u32,
    /// The parameter that opens it.
    pub condition: String,
    /// Its blend, in seconds.
    pub duration: f32,
    /// Its rank among the transitions open at once.
    pub priority: u32,
    /// `none`, `higher_priority` or `any`.
    pub interruption: &'static str,
}

/// One clip a compiled program samples.
#[derive(Clone, PartialEq, Debug)]
pub struct CompiledClip {
    /// The clip's name.
    pub name: String,
    /// Its length as the program records it.
    pub duration: f32,
    /// Whether its clock wraps.
    pub looping: bool,
    /// Whether the preview character has it.
    pub known: bool,
    /// How many events the graph gives it.
    pub events: u32,
}

/// The engine's answer to an `animation.compile`.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct CompileReport {
    /// Whether the graph compiled with no error.
    pub compiled: bool,
    /// The source's semantic digest: what layout does not move.
    pub semantic_digest: u64,
    /// The program's digest: its cook key.
    pub program_digest: u64,
    /// The joints it is compiled for.
    pub joints: u32,
    /// Instructions in the program.
    pub instructions: u32,
    /// The states, in the program's numbering; the first is the entry.
    pub states: Vec<CompiledState>,
    /// The transitions, grouped by the state they leave.
    pub transitions: Vec<CompiledTransition>,
    /// The clips.
    pub clips: Vec<CompiledClip>,
    /// The parameters: `(name, true when it is a clip's clock the runtime advances)`.
    pub parameters: Vec<(String, bool)>,
    /// What the compiler and the authoring checks said, node by node.
    pub diagnostics: Vec<CompileDiagnostic>,
}

impl CompileReport {
    /// Decode an `animation.compile` reply.
    ///
    /// # Errors
    ///
    /// Another format, a truncated reply, or bytes left over.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let action = "read the engine's animation compile";
        let mut reader = Reader::new(payload);
        expect_format(&mut reader, action)?;
        let mut report = Self {
            compiled: reader.u8()? != 0,
            semantic_digest: reader.u64()?,
            program_digest: reader.u64()?,
            joints: reader.u32()?,
            instructions: reader.u32()?,
            ..Self::default()
        };
        for _ in 0..reader.u32()? {
            report.states.push(CompiledState {
                name: reader.text()?,
                node: reader.u64()?,
                transitions: reader.u32()?,
            });
        }
        for _ in 0..reader.u32()? {
            report.transitions.push(CompiledTransition {
                node: reader.u64()?,
                from: reader.u32()?,
                to: reader.u32()?,
                condition: reader.text()?,
                duration: reader.f32()?,
                priority: reader.u32()?,
                interruption: match reader.u8()? {
                    0 => "none",
                    1 => "higher_priority",
                    _ => "any",
                },
            });
        }
        for _ in 0..reader.u32()? {
            report.clips.push(CompiledClip {
                name: reader.text()?,
                duration: reader.f32()?,
                looping: reader.u8()? != 0,
                known: reader.u8()? != 0,
                events: reader.u32()?,
            });
        }
        for _ in 0..reader.u32()? {
            report.parameters.push((reader.text()?, reader.u8()? != 0));
        }
        report.diagnostics = crate::script_graph::read_diagnostics(&mut reader)?;
        finished(&reader, action)?;
        Ok(report)
    }

    /// The errors, the ones that stop the graph compiling.
    pub fn errors(&self) -> impl Iterator<Item = &CompileDiagnostic> {
        self.diagnostics
            .iter()
            .filter(|diagnostic| diagnostic.severity == Severity::Error)
    }

    /// The parameters an author sets: every one that is not a clip's clock.
    pub fn author_parameters(&self) -> impl Iterator<Item = &str> {
        self.parameters
            .iter()
            .filter(|(name, clock)| !clock && !name.is_empty())
            .map(|(name, _)| name.as_str())
    }
}

/// One event the engine's playback crossed.
#[derive(Clone, PartialEq, Debug)]
pub struct FiredEvent {
    /// Increases with every event the preview fires.
    pub sequence: u64,
    /// The event.
    pub name: String,
    /// Where on its clip, from 0 to 1.
    pub normalised_time: f32,
    /// The preview time it was crossed at.
    pub at: f32,
}

/// The preview character, as the engine last evaluated it.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct PreviewState {
    /// Whether anything is previewed.
    pub active: bool,
    /// Whether the engine plays it on.
    pub playing: bool,
    /// The clip node previewed alone, or zero for the state machine.
    pub focus: u64,
    /// That node's clip.
    pub focus_clip: String,
    /// The time shown.
    pub time: f32,
    /// The clip's length, or how long a state machine preview runs.
    pub length: f32,
    /// The state, by number; [`NO_STATE`] in a clip preview.
    pub state: u32,
    /// Its name.
    pub state_name: String,
    /// The state blended towards, or [`NO_STATE`].
    pub target: u32,
    /// Its name.
    pub target_name: String,
    /// How far the blend has gone, 0 to 1.
    pub blend: f32,
    /// The program's digest.
    pub program_digest: u64,
    /// The pose's digest: equal poses, equal digests.
    pub pose_digest: u64,
    /// How many evaluations the engine has made.
    pub generation: u64,
    /// Each joint's local translation, rotation and scale: ten numbers per joint.
    pub joints: Vec<[f32; 10]>,
    /// The most recent events, oldest first.
    pub events: Vec<FiredEvent>,
}

impl PreviewState {
    /// Decode an `animation.preview.*` reply.
    ///
    /// # Errors
    ///
    /// Another format, a truncated reply, or bytes left over.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let action = "read the engine's animation preview";
        let mut reader = Reader::new(payload);
        expect_format(&mut reader, action)?;
        let mut state = Self {
            active: reader.u8()? != 0,
            playing: reader.u8()? != 0,
            focus: reader.u64()?,
            focus_clip: reader.text()?,
            time: reader.f32()?,
            length: reader.f32()?,
            state: reader.u32()?,
            state_name: reader.text()?,
            target: reader.u32()?,
            target_name: reader.text()?,
            blend: reader.f32()?,
            program_digest: reader.u64()?,
            pose_digest: reader.u64()?,
            generation: reader.u64()?,
            ..Self::default()
        };
        for _ in 0..reader.u32()? {
            let mut joint = [0.0_f32; 10];
            for lane in &mut joint {
                *lane = reader.f32()?;
            }
            state.joints.push(joint);
        }
        for _ in 0..reader.u32()? {
            state.events.push(FiredEvent {
                sequence: reader.u64()?,
                name: reader.text()?,
                normalised_time: reader.f32()?,
                at: reader.f32()?,
            });
        }
        finished(&reader, action)?;
        Ok(state)
    }

    /// One line a person reads about what the character is doing.
    #[must_use]
    pub fn describe(&self) -> String {
        if !self.active {
            return "not previewing".into();
        }
        if self.focus != 0 {
            return format!(
                "clip {} at {:.3} s of {:.3} s",
                self.focus_clip, self.time, self.length
            );
        }
        if self.target == NO_STATE {
            format!("state {} at {:.3} s", self.state_name, self.time)
        } else {
            format!(
                "blending {} -> {} ({:.0}%) at {:.3} s",
                self.state_name,
                self.target_name,
                self.blend * 100.0,
                self.time
            )
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine_fixture(name: &str) -> Vec<u8> {
        let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../src/editor_backend/tests/data")
            .join(name);
        std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
    }

    #[test]
    fn the_acceptance_graph_reads_and_writes_back_to_its_own_bytes() {
        let source =
            String::from_utf8(engine_fixture("animation_locomotion_v1.cyanimgraph")).unwrap();
        let graph = ScriptGraph::decode(&source).unwrap();
        assert!(graph.capabilities.is_empty());
        assert_eq!(graph.encode(), source);
        assert_eq!(
            new_graph("game/animation/locomotion.cyanimgraph").encode(),
            "cygraph 1\ngraph \"locomotion\" version 1\ncapability\ndeterministic true\n"
        );
    }

    #[test]
    fn events_read_and_write_as_the_engine_reads_them() {
        let events = parse_events("footstep@0.75; footstep@0.25").unwrap();
        assert_eq!(events.len(), 2);
        assert_eq!(format_events(&events), "footstep@0.25; footstep@0.75");
        assert_eq!(
            format_events(&[ClipEvent {
                name: "hit".into(),
                time: 0.1
            }]),
            "hit@0.100000001"
        );
        assert!(parse_events("").unwrap().is_empty());
        for bad in [
            "footstep",
            "foot step@0.2",
            "footstep@",
            "footstep@x",
            "@0.2",
        ] {
            assert!(parse_events(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn the_engines_compile_and_preview_replies_decode() {
        let compiled = CompileReport::decode(&engine_fixture("animation_compile_v1.wire")).unwrap();
        assert!(compiled.compiled);
        let states: Vec<&str> = compiled.states.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(states, ["idle", "walk"]);
        assert_eq!(compiled.transitions.len(), 2);
        assert_eq!(compiled.transitions[0].condition, "moving");
        let authored: Vec<&str> = compiled.author_parameters().collect();
        assert_eq!(authored, ["moving", "stopped"]);
        assert_eq!(compiled.clips[1].events, 2);

        let cut = CompileReport::decode(&engine_fixture("animation_compile_cut_v1.wire")).unwrap();
        assert!(!cut.compiled);
        assert!(
            cut.errors()
                .any(|d| d.code == "animation.transition.cut" && d.node == 5)
        );

        let state =
            PreviewState::decode(&engine_fixture("animation_preview_state_v1.wire")).unwrap();
        assert!(state.active);
        assert_eq!(state.state_name, "walk");
        assert_eq!(state.target, NO_STATE);
        assert_eq!(state.joints.len(), 12);
        assert!(state.describe().contains("state walk"));
    }

    #[test]
    fn the_preview_request_is_the_one_the_engine_previews() {
        let source =
            String::from_utf8(engine_fixture("animation_locomotion_v1.cyanimgraph")).unwrap();
        let settings = PreviewSettings {
            reference: "game/animation/locomotion.cyanimgraph".into(),
            focus: 0,
            time: 0.4,
            playing: false,
            parameters: BTreeMap::from([("moving".to_owned(), 1.0)]),
        };
        assert_eq!(
            preview_payload(&source, &settings),
            engine_fixture("animation_preview_request_v1.wire")
        );
    }

    #[test]
    fn a_reference_is_a_project_relative_cyanimgraph() {
        assert!(validate_reference("game/animation/locomotion.cyanimgraph").is_ok());
        for bad in [
            "",
            "/abs/a.cyanimgraph",
            "../a.cyanimgraph",
            "a.cyscript",
            "a b.cyanimgraph",
        ] {
            assert!(validate_reference(bad).is_err(), "{bad}");
        }
    }
}
