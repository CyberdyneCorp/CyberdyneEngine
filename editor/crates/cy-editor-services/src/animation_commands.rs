// SPDX-License-Identifier: MIT
//! The animation commands that need no canvas: read a graph, compile it, drive the engine's
//! preview, and read the engine's answers. Issue #29, animation.
//!
//! The character a graph plays on (`animation.character.*`) and its bake into the rig a game loads
//! (`animation.bake`) are here too: the first is one undoable transaction on the graph's character
//! file, the second asks the engine to cook and writes what it answers.
//!
//! The canvas and timeline edits (`animation.graph.create`, `animation.node.*`,
//! `animation.event.*`) live beside the shared canvas in
//! `cy_editor_interface::specialised::animation_authoring_commands`, because they validate against
//! the engine's catalogue there. Every command here is one the Animation panel invokes and an MCP
//! tool of the same name, and none changes a document: a compile asks the engine something, and a
//! preview changes what the engine draws, as `play.enter` does.

use cy_editor_commands::{
    AnimationPreviewChange, Arguments, Command, CommandContext, EffectClass, Metadata, Outcome,
    ParameterSpec, ProjectHost, Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

use crate::animation_graph::{
    BakeReport, CompileReport, NO_STATE, PlayedCharacter, PreviewState, RIG_DIRECTORY, rig_name,
    validate_reference,
};
use crate::animation_requests::AnimationRequests;
use crate::authoring::within_scope;
use crate::script_graph::ScriptGraph;

const CATEGORY: &str = "Animation";

/// Register the animation commands that need no canvas.
///
/// # Errors
///
/// When the registry refuses a command's metadata.
pub fn register(registry: &mut Registry) -> Result<()> {
    for command in [
        read(),
        compile(),
        scrub(),
        play(),
        pause(),
        parameter(),
        stop(),
        status(),
        list_characters(),
        set_character(),
        bake(),
    ] {
        registry.register(command)?;
    }
    Ok(())
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("use an animation graph", "no project is open"))
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn reference_parameter() -> ParameterSpec {
    ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyanimgraph animation graph, for example \
         game/animation/locomotion.cyanimgraph.",
    )
}

fn focus_parameter() -> ParameterSpec {
    ParameterSpec::optional(
        "node",
        ValueKind::Int,
        "The pose.clip node whose clip to preview alone; 0 previews the state machine.",
        Value::Int(0),
    )
}

fn read_class(id: &'static str, label: &'static str, description: &'static str) -> Metadata {
    Metadata::new(id, label, CATEGORY, description, EffectClass::Read)
}

fn request_value(request: u64) -> Value {
    Value::Int(i64::try_from(request).unwrap_or(i64::MAX))
}

/// The graph at `reference`, refused when there is none or it does not read.
fn source_of(host: &dyn ProjectHost, reference: &str) -> Result<(String, ScriptGraph)> {
    validate_reference(reference)?;
    if !host.source_exists(reference) {
        return Err(Problem::new(
            format!("open the animation graph {reference}"),
            "there is no such file",
        )
        .with_remedy("create it with animation.graph.create"));
    }
    let source = host.read_source(reference)?;
    let graph = ScriptGraph::decode(&source)?;
    Ok((source, graph))
}

fn focus_argument(arguments: &Arguments) -> Result<u64> {
    let node = arguments
        .get("node")
        .and_then(Value::as_int)
        .unwrap_or_default();
    u64::try_from(node).map_err(|_| {
        Problem::new(
            "choose what to preview",
            "a node is a key of zero or more; zero is the state machine",
        )
    })
}

fn read() -> Command {
    Command::new(
        read_class(
            "animation.graph.read",
            "Read Animation Graph",
            "Returns an animation graph's canonical cygraph text, and its nodes and wires one per \
             line.",
        )
        .with(reference_parameter()),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let (source, graph) = source_of(host(context)?, reference)?;
            let mut outcome = Outcome::new(format!(
                "{} node(s), {} wire(s)",
                graph.nodes.len(),
                graph.links.len()
            ))
            .with("source", Value::Text(source));
            for node in graph.nodes.values() {
                let properties: Vec<String> = node
                    .properties
                    .iter()
                    .map(|property| {
                        format!(
                            "{}={}",
                            property.name,
                            property.literal.display(&property.literal_type)
                        )
                    })
                    .collect();
                outcome = outcome.with(
                    format!("node.{}", node.key),
                    Value::Text(format!("{} {}", node.type_name, properties.join(" "))),
                );
            }
            for (index, link) in graph.links.iter().enumerate() {
                outcome = outcome.with(
                    format!("wire.{index}"),
                    Value::Text(format!(
                        "{}.{} -> {}.{}",
                        link.from, link.from_pin, link.to, link.to_pin
                    )),
                );
            }
            Ok(outcome)
        },
    )
}

fn compile() -> Command {
    Command::new(
        read_class(
            "animation.graph.compile",
            "Compile Animation Graph",
            "Sends a saved animation graph to the engine's compiler. Read animation.status for the \
             program's states and transitions or for the diagnostics, each on its node.",
        )
        .with(reference_parameter()),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let host = host(context)?;
            let (source, _) = source_of(host, &reference)?;
            let request = host.animation_compile(&reference, &source)?;
            Ok(
                Outcome::new(format!("Sent {reference} to the engine's compiler"))
                    .with("request", request_value(request)),
            )
        },
    )
}

fn preview(context: &mut dyn CommandContext, change: &AnimationPreviewChange) -> Result<u64> {
    if let Some(reference) = &change.reference {
        source_of(host(context)?, reference)?;
    }
    host(context)?.animation_preview(change)
}

fn scrub() -> Command {
    Command::new(
        read_class(
            "animation.preview.scrub",
            "Scrub Animation Preview",
            "Shows the engine's preview character at a time, paused: one clip node alone, or the \
             state machine advanced from its entry state with the parameters set. Read \
             animation.status for the pose the engine evaluated.",
        )
        .with(reference_parameter())
        .with(ParameterSpec::required(
            "time",
            ValueKind::Float,
            "Seconds from the clip's start, or from the state machine's.",
        ))
        .with(focus_parameter()),
        |context, arguments| {
            let time = arguments
                .get("time")
                .and_then(Value::as_float)
                .unwrap_or(-1.0);
            if !(time.is_finite() && time >= 0.0) {
                return Err(Problem::new(
                    "scrub the animation preview",
                    "a time is a finite number of seconds, zero or more",
                ));
            }
            let change = AnimationPreviewChange {
                reference: Some(text(arguments, "reference").to_owned()),
                focus: Some(focus_argument(arguments)?),
                time: Some(time),
                playing: Some(false),
                parameter: None,
            };
            let request = preview(context, &change)?;
            Ok(
                Outcome::new(format!("Asked the engine for the pose at {time} s"))
                    .with("request", request_value(request)),
            )
        },
    )
}

fn play() -> Command {
    Command::new(
        read_class(
            "animation.preview.play",
            "Play Animation Preview",
            "Plays the engine's preview character on from where it is: one clip node, looping or \
             holding as a game would, or the state machine.",
        )
        .with(reference_parameter())
        .with(focus_parameter()),
        |context, arguments| {
            let change = AnimationPreviewChange {
                reference: Some(text(arguments, "reference").to_owned()),
                focus: Some(focus_argument(arguments)?),
                time: None,
                playing: Some(true),
                parameter: None,
            };
            let request = preview(context, &change)?;
            Ok(Outcome::new("Playing the preview").with("request", request_value(request)))
        },
    )
}

fn pause() -> Command {
    Command::new(
        read_class(
            "animation.preview.pause",
            "Pause Animation Preview",
            "Holds the engine's preview character at the pose it has reached.",
        ),
        |context, _| {
            let change = AnimationPreviewChange {
                playing: Some(false),
                ..AnimationPreviewChange::default()
            };
            let request = preview(context, &change)?;
            Ok(Outcome::new("Paused the preview").with("request", request_value(request)))
        },
    )
}

fn parameter() -> Command {
    Command::new(
        read_class(
            "animation.preview.parameter",
            "Set Animation Preview Parameter",
            "Sets one of the graph's parameters for the preview — a transition's condition, a \
             blend's weight — and the engine evaluates the state machine again with it.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "The parameter, as the graph names it: moving, aim_weight.",
        ))
        .with(ParameterSpec::required(
            "value",
            ValueKind::Float,
            "Its value; a condition is open while it is not zero.",
        )),
        |context, arguments| {
            let name = text(arguments, "name").to_owned();
            let value = arguments
                .get("value")
                .and_then(Value::as_float)
                .unwrap_or(f32::NAN);
            if name.is_empty() || !value.is_finite() {
                return Err(Problem::new(
                    "set an animation preview parameter",
                    "a parameter has a name and a finite value",
                ));
            }
            let change = AnimationPreviewChange {
                parameter: Some((name.clone(), value)),
                ..AnimationPreviewChange::default()
            };
            let request = preview(context, &change)?;
            Ok(
                Outcome::new(format!("Set {name} to {value} in the preview"))
                    .with("request", request_value(request)),
            )
        },
    )
}

fn stop() -> Command {
    Command::new(
        read_class(
            "animation.preview.stop",
            "Stop Animation Preview",
            "Stops the engine's preview: the character leaves the viewport.",
        ),
        |context, _| {
            let request = host(context)?.animation_preview_stop()?;
            Ok(Outcome::new("Stopped the preview").with("request", request_value(request)))
        },
    )
}

fn status() -> Command {
    Command::new(
        read_class(
            "animation.status",
            "Animation Status",
            "Reports the engine's last compile of a graph — its states, transitions, clips and \
             parameters, or each diagnostic with its node and code — and the preview: the time, the \
             state or blend, the pose's digest and joints, and the events it fired.",
        )
        .with(ParameterSpec::optional(
            "reference",
            ValueKind::Text,
            "The graph whose compile to report; the preview alone when omitted.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            Ok(host(context)?.animation_status(reference))
        },
    )
}

fn list_characters() -> Command {
    Command::new(
        read_class(
            "animation.character.list",
            "List Animation Characters",
            "Lists the characters a graph can play on: every imported model whose import cooked a \
             skeleton, with its skeleton and mesh, and every clip the project's imports cooked, by \
             the name a pose.clip node gives it. The built-in mannequin is always available.",
        ),
        |context, _| {
            let found = host(context)?.animation_characters()?;
            let mut outcome = Outcome::new(format!(
                "{} character(s), {} clip(s)",
                found.characters.len(),
                found.clips.len()
            ));
            for (index, (model, skeleton, mesh)) in found.characters.iter().enumerate() {
                outcome = outcome.with(
                    format!("character.{index}"),
                    Value::Text(format!("{model} skeleton={skeleton} mesh={mesh}")),
                );
            }
            for (index, (name, source)) in found.clips.iter().enumerate() {
                outcome = outcome.with(
                    format!("clip.{index}"),
                    Value::Text(format!("{name} from {source}")),
                );
            }
            Ok(outcome)
        },
    )
}

fn set_character() -> Command {
    Command::new(
        Metadata::new(
            "animation.character.set",
            "Set Animation Character",
            CATEGORY,
            "Chooses the character an animation graph plays on: a model the project imported \
             with a skeleton (animation.character.list), or the built-in mannequin when the model \
             is empty. Written beside the graph as one undoable transaction; the engine's preview \
             and the bake use it, and a clip node can then name that character's clips.",
            EffectClass::ReversibleMutation,
        )
        .with(reference_parameter())
        .with(ParameterSpec::optional(
            "model",
            ValueKind::Text,
            "The imported model's project-relative source, for example characters/hero.fbx; \
             empty for the mannequin.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            writable(context, &reference)?;
            let model = text(arguments, "model").to_owned();
            let host = host(context)?;
            source_of(host, &reference)?;
            host.animation_character_set(
                &reference,
                (!model.is_empty()).then_some(model.as_str()),
            )?;
            let who = if model.is_empty() {
                "the mannequin".to_owned()
            } else {
                model
            };
            Ok(Outcome::new(format!("{reference} plays on {who}")))
        },
    )
}

fn bake() -> Command {
    Command::new(
        Metadata::new(
            "animation.bake",
            "Bake Animation Graph",
            CATEGORY,
            "Asks the engine to bake a saved animation graph for its character into the rig a \
             game loads: the compiled program and every clip it names, with the events placed on \
             the timeline cooked into them, written under .cy/cooked/animation/<graph name>/. Play \
             registers the rig under the graph's name, so Animator.attach(to:rig:) plays it and \
             Animation.events(for:) delivers its events. Read animation.status for the result.",
            EffectClass::ExternalEffect,
        )
        .with(reference_parameter()),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            writable(context, &reference)?;
            let host = host(context)?;
            source_of(host, &reference)?;
            let request = host.animation_bake(&reference)?;
            Ok(Outcome::new(format!(
                "Sent {reference} to the engine's bake, rig {}",
                rig_name(&reference)
            ))
            .with("rig", Value::Text(rig_name(&reference)))
            .with(
                "directory",
                Value::Text(format!("{RIG_DIRECTORY}/{}", rig_name(&reference))),
            )
            .with("request", request_value(request)))
        },
    )
}

/// `animation.status`: what the engine last said about `reference` and about the preview. Every
/// number here was decoded from an engine reply.
#[must_use]
pub fn status_outcome(
    requests: &AnimationRequests,
    reference: &str,
    current: Option<&str>,
) -> Outcome {
    let mut outcome = Outcome::new("Engine animation")
        .with("pending", Value::Bool(requests.pending()))
        .with(
            "catalogue",
            Value::Text(if requests.catalogue().is_some() {
                "ready".into()
            } else {
                "unavailable".into()
            }),
        );
    if let Some(problem) = requests.problem() {
        outcome = outcome.with("problem", Value::Text(problem.to_owned()));
    }
    if let Some((compiled_source, report)) = requests.report(reference) {
        outcome = compile_outcome(outcome, report).with(
            "current",
            Value::Bool(current.is_some_and(|source| source == compiled_source.as_str())),
        );
    }
    if let Some(character) = requests.played_character() {
        outcome = character_outcome(outcome, character);
    }
    if let Some(report) = requests.bake_report(reference) {
        outcome = bake_outcome(outcome, report);
    }
    match requests.preview_state() {
        Some(state) => preview_outcome(outcome, state),
        None => outcome.with("previewing", Value::Bool(false)),
    }
}

fn character_outcome(mut outcome: Outcome, character: &PlayedCharacter) -> Outcome {
    outcome = outcome
        .with("character", Value::Text(character.describe()))
        .with("character_model", Value::Text(character.model.clone()))
        .with("character_skinned", Value::Bool(character.skinned));
    let clips: Vec<&str> = character
        .clips
        .iter()
        .map(|(name, _, _)| name.as_str())
        .collect();
    outcome = outcome.with("character_clips", Value::Text(clips.join(" ")));
    for (index, (clip, why)) in character.refused.iter().enumerate() {
        outcome = outcome.with(
            format!("character_refused.{index}"),
            Value::Text(format!("{clip}: {why}")),
        );
    }
    outcome
}

fn bake_outcome(mut outcome: Outcome, report: &BakeReport) -> Outcome {
    let files: Vec<&str> = report.files.iter().map(|(path, _)| path.as_str()).collect();
    outcome = outcome
        .with("baked", Value::Bool(report.baked))
        .with("baked_files", Value::Text(files.join(" ")));
    for (index, diagnostic) in report.diagnostics.iter().enumerate() {
        outcome = outcome.with(
            format!("bake_diagnostic.{index}"),
            Value::Text(format!(
                "{:?} node {} {}: {}",
                diagnostic.severity,
                diagnostic.node,
                diagnostic.code,
                diagnostic.describe()
            )),
        );
    }
    outcome
}

fn count(value: usize) -> Value {
    Value::Int(i64::try_from(value).unwrap_or(i64::MAX))
}

fn compile_outcome(mut outcome: Outcome, report: &CompileReport) -> Outcome {
    let states: Vec<&str> = report
        .states
        .iter()
        .map(|state| state.name.as_str())
        .collect();
    let parameters: Vec<&str> = report.author_parameters().collect();
    outcome = outcome
        .with("compiled", Value::Bool(report.compiled))
        .with("states", Value::Text(states.join(" ")))
        .with("parameters", Value::Text(parameters.join(" ")))
        .with("diagnostics", count(report.diagnostics.len()));
    for (index, transition) in report.transitions.iter().enumerate() {
        let name = |state: u32| {
            report
                .states
                .get(state as usize)
                .map_or("?", |found| found.name.as_str())
        };
        outcome = outcome.with(
            format!("transition.{index}"),
            Value::Text(format!(
                "node {} {} -> {} on {} over {} s",
                transition.node,
                name(transition.from),
                name(transition.to),
                transition.condition,
                transition.duration
            )),
        );
    }
    for (index, clip) in report.clips.iter().enumerate() {
        outcome = outcome.with(
            format!("clip.{index}"),
            Value::Text(format!(
                "{} {} s {} events={}{}",
                clip.name,
                clip.duration,
                if clip.looping { "loop" } else { "hold" },
                clip.events,
                if clip.known { "" } else { " unknown" }
            )),
        );
    }
    for (index, diagnostic) in report.diagnostics.iter().enumerate() {
        outcome = outcome.with(
            format!("diagnostic.{index}"),
            Value::Text(format!(
                "{:?} node {} {}: {}",
                diagnostic.severity,
                diagnostic.node,
                diagnostic.code,
                diagnostic.describe()
            )),
        );
    }
    outcome
}

fn preview_outcome(mut outcome: Outcome, state: &PreviewState) -> Outcome {
    outcome = outcome
        .with("previewing", Value::Bool(state.active))
        .with("playing", Value::Bool(state.playing))
        .with("preview", Value::Text(state.describe()))
        .with("time", Value::Float(state.time))
        .with("focus", Value::Int(i64::try_from(state.focus).unwrap_or(0)))
        .with("state", Value::Text(state.state_name.clone()))
        .with(
            "target",
            Value::Text(if state.target == NO_STATE {
                String::new()
            } else {
                state.target_name.clone()
            }),
        )
        .with("blend", Value::Float(state.blend))
        .with(
            "pose_digest",
            Value::Text(format!("{:016x}", state.pose_digest)),
        )
        .with("joints", count(state.joints.len()))
        .with("events", count(state.events.len()));
    for (index, joint) in state.joints.iter().enumerate() {
        let lanes: Vec<String> = joint.iter().map(ToString::to_string).collect();
        outcome = outcome.with(format!("joint.{index}"), Value::Text(lanes.join(" ")));
    }
    for (index, event) in state.events.iter().enumerate() {
        outcome = outcome.with(
            format!("event.{index}"),
            Value::Text(format!("{} at {} s", event.name, event.at)),
        );
    }
    outcome
}

/// Check, before saving, that `reference` may be written by this invocation.
///
/// # Errors
///
/// A reference outside the animation graph rules or outside the caller's scope.
pub fn writable(context: &dyn CommandContext, reference: &str) -> Result<()> {
    validate_reference(reference)?;
    within_scope(context, reference)
}
