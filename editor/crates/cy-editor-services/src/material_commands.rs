// SPDX-License-Identifier: MIT
//! Material graph authoring through the same registry used by panels and MCP.

use cy_editor_commands::{
    Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

use crate::authoring::within_scope;

/// Register graph reads, transient previews, validated saves, and request status.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(read())?;
    registry.register(preview())?;
    registry.register(save())?;
    registry.register(status())?;
    Ok(())
}

fn reference(arguments: &cy_editor_commands::Arguments) -> &str {
    arguments.text("reference").unwrap_or_default()
}

fn source(arguments: &cy_editor_commands::Arguments) -> &str {
    arguments.text("source").unwrap_or_default()
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn cy_editor_commands::ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("edit a material graph", "no project is open"))
}

fn graph_metadata(
    id: &'static str,
    label: &'static str,
    description: &'static str,
    effect: EffectClass,
) -> Metadata {
    Metadata::new(id, label, "Material Graph", description, effect).with(ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cygraph asset path.",
    ))
}

fn read() -> Command {
    Command::new(
        graph_metadata(
            "material.graph.read",
            "Read Material Graph",
            "Returns the editable .cymatcanvas source for a material graph.",
            EffectClass::Read,
        ),
        |context, arguments| {
            let canvas = host(context)?.material_graph_read(reference(arguments))?;
            Ok(Outcome::new("Read material graph").with("source", Value::Text(canvas)))
        },
    )
}

fn preview() -> Command {
    Command::new(
        graph_metadata(
            "material.graph.preview",
            "Preview Material Graph",
            "Applies unsaved canvas text to objects using this graph in the running scene.",
            EffectClass::Read,
        )
        .with(ParameterSpec::required(
            "source",
            ValueKind::Text,
            "Complete cymatcanvas 1 source text.",
        )),
        |context, arguments| {
            let request =
                host(context)?.material_graph_preview(reference(arguments), source(arguments))?;
            Ok(Outcome::new("Material preview requested")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn save() -> Command {
    Command::new(
        graph_metadata("material.graph.save", "Save Material Graph", "Asks the engine to author the canvas, then persists its canonical .cygraph and editable .cymatcanvas files.", EffectClass::ReversibleMutation)
            .with(ParameterSpec::required("source", ValueKind::Text, "Complete cymatcanvas 1 source text.")),
        |context, arguments| {
            let reference = reference(arguments);
            within_scope(context, reference)?;
            let request = host(context)?.material_graph_save(reference, source(arguments))?;
            Ok(Outcome::new("Material save requested; read material.graph.status for the result")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn status() -> Command {
    Command::new(
        Metadata::new(
            "material.graph.status",
            "Material Graph Status",
            "Material Graph",
            "Reports the latest engine material request and live preview state.",
            EffectClass::Read,
        ),
        |context, _| {
            let status = host(context)?.material_graph_status();
            Ok(Outcome::new(status.clone()).with("status", Value::Text(status)))
        },
    )
}
