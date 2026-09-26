// SPDX-License-Identifier: MIT
//! VFX node editing uses the same graph canvas and backend catalogue as material authoring.

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::specialised::graph::{GraphCanvas, Layout};
use cy_editor_interface::specialised::vfx::{
    Attribute, Emitter, EventChannel, Parameter, SimulationPath, Stage, VfxDocument,
};
use cy_editor_services::MaterialCatalogueState;
use cy_editor_services::backend::VfxCompileState;
use cy_editor_services::vfx_capabilities::VfxAuthoringCapabilities;
use cy_editor_services::vfx_preview::VfxPreviewAction;

use super::{Intent, Panels, material_graph, nothing_here, secondary};

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
    document_controls(panels, ui);
    preview_controls(panels, ui);
    compile_report(panels, ui);
    if panels.specialised.active_vfx_stage().is_none() {
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
    ui.label(secondary(
        panels.shell,
        "Editable stage draft · engine simulation preview · viewport particles",
    ));
    let available = ui.available_size();
    ui.horizontal(|ui| {
        ui.allocate_ui_with_layout(
            egui::vec2(220.0_f32.min(available.x * 0.38), available.y),
            egui::Layout::top_down(egui::Align::Min),
            |ui| {
                palette(ui, panels.shell, canvas, &mut panels.inputs.vfx_filter);
                material_graph::graph_properties(
                    ui,
                    canvas,
                    &panels.editor.asset_catalogue,
                    &mut panels.inputs.vfx_property_problem,
                );
            },
        );
        ui.separator();
        ui.allocate_ui(egui::vec2(ui.available_width(), available.y), |ui| {
            material_graph::draw_canvas(
                ui,
                panels.shell,
                canvas,
                state,
                "Empty VFX stage graph\nChoose a node from the engine catalogue",
                &mut panels.inputs.vfx_link_source,
                &mut panels.inputs.vfx_link_problem,
            );
        });
    });
    auto_compile(panels);
}

fn submit_compile(
    document: &VfxDocument,
    last: &mut Option<Vec<u8>>,
    force: bool,
    mut submit: impl FnMut(String) -> cy_editor_core::problem::Result<()>,
) -> cy_editor_core::problem::Result<bool> {
    let signature = document.compile_signature()?;
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
    let result = submit_compile(
        &document,
        &mut panels.inputs.vfx_compile_signature,
        false,
        |source| panels.editor.request_vfx_compile(source).map(|_| ()),
    );
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
                    submit_compile(
                        &document,
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
            let result = panels.specialised.edit_vfx_metadata(|document| {
                if remove {
                    document.parameters.remove(index);
                } else {
                    document.parameters[index] = parameter;
                }
                Ok(())
            });
            if result.is_ok()
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
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
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
            let result = panels.specialised.edit_vfx_metadata(|document| {
                document.parameters.push(parameter);
                Ok(())
            });
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
    });
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
            let result = panels.specialised.edit_vfx_metadata(|document| {
                if remove {
                    document.channels.remove(index);
                } else {
                    document.channels[index] = channel;
                }
                Ok(())
            });
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
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
            let result = panels.specialised.edit_vfx_metadata(|document| {
                document.channels.push(channel);
                Ok(())
            });
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
    });
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
            let result = panels.specialised.edit_vfx_metadata(|document| {
                document.emitters[emitter].capacity = capacity;
                Ok(())
            });
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
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
            let result = panels.specialised.edit_vfx_metadata(|document| {
                if remove {
                    document.emitters[emitter].attributes.remove(index);
                } else {
                    document.emitters[emitter].attributes[index] = attribute;
                }
                Ok(())
            });
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
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
            let result = panels.specialised.edit_vfx_metadata(|document| {
                document.emitters[emitter].attributes.push(attribute);
                Ok(())
            });
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
    });
}

fn compile_report(panels: &Panels<'_>, ui: &mut egui::Ui) {
    match panels.editor.backend.vfx_compile_state() {
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
            ui.colored_label(
                egui::Color32::RED,
                format!("{}: {}", failure.code, failure.message),
            );
            for diagnostic in &failure.diagnostics {
                ui.colored_label(
                    egui::Color32::RED,
                    format!(
                        "{} · node {}{}: {}",
                        diagnostic.code,
                        diagnostic.node,
                        if diagnostic.pin.is_empty() {
                            String::new()
                        } else {
                            format!(" · {}", diagnostic.pin)
                        },
                        diagnostic.message
                    ),
                );
            }
        }
        VfxCompileState::Cancelled(_) => {
            ui.label(secondary(panels.shell, "Engine VFX compilation cancelled."));
        }
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
                    let result = panels.specialised.edit_vfx_metadata(|document| {
                        document.emitters[emitter_index].interfaces.remove(index);
                        Ok(())
                    });
                    panels.inputs.vfx_document_problem =
                        result.err().map(|error| error.to_string());
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
        let result = panels.specialised.edit_vfx_metadata(|document| {
            document.emitters[emitter_index].interfaces.push(name);
            Ok(())
        });
        panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
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
    ui.horizontal(|ui| {
        ui.label(format!("{} settings", emitter.name));
        renderer_picker(ui, &capabilities, &mut renderer);
        target_picker(ui, &capabilities, &mut path);
    });
    let target = if path == 1 {
        SimulationPath::CpuRequired
    } else {
        SimulationPath::GpuPreferred
    };
    if renderer != emitter.renderer || target != emitter.path {
        let result = panels
            .specialised
            .set_vfx_emitter_settings(index, target, renderer);
        panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
    }
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

fn palette(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    canvas: &mut GraphCanvas,
    filter: &mut String,
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
                let _ = canvas.add(
                    &name,
                    Layout {
                        x: 28.0 + f32::from(u16::try_from(column).unwrap_or(u16::MAX)) * 224.0,
                        y: 34.0 + f32::from(u16::try_from(row).unwrap_or(u16::MAX)) * 150.0,
                    },
                );
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_interface::specialised::vfx::StageGraph;

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
            assert!(submit_compile(&document, &mut last, false, &mut submit).unwrap());
            document.parameters[0].value[0] = 4.0;
            assert!(!submit_compile(&document, &mut last, false, &mut submit).unwrap());
            document.emitters[0].stages[0].canvas =
                "cyvfxcanvas 1\nemitter smoke\nnode 1 vfx.random\n".into();
            assert!(submit_compile(&document, &mut last, false, &mut submit).unwrap());
            assert!(submit_compile(&document, &mut last, true, &mut submit).unwrap());
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
        let refusal = submit_compile(&document, &mut last, false, |_| {
            Err(cy_editor_core::problem::Problem::new(
                "compile",
                "runtime unavailable",
            ))
        });
        assert!(refusal.is_err());
        assert!(last.is_none());
        assert!(submit_compile(&document, &mut last, false, |_| Ok(())).unwrap());
    }
}
