// SPDX-License-Identifier: MIT
//! The gameplay graph commands that need no canvas: attach a graph to an entity, compile it, raise
//! an event in Play, and read the engine's answers. Issue #29, visual scripting.
//!
//! The canvas edits (`script.graph.create`, `script.node.*`) live beside the shared canvas in
//! `cy_editor_interface::specialised::script_authoring_commands`, because they validate against the
//! engine's catalogue there. Every command here is one the Gameplay Graph panel invokes and an MCP
//! tool of the same name; attaching is one undoable scene transaction, and the rest change no
//! document — a compile and a raise ask the engine something, as `play.enter` does.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, ProjectHost,
    Registry,
};
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;

use crate::authoring::within_scope;
use crate::script_graph::{
    COMPONENT, CompileReport, DEFAULT_EVENT, FIELD, PlayState, ScriptGraph, validate_reference,
};
use crate::script_requests::ScriptRequests;

const CATEGORY: &str = "Gameplay Graph";

/// Register the gameplay graph commands that need no canvas.
///
/// # Errors
///
/// When the registry refuses a command's metadata.
pub fn register(registry: &mut Registry) -> Result<()> {
    for command in [read(), attach(), compile(), raise(), refresh(), status()] {
        registry.register(command)?;
    }
    Ok(())
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("use a gameplay graph", "no project is open"))
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn reference_parameter() -> ParameterSpec {
    ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyscript gameplay graph, for example game/scripts/unit_command.cyscript.",
    )
}

fn entity_parameter(what: &str) -> ParameterSpec {
    ParameterSpec::required(
        "entity",
        ValueKind::Text,
        format!("Hexadecimal identity of the entity {what}."),
    )
}

fn entity_argument(arguments: &Arguments) -> Result<NodeId> {
    let entity = text(arguments, "entity");
    u128::from_str_radix(entity, 16)
        .map(NodeId::from_u128)
        .map_err(|_| {
            Problem::new(
                format!("find the entity {entity:?}"),
                "it is not an entity identity",
            )
        })
}

fn read_class(id: &'static str, label: &'static str, description: &'static str) -> Metadata {
    Metadata::new(id, label, CATEGORY, description, EffectClass::Read)
}

/// The graph at `reference`, refused when there is none or it does not read.
fn source_of(host: &dyn ProjectHost, reference: &str) -> Result<(String, ScriptGraph)> {
    validate_reference(reference)?;
    if !host.source_exists(reference) {
        return Err(Problem::new(
            format!("open the gameplay graph {reference}"),
            "there is no such file",
        )
        .with_remedy("create it with script.graph.create"));
    }
    let source = host.read_source(reference)?;
    let graph = ScriptGraph::decode(&source)?;
    Ok((source, graph))
}

fn read() -> Command {
    Command::new(
        read_class(
            "script.graph.read",
            "Read Gameplay Graph",
            "Returns a gameplay graph's canonical cygraph text, and its nodes and wires one per line.",
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

fn attach() -> Command {
    Command::new(
        Metadata::new(
            "script.graph.attach",
            "Attach Gameplay Graph",
            CATEGORY,
            "Runs a gameplay graph on an entity during Play: sets the entity's ScriptGraph \
             component to the graph, as one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(reference_parameter())
        .with(entity_parameter("that runs the graph")),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let entity = entity_argument(arguments)?;
            source_of(host(context)?, &reference)?;
            let document_id = context
                .active_document()
                .ok_or_else(|| Problem::new("attach a gameplay graph", "no world is open"))?;
            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the open world"))?;
            if document.content().node(entity).is_none() {
                return Err(Problem::new(
                    "attach a gameplay graph",
                    format!("the open world has no entity {entity}"),
                ));
            }
            document.with_transaction("Attach Gameplay Graph", actor, |document| {
                set_graph(document, entity, &reference)
            })?;
            Ok(Outcome::new(format!("{entity} runs {reference} during Play"))
                .with("entity", Value::Text(entity.to_string()))
                .with("reference", Value::Text(reference)))
        },
    )
}

/// Give `node` a `ScriptGraph` naming `reference`, or point its existing one there.
fn set_graph(document: &mut Document, node: NodeId, reference: &str) -> Result<()> {
    let schema = document.schema_mut();
    // Runtime, not authoring-only: the hosted runtime reads it from the world when Play starts.
    let component = match schema.type_named(COMPONENT) {
        Some(definition) => definition.id,
        None => schema.declare_type(COMPONENT, false),
    };
    let field = match schema
        .type_of(component)
        .and_then(|definition| definition.field_named(FIELD))
    {
        Some(field) => field.id,
        None => schema.declare_field(
            component,
            FIELD,
            ValueKind::Text,
            "The project-relative .cyscript gameplay graph this entity runs during Play.",
        )?,
    };
    let value = Value::Text(reference.to_owned());
    if document.content().has_component(node, component) {
        document.set_field(node, component, field, value)
    } else {
        document.add_component(node, component, vec![(field, value)])
    }
}

/// The graph an entity runs, read back from its document.
#[must_use]
pub fn attached_graph(document: &Document, node: NodeId) -> Option<String> {
    let component = document.schema().type_named(COMPONENT)?;
    let field = component.field_named(FIELD)?.id;
    match document.content().field(node, component.id, field) {
        Some(Value::Text(reference)) => Some(reference.clone()),
        _ => None,
    }
}

fn compile() -> Command {
    Command::new(
        read_class(
            "script.graph.compile",
            "Compile Gameplay Graph",
            "Sends a saved gameplay graph to the engine's compiler. Read script.status for the \
             program or for the diagnostics, each on the node it is about.",
        )
        .with(reference_parameter()),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let host = host(context)?;
            let (source, _) = source_of(host, &reference)?;
            let request = host.script_compile(&reference, &source)?;
            Ok(Outcome::new(format!("Sent {reference} to the engine's compiler"))
                .with("request", Value::Int(i64::try_from(request).unwrap_or(i64::MAX))))
        },
    )
}

fn raise() -> Command {
    let coordinate = |name: &'static str, which: &str| {
        ParameterSpec::optional(
            name,
            ValueKind::Float,
            format!("The event's {which} argument: for unit.command, the target's {name}."),
            Value::Float(0.0),
        )
    };
    Command::new(
        read_class(
            "script.event.raise",
            "Raise Gameplay Event",
            "During Play, raises an event on an entity's gameplay graphs: unit.command with a \
             target sends a unit there. Read script.status for what the graphs did.",
        )
        .with(entity_parameter("whose graphs receive the event"))
        .with(ParameterSpec::optional(
            "event",
            ValueKind::Text,
            "The event to raise.",
            Value::Text(DEFAULT_EVENT.into()),
        ))
        .with(coordinate("x", "first"))
        .with(coordinate("y", "second"))
        .with(coordinate("z", "third")),
        |context, arguments| {
            let entity = entity_argument(arguments)?;
            let event = match text(arguments, "event") {
                "" => DEFAULT_EVENT,
                event => event,
            }
            .to_owned();
            let argument = |name: &str| {
                arguments
                    .get(name)
                    .and_then(Value::as_float)
                    .unwrap_or_default()
            };
            let values = [argument("x"), argument("y"), argument("z")];
            if values.iter().any(|value| !value.is_finite()) {
                return Err(Problem::new(
                    "raise a gameplay event",
                    "its arguments are finite numbers",
                ));
            }
            let payload = crate::script_graph::raise_payload(
                crate::mirror::engine_identity(entity),
                &event,
                &values,
            );
            let request = host(context)?.script_raise(payload)?;
            Ok(Outcome::new(format!("Raised {event} on {entity}"))
                .with("request", Value::Int(i64::try_from(request).unwrap_or(i64::MAX))))
        },
    )
}

fn refresh() -> Command {
    Command::new(
        read_class(
            "script.refresh",
            "Refresh Gameplay Graphs",
            "Asks the engine for Play's gameplay graph instances and the cues they played.",
        ),
        |context, _| {
            let request = host(context)?.script_refresh()?;
            Ok(Outcome::new("Asked the engine for Play's gameplay graphs")
                .with("request", Value::Int(i64::try_from(request).unwrap_or(i64::MAX))))
        },
    )
}

fn status() -> Command {
    Command::new(
        read_class(
            "script.status",
            "Gameplay Graph Status",
            "Reports the engine's last compile of a graph — its program, or each diagnostic with \
             its node, pin and code — and Play's graph instances and the cues they played.",
        )
        .with(ParameterSpec::optional(
            "reference",
            ValueKind::Text,
            "The graph whose compile to report; Play's state alone when omitted.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            Ok(host(context)?.script_status(reference))
        },
    )
}

/// `script.status`: what the engine last said about `reference` and about Play. Every number here
/// was decoded from an engine reply.
#[must_use]
pub fn status_outcome(requests: &ScriptRequests, reference: &str, current: Option<&str>) -> Outcome {
    let mut outcome = Outcome::new("Engine gameplay graphs")
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
        outcome = compile_outcome(outcome, report);
        outcome = outcome.with(
            "current",
            Value::Bool(current.is_some_and(|source| source == compiled_source)),
        );
    }
    if let Some(started) = requests.started() {
        outcome = outcome.with("started", Value::Int(i64::from(started)));
    }
    match requests.state() {
        Some(state) => play_outcome(outcome, state),
        None => outcome.with("playing", Value::Bool(false)),
    }
}

fn compile_outcome(mut outcome: Outcome, report: &CompileReport) -> Outcome {
    let handlers: Vec<String> = report
        .handlers
        .iter()
        .map(|handler| format!("{}@node{}", handler.event, handler.node))
        .collect();
    outcome = outcome
        .with("compiled", Value::Bool(report.compiled))
        .with("instructions", Value::Int(i64::from(report.instructions)))
        .with("state_slots", Value::Int(i64::from(report.state_slots)))
        .with("handlers", Value::Text(handlers.join(" ")))
        .with(
            "diagnostics",
            Value::Int(i64::try_from(report.diagnostics.len()).unwrap_or(i64::MAX)),
        )
        .with("listing", Value::Text(report.listing.clone()));
    for (index, diagnostic) in report.diagnostics.iter().enumerate() {
        outcome = outcome.with(
            format!("diagnostic.{index}"),
            Value::Text(format!(
                "{:?} node {}{} {}: {}",
                diagnostic.severity,
                diagnostic.node,
                if diagnostic.pin.is_empty() {
                    String::new()
                } else {
                    format!(" pin {}", diagnostic.pin)
                },
                diagnostic.code,
                diagnostic.describe()
            )),
        );
    }
    outcome
}

fn play_outcome(mut outcome: Outcome, state: &PlayState) -> Outcome {
    outcome = outcome
        .with("playing", Value::Bool(state.playing))
        .with("tick", Value::Int(i64::try_from(state.tick).unwrap_or(i64::MAX)))
        .with(
            "instances",
            Value::Int(i64::try_from(state.instances.len()).unwrap_or(i64::MAX)),
        )
        .with(
            "cues",
            Value::Int(i64::try_from(state.cues.len()).unwrap_or(i64::MAX)),
        );
    for (index, instance) in state.instances.iter().enumerate() {
        outcome = outcome.with(
            format!("instance.{index}"),
            Value::Text(format!(
                "node={:x} graph={} status={} waiting={} at={} {} {} runs={}{}",
                instance.node,
                instance.graph,
                instance.status,
                if instance.waiting.is_empty() {
                    "-"
                } else {
                    &instance.waiting
                },
                instance.position[0],
                instance.position[1],
                instance.position[2],
                instance.runs,
                if instance.problem.is_empty() {
                    String::new()
                } else {
                    format!(" problem={}", instance.problem)
                }
            )),
        );
    }
    for (index, cue) in state.cues.iter().enumerate() {
        outcome = outcome.with(
            format!("cue.{index}"),
            Value::Text(format!(
                "{} tick={} at={} {} {}",
                cue.cue, cue.tick, cue.position[0], cue.position[1], cue.position[2]
            )),
        );
    }
    outcome
}

/// Check, before saving, that `reference` may be written by this invocation.
///
/// # Errors
///
/// A reference outside the gameplay graph rules or outside the caller's scope.
pub fn writable(context: &dyn CommandContext, reference: &str) -> Result<()> {
    validate_reference(reference)?;
    within_scope(context, reference)
}
