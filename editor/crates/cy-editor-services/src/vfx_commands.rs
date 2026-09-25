// SPDX-License-Identifier: MIT
//! VFX authoring document commands shared by the desktop, scripts, and MCP.

use cy_editor_commands::{
    Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

use crate::authoring::within_scope;

/// Register VFX draft read and transactional save commands.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(read())?;
    registry.register(save())?;
    Ok(())
}

fn metadata(
    id: &'static str,
    label: &'static str,
    description: &'static str,
    effect: EffectClass,
) -> Metadata {
    Metadata::new(id, label, "VFX Graph", description, effect).with(ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyvfxdoc authoring document path.",
    ))
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn cy_editor_commands::ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("edit a VFX document", "no project is open"))
}

fn read() -> Command {
    Command::new(
        metadata(
            "vfx.document.read",
            "Read VFX Document",
            "Returns the editable VFX system source for a project asset.",
            EffectClass::Read,
        ),
        |context, arguments| {
            let reference = arguments.text("reference").unwrap_or_default();
            let source = host(context)?.vfx_document_read(reference)?;
            Ok(Outcome::new("Read VFX document").with("source", Value::Text(source)))
        },
    )
}

fn save() -> Command {
    Command::new(
        metadata(
            "vfx.document.save",
            "Save VFX Document",
            "Saves the editable system and stage canvases as one undoable project asset.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "source",
            ValueKind::Text,
            "Complete cyvfxdoc 1 authoring source.",
        )),
        |context, arguments| {
            let reference = arguments.text("reference").unwrap_or_default();
            within_scope(context, reference)?;
            let source = arguments.text("source").unwrap_or_default();
            host(context)?.vfx_document_save(reference, source)?;
            Ok(Outcome::new(format!("Saved VFX document {reference}")))
        },
    )
}
