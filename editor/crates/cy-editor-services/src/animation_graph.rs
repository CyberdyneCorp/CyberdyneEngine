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
/// Play the preview on the built-in mannequin or on one of the project's imported characters.
pub const CHARACTER_SET: &str = "animation.character.set";
/// Cook a graph, for its character, into the rig a game loads.
pub const BAKE: &str = "animation.bake";

/// What a graph's character file is called, beside the graph: `hero.cyanimgraph` ->
/// `hero.cyanimcharacter`.
pub const CHARACTER_EXTENSION: &str = "cyanimcharacter";
/// Where the engine's bake of a graph is written, project-relative: one directory per rig.
pub const RIG_DIRECTORY: &str = ".cy/cooked/animation";

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

/// The character file beside a graph: the same path with the `.cyanimcharacter` extension.
#[must_use]
pub fn character_reference(reference: &str) -> String {
    std::path::Path::new(reference)
        .with_extension(CHARACTER_EXTENSION)
        .to_string_lossy()
        .replace('\\', "/")
}

/// The graph a character file belongs to, when `reference` is one.
#[must_use]
pub fn graph_of_character(reference: &str) -> Option<String> {
    let path = std::path::Path::new(reference);
    (path.extension().and_then(|extension| extension.to_str()) == Some(CHARACTER_EXTENSION)).then(
        || {
            path.with_extension(EXTENSION)
                .to_string_lossy()
                .replace('\\', "/")
        },
    )
}

/// The name a baked graph's rig is registered under, which a game attaches by: the file's stem.
#[must_use]
pub fn rig_name(reference: &str) -> String {
    crate::script_graph::graph_name(reference)
}

/// The character file's text for a model: `cyanimcharacter 1` and the model's project-relative
/// source. The mannequin has no file.
#[must_use]
pub fn format_character(model: &str) -> String {
    format!("cyanimcharacter 1\nmodel \"{model}\"\n")
}

/// Read a character file: the model it names.
///
/// # Errors
///
/// Anything but `cyanimcharacter 1` and one quoted `model`.
pub fn parse_character(text: &str) -> Result<String> {
    let refuse = || {
        Problem::new(
            "read an animation graph's character",
            "a character file is `cyanimcharacter 1` and one `model \"<source>\"` line",
        )
    };
    let mut lines = text.lines().filter(|line| !line.trim().is_empty());
    if lines.next().map(str::trim) != Some("cyanimcharacter 1") {
        return Err(refuse());
    }
    let model = lines
        .next()
        .and_then(|line| line.trim().strip_prefix("model "))
        .and_then(|quoted| quoted.strip_prefix('"')?.strip_suffix('"'))
        .filter(|model| !model.is_empty() && !model.contains('"'))
        .ok_or_else(refuse)?;
    if lines.next().is_some() {
        return Err(refuse());
    }
    Ok(model.to_owned())
}

/// One character a project can preview: an imported model whose import cooked a skeleton.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct ProjectCharacter {
    /// The model's project-relative source, `characters/hero.fbx`.
    pub model: String,
    /// The cooked skeleton's id.
    pub skeleton: String,
    /// The model's first mesh's id, which the engine draws skinned; empty when it has none.
    pub mesh: String,
}

/// One clip a project's imports cooked: the name a graph gives it and its id.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ProjectClip {
    /// The sub-asset's leaf: `animation/Walking` -> `Walking`.
    pub name: String,
    /// The cooked clip's id.
    pub id: String,
    /// The source that cooked it.
    pub source: String,
}

/// Every imported model with a skeleton, in source order.
#[must_use]
pub fn project_characters(entries: &[crate::asset_catalogue::AssetEntry]) -> Vec<ProjectCharacter> {
    let mut characters: Vec<ProjectCharacter> = Vec::new();
    for entry in entries {
        let (Some(name), Some(identity)) = (&entry.sub_asset, &entry.identity) else {
            continue;
        };
        if name.starts_with("skeleton/")
            && !characters.iter().any(|known| known.model == entry.source)
        {
            let mesh = entries
                .iter()
                .find(|other| {
                    other.source == entry.source
                        && other
                            .sub_asset
                            .as_deref()
                            .is_some_and(|sub| sub.starts_with("mesh/"))
                })
                .and_then(|other| other.identity.clone())
                .unwrap_or_default();
            characters.push(ProjectCharacter {
                model: entry.source.clone(),
                skeleton: identity.clone(),
                mesh,
            });
        }
    }
    characters.sort_by(|a, b| a.model.cmp(&b.model));
    characters
}

/// Every clip the project's imports cooked, by the name a graph gives it, in name order.
#[must_use]
pub fn project_clips(entries: &[crate::asset_catalogue::AssetEntry]) -> Vec<ProjectClip> {
    let mut clips: Vec<ProjectClip> = entries
        .iter()
        .filter_map(|entry| {
            let leaf = entry.sub_asset.as_deref()?.strip_prefix("animation/")?;
            Some(ProjectClip {
                name: leaf.to_owned(),
                id: entry.identity.clone()?,
                source: entry.source.clone(),
            })
        })
        .collect();
    clips.sort_by(|a, b| a.name.cmp(&b.name).then_with(|| a.source.cmp(&b.source)));
    clips
}

/// What the engine is asked to play: the mannequin (the default, every field empty) or a project
/// character with every clip the project has, which the engine checks against its skeleton.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct CharacterChoice {
    /// The model's project-relative source; empty for the mannequin.
    pub model: String,
    /// The cooked skeleton's id; empty for the mannequin.
    pub skeleton: String,
    /// The skinned mesh's id, or empty to draw one box per bone.
    pub mesh: String,
    /// `(name, id)` of every clip offered.
    pub clips: Vec<(String, String)>,
}

impl CharacterChoice {
    /// The project character imported from `model`, with every clip the project has.
    ///
    /// # Errors
    ///
    /// When no import of `model` cooked a skeleton.
    pub fn for_model(entries: &[crate::asset_catalogue::AssetEntry], model: &str) -> Result<Self> {
        let character = project_characters(entries)
            .into_iter()
            .find(|character| character.model == model)
            .ok_or_else(|| {
                Problem::new(
                    format!("play an animation graph on {model}"),
                    "no import of that model cooked a skeleton",
                )
                .with_remedy(
                    "import a rigged FBX, then choose it; animation.character.list lists them",
                )
            })?;
        Ok(Self {
            model: character.model,
            skeleton: character.skeleton,
            mesh: character.mesh,
            clips: project_clips(entries)
                .into_iter()
                .map(|clip| (clip.name, clip.id))
                .collect(),
        })
    }

    /// Whether this is the built-in mannequin.
    #[must_use]
    pub fn is_mannequin(&self) -> bool {
        self.skeleton.is_empty()
    }

    fn write(&self, out: &mut Writer) {
        out.text(&self.model);
        out.text(&self.skeleton);
        out.text(&self.mesh);
        out.u32(u32::try_from(self.clips.len()).unwrap_or(u32::MAX));
        for (name, id) in &self.clips {
            out.text(name);
            out.text(id);
        }
    }
}

/// The `animation.character.set` request.
#[must_use]
pub fn character_payload(choice: &CharacterChoice) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    choice.write(&mut out);
    out.finish()
}

/// The `animation.bake` request: the rig's name, the graph's text and its character.
#[must_use]
pub fn bake_payload(rig: &str, source: &str, choice: &CharacterChoice) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.text(rig);
    out.text(source);
    choice.write(&mut out);
    out.finish()
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

/// The character the engine's preview plays, as it answered `animation.character.set`.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct PlayedCharacter {
    /// The model it was imported from; empty for the mannequin.
    pub model: String,
    /// Whether it is a project's own.
    pub project: bool,
    /// Its skeleton's joints.
    pub joints: u32,
    /// Whether it is drawn with its skin rather than one box per bone.
    pub skinned: bool,
    /// `(name, seconds, looping)` of every clip it took.
    pub clips: Vec<(String, f32, bool)>,
    /// `(name, why)` of every clip it refused.
    pub refused: Vec<(String, String)>,
}

impl PlayedCharacter {
    /// Decode an `animation.character.set` reply.
    ///
    /// # Errors
    ///
    /// Another format, a truncated reply, or bytes left over.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let action = "read the engine's animation character";
        let mut reader = Reader::new(payload);
        expect_format(&mut reader, action)?;
        let mut played = Self {
            model: reader.text()?,
            project: reader.u8()? != 0,
            joints: reader.u32()?,
            skinned: reader.u8()? != 0,
            ..Self::default()
        };
        for _ in 0..reader.u32()? {
            played
                .clips
                .push((reader.text()?, reader.f32()?, reader.u8()? != 0));
        }
        for _ in 0..reader.u32()? {
            played.refused.push((reader.text()?, reader.text()?));
        }
        finished(&reader, action)?;
        Ok(played)
    }

    /// One line a person reads about it.
    #[must_use]
    pub fn describe(&self) -> String {
        let who = if self.project {
            self.model.clone()
        } else {
            "the mannequin".to_owned()
        };
        let refused = if self.refused.is_empty() {
            String::new()
        } else {
            format!(", {} refused", self.refused.len())
        };
        format!(
            "{who}: {} joint(s), {} clip(s){refused}",
            self.joints,
            self.clips.len()
        )
    }
}

/// The engine's answer to an `animation.bake`.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct BakeReport {
    /// Whether the rig was cooked; false when the graph has an error.
    pub baked: bool,
    /// `(path in the rig's directory, bytes)` of every file to write.
    pub files: Vec<(String, Vec<u8>)>,
    /// What the compiler and the authoring checks said.
    pub diagnostics: Vec<CompileDiagnostic>,
}

impl BakeReport {
    /// Decode an `animation.bake` reply.
    ///
    /// # Errors
    ///
    /// Another format, a truncated reply, a path that would leave the rig's directory, or bytes
    /// left over.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let action = "read the engine's animation bake";
        let mut reader = Reader::new(payload);
        expect_format(&mut reader, action)?;
        let mut report = Self {
            baked: reader.u8()? != 0,
            ..Self::default()
        };
        for _ in 0..reader.u32()? {
            let path = reader.text()?;
            let inside = std::path::Path::new(&path)
                .components()
                .all(|component| matches!(component, std::path::Component::Normal(_)));
            if path.is_empty() || !inside {
                return Err(Problem::new(
                    action,
                    format!("the engine named a file outside the rig: {path:?}"),
                ));
            }
            report.files.push((path, reader.bytes()?));
        }
        report.diagnostics = crate::script_graph::read_diagnostics(&mut reader)?;
        finished(&reader, action)?;
        Ok(report)
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

    /// The hero's import records, as the importer leaves them, with the engine suite's ids.
    fn hero_entries() -> Vec<crate::asset_catalogue::AssetEntry> {
        let entry =
            |source: &str, sub: Option<&str>, id: &str| crate::asset_catalogue::AssetEntry {
                path: sub.map_or_else(|| source.to_owned(), |sub| format!("{source}#{sub}")),
                kind: String::new(),
                fingerprint: String::new(),
                identity: Some(id.to_owned()),
                source: source.to_owned(),
                sub_asset: sub.map(str::to_owned),
            };
        vec![
            entry(
                "characters/hero.fbx",
                None,
                "0000000000005e1e0000000000000000",
            ),
            entry(
                "characters/hero.fbx",
                Some("animation/hero"),
                "0000000000005e1e0000000000000002",
            ),
            entry(
                "characters/hero.fbx",
                Some("mesh/Body"),
                "0000000000005e1e0000000000000003",
            ),
            entry(
                "characters/hero.fbx",
                Some("skeleton/Hips"),
                "0000000000005e1e0000000000000001",
            ),
            entry(
                "props/crate.obj",
                Some("mesh/Crate"),
                "00000000000000000000000000000009",
            ),
        ]
    }

    #[test]
    fn a_project_character_is_a_model_whose_import_cooked_a_skeleton() {
        let entries = hero_entries();
        let characters = project_characters(&entries);
        assert_eq!(characters.len(), 1, "the crate has a mesh and no skeleton");
        assert_eq!(characters[0].model, "characters/hero.fbx");
        assert_eq!(characters[0].mesh, "0000000000005e1e0000000000000003");
        let clips = project_clips(&entries);
        assert_eq!(clips.len(), 1);
        assert_eq!(
            clips[0].name, "hero",
            "the sub-asset's leaf, not the stack's name"
        );
        assert!(CharacterChoice::for_model(&entries, "props/crate.obj").is_err());
        assert!(CharacterChoice::default().is_mannequin());
    }

    #[test]
    fn the_character_request_is_the_one_the_engine_plays() {
        // The engine's suite plays this request on the imported hero and commits it; the editor
        // writes it from the project's import records, byte for byte.
        let choice = CharacterChoice::for_model(&hero_entries(), "characters/hero.fbx").unwrap();
        assert_eq!(
            character_payload(&choice),
            engine_fixture("animation_character_request_v1.wire")
        );
        let source = String::from_utf8(engine_fixture("animation_hero_v1.cyanimgraph")).unwrap();
        assert_eq!(rig_name("game/animation/hero.cyanimgraph"), "hero");
        assert_eq!(
            bake_payload("hero", &source, &choice),
            engine_fixture("animation_bake_request_v1.wire")
        );
    }

    #[test]
    fn the_engines_character_and_bake_replies_decode() {
        let played =
            PlayedCharacter::decode(&engine_fixture("animation_character_v1.wire")).unwrap();
        assert!(played.project);
        assert_eq!(played.model, "characters/hero.fbx");
        assert_eq!(played.joints, 3);
        assert!(played.skinned);
        assert_eq!(played.clips.len(), 1);
        assert_eq!(played.clips[0].0, "hero");
        assert!(played.refused.is_empty());
        assert!(played.describe().contains("3 joint(s)"));

        let baked = BakeReport::decode(&engine_fixture("animation_bake_v1.wire")).unwrap();
        assert!(baked.baked);
        let paths: Vec<&str> = baked.files.iter().map(|(path, _)| path.as_str()).collect();
        assert_eq!(paths, ["clips/0.cyasset", "program.cyasset", "rig.cyrig"]);
        let manifest = String::from_utf8(baked.files[2].1.clone()).unwrap();
        assert!(
            manifest.starts_with("cyrig 1\nrig \"hero\"\n"),
            "{manifest}"
        );
        assert!(
            manifest.contains("clip \"hero\" \"clips/0.cyasset\""),
            "{manifest}"
        );
        assert_eq!(&baked.files[0].1[..6], b"CYCOOK");
    }

    #[test]
    fn a_bake_that_names_a_file_outside_its_rig_is_refused() {
        let mut out = Writer::new();
        out.u32(WIRE_FORMAT);
        out.u8(1);
        out.u32(1);
        out.text("../escape.cyasset");
        out.bytes(b"x");
        out.u32(0);
        assert!(BakeReport::decode(&out.finish()).is_err());
    }

    #[test]
    fn a_graphs_character_lives_beside_it() {
        assert_eq!(
            character_reference("game/animation/hero.cyanimgraph"),
            "game/animation/hero.cyanimcharacter"
        );
        assert_eq!(
            graph_of_character("game/animation/hero.cyanimcharacter").as_deref(),
            Some("game/animation/hero.cyanimgraph")
        );
        assert!(graph_of_character("game/animation/hero.cyanimgraph").is_none());
        let text = format_character("characters/hero.fbx");
        assert_eq!(text, "cyanimcharacter 1\nmodel \"characters/hero.fbx\"\n");
        assert_eq!(parse_character(&text).unwrap(), "characters/hero.fbx");
        for bad in [
            "",
            "cyanimcharacter 2\nmodel \"a.fbx\"\n",
            "cyanimcharacter 1\n",
            "cyanimcharacter 1\nmodel a.fbx\n",
            "cyanimcharacter 1\nmodel \"\"\n",
            "cyanimcharacter 1\nmodel \"a.fbx\"\nmodel \"b.fbx\"\n",
        ] {
            assert!(parse_character(bad).is_err(), "{bad:?}");
        }
    }
}
