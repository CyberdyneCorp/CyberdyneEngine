// SPDX-License-Identifier: MIT
//! The Play debugger and hot reload as commands: breakpoints, pause, continue, step into and over,
//! watches, the debugger's state, and reloading a graph into the running Play. Issues #84 and #29.
//!
//! Every command here is a button of the Gameplay Graph panel and an MCP tool of the same name, so
//! an agent debugs a graph exactly as a person does. None changes a document — a breakpoint and a
//! watch are the editor's, and the rest ask the engine something — so all are reads, as
//! `script.event.raise` is. What pauses when a graph breaks is the engine's whole simulation tick;
//! see `cy_editor_services::script_debug`.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, ProjectHost,
    Registry,
};
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

use crate::script_graph::{graph_name, validate_reference};
use crate::script_requests::ScriptRequests;

const CATEGORY: &str = "Gameplay Graph";
/// How many trace entries `script.debug.status` reports, newest last.
const STATUS_TRACE: usize = 12;

/// Register the debugger commands.
///
/// # Errors
///
/// When the registry refuses a command's metadata.
pub fn register(registry: &mut Registry) -> Result<()> {
    for command in [
        breakpoint(),
        control(
            "script.debug.pause",
            "Pause Gameplay Graphs",
            "During Play, stops at the next node any gameplay graph runs. The whole simulation \
             tick pauses with it: physics, behaviours and the clock wait until it continues.",
            "pause",
        ),
        control(
            "script.debug.continue",
            "Continue Gameplay Graphs",
            "Runs the paused tick on from the paused node until the next breakpoint, then lets the \
             simulation run again.",
            "continue",
        ),
        step(),
        watch(),
        inspect(),
        refresh(),
        status(),
        reload(),
    ] {
        registry.register(command)?;
    }
    Ok(())
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("debug a gameplay graph", "no project is open"))
}

fn read_class(id: &'static str, label: &'static str, description: &'static str) -> Metadata {
    Metadata::new(id, label, CATEGORY, description, EffectClass::Read)
}

fn reference_parameter() -> ParameterSpec {
    ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyscript gameplay graph, for example game/scripts/unit_command.cyscript.",
    )
}

fn node_parameter(what: &str) -> ParameterSpec {
    ParameterSpec::required(
        "node",
        ValueKind::Int,
        format!("The key of the node {what}."),
    )
}

fn entity_parameter(what: &str, when_empty: &str) -> ParameterSpec {
    ParameterSpec::optional(
        "entity",
        ValueKind::Text,
        format!("Hexadecimal identity of the entity {what}; empty for {when_empty}."),
        Value::Text(String::new()),
    )
}

fn enabled_parameter(what: &str) -> ParameterSpec {
    ParameterSpec::optional(
        "enabled",
        ValueKind::Bool,
        format!("True to {what}, false to remove it."),
        Value::Bool(true),
    )
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn graph_of(arguments: &Arguments) -> Result<String> {
    let reference = text(arguments, "reference");
    validate_reference(reference)?;
    Ok(graph_name(reference))
}

fn node_of(arguments: &Arguments) -> Result<u64> {
    arguments
        .get("node")
        .and_then(Value::as_int)
        .and_then(|node| u64::try_from(node).ok())
        .filter(|node| *node != 0)
        .ok_or_else(|| Problem::new("name a gameplay graph node", "a node key is one or more"))
}

/// The engine identity of the entity a command names, or zero for none.
fn entity_of(arguments: &Arguments) -> Result<u64> {
    match text(arguments, "entity") {
        "" => Ok(0),
        entity => u128::from_str_radix(entity, 16)
            .map(|id| crate::mirror::engine_identity(NodeId::from_u128(id)))
            .map_err(|_| {
                Problem::new(
                    format!("find the entity {entity:?}"),
                    "it is not an entity identity",
                )
            }),
    }
}

fn enabled(arguments: &Arguments) -> bool {
    !matches!(arguments.get("enabled"), Some(Value::Bool(false)))
}

fn request_outcome(summary: String, request: u64) -> Outcome {
    Outcome::new(summary).with(
        "request",
        Value::Int(i64::try_from(request).unwrap_or(i64::MAX)),
    )
}

fn breakpoint() -> Command {
    Command::new(
        read_class(
            "script.debug.breakpoint",
            "Gameplay Graph Breakpoint",
            "Stops Play before a gameplay graph node runs, for every entity running the graph or \
             for one. Kept by the editor, so it can be set before Play; it is sent when Play \
             starts. The whole simulation tick pauses at it.",
        )
        .with(reference_parameter())
        .with(node_parameter("to stop before"))
        .with(entity_parameter(
            "to stop for",
            "every entity running the graph",
        ))
        .with(enabled_parameter("set the breakpoint")),
        |context, arguments| {
            let graph = graph_of(arguments)?;
            let node = node_of(arguments)?;
            let entity = entity_of(arguments)?;
            let enabled = enabled(arguments);
            let request = host(context)?.script_breakpoint(&graph, node, entity, enabled)?;
            Ok(request_outcome(
                format!(
                    "{} a breakpoint on node {node} of {graph}{}",
                    if enabled { "Set" } else { "Removed" },
                    if request == 0 {
                        " (sent when Play runs it)"
                    } else {
                        ""
                    }
                ),
                request,
            ))
        },
    )
}

fn control(
    id: &'static str,
    label: &'static str,
    description: &'static str,
    action: &'static str,
) -> Command {
    Command::new(read_class(id, label, description), move |context, _| {
        let request = host(context)?.script_debug_control(action)?;
        Ok(request_outcome(
            format!("Asked Play's graphs to {action}"),
            request,
        ))
    })
}

fn step() -> Command {
    Command::new(
        read_class(
            "script.debug.step",
            "Step Gameplay Graph",
            "While a gameplay graph is paused, runs that entity's graph to its next node: `over` \
             stops at the next node on the execution chain, `into` also at each data node feeding \
             it. Across a wait the step stays armed until the entity resumes.",
        )
        .with(ParameterSpec::optional(
            "mode",
            ValueKind::Text,
            "`over` (the default) or `into`.",
            Value::Text("over".into()),
        )),
        |context, arguments| {
            let mode = match text(arguments, "mode") {
                "" => "over",
                mode => mode,
            };
            crate::script_debug::DebugAction::step(mode)?;
            let request = host(context)?.script_debug_control(mode)?;
            Ok(request_outcome(format!("Stepped {mode}"), request))
        },
    )
}

fn watch() -> Command {
    Command::new(
        read_class(
            "script.debug.watch",
            "Watch Gameplay Graph Pin",
            "Adds a node's pin to the debugger's watch list, read for the paused entity or the one \
             named. Read script.debug.status for the values, with the entity's graph variables.",
        )
        .with(reference_parameter())
        .with(node_parameter("whose pin to watch"))
        .with(ParameterSpec::optional(
            "pin",
            ValueKind::Text,
            "The pin: `value` for a node's result, `arg0` and `arg1` for a call's arguments.",
            Value::Text("value".into()),
        ))
        .with(entity_parameter("to inspect", "the paused one"))
        .with(enabled_parameter("watch the pin")),
        |context, arguments| {
            let graph = graph_of(arguments)?;
            let node = node_of(arguments)?;
            let pin = match text(arguments, "pin") {
                "" => "value",
                pin => pin,
            }
            .to_owned();
            let entity = entity_of(arguments)?;
            let enabled = enabled(arguments);
            host(context)?.script_watch(
                &graph,
                (entity != 0).then_some(entity),
                node,
                &pin,
                enabled,
            )?;
            Ok(Outcome::new(format!(
                "{} {graph} node {node} pin {pin}",
                if enabled {
                    "Watching"
                } else {
                    "No longer watching"
                }
            )))
        },
    )
}

fn inspect() -> Command {
    Command::new(
        read_class(
            "script.debug.inspect",
            "Inspect Gameplay Graph Entity",
            "Reads one entity's graph variables and watched pins in the debugger, rather than the \
             paused entity's. Empty goes back to the paused one.",
        )
        .with(reference_parameter())
        .with(entity_parameter("to inspect", "the paused one")),
        |context, arguments| {
            let graph = graph_of(arguments)?;
            let entity = entity_of(arguments)?;
            host(context)?.script_inspect(&graph, entity)?;
            Ok(Outcome::new(if entity == 0 {
                format!("Inspecting the paused entity's {graph}")
            } else {
                format!("Inspecting {graph} on entity {entity:x}")
            }))
        },
    )
}

fn refresh() -> Command {
    Command::new(
        read_class(
            "script.debug.refresh",
            "Refresh Gameplay Graph Debugger",
            "Asks the engine for the debugger's state: where Play is paused, the nodes that ran, \
             and the watched values.",
        ),
        |context, _| {
            let request = host(context)?.script_debug_refresh()?;
            Ok(request_outcome(
                "Asked the engine for the debugger's state".into(),
                request,
            ))
        },
    )
}

fn status() -> Command {
    Command::new(
        read_class(
            "script.debug.status",
            "Gameplay Graph Debugger Status",
            "Reports the debugger's last state: whether Play is paused and at which node of which \
             entity, the breakpoints, the most recent nodes run, the inspected entity's variables \
             and watched pins, and the last reload of a graph.",
        )
        .with(ParameterSpec::optional(
            "reference",
            ValueKind::Text,
            "The graph whose last reload to report.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            Ok(host(context)?.script_debug_status(reference))
        },
    )
}

fn reload() -> Command {
    Command::new(
        read_class(
            "script.graph.reload",
            "Reload Gameplay Graph",
            "During Play, recompiles a saved gameplay graph and swaps it in for every entity \
             running it at the next tick, keeping each entity's variables. A variable whose type \
             changed refuses the reload on its node and the running program is kept. Saving a \
             graph Play runs does this by itself.",
        )
        .with(reference_parameter()),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            validate_reference(&reference)?;
            let host = host(context)?;
            if !host.source_exists(&reference) {
                return Err(Problem::new(
                    format!("reload the gameplay graph {reference}"),
                    "there is no such file",
                ));
            }
            let source = host.read_source(&reference)?;
            let request = host.script_reload(&reference, &source)?;
            Ok(request_outcome(
                format!("Sent {reference} to Play to reload"),
                request,
            ))
        },
    )
}

/// `script.debug.status`: what the engine last said about the debugger, and about the last reload
/// of `reference`. Every value here was decoded from an engine reply.
#[must_use]
pub fn debug_outcome(requests: &ScriptRequests, reference: &str) -> Outcome {
    let mut outcome = Outcome::new("Gameplay graph debugger")
        .with("pending", Value::Bool(requests.pending()))
        .with("wanted_breakpoints", count(requests.breakpoints().len()));
    if let Some(problem) = requests.problem() {
        outcome = outcome.with("problem", Value::Text(problem.to_owned()));
    }
    if let Some((_, reply)) = requests.reload_reply(reference) {
        outcome = reload_outcome(outcome, reply);
    }
    match requests.debug() {
        Some(debug) => state_outcome(outcome, debug),
        None => outcome.with("playing", Value::Bool(false)),
    }
}

fn count(n: usize) -> Value {
    Value::Int(i64::try_from(n).unwrap_or(i64::MAX))
}

fn reload_outcome(mut outcome: Outcome, reply: &crate::script_debug::ReloadReply) -> Outcome {
    outcome = outcome
        .with("reload_accepted", Value::Bool(reply.accepted))
        .with("reload_generation", Value::Int(i64::from(reply.generation)));
    for (index, diagnostic) in reply.diagnostics.iter().enumerate() {
        outcome = outcome.with(
            format!("reload_diagnostic.{index}"),
            Value::Text(format!(
                "node {} {}: {}",
                diagnostic.node,
                diagnostic.code,
                diagnostic.describe()
            )),
        );
    }
    outcome
}

fn state_outcome(mut outcome: Outcome, debug: &crate::script_debug::DebugState) -> Outcome {
    outcome = outcome
        .with("playing", Value::Bool(debug.playing))
        .with("debugging", Value::Bool(debug.debugging))
        .with("paused", Value::Bool(debug.paused))
        .with(
            "tick",
            Value::Int(i64::try_from(debug.tick).unwrap_or(i64::MAX)),
        )
        .with("breakpoints", count(debug.breakpoints.len()));
    if debug.paused {
        outcome = outcome.with(
            "paused_at",
            Value::Text(format!(
                "{} node {} entity {:x} ({}, tick {})",
                debug.paused_graph,
                debug.paused_node,
                debug.paused_entity,
                debug.reason,
                debug.paused_tick
            )),
        );
    }
    let skip = debug.trace.len().saturating_sub(STATUS_TRACE);
    let trace: Vec<String> = debug
        .trace
        .iter()
        .skip(skip)
        .map(|entry| format!("{}:{}@{}", entry.graph, entry.node, entry.tick))
        .collect();
    outcome = outcome.with("trace", Value::Text(trace.join(" ")));
    if debug.inspected_entity != 0 {
        outcome = outcome.with(
            "inspected",
            Value::Text(format!(
                "{} on entity {:x}",
                debug.inspected_graph, debug.inspected_entity
            )),
        );
    }
    for variable in &debug.variables {
        outcome = outcome.with(
            format!("variable.{}", variable.name),
            Value::Text(variable.value.display()),
        );
    }
    for watch in &debug.watches {
        outcome = outcome.with(
            format!("watch.{}.{}", watch.node, watch.pin),
            Value::Text(if watch.found {
                watch.value.display()
            } else {
                "-".into()
            }),
        );
    }
    let reload = &debug.last_reload;
    if reload.graph.is_empty() {
        return outcome;
    }
    outcome.with(
        "last_reload",
        Value::Text(format!(
            "{} generation {}: {} instance(s), {} kept, {} added, {} dropped, {} wait(s) kept, \
             {} dropped",
            reload.graph,
            reload.generation,
            reload.instances,
            reload.kept,
            reload.added,
            reload.dropped,
            reload.waits_kept,
            reload.waits_dropped
        )),
    )
}
