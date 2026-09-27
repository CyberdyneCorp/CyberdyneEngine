// SPDX-License-Identifier: MIT
//! VFX authoring document commands shared by the desktop, scripts, and MCP.

use cy_editor_commands::{
    Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

use crate::authoring::within_scope;

/// Register VFX draft and engine preview commands.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(read())?;
    registry.register(save())?;
    registry.register(module_read())?;
    registry.register(module_save())?;
    registry.register(preview_load())?;
    registry.register(preview_control())?;
    registry.register(preview_step())?;
    registry.register(preview_parameter())?;
    registry.register(preview_status())?;
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

fn module_metadata(
    id: &'static str,
    label: &'static str,
    description: &'static str,
    effect: EffectClass,
) -> Metadata {
    Metadata::new(id, label, "VFX Graph", description, effect).with(ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyvfxmodule asset path.",
    ))
}

fn module_read() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.read",
            "Read VFX Module",
            "Returns the editable source of a reusable VFX stage module.",
            EffectClass::Read,
        ),
        |context, arguments| {
            let reference = arguments.text("reference").unwrap_or_default();
            let source = host(context)?.vfx_module_read(reference)?;
            Ok(Outcome::new("Read VFX module").with("source", Value::Text(source)))
        },
    )
}

fn module_save() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.save",
            "Save VFX Module",
            "Saves a reusable stage graph and typed interface as one undoable project asset.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "source",
            ValueKind::Text,
            "Complete cyvfxmodule 1 authoring source.",
        )),
        |context, arguments| {
            let reference = arguments.text("reference").unwrap_or_default();
            within_scope(context, reference)?;
            let source = arguments.text("source").unwrap_or_default();
            host(context)?.vfx_module_save(reference, source)?;
            Ok(Outcome::new(format!("Saved VFX module {reference}")))
        },
    )
}

fn preview_metadata(id: &str, label: &str, description: &str) -> Metadata {
    Metadata::new(id, label, "VFX Graph", description, EffectClass::Read)
}

fn preview_load() -> Command {
    Command::new(
        preview_metadata(
            "vfx.preview.load",
            "Load VFX Preview",
            "Cooks an editable VFX draft in the engine's isolated preview world.",
        )
        .with(ParameterSpec::required(
            "source",
            ValueKind::Text,
            "Complete cyvfxdoc 1 source text to preview.",
        )),
        |context, arguments| {
            let source = arguments.text("source").unwrap_or_default();
            let request = host(context)?.vfx_preview_load(source)?;
            Ok(Outcome::new("VFX preview load requested")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn preview_control() -> Command {
    Command::new(
        preview_metadata(
            "vfx.preview.control",
            "Control VFX Preview",
            "Plays, pauses, restarts, scrubs, or changes time scale in the engine preview.",
        )
        .with(ParameterSpec::required(
            "action",
            ValueKind::Text,
            "One of play, pause, restart, scrub, or time-scale.",
        ))
        .with(ParameterSpec::optional(
            "value",
            ValueKind::Float,
            "Seconds for scrub or multiplier for time-scale; ignored for other actions.",
            Value::Float(0.0),
        )),
        |context, arguments| {
            let action = arguments.text("action").unwrap_or_default();
            let value = arguments
                .get("value")
                .and_then(Value::as_float)
                .unwrap_or(0.0);
            let request = host(context)?.vfx_preview_control(action, value)?;
            Ok(Outcome::new(format!("VFX preview {action} requested"))
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn preview_step() -> Command {
    Command::new(
        preview_metadata(
            "vfx.preview.step",
            "Step VFX Preview",
            "Advances the playing engine preview by a bounded frame interval.",
        )
        .with(ParameterSpec::required(
            "seconds",
            ValueKind::Float,
            "Frame interval in seconds, from 0 to 0.25.",
        )),
        |context, arguments| {
            let seconds = arguments
                .get("seconds")
                .and_then(Value::as_float)
                .unwrap_or(0.0);
            let request = host(context)?.vfx_preview_step(seconds)?;
            Ok(Outcome::new("VFX preview step requested")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn preview_parameter() -> Command {
    Command::new(
        preview_metadata(
            "vfx.preview.parameter.set",
            "Set VFX Preview Parameter",
            "Updates one exposed engine preview parameter without recompiling.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Authored parameter name.",
        ))
        .with(ParameterSpec::required(
            "values",
            ValueKind::Vec4,
            "Up to four float components, with unused lanes set to zero.",
        ))
        .with(ParameterSpec::required(
            "lanes",
            ValueKind::Int,
            "Number of components to apply, from 1 to 4.",
        )),
        |context, arguments| {
            let name = arguments.text("name").unwrap_or_default();
            let Some(Value::Vec4(values)) = arguments.get("values") else {
                return Err(Problem::new(
                    "set VFX preview parameter",
                    "four values are required",
                ));
            };
            let lanes = arguments
                .get("lanes")
                .and_then(Value::as_int)
                .unwrap_or_default();
            if !(1..=4).contains(&lanes) {
                return Err(Problem::new(
                    "set VFX preview parameter",
                    "lanes must be between 1 and 4",
                ));
            }
            let request = host(context)?.vfx_preview_parameter(
                name,
                &values[..usize::try_from(lanes).expect("validated lane count")],
            )?;
            Ok(Outcome::new("VFX preview parameter update requested")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn preview_status() -> Command {
    Command::new(
        preview_metadata(
            "vfx.preview.status",
            "VFX Preview Status",
            "Reports the latest engine preview snapshot, pending request, and error state.",
        ),
        |context, _| Ok(host(context)?.vfx_preview_status()),
    )
}
