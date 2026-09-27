// SPDX-License-Identifier: MIT
//! Engine-catalogue material edits shared by desktop actions and MCP.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_services::authoring::within_scope;

use super::graph::{Catalogue, GraphCanvas, Layout, NodeKey};
use super::material::{canvas_interchange, catalogue_from_service, load_canvas_interchange};

/// Install material graph node commands into the shared action registry.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(add_node())?;
    registry.register(move_node())?;
    registry.register(connect_nodes())?;
    registry.register(disconnect_nodes())?;
    registry.register(remove_node())?;
    registry.register(set_property())?;
    Ok(())
}

fn metadata(id: &str, label: &str, description: &str) -> Metadata {
    Metadata::new(
        id,
        label,
        "Material Graph",
        description,
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cygraph material asset path.",
    ))
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn node_key(arguments: &Arguments, name: &str) -> Result<NodeKey> {
    let value = arguments
        .get(name)
        .and_then(Value::as_int)
        .unwrap_or_default();
    NodeKey::new(u64::try_from(value).unwrap_or_default())
}

fn position(arguments: &Arguments) -> Result<Layout> {
    let x = arguments
        .get("x")
        .and_then(Value::as_float)
        .unwrap_or_default();
    let y = arguments
        .get("y")
        .and_then(Value::as_float)
        .unwrap_or_default();
    if !x.is_finite() || !y.is_finite() {
        return Err(Problem::new(
            "place a material node",
            "canvas position must be finite",
        ));
    }
    Ok(Layout { x, y })
}

fn edit_canvas(
    context: &mut dyn CommandContext,
    reference: &str,
    edit: impl FnOnce(&mut GraphCanvas) -> Result<Outcome>,
) -> Result<Outcome> {
    within_scope(context, reference)?;
    let project = context
        .project()
        .ok_or_else(|| Problem::new("edit a material graph", "no project is open"))?;
    let payload = project.material_catalogue().ok_or_else(|| {
        Problem::new(
            "edit a material graph",
            "the engine material catalogue is unavailable",
        )
    })?;
    let mut canvas = GraphCanvas::new(1);
    canvas.load(Catalogue::new(catalogue_from_service(&payload)?)?);
    let name = load_canvas_interchange(&project.material_graph_read(reference)?, &mut canvas)?;
    let outcome = edit(&mut canvas)?;
    let source = canvas_interchange(&name, &canvas)?;
    let request = project.material_graph_save(reference, &source)?;
    Ok(outcome.with("request", Value::Text(request.to_string())))
}

fn add_node() -> Command {
    Command::new(
        metadata(
            "material.node.add",
            "Add Material Node",
            "Adds a node from the engine material catalogue to a saved graph and asks the engine to author it.",
        )
        .with(ParameterSpec::required(
            "node_type",
            ValueKind::Text,
            "Node type from the engine material catalogue.",
        ))
        .with(ParameterSpec::required(
            "x",
            ValueKind::Float,
            "Horizontal node position on the shared canvas.",
        ))
        .with(ParameterSpec::required(
            "y",
            ValueKind::Float,
            "Vertical node position on the shared canvas.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let node_type = text(arguments, "node_type");
            let at = position(arguments)?;
            edit_canvas(context, reference, |canvas| {
                let node = canvas.add(node_type, at)?;
                Ok(Outcome::new(format!("Added {node_type} to {reference}"))
                    .with("node", Value::Text(node.ordinal().to_string())))
            })
        },
    )
}

fn move_node() -> Command {
    Command::new(
        metadata(
            "material.node.move",
            "Move Material Node",
            "Moves a saved material node on the shared canvas in one undoable edit.",
        )
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the material node to move.",
        ))
        .with(ParameterSpec::required(
            "x",
            ValueKind::Float,
            "Horizontal canvas position.",
        ))
        .with(ParameterSpec::required(
            "y",
            ValueKind::Float,
            "Vertical canvas position.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let node = node_key(arguments, "node")?;
            let at = position(arguments)?;
            edit_canvas(context, reference, |canvas| {
                canvas.move_to(node, at)?;
                Ok(Outcome::new(format!(
                    "Moved material node {}",
                    node.ordinal()
                )))
            })
        },
    )
}

fn link_metadata(id: &str, label: &str, description: &str) -> Metadata {
    metadata(id, label, description)
        .with(ParameterSpec::required(
            "from",
            ValueKind::Int,
            "Stable key of the node providing the output value.",
        ))
        .with(ParameterSpec::required(
            "from_pin",
            ValueKind::Text,
            "Name of the source node's engine-declared output pin.",
        ))
        .with(ParameterSpec::required(
            "to",
            ValueKind::Int,
            "Stable key of the node receiving the input value.",
        ))
        .with(ParameterSpec::required(
            "to_pin",
            ValueKind::Text,
            "Name of the destination node's engine-declared input pin.",
        ))
}

fn connect_nodes() -> Command {
    Command::new(
        link_metadata(
            "material.node.connect",
            "Connect Material Nodes",
            "Connects two engine-typed pins in a saved material graph.",
        ),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let from = node_key(arguments, "from")?;
            let to = node_key(arguments, "to")?;
            let from_pin = text(arguments, "from_pin");
            let to_pin = text(arguments, "to_pin");
            edit_canvas(context, reference, |canvas| {
                canvas.connect(from, from_pin, to, to_pin)?;
                Ok(Outcome::new(format!(
                    "Connected material nodes {} and {}",
                    from.ordinal(),
                    to.ordinal()
                )))
            })
        },
    )
}

fn disconnect_nodes() -> Command {
    Command::new(
        link_metadata(
            "material.node.disconnect",
            "Disconnect Material Nodes",
            "Removes one exact wire from a saved material graph.",
        ),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let from = node_key(arguments, "from")?;
            let to = node_key(arguments, "to")?;
            let from_pin = text(arguments, "from_pin");
            let to_pin = text(arguments, "to_pin");
            edit_canvas(context, reference, |canvas| {
                canvas.disconnect(from, from_pin, to, to_pin)?;
                Ok(Outcome::new(format!(
                    "Disconnected material nodes {} and {}",
                    from.ordinal(),
                    to.ordinal()
                )))
            })
        },
    )
}

fn remove_node() -> Command {
    Command::new(
        metadata(
            "material.node.remove",
            "Remove Material Node",
            "Removes a saved material node and its attached wires in one undoable edit.",
        )
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the material node to remove.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let node = node_key(arguments, "node")?;
            edit_canvas(context, reference, |canvas| {
                canvas.remove(node)?;
                Ok(Outcome::new(format!(
                    "Removed material node {}",
                    node.ordinal()
                )))
            })
        },
    )
}

fn set_property() -> Command {
    Command::new(
        metadata(
            "material.node.property.set",
            "Set Material Node Property",
            "Sets an engine-declared typed property on a saved material node.",
        )
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the material node whose property changes.",
        ))
        .with(ParameterSpec::required(
            "property",
            ValueKind::Text,
            "Name of the typed property declared by the engine node catalogue.",
        ))
        .with(ParameterSpec::required(
            "value",
            ValueKind::Text,
            "Literal value to assign to the engine-declared typed property.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let node = node_key(arguments, "node")?;
            let name = text(arguments, "property");
            let value = text(arguments, "value");
            edit_canvas(context, reference, |canvas| {
                let descriptor = canvas
                    .node(node)
                    .and_then(|entry| canvas.catalogue().get(&entry.type_name))
                    .and_then(|kind| kind.properties.iter().find(|entry| entry.name == name))
                    .ok_or_else(|| {
                        Problem::new(
                            "set a material node property",
                            format!("node {} has no engine property {name}", node.ordinal()),
                        )
                    })?;
                canvas.set_property_by_identity(node, descriptor.identity, value)?;
                Ok(Outcome::new(format!(
                    "Set material node {} {name}",
                    node.ordinal()
                )))
            })
        },
    )
}
