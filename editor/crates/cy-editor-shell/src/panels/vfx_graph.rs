// SPDX-License-Identifier: MIT
//! VFX node editing uses the same graph canvas and backend catalogue as material authoring.

use cy_editor_interface::Domain;
use cy_editor_interface::specialised::graph::{GraphCanvas, Layout};
use cy_editor_interface::specialised::vfx::{Emitter, SimulationPath, Stage, VfxDocument};
use cy_editor_services::MaterialCatalogueState;

use super::{Panels, material_graph, nothing_here, secondary};

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
        "Editable stage draft · saving and runtime preview are not available yet",
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
    ui.horizontal(|ui| {
        ui.label("Emitter");
        ui.text_edit_singleline(&mut panels.inputs.vfx_emitter_name);
        if ui.button("Add emitter").clicked() {
            let emitter = Emitter {
                name: panels.inputs.vfx_emitter_name.clone(),
                path: SimulationPath::GpuPreferred,
                renderer: "Sprite".into(),
                stages: Vec::new(),
                modules: Vec::new(),
                interfaces: Vec::new(),
            };
            let result = panels
                .specialised
                .add_vfx_emitter(emitter)
                .and_then(|index| panels.specialised.select_vfx_stage(index, Stage::Spawn));
            panels.inputs.vfx_document_problem = result.err().map(|error| error.to_string());
        }
    });
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
    if let Some(problem) = &panels.inputs.vfx_document_problem {
        ui.colored_label(egui::Color32::RED, problem);
    }
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
