// SPDX-License-Identifier: MIT
//! Engine-catalogue material edits shared by desktop actions and MCP.

use cy_editor_commands::{Command, EffectClass, Metadata, Outcome, ParameterSpec, Registry};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_services::authoring::within_scope;

use super::graph::{Catalogue, GraphCanvas, Layout};
use super::material::{canvas_interchange, catalogue_from_service, load_canvas_interchange};

/// Install material graph node commands into the shared action registry.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(add_node())
}

fn add_node() -> Command {
    Command::new(
        Metadata::new(
            "material.node.add",
            "Add Material Node",
            "Material Graph",
            "Adds a node from the engine material catalogue to a saved graph and asks the engine to author it.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "reference",
            ValueKind::Text,
            "Project-relative .cygraph material asset path.",
        ))
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
            let reference = arguments.text("reference").unwrap_or_default();
            within_scope(context, reference)?;
            let node_type = arguments.text("node_type").unwrap_or_default();
            let x = arguments.get("x").and_then(Value::as_float).unwrap_or_default();
            let y = arguments.get("y").and_then(Value::as_float).unwrap_or_default();
            if !x.is_finite() || !y.is_finite() {
                return Err(Problem::new("place a material node", "canvas position must be finite"));
            }
            let project = context
                .project()
                .ok_or_else(|| Problem::new("edit a material graph", "no project is open"))?;
            let payload = project.material_catalogue().ok_or_else(|| {
                Problem::new("edit a material graph", "the engine material catalogue is unavailable")
            })?;
            let mut canvas = GraphCanvas::new(1);
            canvas.load(Catalogue::new(catalogue_from_service(&payload)?)?);
            let name = load_canvas_interchange(&project.material_graph_read(reference)?, &mut canvas)?;
            let node = canvas.add(node_type, Layout { x, y })?;
            let source = canvas_interchange(&name, &canvas)?;
            let request = project.material_graph_save(reference, &source)?;
            Ok(Outcome::new(format!("Added {node_type} to {reference}"))
                .with("node", Value::Text(node.ordinal().to_string()))
                .with("request", Value::Text(request.to_string())))
        },
    )
}
