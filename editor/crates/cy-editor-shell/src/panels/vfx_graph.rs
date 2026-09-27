// SPDX-License-Identifier: MIT
//! VFX node editing uses the same graph canvas and backend catalogue as material authoring.

use cy_editor_commands::Arguments;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::SpecialisedEditors;
use cy_editor_interface::specialised::graph::{GraphCanvas, Layout, NodeKey};
use cy_editor_interface::specialised::vfx::{
    Attribute, Emitter, EventChannel, Parameter, SimulationPath, Stage, VfxDocument,
};
use cy_editor_interface::specialised::vfx_module::{ModuleInput, VfxModule};
use cy_editor_services::backend::VfxCompileState;
use cy_editor_services::vfx_capabilities::VfxAuthoringCapabilities;
use cy_editor_services::vfx_compile::VfxCompileDiagnostic;
use cy_editor_services::vfx_preview::VfxPreviewAction;
use cy_editor_services::{AssetCatalogueService, MaterialCatalogueState};

use super::{Inputs, Intent, Panels, material_graph, nothing_here, secondary};

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let state = panels.editor.backend.vfx_catalogue_state();
    if !panels.specialised.can_open(Domain::VfxGraph) {
        if !panels.editor.runtime.is_connected() {
            panels.inputs.vfx_compile_signature = None;
        }
        let (message, remedy) = match state {
            MaterialCatalogueState::Loading => (
                "Loading the engine VFX catalogue…",
                "The palette will populate when the backend request completes.",
            ),
            MaterialCatalogueState::Failed => (
                "The engine VFX catalogue is unavailable.",
                "Open Problems for the backend diagnostic, then reconnect a compatible runtime.",
            ),
            _ => (
                "No engine VFX catalogue is loaded.",
                "Start a hosted runtime with CyberVFX enabled.",
            ),
        };
        nothing_here(ui, panels.shell, message, remedy);
        return;
    }

    ui.heading("VFX Graph");
    module_controls(panels, ui);
    document_controls(panels, ui);
    preview_controls(panels, ui);
    compile_report(panels, ui);
    if panels.specialised.active_vfx_stage().is_none()
        && panels.specialised.active_vfx_module().is_none()
    {
        auto_compile(panels);
        ui.heading("Engine catalogue");
        nothing_here(
            ui,
            panels.shell,
            "No VFX stage is selected.",
            "Create a system and add an emitter to edit its stage graphs.",
        );
        return;
    }
    let active_stage = panels.specialised.active_vfx_stage();
    let failed_diagnostics = match panels.editor.backend.vfx_compile_state() {
        VfxCompileState::Failed(_, failure) => failure.diagnostics.clone(),
        _ => Vec::new(),
    };
    let context = panels.specialised.active_vfx_module().map_or_else(
        || "Editable emitter stage draft".to_owned(),
        |module| format!("Reusable module {} · {}", module.name, module.stage.label()),
    );
    let saved_canvas = saved_canvas(panels);
    let session = match panels.specialised.open(Domain::VfxGraph) {
        Ok(session) => session,
        Err(problem) => {
            nothing_here(
                ui,
                panels.shell,
                "The VFX graph could not be opened.",
                &problem.to_string(),
            );
            return;
        }
    };
    let canvas = session.graph.expect("VFX uses the shared graph canvas");
    let node_alerts = active_stage
        .map(|stage| active_node_alerts(stage, &failed_diagnostics))
        .unwrap_or_default();
    ui.label(secondary(
        panels.shell,
        format!("{context} · engine simulation preview · viewport particles"),
    ));
    canvas_area(
        ui,
        panels.shell,
        canvas,
        state,
        &panels.editor.asset_catalogue,
        panels.inputs,
        &mut CanvasActions {
            saved: saved_canvas.as_ref(),
            intents: panels.intents,
            node_alerts: &node_alerts,
        },
    );
    auto_compile(panels);
}

struct CanvasActions<'a> {
    saved: Option<&'a SavedCanvas>,
    intents: &'a mut Vec<Intent>,
    node_alerts: &'a [(u64, String)],
}

fn canvas_area(
    ui: &mut egui::Ui,
    shell: &Shell,
    canvas: &mut GraphCanvas,
    state: MaterialCatalogueState,
    assets: &AssetCatalogueService,
    inputs: &mut Inputs,
    actions: &mut CanvasActions<'_>,
) {
    let available = ui.available_size();
    ui.horizontal(|ui| {
        ui.allocate_ui_with_layout(
            egui::vec2(220.0_f32.min(available.x * 0.38), available.y),
            egui::Layout::top_down(egui::Align::Min),
            |ui| {
                palette(
                    ui,
                    shell,
                    canvas,
                    &mut inputs.vfx_filter,
                    actions.saved,
                    actions.intents,
                );
                material_graph::graph_properties(
                    ui,
                    canvas,
                    assets,
                    &mut inputs.vfx_property_problem,
                );
            },
        );
        ui.separator();
        ui.allocate_ui(egui::vec2(ui.available_width(), available.y), |ui| {
            material_graph::draw_canvas(
                ui,
                shell,
                canvas,
                state,
                "Empty VFX stage graph\nChoose a node from the engine catalogue",
                &mut inputs.vfx_link_source,
                &mut material_graph::CanvasFeedback {
                    link_problem: &mut inputs.vfx_link_problem,
                    node_alerts: actions.node_alerts,
                },
            );
        });
    });
}

fn active_node_alerts(
    active_stage: (usize, Stage),
    diagnostics: &[VfxCompileDiagnostic],
) -> Vec<(u64, String)> {
    diagnostics
        .iter()
        .filter(|diagnostic| {
            diagnostic.emitter == u32::try_from(active_stage.0).ok()
                && diagnostic.stage == Some(active_stage.1 as u8)
        })
        .map(|diagnostic| {
            let pin = if diagnostic.pin.is_empty() {
                String::new()
            } else {
                format!(" · {}", diagnostic.pin)
            };
            (
                diagnostic.node,
                format!("{}{}: {}", diagnostic.code, pin, diagnostic.message),
            )
        })
        .collect()
}

fn submit_compile(
    document: &VfxDocument,
    signature: Vec<u8>,
    last: &mut Option<Vec<u8>>,
    force: bool,
    mut submit: impl FnMut(String) -> cy_editor_core::problem::Result<()>,
) -> cy_editor_core::problem::Result<bool> {
    if !force && last.as_ref() == Some(&signature) {
        return Ok(false);
    }
    submit(document.encode_text()?)?;
    *last = Some(signature);
    Ok(true)
}

fn auto_compile(panels: &mut Panels<'_>) {
    if !panels.editor.runtime.is_connected() {
        panels.inputs.vfx_compile_signature = None;
        return;
    }
    if matches!(
        panels.editor.backend.vfx_compile_state(),
        VfxCompileState::Pending(_)
    ) {
        return;
    }
    let document = match panels.specialised.vfx_document_snapshot() {
        Ok(Some(document)) => document,
        Ok(None) => return,
        Err(problem) => {
            panels.inputs.vfx_document_problem = Some(problem.to_string());
            return;
        }
    };
    if document.emitters.is_empty() {
        return;
    }
    let signature =
        document.compile_signature_with_modules(|path| panels.editor.project.read_source(path));
    let result = signature.and_then(|signature| {
        submit_compile(
            &document,
            signature,
            &mut panels.inputs.vfx_compile_signature,
            false,
            |source| panels.editor.request_vfx_compile(source).map(|_| ()),
        )
    });
    if let Err(problem) = result {
        panels.inputs.vfx_document_problem = Some(problem.to_string());
    }
}

fn preview_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    if panels.specialised.vfx_document().is_none() {
        return;
    }
    ui.collapsing("Engine VFX preview", |ui| {
        let connected = panels.editor.runtime.is_connected();
        let pending = panels.editor.backend.vfx_preview_pending();
        let snapshot = panels.editor.backend.vfx_preview_snapshot().cloned();
        ui.horizontal(|ui| {
            if ui
                .add_enabled(connected && !pending, egui::Button::new("Load preview"))
                .clicked()
            {
                let result = panels
                    .specialised
                    .vfx_document_snapshot()
                    .and_then(|document| {
                        document
                            .ok_or_else(|| cy_editor_core::problem::Problem::new(
                                "load VFX preview", "no VFX document is open"
                            ))?
                            .encode_text()
                    })
                    .and_then(|source| panels.editor.request_vfx_preview_load(source).map(|_| ()));
                panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
            }
            if let Some(state) = &snapshot {
                let action = if state.playing { VfxPreviewAction::Pause } else { VfxPreviewAction::Play };
                let label = if state.playing { "Pause" } else { "Play" };
                if ui.add_enabled(connected && !pending, egui::Button::new(label)).clicked() {
                    let result = panels.editor.request_vfx_preview_action(action);
                    panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
                }
                if ui.add_enabled(connected && !pending, egui::Button::new("Restart")).clicked() {
                    let result = panels.editor.request_vfx_preview_action(VfxPreviewAction::Restart);
                    panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
                }
            }
        });
        if let Some(state) = &snapshot {
            preview_timeline(panels, ui, connected && !pending);
            ui.label(format!(
                "{:.2} s · {} live · {} spawned · {} killed · {} CPU fallback · pool {}/{} bytes · shortfall {} / reduced {} · events raised {} / delivered {} / dropped {} / truncated {} / deferred {}",
                state.time_seconds, state.live_particles, state.spawned, state.killed,
                state.cpu_fallbacks, state.pool_used_bytes, state.pool_total_bytes,
                state.pool_shortfall_particles, state.pool_reduced_requests,
                state.events_raised, state.events_delivered, state.events_dropped,
                state.events_truncated, state.readback_deferred
            ));
            for emitter in &state.emitters {
                ui.label(format!("{}: {} particles", emitter.name, emitter.live));
            }
            if let Some(sample) = &state.sample {
                ui.collapsing(format!("Particle sample · {} #{}", state.emitters[sample.emitter as usize].name, sample.slot), |ui| {
                    for attribute in &sample.attributes {
                        ui.label(format!("{}: {:?}", attribute.name, attribute.values));
                    }
                });
            }
            ui.label(secondary(panels.shell, "Preview particles are drawn by the engine in the viewport."));
        }
        if pending {
            ui.label("Engine preview request pending…");
        }
        if let Some(problem) = panels.editor.backend.vfx_preview_problem() {
            ui.colored_label(egui::Color32::RED, problem);
        }
    });
    if !panels.editor.backend.vfx_preview_pending()
        && panels.editor.backend.vfx_preview_snapshot().is_some()
        && let Some((name, values, lanes)) = panels.inputs.vfx_live_parameters.pop_front()
    {
        let result = panels
            .editor
            .request_vfx_preview_parameter(&name, &values[..lanes]);
        panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
    }
    if panels
        .editor
        .backend
        .vfx_preview_snapshot()
        .is_some_and(|state| state.playing)
        && !panels.editor.backend.vfx_preview_pending()
    {
        let seconds = ui
            .ctx()
            .input(|input| input.stable_dt)
            .clamp(1.0 / 240.0, 0.25);
        let result = panels.editor.request_vfx_preview_step(seconds);
        panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        ui.ctx().request_repaint();
    }
}

fn preview_timeline(panels: &mut Panels<'_>, ui: &mut egui::Ui, enabled: bool) {
    ui.horizontal(|ui| {
        ui.label("Scrub (seconds)");
        ui.add(
            egui::DragValue::new(&mut panels.inputs.vfx_preview_scrub_seconds).range(0.0..=30.0),
        );
        if ui
            .add_enabled(enabled, egui::Button::new("Scrub"))
            .clicked()
        {
            let result = panels
                .editor
                .request_vfx_preview_action(VfxPreviewAction::Scrub(
                    panels.inputs.vfx_preview_scrub_seconds,
                ));
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
        ui.label("Time scale");
        ui.add(egui::DragValue::new(&mut panels.inputs.vfx_preview_time_scale).range(0.1..=4.0));
        if ui
            .add_enabled(enabled, egui::Button::new("Apply speed"))
            .clicked()
        {
            let result = panels
                .editor
                .request_vfx_preview_action(VfxPreviewAction::TimeScale(
                    panels.inputs.vfx_preview_time_scale,
                ));
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
    });
}

fn module_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    ui.collapsing("Reusable VFX module", |ui| {
        module_toolbar(panels, ui);
        if let Some(module) = panels.specialised.active_vfx_module() {
            ui.label(format!(
                "Editing {} · {}",
                module.name,
                module.stage.label()
            ));
            module_stage_controls(panels, ui);
            module_input_controls(panels, ui);
            module_dependency_controls(panels, ui);
            module_attachment_controls(panels, ui);
        }
    });
}

fn module_toolbar(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.label("Asset");
        ui.text_edit_singleline(&mut panels.inputs.vfx_module_reference);
        if ui.button("Open module").clicked() {
            panels.intents.push(Intent::OpenVfxModule(
                panels.inputs.vfx_module_reference.clone(),
            ));
        }
        if panels.specialised.active_vfx_module().is_some() && ui.button("Save module").clicked() {
            let result = panels
                .specialised
                .vfx_module_snapshot()
                .and_then(|module| {
                    module.ok_or_else(|| Problem::new("save a VFX module", "no module is open"))
                })
                .and_then(|module| module.encode_text());
            match result {
                Ok(source) => panels.intents.push(Intent::Invoke(
                    "vfx.module.save".into(),
                    Arguments::new()
                        .with(
                            "reference",
                            Value::Text(panels.inputs.vfx_module_reference.clone()),
                        )
                        .with("source", Value::Text(source)),
                )),
                Err(problem) => panels.inputs.vfx_document_problem = Some(problem.to_string()),
            }
        }
        if panels.specialised.active_vfx_module().is_some()
            && ui.button("Discard module edits").clicked()
        {
            panels.intents.push(Intent::DiscardVfxModuleChanges);
        }
    });
    ui.horizontal(|ui| {
        ui.label("New");
        ui.text_edit_singleline(&mut panels.inputs.vfx_module_name);
        egui::ComboBox::from_id_salt("new-vfx-module-stage")
            .selected_text(panels.inputs.vfx_module_stage.label())
            .show_ui(ui, |ui| {
                for stage in Stage::ALL {
                    ui.selectable_value(&mut panels.inputs.vfx_module_stage, stage, stage.label());
                }
            });
        if ui.button("Create module").clicked() {
            panels.intents.push(Intent::CreateVfxModule(
                panels.inputs.vfx_module_name.clone(),
                panels.inputs.vfx_module_stage,
            ));
        }
    });
}

fn module_stage_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let current = panels.specialised.active_vfx_module().unwrap().stage;
    let mut selected = current;
    ui.horizontal(|ui| {
        ui.label("Compatible stage");
        egui::ComboBox::from_id_salt("open-vfx-module-stage")
            .selected_text(current.label())
            .show_ui(ui, |ui| {
                for stage in Stage::ALL {
                    ui.selectable_value(&mut selected, stage, stage.label());
                }
            });
    });
    if selected != current {
        let arguments = Arguments::new().with("stage", Value::Text(selected.label().into()));
        edit_module_metadata(panels, "vfx.module.stage.set", arguments, |module| {
            module.stage = selected;
        });
    }
}

fn module_input_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let inputs = panels
        .specialised
        .active_vfx_module()
        .unwrap()
        .inputs
        .clone();
    for (index, input) in inputs.into_iter().enumerate() {
        ui.horizontal(|ui| {
            ui.label(format!("Input {}: {}", input.name, input.kind));
            if ui.button("Remove").clicked() {
                let arguments = Arguments::new().with("name", Value::Text(input.name));
                edit_module_metadata(panels, "vfx.module.input.remove", arguments, |module| {
                    module.inputs.remove(index);
                });
            }
        });
    }
    ui.horizontal(|ui| {
        ui.label("Host input");
        ui.text_edit_singleline(&mut panels.inputs.vfx_module_input_name);
        numeric_kind(
            ui,
            "vfx-module-input-kind",
            &mut panels.inputs.vfx_module_input_kind,
        );
        if ui.button("Add input").clicked() {
            let input = ModuleInput {
                name: panels.inputs.vfx_module_input_name.clone(),
                kind: panels.inputs.vfx_module_input_kind.clone(),
            };
            let arguments = Arguments::new()
                .with("name", Value::Text(input.name.clone()))
                .with("kind", Value::Text(input.kind.clone()));
            edit_module_metadata(panels, "vfx.module.input.add", arguments, |module| {
                module.inputs.push(input);
            });
        }
    });
}

fn module_dependency_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let dependencies = panels
        .specialised
        .active_vfx_module()
        .unwrap()
        .dependencies
        .clone();
    for (index, dependency) in dependencies.into_iter().enumerate() {
        ui.horizontal(|ui| {
            ui.label(format!("Depends on {dependency}"));
            if ui.button("Remove").clicked() {
                let arguments = Arguments::new().with("name", Value::Text(dependency));
                edit_module_metadata(
                    panels,
                    "vfx.module.dependency.remove",
                    arguments,
                    |module| {
                        module.dependencies.remove(index);
                    },
                );
            }
        });
    }
    ui.horizontal(|ui| {
        ui.label("Dependency");
        ui.text_edit_singleline(&mut panels.inputs.vfx_module_dependency_name);
        if ui.button("Add dependency").clicked() {
            let name = panels.inputs.vfx_module_dependency_name.clone();
            let arguments = Arguments::new().with("name", Value::Text(name.clone()));
            edit_module_metadata(panels, "vfx.module.dependency.add", arguments, |module| {
                module.dependencies.push(name);
            });
        }
    });
}

fn edit_module_metadata(
    panels: &mut Panels<'_>,
    command: &str,
    arguments: Arguments,
    edit_draft: impl FnOnce(&mut VfxModule),
) {
    if let Some(intent) =
        saved_asset_edit_intent(panels.saved_vfx_module_reference, command, arguments)
    {
        panels.intents.push(intent);
        panels.inputs.vfx_document_problem = None;
        return;
    }
    let result = panels.specialised.edit_vfx_module_metadata(|module| {
        edit_draft(module);
        Ok(())
    });
    panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
}

fn saved_asset_edit_intent(
    reference: Option<&str>,
    command: &str,
    arguments: Arguments,
) -> Option<Intent> {
    reference.map(|reference| {
        Intent::Invoke(
            command.into(),
            arguments.with("reference", Value::Text(reference.into())),
        )
    })
}

fn module_attachment_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let Some(document) = panels.specialised.vfx_document() else {
        return;
    };
    let emitters: Vec<String> = document
        .emitters
        .iter()
        .map(|emitter| emitter.name.clone())
        .collect();
    if emitters.is_empty() {
        return;
    }
    ui.horizontal(|ui| {
        egui::ComboBox::from_id_salt("vfx-module-emitter")
            .selected_text(
                emitters
                    .get(panels.inputs.vfx_module_emitter)
                    .map_or("Emitter", String::as_str),
            )
            .show_ui(ui, |ui| {
                for (index, name) in emitters.iter().enumerate() {
                    ui.selectable_value(&mut panels.inputs.vfx_module_emitter, index, name);
                }
            });
        if ui.button("Attach saved module").clicked() {
            let result = if let Some(reference) = panels.saved_vfx_document_reference {
                validated_module_attachment(panels).map(|(emitter, path, _)| {
                    panels.intents.push(Intent::Invoke(
                        "vfx.module.attach".into(),
                        Arguments::new()
                            .with("reference", Value::Text(reference.into()))
                            .with("emitter", Value::Text(emitter))
                            .with("module_reference", Value::Text(path)),
                    ));
                })
            } else {
                attach_saved_module(panels).map(|source| {
                    panels.intents.push(Intent::Invoke(
                        "vfx.document.save".into(),
                        Arguments::new()
                            .with(
                                "reference",
                                Value::Text(panels.inputs.vfx_reference.clone()),
                            )
                            .with("source", Value::Text(source)),
                    ));
                })
            };
            match result {
                Ok(()) => panels.inputs.vfx_document_problem = None,
                Err(problem) => panels.inputs.vfx_document_problem = Some(problem.to_string()),
            }
        }
    });
}

fn attach_saved_module(panels: &mut Panels<'_>) -> Result<String> {
    cy_editor_services::vfx_document::validate_reference(&panels.inputs.vfx_reference)?;
    let (_, path, module) = validated_module_attachment(panels)?;
    let emitter = panels.inputs.vfx_module_emitter;
    panels
        .specialised
        .edit_vfx_metadata(|document| document.attach_module(emitter, module.name, path))?;
    panels
        .specialised
        .vfx_document_snapshot()?
        .ok_or_else(|| Problem::new("attach a VFX module", "system document closed"))?
        .encode_text()
}

fn validated_module_attachment(panels: &Panels<'_>) -> Result<(String, String, VfxModule)> {
    if panels.editor.workspace.active().is_none() {
        return Err(Problem::new(
            "attach a VFX module",
            "no scene document is active for undo history",
        ));
    }
    let path = panels.inputs.vfx_module_reference.clone();
    cy_editor_services::vfx_module::validate_reference(&path)?;
    let saved = panels.editor.project.read_source(&path)?;
    let module = VfxModule::decode_text(&saved)?;
    let emitter = panels.inputs.vfx_module_emitter;
    let document = panels
        .specialised
        .vfx_document()
        .ok_or_else(|| Problem::new("attach a VFX module", "no system document is open"))?;
    let host = document
        .emitters
        .get(emitter)
        .ok_or_else(|| Problem::new("attach a VFX module", "unknown emitter"))?;
    for input in &module.inputs {
        if !host
            .attributes
            .iter()
            .any(|attribute| attribute.name == input.name && attribute.kind == input.kind)
        {
            return Err(Problem::new(
                "attach a VFX module",
                format!(
                    "emitter {} lacks {} ({})",
                    host.name, input.name, input.kind
                ),
            ));
        }
    }
    Ok((host.name.clone(), path, module))
}

fn document_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.label("Document");
        ui.text_edit_singleline(&mut panels.inputs.vfx_reference);
        if ui.button("Open VFX document").clicked() {
            panels
                .intents
                .push(Intent::OpenVfxDocument(panels.inputs.vfx_reference.clone()));
        }
        if panels.specialised.vfx_document().is_some() && ui.button("Save VFX draft").clicked() {
            let result = panels
                .specialised
                .vfx_document_snapshot()
                .and_then(|document| {
                    document
                        .ok_or_else(|| {
                            cy_editor_core::problem::Problem::new(
                                "save a VFX document",
                                "no VFX document is open",
                            )
                        })?
                        .encode_text()
                });
            match result {
                Ok(source) => {
                    let arguments = Arguments::new()
                        .with(
                            "reference",
                            Value::Text(panels.inputs.vfx_reference.clone()),
                        )
                        .with("source", Value::Text(source));
                    panels
                        .intents
                        .push(Intent::Invoke("vfx.document.save".into(), arguments));
                    panels.inputs.vfx_document_problem = None;
                }
                Err(problem) => panels.inputs.vfx_document_problem = Some(problem.to_string()),
            }
        }
        if panels.specialised.vfx_document().is_some()
            && ui
                .add_enabled(
                    panels.editor.runtime.is_connected()
                        && !matches!(
                            panels.editor.backend.vfx_compile_state(),
                            VfxCompileState::Pending(_)
                        ),
                    egui::Button::new("Compile VFX"),
                )
                .clicked()
        {
            let result = panels
                .specialised
                .vfx_document_snapshot()
                .and_then(|document| {
                    let document = document.ok_or_else(|| {
                        cy_editor_core::problem::Problem::new(
                            "compile a VFX system",
                            "no VFX document is open",
                        )
                    })?;
                    let signature = document.compile_signature_with_modules(|path| {
                        panels.editor.project.read_source(path)
                    })?;
                    submit_compile(
                        &document,
                        signature,
                        &mut panels.inputs.vfx_compile_signature,
                        true,
                        |source| panels.editor.request_vfx_compile(source).map(|_| ()),
                    )
                });
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
    });
    if panels.specialised.vfx_document().is_none() {
        ui.horizontal(|ui| {
            ui.label("System");
            ui.text_edit_singleline(&mut panels.inputs.vfx_system_name);
            if ui.button("New VFX system").clicked() {
                let result = VfxDocument::new(panels.inputs.vfx_system_name.clone())
                    .and_then(|document| panels.specialised.start_vfx_document(document));
                panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
            }
        });
        return;
    }

    let name = panels.specialised.vfx_document().unwrap().name.clone();
    ui.label(format!("System: {name}"));
    emitter_choices(panels, ui);
    stage_tabs(panels, ui);
    current_emitter_settings(panels, ui);
    declaration_controls(panels, ui);
    if let Some(problem) = &panels.inputs.vfx_document_problem {
        ui.colored_label(egui::Color32::RED, problem);
    }
}

fn declaration_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    egui::ScrollArea::vertical()
        .id_salt("vfx-declarations")
        .max_height(240.0)
        .show(ui, |ui| {
            ui.collapsing("System parameters", |ui| parameter_controls(panels, ui));
            ui.collapsing("Event channels", |ui| channel_controls(panels, ui));
            if let Some((emitter, _)) = panels.specialised.active_vfx_stage() {
                ui.collapsing("Particle attributes", |ui| {
                    attribute_controls(panels, ui, emitter);
                });
                ui.collapsing("Data interfaces", |ui| {
                    interface_controls(panels, ui, emitter);
                });
            }
        });
}

fn numeric_kind(ui: &mut egui::Ui, id: impl std::hash::Hash + std::fmt::Debug, kind: &mut String) {
    egui::ComboBox::from_id_salt(id)
        .selected_text(kind.as_str())
        .show_ui(ui, |ui| {
            for choice in ["float", "vec2", "vec3", "vec4", "int", "bool"] {
                ui.selectable_value(kind, choice.to_string(), choice);
            }
        });
}

fn precision(ui: &mut egui::Ui, id: impl std::hash::Hash + std::fmt::Debug, selected: &mut String) {
    egui::ComboBox::from_id_salt(id)
        .selected_text(selected.as_str())
        .show_ui(ui, |ui| {
            for choice in ["Auto", "Float32", "Float16", "Unorm8", "Snorm16"] {
                ui.selectable_value(selected, choice.to_string(), choice);
            }
        });
}

fn parameter_values(ui: &mut egui::Ui, kind: &str, values: &mut [f32; 4]) -> bool {
    match kind {
        "bool" => {
            let mut enabled = values[0] != 0.0;
            let changed = ui.checkbox(&mut enabled, "Value").changed();
            if changed {
                values[0] = if enabled { 1.0 } else { 0.0 };
            }
            changed
        }
        "int" => {
            let mut value = values[0];
            let changed = ui
                .add(egui::DragValue::new(&mut value).speed(1.0))
                .changed();
            if changed {
                values[0] = value.round().clamp(-16_777_216.0, 16_777_216.0);
            }
            changed
        }
        _ => {
            let lanes = match kind {
                "vec2" => 2,
                "vec3" => 3,
                "vec4" => 4,
                _ => 1,
            };
            let mut changed = false;
            for (lane, value) in values.iter_mut().take(lanes).enumerate() {
                ui.label(["X", "Y", "Z", "W"][lane]);
                changed |= ui.add(egui::DragValue::new(value)).changed();
            }
            changed
        }
    }
}

fn parameter_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let parameters = panels
        .specialised
        .vfx_document()
        .unwrap()
        .parameters
        .clone();
    for (index, mut parameter) in parameters.into_iter().enumerate() {
        let mut remove = false;
        let mut changed = false;
        let mut value_changed = false;
        ui.horizontal(|ui| {
            ui.label(format!("{} ({})", parameter.name, parameter.kind));
            value_changed = parameter_values(ui, &parameter.kind, &mut parameter.value);
            changed |= value_changed;
            changed |= ui
                .checkbox(&mut parameter.exposed, "Runtime exposed")
                .changed();
            remove = ui.button("Remove").clicked();
        });
        if changed || remove {
            let live = parameter.clone();
            let command = if remove {
                "vfx.parameter.remove"
            } else {
                "vfx.parameter.set"
            };
            let arguments = parameter_arguments(&parameter);
            edit_document_metadata(panels, command, arguments, |document| {
                if remove {
                    document.parameters.remove(index);
                } else {
                    document.parameters[index] = parameter;
                }
            });
            if panels.inputs.vfx_document_problem.is_none()
                && value_changed
                && !remove
                && live.exposed
                && panels.editor.backend.vfx_preview_snapshot().is_some()
            {
                let lanes = match live.kind.as_str() {
                    "vec2" => 2,
                    "vec3" => 3,
                    "vec4" => 4,
                    _ => 1,
                };
                panels
                    .inputs
                    .vfx_live_parameters
                    .push_back((live.name, live.value, lanes));
            }
        }
    }
    ui.horizontal(|ui| {
        ui.label("New");
        ui.text_edit_singleline(&mut panels.inputs.vfx_parameter_name);
        numeric_kind(
            ui,
            "new-vfx-parameter-kind",
            &mut panels.inputs.vfx_parameter_kind,
        );
        parameter_values(
            ui,
            &panels.inputs.vfx_parameter_kind,
            &mut panels.inputs.vfx_parameter_values,
        );
        ui.checkbox(&mut panels.inputs.vfx_parameter_exposed, "Runtime exposed");
        if ui.button("Add parameter").clicked() {
            let parameter = Parameter {
                name: panels.inputs.vfx_parameter_name.clone(),
                kind: panels.inputs.vfx_parameter_kind.clone(),
                value: panels.inputs.vfx_parameter_values,
                exposed: panels.inputs.vfx_parameter_exposed,
            };
            let arguments = parameter_arguments(&parameter);
            edit_document_metadata(panels, "vfx.parameter.set", arguments, |document| {
                document.parameters.push(parameter);
            });
        }
    });
}

fn parameter_arguments(parameter: &Parameter) -> Arguments {
    Arguments::new()
        .with("name", Value::Text(parameter.name.clone()))
        .with("kind", Value::Text(parameter.kind.clone()))
        .with("values", Value::Vec4(parameter.value))
        .with("exposed", Value::Bool(parameter.exposed))
}

fn channel_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let channels = panels.specialised.vfx_document().unwrap().channels.clone();
    for (index, mut channel) in channels.into_iter().enumerate() {
        let mut remove = false;
        let mut changed = false;
        ui.horizontal(|ui| {
            ui.label(&channel.name);
            ui.label("Events/frame");
            changed |= ui
                .add(egui::DragValue::new(&mut channel.max_events_per_frame).range(1..=1_000_000))
                .changed();
            ui.label("Depth");
            changed |= ui
                .add(egui::DragValue::new(&mut channel.max_chain_depth).range(1..=64))
                .changed();
            changed |= ui.checkbox(&mut channel.readback, "CPU readback").changed();
            remove = ui.button("Remove").clicked();
        });
        if changed || remove {
            let command = if remove {
                "vfx.channel.remove"
            } else {
                "vfx.channel.set"
            };
            let arguments = channel_arguments(&channel);
            edit_document_metadata(panels, command, arguments, |document| {
                if remove {
                    document.channels.remove(index);
                } else {
                    document.channels[index] = channel;
                }
            });
        }
    }
    ui.horizontal(|ui| {
        ui.label("New");
        ui.text_edit_singleline(&mut panels.inputs.vfx_channel_name);
        ui.label("Events/frame");
        ui.add(egui::DragValue::new(&mut panels.inputs.vfx_channel_events).range(1..=1_000_000));
        ui.label("Depth");
        ui.add(egui::DragValue::new(&mut panels.inputs.vfx_channel_depth).range(1..=64));
        ui.checkbox(&mut panels.inputs.vfx_channel_readback, "CPU readback");
        if ui.button("Add channel").clicked() {
            let channel = EventChannel {
                name: panels.inputs.vfx_channel_name.clone(),
                max_events_per_frame: panels.inputs.vfx_channel_events,
                max_chain_depth: panels.inputs.vfx_channel_depth,
                readback: panels.inputs.vfx_channel_readback,
            };
            let arguments = channel_arguments(&channel);
            edit_document_metadata(panels, "vfx.channel.set", arguments, |document| {
                document.channels.push(channel);
            });
        }
    });
}

fn channel_arguments(channel: &EventChannel) -> Arguments {
    Arguments::new()
        .with("name", Value::Text(channel.name.clone()))
        .with(
            "max_events_per_frame",
            Value::Int(i64::from(channel.max_events_per_frame)),
        )
        .with(
            "max_chain_depth",
            Value::Int(i64::from(channel.max_chain_depth)),
        )
        .with("readback", Value::Bool(channel.readback))
}

fn edit_document_metadata(
    panels: &mut Panels<'_>,
    command: &str,
    arguments: Arguments,
    edit_draft: impl FnOnce(&mut VfxDocument),
) {
    if let Some(intent) =
        saved_asset_edit_intent(panels.saved_vfx_document_reference, command, arguments)
    {
        panels.intents.push(intent);
        panels.inputs.vfx_document_problem = None;
        return;
    }
    let result = panels.specialised.edit_vfx_metadata(|document| {
        edit_draft(document);
        Ok(())
    });
    panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
}

fn attribute_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui, emitter: usize) {
    let authored = panels.specialised.vfx_document().unwrap().emitters[emitter].clone();
    let mut capacity = authored.capacity;
    ui.horizontal(|ui| {
        ui.label(format!("{} capacity", authored.name));
        if ui
            .add(egui::DragValue::new(&mut capacity).range(1..=1_000_000))
            .changed()
        {
            let arguments = Arguments::new()
                .with("emitter", Value::Text(authored.name.clone()))
                .with("capacity", Value::Int(i64::from(capacity)));
            edit_document_metadata(panels, "vfx.emitter.capacity.set", arguments, |document| {
                document.emitters[emitter].capacity = capacity;
            });
        }
    });
    for (index, mut attribute) in authored.attributes.into_iter().enumerate() {
        let mut remove = false;
        let mut changed = false;
        ui.horizontal(|ui| {
            ui.label(format!("{} ({})", attribute.name, attribute.kind));
            ui.label("Min");
            changed |= ui
                .add(egui::DragValue::new(&mut attribute.minimum))
                .changed();
            ui.label("Max");
            changed |= ui
                .add(egui::DragValue::new(&mut attribute.maximum))
                .changed();
            ui.label("Tolerance");
            changed |= ui
                .add(egui::DragValue::new(&mut attribute.tolerance).range(0.0..=f32::MAX))
                .changed();
            let previous = attribute.precision.clone();
            precision(
                ui,
                ("vfx-precision", emitter, index),
                &mut attribute.precision,
            );
            changed |= attribute.precision != previous;
            remove = ui.button("Remove").clicked();
        });
        if changed || remove {
            let command = if remove {
                "vfx.attribute.remove"
            } else {
                "vfx.attribute.set"
            };
            let arguments = attribute_arguments(&authored.name, &attribute);
            edit_document_metadata(panels, command, arguments, |document| {
                if remove {
                    document.emitters[emitter].attributes.remove(index);
                } else {
                    document.emitters[emitter].attributes[index] = attribute;
                }
            });
        }
    }
    ui.horizontal(|ui| {
        ui.label("New");
        ui.text_edit_singleline(&mut panels.inputs.vfx_attribute_name);
        numeric_kind(
            ui,
            "new-vfx-attribute-kind",
            &mut panels.inputs.vfx_attribute_kind,
        );
        ui.label("Min");
        ui.add(egui::DragValue::new(
            &mut panels.inputs.vfx_attribute_minimum,
        ));
        ui.label("Max");
        ui.add(egui::DragValue::new(
            &mut panels.inputs.vfx_attribute_maximum,
        ));
        ui.label("Tolerance");
        ui.add(
            egui::DragValue::new(&mut panels.inputs.vfx_attribute_tolerance).range(0.0..=f32::MAX),
        );
        precision(
            ui,
            "new-vfx-attribute-precision",
            &mut panels.inputs.vfx_attribute_precision,
        );
        if ui.button("Add attribute").clicked() {
            let attribute = Attribute {
                name: panels.inputs.vfx_attribute_name.clone(),
                kind: panels.inputs.vfx_attribute_kind.clone(),
                minimum: panels.inputs.vfx_attribute_minimum,
                maximum: panels.inputs.vfx_attribute_maximum,
                tolerance: panels.inputs.vfx_attribute_tolerance,
                precision: panels.inputs.vfx_attribute_precision.clone(),
            };
            let arguments = attribute_arguments(&authored.name, &attribute);
            edit_document_metadata(panels, "vfx.attribute.set", arguments, |document| {
                document.emitters[emitter].attributes.push(attribute);
            });
        }
    });
}

fn attribute_arguments(emitter: &str, attribute: &Attribute) -> Arguments {
    Arguments::new()
        .with("emitter", Value::Text(emitter.into()))
        .with("name", Value::Text(attribute.name.clone()))
        .with("kind", Value::Text(attribute.kind.clone()))
        .with("minimum", Value::Float(attribute.minimum))
        .with("maximum", Value::Float(attribute.maximum))
        .with("tolerance", Value::Float(attribute.tolerance))
        .with("precision", Value::Text(attribute.precision.clone()))
}

fn compile_report(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let compile_state = panels.editor.backend.vfx_compile_state().clone();
    match compile_state {
        VfxCompileState::Idle => {}
        VfxCompileState::Pending(_) => {
            ui.label(secondary(
                panels.shell,
                "Engine VFX compilation in progress…",
            ));
        }
        VfxCompileState::Compiled(_, report) => {
            ui.label(format!(
                "Last engine cook {:016x} · {} kernels · {} bytes/particle",
                report.cook_key, report.kernels, report.total_bytes_per_particle
            ));
            for emitter in &report.emitters {
                ui.collapsing(&emitter.name, |ui| {
                    ui.label(format!(
                        "{} kernels · {} bytes/particle · max population {} · cost {} units",
                        emitter.kernels,
                        emitter.bytes_per_particle,
                        emitter.max_population,
                        emitter.estimated_cost_units
                    ));
                    for slot in &emitter.layout {
                        ui.label(format!(
                            "{}: {} · precision {} · offset {} · stride {}{}",
                            slot.name,
                            slot.kind,
                            slot.precision,
                            slot.offset,
                            slot.stride,
                            if slot.elided { " · elided" } else { "" }
                        ));
                    }
                    for (index, source) in emitter.sources.iter().enumerate() {
                        ui.collapsing(format!("Generated Slang {}", index + 1), |ui| {
                            ui.code(source);
                        });
                    }
                });
            }
        }
        VfxCompileState::Failed(_, failure) => {
            let document = panels.specialised.vfx_document_snapshot().ok().flatten();
            ui.colored_label(
                egui::Color32::RED,
                format!("{}: {}", failure.code, failure.message),
            );
            for diagnostic in &failure.diagnostics {
                let target = diagnostic_target(document.as_ref(), diagnostic);
                let location = target.map_or_else(
                    || format!("node {}", diagnostic.node),
                    |(emitter, stage)| {
                        let name =
                            &document.as_ref().expect("target has document").emitters[emitter].name;
                        format!("{name} / {} / node {}", stage.label(), diagnostic.node)
                    },
                );
                let pin = if diagnostic.pin.is_empty() {
                    String::new()
                } else {
                    format!(" · {}", diagnostic.pin)
                };
                let label = format!(
                    "{} · {}{}: {}",
                    diagnostic.code, location, pin, diagnostic.message
                );
                if ui
                    .add_enabled(
                        target.is_some(),
                        egui::Button::new(egui::RichText::new(label).color(egui::Color32::RED))
                            .frame(false),
                    )
                    .clicked()
                    && let Some((emitter, stage)) = target
                {
                    navigate_to_diagnostic(panels.specialised, emitter, stage, diagnostic.node);
                }
            }
        }
        VfxCompileState::Cancelled(_) => {
            ui.label(secondary(panels.shell, "Engine VFX compilation cancelled."));
        }
    }
}

fn diagnostic_target(
    document: Option<&VfxDocument>,
    diagnostic: &VfxCompileDiagnostic,
) -> Option<(usize, Stage)> {
    let document = document?;
    let emitter = usize::try_from(diagnostic.emitter?).ok()?;
    let stage = *Stage::ALL.get(usize::from(diagnostic.stage?))?;
    if document
        .emitters
        .get(emitter)?
        .stages
        .iter()
        .any(|entry| entry.stage == stage)
    {
        Some((emitter, stage))
    } else {
        None
    }
}

fn navigate_to_diagnostic(
    editors: &mut SpecialisedEditors,
    emitter: usize,
    stage: Stage,
    node: u64,
) {
    if editors.select_vfx_stage(emitter, stage).is_err() {
        return;
    }
    if let Ok(session) = editors.open(Domain::VfxGraph)
        && let (Some(canvas), Ok(key)) = (session.graph, NodeKey::new(node))
        && canvas.node(key).is_some()
    {
        let _ = canvas.select([key]);
    }
}

fn emitter_choices(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let Some(capabilities) = panels.editor.backend.vfx_authoring_capabilities().cloned() else {
        ui.label(secondary(
            panels.shell,
            "Renderer and simulation target options are loading from the engine.",
        ));
        return;
    };
    let selected_renderer = capabilities
        .renderers
        .iter()
        .find(|renderer| renderer.kind == panels.inputs.vfx_new_renderer);
    let selected_target = capabilities
        .targets
        .iter()
        .find(|target| target.path == panels.inputs.vfx_new_path);
    ui.horizontal(|ui| {
        ui.label("Emitter");
        ui.text_edit_singleline(&mut panels.inputs.vfx_emitter_name);
        egui::ComboBox::from_id_salt("vfx-new-renderer")
            .selected_text(selected_renderer.map_or("Renderer", |value| value.name.as_str()))
            .show_ui(ui, |ui| {
                for renderer in &capabilities.renderers {
                    ui.add_enabled_ui(renderer.available, |ui| {
                        ui.selectable_value(
                            &mut panels.inputs.vfx_new_renderer,
                            renderer.kind,
                            &renderer.name,
                        )
                        .on_hover_text(&renderer.reason);
                    });
                }
            });
        egui::ComboBox::from_id_salt("vfx-new-target")
            .selected_text(selected_target.map_or("Target", |value| value.name.as_str()))
            .show_ui(ui, |ui| {
                for target in &capabilities.targets {
                    ui.add_enabled_ui(target.compile_available, |ui| {
                        ui.selectable_value(
                            &mut panels.inputs.vfx_new_path,
                            target.path,
                            &target.name,
                        )
                        .on_hover_text(&target.explanation);
                    });
                }
            });
        let renderer = capabilities
            .renderers
            .iter()
            .find(|entry| entry.kind == panels.inputs.vfx_new_renderer && entry.available);
        let target = capabilities
            .targets
            .iter()
            .find(|entry| entry.path == panels.inputs.vfx_new_path && entry.compile_available);
        if ui
            .add_enabled(
                renderer.is_some() && target.is_some(),
                egui::Button::new("Add emitter"),
            )
            .clicked()
        {
            let emitter = Emitter {
                name: panels.inputs.vfx_emitter_name.clone(),
                path: if panels.inputs.vfx_new_path == 1 {
                    SimulationPath::CpuRequired
                } else {
                    SimulationPath::GpuPreferred
                },
                renderer: renderer.expect("enabled above").name.clone(),
                stages: Vec::new(),
                modules: Vec::new(),
                interfaces: Vec::new(),
                capacity: 1024,
                attributes: Vec::new(),
            };
            let result = panels
                .specialised
                .add_vfx_emitter(emitter)
                .and_then(|index| panels.specialised.select_vfx_stage(index, Stage::Spawn));
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
    });
    for renderer in &capabilities.renderers {
        if !renderer.available {
            ui.label(secondary(
                panels.shell,
                format!("{} unavailable: {}", renderer.name, renderer.reason),
            ));
        }
    }
    for target in &capabilities.targets {
        if !target.runtime_available {
            ui.label(secondary(
                panels.shell,
                format!("{} preview: {}", target.name, target.explanation),
            ));
        }
    }
}

fn stage_tabs(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let emitters: Vec<_> = panels
        .specialised
        .vfx_document()
        .unwrap()
        .emitters
        .iter()
        .map(|emitter| emitter.name.clone())
        .collect();
    egui::ScrollArea::horizontal().show(ui, |ui| {
        ui.horizontal(|ui| {
            for (index, emitter) in emitters.iter().enumerate() {
                for stage in Stage::ALL {
                    let selected = panels.specialised.active_vfx_stage() == Some((index, stage));
                    if ui
                        .selectable_label(selected, format!("{emitter} · {}", stage.label()))
                        .clicked()
                    {
                        let result = panels.specialised.select_vfx_stage(index, stage);
                        panels.inputs.vfx_document_problem =
                            result.err().map(|error| error.to_string());
                    }
                }
            }
        });
    });
}

fn interface_controls(panels: &mut Panels<'_>, ui: &mut egui::Ui, emitter_index: usize) {
    let Some(capabilities) = panels.editor.backend.vfx_authoring_capabilities().cloned() else {
        ui.label("Engine interface catalogue is loading.");
        return;
    };
    let emitter = panels.specialised.vfx_document().unwrap().emitters[emitter_index].clone();
    for (index, name) in emitter.interfaces.iter().enumerate() {
        ui.push_id(index, |ui| {
            ui.horizontal(|ui| {
                ui.label(name);
                if ui.button("Remove").clicked() {
                    let arguments = Arguments::new()
                        .with("emitter", Value::Text(emitter.name.clone()))
                        .with("interface", Value::Text(name.clone()));
                    edit_document_metadata(panels, "vfx.interface.unbind", arguments, |document| {
                        document.emitters[emitter_index].interfaces.remove(index);
                    });
                }
            });
        });
    }
    let mut selected = None;
    egui::ComboBox::from_id_salt(("vfx-interface", emitter_index))
        .selected_text("Bind interface")
        .show_ui(ui, |ui| {
            for interface in &capabilities.interfaces {
                let supported = if emitter.path == SimulationPath::CpuRequired {
                    interface.cpu_available
                } else {
                    interface.gpu_available
                };
                let enabled = supported && !emitter.interfaces.contains(&interface.name);
                ui.add_enabled_ui(enabled, |ui| {
                    if ui
                        .selectable_label(false, &interface.name)
                        .on_hover_text(if supported {
                            "Available for this emitter"
                        } else {
                            "Unavailable for this emitter's simulation target"
                        })
                        .clicked()
                    {
                        selected = Some(interface.name.clone());
                    }
                });
            }
        });
    if let Some(name) = selected {
        let arguments = Arguments::new()
            .with("emitter", Value::Text(emitter.name))
            .with("interface", Value::Text(name.clone()));
        edit_document_metadata(panels, "vfx.interface.bind", arguments, |document| {
            document.emitters[emitter_index].interfaces.push(name);
        });
    }
}

fn current_emitter_settings(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let Some((index, _)) = panels.specialised.active_vfx_stage() else {
        return;
    };
    let Some(emitter) = panels
        .specialised
        .vfx_document()
        .and_then(|document| document.emitters.get(index))
        .cloned()
    else {
        return;
    };
    let Some(capabilities) = panels.editor.backend.vfx_authoring_capabilities().cloned() else {
        return;
    };
    let mut renderer = emitter.renderer.clone();
    let mut path = u8::from(emitter.path == SimulationPath::CpuRequired);
    let mut remove = false;
    ui.horizontal(|ui| {
        ui.label(format!("{} settings", emitter.name));
        renderer_picker(ui, &capabilities, &mut renderer);
        target_picker(ui, &capabilities, &mut path);
        remove = ui.button("Remove emitter").clicked();
    });
    if remove {
        let arguments = Arguments::new().with("emitter", Value::Text(emitter.name.clone()));
        if let Some(intent) = saved_asset_edit_intent(
            panels.saved_vfx_document_reference,
            "vfx.emitter.remove",
            arguments,
        ) {
            panels.intents.push(intent);
            panels.inputs.vfx_document_problem = None;
        } else {
            let result = remove_open_emitter(panels.specialised, index);
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
        return;
    }
    let target = if path == 1 {
        SimulationPath::CpuRequired
    } else {
        SimulationPath::GpuPreferred
    };
    if renderer != emitter.renderer || target != emitter.path {
        let arguments = Arguments::new()
            .with("emitter", Value::Text(emitter.name))
            .with(
                "target",
                Value::Text(if path == 1 { "cpu" } else { "gpu" }.into()),
            )
            .with("renderer", Value::Text(renderer.clone()));
        if let Some(intent) = saved_asset_edit_intent(
            panels.saved_vfx_document_reference,
            "vfx.emitter.configure",
            arguments,
        ) {
            panels.intents.push(intent);
            panels.inputs.vfx_document_problem = None;
        } else {
            let result = panels
                .specialised
                .set_vfx_emitter_settings(index, target, renderer);
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
    }
}

fn remove_open_emitter(specialised: &mut SpecialisedEditors, index: usize) -> Result<()> {
    let mut document = specialised
        .vfx_document_snapshot()?
        .ok_or_else(|| Problem::new("remove a VFX emitter", "no VFX document is open"))?;
    if index >= document.emitters.len() {
        return Err(Problem::new("remove a VFX emitter", "unknown emitter"));
    }
    document.emitters.remove(index);
    let next = index.min(document.emitters.len().saturating_sub(1));
    let select_next = !document.emitters.is_empty();
    specialised.start_vfx_document(document)?;
    if select_next {
        specialised.select_vfx_stage(next, Stage::Spawn)?;
    }
    Ok(())
}

fn renderer_picker(
    ui: &mut egui::Ui,
    capabilities: &VfxAuthoringCapabilities,
    current: &mut String,
) {
    egui::ComboBox::from_id_salt("vfx-current-renderer")
        .selected_text(current.as_str())
        .show_ui(ui, |ui| {
            for renderer in &capabilities.renderers {
                ui.add_enabled_ui(renderer.available, |ui| {
                    ui.selectable_value(current, renderer.name.clone(), &renderer.name)
                        .on_hover_text(&renderer.reason);
                });
            }
        });
}

fn target_picker(ui: &mut egui::Ui, capabilities: &VfxAuthoringCapabilities, current: &mut u8) {
    let label = capabilities
        .targets
        .iter()
        .find(|target| target.path == *current)
        .map_or("Target", |target| target.name.as_str());
    egui::ComboBox::from_id_salt("vfx-current-target")
        .selected_text(label)
        .show_ui(ui, |ui| {
            for target in &capabilities.targets {
                ui.add_enabled_ui(target.compile_available, |ui| {
                    ui.selectable_value(current, target.path, &target.name)
                        .on_hover_text(&target.explanation);
                });
            }
        });
}

enum SavedCanvas {
    Stage {
        reference: String,
        emitter: String,
        stage: Stage,
    },
    Module {
        reference: String,
    },
}

fn saved_canvas(panels: &Panels<'_>) -> Option<SavedCanvas> {
    if panels.specialised.active_vfx_module().is_some() {
        let reference = panels.saved_vfx_module_reference?;
        let open = panels
            .specialised
            .vfx_module_snapshot()
            .ok()??
            .encode_text()
            .ok()?;
        let committed = panels.editor.project.read_source(reference).ok()?;
        return (open == committed).then(|| SavedCanvas::Module {
            reference: reference.into(),
        });
    }
    let (index, stage) = panels.specialised.active_vfx_stage()?;
    let reference = panels.saved_vfx_document_reference?;
    let open = panels
        .specialised
        .vfx_document_snapshot()
        .ok()??
        .encode_text()
        .ok()?;
    let committed = panels.editor.project.read_source(reference).ok()?;
    if open != committed {
        return None;
    }
    let emitter = panels.specialised.vfx_document()?.emitters.get(index)?;
    Some(SavedCanvas::Stage {
        reference: reference.into(),
        emitter: emitter.name.clone(),
        stage,
    })
}

fn palette_add_intent(saved: &SavedCanvas, node_type: &str, at: Layout) -> Intent {
    let arguments = Arguments::new()
        .with("node_type", Value::Text(node_type.into()))
        .with("x", Value::Float(at.x))
        .with("y", Value::Float(at.y));
    match saved {
        SavedCanvas::Stage {
            reference,
            emitter,
            stage,
        } => Intent::Invoke(
            "vfx.node.add".into(),
            arguments
                .with("reference", Value::Text(reference.clone()))
                .with("emitter", Value::Text(emitter.clone()))
                .with("stage", Value::Text(stage.label().to_ascii_lowercase())),
        ),
        SavedCanvas::Module { reference } => Intent::Invoke(
            "vfx.module.node.add".into(),
            arguments.with("reference", Value::Text(reference.clone())),
        ),
    }
}

fn add_palette_node(
    canvas: &mut GraphCanvas,
    saved: Option<&SavedCanvas>,
    intents: &mut Vec<Intent>,
    node_type: &str,
    at: Layout,
) -> Result<()> {
    if let Some(saved) = saved {
        intents.push(palette_add_intent(saved, node_type, at));
    } else {
        canvas.add(node_type, at)?;
    }
    Ok(())
}

fn palette(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    canvas: &mut GraphCanvas,
    filter: &mut String,
    saved: Option<&SavedCanvas>,
    intents: &mut Vec<Intent>,
) {
    ui.heading("Engine catalogue");
    ui.label(secondary(
        shell,
        format!("{} VFX node types", canvas.catalogue().len()),
    ));
    ui.add(egui::TextEdit::singleline(filter).hint_text("Search VFX nodes"));
    let query = filter.trim().to_ascii_lowercase();
    let names: Vec<String> = canvas
        .catalogue()
        .type_names()
        .into_iter()
        .filter(|name| name.to_ascii_lowercase().contains(&query))
        .map(ToOwned::to_owned)
        .collect();
    egui::ScrollArea::vertical().show(ui, |ui| {
        for name in names {
            let label = name.strip_prefix("vfx.").unwrap_or(&name).replace('_', " ");
            if ui
                .button(format!("＋ {label}"))
                .on_hover_text(&name)
                .clicked()
            {
                let index = canvas.nodes().count();
                let column = index % 3;
                let row = index / 3;
                let at = Layout {
                    x: 28.0 + f32::from(u16::try_from(column).unwrap_or(u16::MAX)) * 224.0,
                    y: 34.0 + f32::from(u16::try_from(row).unwrap_or(u16::MAX)) * 150.0,
                };
                let _ = add_palette_node(canvas, saved, intents, &name, at);
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::codec::Writer;
    use cy_editor_interface::specialised::graph::{Catalogue, NodeType};
    use cy_editor_interface::specialised::vfx::StageGraph;

    #[test]
    fn saved_palette_additions_use_the_same_commands_as_mcp() {
        let at = Layout { x: 28.0, y: 34.0 };
        let mut canvas = GraphCanvas::new(1);
        canvas.load(Catalogue::new(vec![NodeType::new("vfx.constant", Vec::new())]).unwrap());
        let mut intents = Vec::new();
        let stage = SavedCanvas::Stage {
            reference: "effects/sparks.cyvfxdoc".into(),
            emitter: "embers".into(),
            stage: Stage::Spawn,
        };
        add_palette_node(&mut canvas, Some(&stage), &mut intents, "vfx.constant", at).unwrap();
        assert_eq!(canvas.nodes().count(), 0);
        let Intent::Invoke(command, arguments) = intents.remove(0) else {
            panic!("saved stage insertion must invoke a command");
        };
        assert_eq!(command, "vfx.node.add");
        assert_eq!(arguments.text("reference"), Some("effects/sparks.cyvfxdoc"));
        assert_eq!(arguments.text("emitter"), Some("embers"));
        assert_eq!(arguments.text("stage"), Some("spawn"));
        assert_eq!(arguments.text("node_type"), Some("vfx.constant"));
        assert_eq!(arguments.get("x"), Some(&Value::Float(28.0)));
        assert_eq!(arguments.get("y"), Some(&Value::Float(34.0)));

        let module = SavedCanvas::Module {
            reference: "effects/shared.cyvfxmodule".into(),
        };
        add_palette_node(&mut canvas, Some(&module), &mut intents, "vfx.constant", at).unwrap();
        assert_eq!(canvas.nodes().count(), 0);
        let Intent::Invoke(command, arguments) = intents.remove(0) else {
            panic!("saved module insertion must invoke a command");
        };
        assert_eq!(command, "vfx.module.node.add");
        assert_eq!(
            arguments.text("reference"),
            Some("effects/shared.cyvfxmodule")
        );
        assert_eq!(arguments.text("node_type"), Some("vfx.constant"));

        add_palette_node(&mut canvas, None, &mut intents, "vfx.constant", at).unwrap();
        assert_eq!(canvas.nodes().count(), 1);
        assert!(intents.is_empty());
    }

    #[test]
    fn saved_module_controls_route_to_mcp_commands_at_the_open_asset_path() {
        let reference = "effects/open.cyvfxmodule";
        for command in [
            "vfx.module.stage.set",
            "vfx.module.input.add",
            "vfx.module.input.remove",
            "vfx.module.dependency.add",
            "vfx.module.dependency.remove",
        ] {
            let arguments = Arguments::new().with("name", Value::Text("drag".into()));
            let Some(Intent::Invoke(actual, arguments)) =
                saved_asset_edit_intent(Some(reference), command, arguments)
            else {
                panic!("saved module edit did not produce a command");
            };
            assert_eq!(actual, command);
            assert_eq!(arguments.text("reference"), Some(reference));
            assert_eq!(arguments.text("name"), Some("drag"));
        }
        assert!(saved_asset_edit_intent(None, "vfx.module.input.add", Arguments::new()).is_none());
    }

    #[test]
    fn saved_system_metadata_uses_typed_commands_and_values() {
        let reference = "effects/open.cyvfxdoc";
        let channel = EventChannel {
            name: "on_death".into(),
            max_events_per_frame: 128,
            max_chain_depth: 3,
            readback: true,
        };
        let Some(Intent::Invoke(command, arguments)) = saved_asset_edit_intent(
            Some(reference),
            "vfx.channel.set",
            channel_arguments(&channel),
        ) else {
            panic!("saved channel edit did not produce a command");
        };
        assert_eq!(command, "vfx.channel.set");
        assert_eq!(arguments.text("reference"), Some(reference));
        assert_eq!(
            arguments.get("max_events_per_frame"),
            Some(&Value::Int(128))
        );
        assert_eq!(arguments.get("max_chain_depth"), Some(&Value::Int(3)));
        assert_eq!(arguments.get("readback"), Some(&Value::Bool(true)));

        let parameter = Parameter {
            name: "wind".into(),
            kind: "vec3".into(),
            value: [1.0, 2.0, 3.0, 0.0],
            exposed: true,
        };
        let arguments = parameter_arguments(&parameter);
        assert_eq!(arguments.get("values"), Some(&Value::Vec4(parameter.value)));
        assert_eq!(arguments.get("exposed"), Some(&Value::Bool(true)));
        for command in [
            "vfx.interface.bind",
            "vfx.interface.unbind",
            "vfx.emitter.configure",
            "vfx.emitter.remove",
        ] {
            let intent = saved_asset_edit_intent(Some(reference), command, Arguments::new());
            let Some(Intent::Invoke(actual, arguments)) = intent else {
                panic!("saved hierarchy edit did not produce a command");
            };
            assert_eq!(actual, command);
            assert_eq!(arguments.text("reference"), Some(reference));
        }
    }

    #[test]
    fn removing_an_open_emitter_preserves_the_other_stage_draft() {
        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(42);
        catalogue.u32(1);
        catalogue.text("vfx.constant");
        catalogue.u32(0);
        catalogue.u32(0);
        let mut editors = SpecialisedEditors::new().unwrap();
        editors.install_vfx_catalogue(&catalogue.finish()).unwrap();

        let mut document = VfxDocument::new("sparks").unwrap();
        for name in ["smoke", "embers"] {
            document.emitters.push(Emitter {
                name: name.into(),
                path: SimulationPath::CpuRequired,
                renderer: "Sprite".into(),
                stages: Vec::new(),
                modules: Vec::new(),
                interfaces: Vec::new(),
                capacity: 1024,
                attributes: Vec::new(),
            });
        }
        editors.start_vfx_document(document).unwrap();
        editors.select_vfx_stage(1, Stage::Update).unwrap();
        editors
            .open(Domain::VfxGraph)
            .unwrap()
            .graph
            .unwrap()
            .add("vfx.constant", Layout::default())
            .unwrap();

        remove_open_emitter(&mut editors, 0).unwrap();
        assert_eq!(editors.active_vfx_stage(), Some((0, Stage::Spawn)));
        let remaining = editors.vfx_document_snapshot().unwrap().unwrap();
        assert_eq!(remaining.emitters.len(), 1);
        assert_eq!(remaining.emitters[0].name, "embers");
        assert!(remaining.emitters[0].stages.iter().any(|stage| {
            stage.stage == Stage::Update && stage.canvas.contains("node 1 vfx.constant")
        }));

        remove_open_emitter(&mut editors, 0).unwrap();
        assert_eq!(editors.active_vfx_stage(), None);
        assert!(editors.vfx_document().unwrap().emitters.is_empty());
    }

    #[test]
    fn compiler_alerts_use_emitter_and_stage_even_when_node_keys_repeat() {
        let diagnostic = |emitter, stage| VfxCompileDiagnostic {
            severity: 2,
            code: "graph.invalid-node".into(),
            message: "unknown node type".into(),
            detail: String::new(),
            node: 1,
            pin: String::new(),
            emitter: Some(emitter),
            stage: Some(stage),
        };
        let diagnostics = [diagnostic(0, 0), diagnostic(1, 2)];
        let alerts = active_node_alerts((1, Stage::Update), &diagnostics);
        assert_eq!(alerts.len(), 1);
        assert_eq!(alerts[0].0, 1);
        assert!(alerts[0].1.contains("unknown node type"));
    }

    #[test]
    fn diagnostic_target_keeps_the_engine_emitter_and_stage() {
        let mut document = VfxDocument::new("sparks").unwrap();
        for name in ["smoke", "embers"] {
            document.emitters.push(Emitter {
                name: name.into(),
                path: SimulationPath::GpuPreferred,
                renderer: "Sprite".into(),
                stages: vec![StageGraph {
                    stage: Stage::Update,
                    canvas: format!("cyvfxcanvas 1\nemitter {name}\nnode 1 vfx.constant\n"),
                }],
                modules: Vec::new(),
                interfaces: Vec::new(),
                capacity: 1024,
                attributes: Vec::new(),
            });
        }
        let diagnostic = VfxCompileDiagnostic {
            severity: 2,
            code: "graph.invalid-node".into(),
            message: "unknown node type".into(),
            detail: String::new(),
            node: 1,
            pin: String::new(),
            emitter: Some(1),
            stage: Some(Stage::Update as u8),
        };
        assert_eq!(
            diagnostic_target(Some(&document), &diagnostic),
            Some((1, Stage::Update))
        );
    }

    #[test]
    fn diagnostic_navigation_opens_the_scoped_stage_and_selects_its_node() {
        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(42);
        catalogue.u32(1);
        catalogue.text("vfx.constant");
        catalogue.u32(0);
        catalogue.u32(0);
        let mut editors = SpecialisedEditors::new().unwrap();
        editors.install_vfx_catalogue(&catalogue.finish()).unwrap();

        let mut canvas = GraphCanvas::new(1);
        canvas.load(
            Catalogue::new(vec![NodeType::identified(
                42,
                1,
                "vfx.constant".into(),
                Vec::new(),
            )])
            .unwrap(),
        );
        let node = canvas.add("vfx.constant", Layout::default()).unwrap();
        let mut document = VfxDocument::new("sparks").unwrap();
        for name in ["smoke", "embers"] {
            document.emitters.push(Emitter {
                name: name.into(),
                path: SimulationPath::GpuPreferred,
                renderer: "Sprite".into(),
                stages: Vec::new(),
                modules: Vec::new(),
                interfaces: Vec::new(),
                capacity: 1024,
                attributes: Vec::new(),
            });
        }
        document.capture_stage(0, Stage::Update, &canvas).unwrap();
        document.capture_stage(1, Stage::Update, &canvas).unwrap();
        editors.start_vfx_document(document).unwrap();
        editors.select_vfx_stage(0, Stage::Update).unwrap();

        navigate_to_diagnostic(&mut editors, 1, Stage::Update, node.ordinal());

        assert_eq!(editors.active_vfx_stage(), Some((1, Stage::Update)));
        assert_eq!(
            editors
                .open(Domain::VfxGraph)
                .unwrap()
                .graph
                .unwrap()
                .selection(),
            vec![node]
        );
    }

    #[test]
    fn graph_edits_submit_a_new_cook_but_live_values_do_not() {
        let mut document = VfxDocument::new("sparks").unwrap();
        document.emitters.push(Emitter {
            name: "smoke".into(),
            path: SimulationPath::GpuPreferred,
            renderer: "Sprite".into(),
            stages: vec![StageGraph {
                stage: Stage::Spawn,
                canvas: "cyvfxcanvas 1\nemitter smoke\nnode 1 vfx.constant\n".into(),
            }],
            modules: Vec::new(),
            interfaces: Vec::new(),
            capacity: 1024,
            attributes: Vec::new(),
        });
        document.parameters.push(Parameter {
            name: "speed".into(),
            kind: "float".into(),
            value: [2.0, 0.0, 0.0, 0.0],
            exposed: true,
        });
        let mut last = None;
        let mut submitted = Vec::new();
        {
            let mut submit = |source: String| {
                submitted.push(source);
                Ok(())
            };
            assert!(
                submit_compile(
                    &document,
                    document.compile_signature().unwrap(),
                    &mut last,
                    false,
                    &mut submit
                )
                .unwrap()
            );
            document.parameters[0].value[0] = 4.0;
            assert!(
                !submit_compile(
                    &document,
                    document.compile_signature().unwrap(),
                    &mut last,
                    false,
                    &mut submit
                )
                .unwrap()
            );
            document.emitters[0].stages[0].canvas =
                "cyvfxcanvas 1\nemitter smoke\nnode 1 vfx.random\n".into();
            assert!(
                submit_compile(
                    &document,
                    document.compile_signature().unwrap(),
                    &mut last,
                    false,
                    &mut submit
                )
                .unwrap()
            );
            assert!(
                submit_compile(
                    &document,
                    document.compile_signature().unwrap(),
                    &mut last,
                    true,
                    &mut submit
                )
                .unwrap()
            );
        }
        assert_eq!(submitted.len(), 3);
        assert!(
            VfxDocument::decode_text(&submitted[1]).unwrap().emitters[0].stages[0]
                .canvas
                .contains("node 1 vfx.random")
        );
    }

    #[test]
    fn refused_compile_request_keeps_the_document_eligible_for_retry() {
        let mut document = VfxDocument::new("sparks").unwrap();
        document.emitters.push(Emitter {
            name: "smoke".into(),
            path: SimulationPath::GpuPreferred,
            renderer: "Sprite".into(),
            stages: Vec::new(),
            modules: Vec::new(),
            interfaces: Vec::new(),
            capacity: 1024,
            attributes: Vec::new(),
        });
        let mut last = None;
        let refusal = submit_compile(
            &document,
            document.compile_signature().unwrap(),
            &mut last,
            false,
            |_| {
                Err(cy_editor_core::problem::Problem::new(
                    "compile",
                    "runtime unavailable",
                ))
            },
        );
        assert!(refusal.is_err());
        assert!(last.is_none());
        assert!(
            submit_compile(
                &document,
                document.compile_signature().unwrap(),
                &mut last,
                false,
                |_| Ok(())
            )
            .unwrap()
        );
    }
}
