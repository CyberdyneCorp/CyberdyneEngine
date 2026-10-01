// SPDX-License-Identifier: MIT
//! Gameplay graph edits through the shared command registry: the Gameplay Graph panel, the
//! palette, scripts and MCP all reach these. Issue #29, visual scripting.
//!
//! Each edit reads the `.cyscript`, opens it on a canvas holding the ENGINE's catalogue, makes one
//! change through the canvas's checked operations, captures it and saves it as one undoable
//! transaction in the open world's history. So a gesture in the panel and a tool call from an agent
//! are refused for the same reasons — a pin of another type, a property value outside the engine's
//! declared names, a node type the engine does not have — and undo the same way.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, ProjectHost,
    Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_services::script_commands::writable;
use cy_editor_services::script_graph::{
    DEFAULT_EVENT, Literal, Property, ScriptGraph, ScriptNode, graph_name,
};

use super::graph::{GraphCanvas, Layout, NodeKey};
use super::script::{canvas_for, capture, catalogue};

const CATEGORY: &str = "Gameplay Graph";

/// Install the gameplay graph edits.
///
/// # Errors
///
/// When the registry refuses a command's metadata.
pub fn register(registry: &mut Registry) -> Result<()> {
    for command in [
        create_graph(),
        add_node(),
        move_node(),
        connect_nodes(),
        disconnect_nodes(),
        remove_node(),
        set_property(),
    ] {
        registry.register(command)?;
    }
    Ok(())
}

fn metadata(id: &str, label: &str, description: &str) -> Metadata {
    Metadata::new(
        id,
        label,
        CATEGORY,
        description,
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyscript gameplay graph, for example game/scripts/unit_command.cyscript.",
    ))
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn node_parameter(name: &'static str, what: &str) -> ParameterSpec {
    ParameterSpec::required(name, ValueKind::Int, format!("Stable key of {what}."))
}

fn node_key(arguments: &Arguments, name: &str) -> Result<NodeKey> {
    let value = arguments
        .get(name)
        .and_then(Value::as_int)
        .unwrap_or_default();
    NodeKey::new(u64::try_from(value).unwrap_or_default())
}

fn position(arguments: &Arguments) -> Result<Layout> {
    let coordinate = |name: &str| {
        arguments
            .get(name)
            .and_then(Value::as_float)
            .unwrap_or_default()
    };
    let (x, y) = (coordinate("x"), coordinate("y"));
    if !x.is_finite() || !y.is_finite() {
        return Err(Problem::new(
            "place a gameplay graph node",
            "a canvas position is finite",
        ));
    }
    Ok(Layout { x, y })
}

fn position_parameters(metadata: Metadata) -> Metadata {
    metadata
        .with(ParameterSpec::required(
            "x",
            ValueKind::Float,
            "Horizontal position on the shared canvas.",
        ))
        .with(ParameterSpec::required(
            "y",
            ValueKind::Float,
            "Vertical position on the shared canvas.",
        ))
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("edit a gameplay graph", "no project is open"))
}

/// Read, open on the engine's catalogue, change, capture, save: one undoable transaction.
fn edit_graph(
    context: &mut dyn CommandContext,
    reference: &str,
    edit: impl FnOnce(&mut GraphCanvas) -> Result<Outcome>,
) -> Result<Outcome> {
    writable(context, reference)?;
    let project = host(context)?;
    let payload = project.script_catalogue().ok_or_else(|| {
        Problem::new(
            "edit a gameplay graph",
            "the engine's gameplay graph catalogue has not arrived; it has been requested",
        )
        .with_remedy("attach a runtime, then try again once it has answered")
    })?;
    if !project.source_exists(reference) {
        return Err(Problem::new(
            format!("edit the gameplay graph {reference}"),
            "there is no such file",
        )
        .with_remedy("create it with script.graph.create"));
    }
    let graph = ScriptGraph::decode(&project.read_source(reference)?)?;
    let mut canvas = canvas_for(catalogue(&payload)?, &graph)?;
    let outcome = edit(&mut canvas)?;
    let source = capture(&graph, &canvas)?.encode();
    project.script_graph_save(reference, &source)?;
    Ok(outcome.with("source", Value::Text(source)))
}

fn create_graph() -> Command {
    Command::new(
        metadata(
            "script.graph.create",
            "Create Gameplay Graph",
            "Creates a gameplay graph that answers one event, as one undoable transaction. Graphs \
             start at events, never at a per-frame tick.",
        )
        .with(ParameterSpec::optional(
            "event",
            ValueKind::Text,
            "The event its first node answers.",
            Value::Text(DEFAULT_EVENT.into()),
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let event = match text(arguments, "event") {
                "" => DEFAULT_EVENT,
                event => event,
            }
            .to_owned();
            writable(context, &reference)?;
            let project = host(context)?;
            if project.source_exists(&reference) {
                return Err(Problem::new(
                    "create a gameplay graph",
                    format!("{reference} already exists"),
                ));
            }
            let mut graph = ScriptGraph::new(graph_name(&reference));
            graph.nodes.insert(
                1,
                ScriptNode {
                    key: 1,
                    type_name: "script.on_event".into(),
                    version: 1,
                    muted: false,
                    properties: vec![Property {
                        name: "event".into(),
                        literal_type: "name".into(),
                        literal: Literal::Text(event.clone()),
                    }],
                    opaque: None,
                },
            );
            graph.layout.insert(1, (16.0, 16.0, String::new()));
            let source = graph.encode();
            project.script_graph_save(&reference, &source)?;
            Ok(
                Outcome::new(format!("Created {reference}, answering {event}"))
                    .with("node", Value::Int(1))
                    .with("source", Value::Text(source)),
            )
        },
    )
}

fn add_node() -> Command {
    Command::new(
        position_parameters(
            metadata(
                "script.node.add",
                "Add Gameplay Graph Node",
                "Places a node from the engine's gameplay graph catalogue, with every property at \
                 the engine's default, as one undoable transaction.",
            )
            .with(ParameterSpec::required(
                "node_type",
                ValueKind::Text,
                "Node type from the engine's catalogue, such as script.call.",
            )),
        ),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node_type = text(arguments, "node_type").to_owned();
            let at = position(arguments)?;
            edit_graph(context, &reference, |canvas| {
                let node = canvas.add(&node_type, at)?;
                let defaults: Vec<(u32, String)> = canvas
                    .catalogue()
                    .get(&node_type)
                    .map(|declared| {
                        declared
                            .properties
                            .iter()
                            .map(|property| (property.identity, property.default.clone()))
                            .collect()
                    })
                    .unwrap_or_default();
                for (identity, default) in defaults {
                    canvas.set_property_by_identity(node, identity, default)?;
                }
                Ok(Outcome::new(format!("Added {node_type}")).with(
                    "node",
                    Value::Int(i64::try_from(node.ordinal()).unwrap_or(0)),
                ))
            })
        },
    )
}

fn move_node() -> Command {
    Command::new(
        position_parameters(
            metadata(
                "script.node.move",
                "Move Gameplay Graph Node",
                "Moves a node on the canvas. Layout is not meaning: the compiled program does not \
                 change.",
            )
            .with(node_parameter("node", "the node to move")),
        ),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            let at = position(arguments)?;
            edit_graph(context, &reference, |canvas| {
                canvas.move_to(node, at)?;
                Ok(Outcome::new(format!("Moved node {}", node.ordinal())))
            })
        },
    )
}

fn wire_parameters(metadata: Metadata) -> Metadata {
    metadata
        .with(node_parameter("from", "the node the wire leaves"))
        .with(ParameterSpec::required(
            "from_pin",
            ValueKind::Text,
            "The output pin the wire leaves by, as the engine names it.",
        ))
        .with(node_parameter("to", "the node the wire enters"))
        .with(ParameterSpec::required(
            "to_pin",
            ValueKind::Text,
            "The input pin the wire enters by, as the engine names it.",
        ))
}

fn connect_nodes() -> Command {
    Command::new(
        wire_parameters(metadata(
            "script.node.connect",
            "Connect Gameplay Graph Nodes",
            "Wires an output pin to an input pin of the same engine type, as one undoable \
             transaction. An execution pin carries control; any other carries a value.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let (from, to) = (node_key(arguments, "from")?, node_key(arguments, "to")?);
            let (from_pin, to_pin) = (
                text(arguments, "from_pin").to_owned(),
                text(arguments, "to_pin").to_owned(),
            );
            edit_graph(context, &reference, |canvas| {
                canvas.connect(from, &from_pin, to, &to_pin)?;
                Ok(Outcome::new(format!(
                    "Wired node {} {from_pin} to node {} {to_pin}",
                    from.ordinal(),
                    to.ordinal()
                )))
            })
        },
    )
}

fn disconnect_nodes() -> Command {
    Command::new(
        wire_parameters(metadata(
            "script.node.disconnect",
            "Disconnect Gameplay Graph Nodes",
            "Removes one wire, as one undoable transaction.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let (from, to) = (node_key(arguments, "from")?, node_key(arguments, "to")?);
            let (from_pin, to_pin) = (
                text(arguments, "from_pin").to_owned(),
                text(arguments, "to_pin").to_owned(),
            );
            edit_graph(context, &reference, |canvas| {
                canvas.disconnect(from, &from_pin, to, &to_pin)?;
                Ok(Outcome::new("Removed the wire"))
            })
        },
    )
}

fn remove_node() -> Command {
    Command::new(
        metadata(
            "script.node.remove",
            "Remove Gameplay Graph Node",
            "Removes a node and every wire that touched it, as one undoable transaction.",
        )
        .with(node_parameter("node", "the node to remove")),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            edit_graph(context, &reference, |canvas| {
                canvas.remove(node)?;
                Ok(Outcome::new(format!("Removed node {}", node.ordinal())))
            })
        },
    )
}

fn set_property() -> Command {
    Command::new(
        metadata(
            "script.node.property.set",
            "Set Gameplay Graph Property",
            "Sets one of a node's engine-declared properties — an event, a function, a constant — \
             as one undoable transaction. A value outside the engine's declared names is refused.",
        )
        .with(node_parameter("node", "the node whose property changes"))
        .with(ParameterSpec::required(
            "property",
            ValueKind::Text,
            "The property's name, as the engine declares it.",
        ))
        .with(ParameterSpec::required(
            "value",
            ValueKind::Text,
            "The value to write, as the property's engine-declared kind reads it.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            let property = text(arguments, "property").to_owned();
            let value = text(arguments, "value").to_owned();
            edit_graph(context, &reference, |canvas| {
                let identity = canvas
                    .node(node)
                    .and_then(|authored| canvas.catalogue().get(&authored.type_name))
                    .and_then(|declared| {
                        declared
                            .properties
                            .iter()
                            .find(|candidate| candidate.name == property)
                    })
                    .map(|declared| declared.identity)
                    .ok_or_else(|| {
                        Problem::new(
                            "set a gameplay graph property",
                            format!(
                                "node {} has no engine-declared property {property}",
                                node.ordinal()
                            ),
                        )
                    })?;
                canvas.set_property_by_identity(node, identity, value.clone())?;
                Ok(Outcome::new(format!(
                    "Set node {} {property} to {value}",
                    node.ordinal()
                )))
            })
        },
    )
}
