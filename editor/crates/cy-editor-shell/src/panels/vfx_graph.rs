// SPDX-License-Identifier: MIT
//! VFX node editing uses the same graph canvas and backend catalogue as material authoring.

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::specialised::graph::{GraphCanvas, Layout};
use cy_editor_interface::specialised::vfx::{Emitter, SimulationPath, Stage, VfxDocument};
use cy_editor_services::MaterialCatalogueState;
use cy_editor_services::backend::VfxCompileState;
use cy_editor_services::vfx_capabilities::VfxAuthoringCapabilities;

use super::{Intent, Panels, material_graph, nothing_here, secondary};

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let state = panels.editor.backend.vfx_catalogue_state();
    if !panels.specialised.can_open(Domain::VfxGraph) {
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
    compile_report(panels, ui);
    if panels.specialised.active_vfx_stage().is_none() {
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
        "Editable stage draft · engine compilation available · runtime preview pending",
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
                    document
                        .ok_or_else(|| {
                            cy_editor_core::problem::Problem::new(
                                "compile a VFX system",
                                "no VFX document is open",
                            )
                        })?
                        .encode_text()
                })
                .and_then(|source| panels.editor.request_vfx_compile(source).map(|_| ()));
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
    if let Some(problem) = &panels.inputs.vfx_document_problem {
        ui.colored_label(egui::Color32::RED, problem);
    }
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
