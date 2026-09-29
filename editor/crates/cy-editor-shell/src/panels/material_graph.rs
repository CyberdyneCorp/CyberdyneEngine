// SPDX-License-Identifier: MIT
//! The visible material graph: engine palette at the left, the shared graph canvas at the right.
//!
//! This module is a renderer and interaction adapter only. Node identity, catalogue membership,
//! selection, layout, links, type checking, and diagnostics remain in `GraphCanvas`; material code
//! contributes no second graph model.

use std::cell::{Cell, RefCell};

use cy_editor_commands::Arguments;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::specialised::graph::{GraphCanvas, Layout as GraphLayout, NodeKey};
use cy_editor_interface::specialised::material::{
    SURFACE_STAGE, VERTEX_STAGE, canvas_interchange, load_canvas_interchange,
};
use cy_editor_services::primitives::material_of;
use cy_editor_services::{
    AssetCatalogueService, Editor, MaterialCatalogueState, MaterialDiagnosticSeverity,
    MaterialOperation, MaterialPreviewState, MaterialRequestState,
};
use cy_editor_visual::colour::Semantic;
use cy_editor_visual::density::TextRole;

// Re-exported because the VFX panel's tests name these gestures by this module's path.
use super::graph_canvas::{
    CanvasFeedback, PaletteEntry, display_name, draw_canvas, graph_properties_with, matches_filter,
    node_palette, palette_slot,
};
pub(super) use super::graph_canvas::{GraphConnection, GraphMovement};
// The canvas internals this panel's own tests exercise moved with the canvas; importing them here
// keeps those tests reading exactly as they did.
#[cfg(test)]
use super::graph_canvas::{PinAction, apply_pin_action, node_cards};
use super::{Intent, Panels, nothing_here, secondary, status};
use crate::theme;
#[cfg(test)]
use cy_editor_interface::specialised::graph::{Pin, PinDirection};

const PALETTE_WIDTH: f32 = 220.0;

/// One saved-canvas drag; only its final position becomes a project transaction.
pub struct MaterialDragState {
    reference: String,
    node: NodeKey,
    initial: GraphLayout,
}

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let selected_graph = selected_material_graph(panels.editor);
    let geometry = panels
        .editor
        .assigned_material_geometry(panels.inputs.material_open_reference.as_deref());
    let project_root = panels.editor.project.root().to_path_buf();
    let state = panels.editor.backend.material_catalogue_state();
    let request_state = panels.editor.backend.material_request_state().clone();
    let preview_state = panels.editor.backend.material_preview_state().clone();
    if !panels.editor.runtime.is_connected() {
        panels.inputs.material_preview_source = None;
    }
    if !panels.specialised.can_open(Domain::Materials) {
        unavailable(panels, ui, state);
        return;
    }

    let session = match panels.specialised.open(Domain::Materials) {
        Ok(session) => session,
        Err(problem) => {
            nothing_here(
                ui,
                panels.shell,
                "The material graph could not be opened.",
                &problem.to_string(),
            );
            return;
        }
    };
    let canvas = session
        .graph
        .expect("the material domain is declared as a graph surface");
    selected_graph_control(
        ui,
        panels.inputs,
        canvas,
        selected_graph.as_deref(),
        &project_root,
    );
    let available = ui.available_size();
    let mut action = None;
    let backend = PaletteBackendState {
        catalogue: state,
        request: &request_state,
        preview: &preview_state,
        project_root: &project_root,
    };
    ui.horizontal(|ui| {
        ui.allocate_ui_with_layout(
            egui::vec2(PALETTE_WIDTH.min(available.x * 0.38), available.y),
            egui::Layout::top_down(egui::Align::Min),
            |ui| {
                action = palette(
                    ui,
                    panels.shell,
                    canvas,
                    panels.inputs,
                    panels.intents,
                    backend,
                    &panels.editor.asset_catalogue,
                );
            },
        );
        ui.separator();
        ui.allocate_ui(egui::vec2(ui.available_width(), available.y), |ui| {
            draw_material_canvas(
                ui,
                panels.shell,
                canvas,
                panels.inputs,
                panels.intents,
                backend,
            );
        });
    });
    let had_action = action.is_some();
    handle_palette_action(
        panels.editor,
        panels.inputs,
        panels.intents,
        canvas,
        action,
        &geometry,
    );
    if !had_action
        && panels.editor.runtime.is_connected()
        && state == MaterialCatalogueState::Ready
        && !matches!(request_state, MaterialRequestState::Pending { .. })
    {
        preview_changed_graph(panels.editor, panels.inputs, canvas);
    }
}

fn draw_material_canvas(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    canvas: &mut GraphCanvas,
    inputs: &mut super::Inputs,
    intents: &mut Vec<Intent>,
    backend: PaletteBackendState<'_>,
) {
    let saved_reference = saved_canvas_is_current(canvas, inputs, backend.project_root)
        .then(|| inputs.material_open_reference.clone())
        .flatten();
    let pending = matches!(backend.request, MaterialRequestState::Pending { .. });
    let queued = RefCell::new(Vec::new());
    let draft = RefCell::new(None);
    let draft_name = inputs.material_name.clone();
    let draft_reference = inputs.material_open_reference.clone();
    let move_seen = Cell::new(false);
    let mut connect = |canvas: &mut GraphCanvas, link: &GraphConnection| -> Result<()> {
        if pending {
            return Err(Problem::new(
                "connect material nodes",
                "engine authoring is pending",
            ));
        }
        if let Some(reference) = saved_reference.as_ref() {
            let mut candidate = canvas.clone();
            candidate.connect_identified(link.from, link.from_pin, link.to, link.to_pin)?;
            queued
                .borrow_mut()
                .push(material_connect_intent(reference, link));
            Ok(())
        } else {
            canvas.connect_identified(link.from, link.from_pin, link.to, link.to_pin)?;
            *draft.borrow_mut() = Some(material_draft_intent(
                canvas,
                &draft_name,
                draft_reference.as_deref(),
            )?);
            Ok(())
        }
    };
    let mut move_node = |canvas: &mut GraphCanvas, movement: GraphMovement| {
        move_seen.set(true);
        if pending {
            return Err(Problem::new(
                "move a material node",
                "engine authoring is pending",
            ));
        }
        let saved_drag = inputs.material_drag.is_some();
        move_material_node_gesture(
            canvas,
            saved_reference.as_deref(),
            &mut inputs.material_drag,
            pending,
            &queued,
            movement,
        )?;
        if movement.finished && saved_reference.is_none() && !saved_drag {
            *draft.borrow_mut() = Some(material_draft_intent(
                canvas,
                &draft_name,
                draft_reference.as_deref(),
            )?);
        }
        Ok(())
    };
    draw_canvas(
        ui,
        shell,
        canvas,
        backend.catalogue,
        "Empty material graph\nChoose a node from the engine catalogue",
        &mut inputs.material_link_source,
        &mut CanvasFeedback {
            link_problem: &mut inputs.material_link_problem,
            node_alerts: &[],
            on_connect: Some(&mut connect),
            on_move: Some(&mut move_node),
        },
    );
    if !move_seen.get() {
        inputs.material_drag = None;
    }
    intents.extend(queued.into_inner());
    if let Some((reference, intent)) = draft.into_inner() {
        inputs.material_open_reference = Some(reference);
        inputs.material_canvas_state = super::MaterialCanvasState::Draft;
        intents.push(intent);
    }
}

fn selected_graph_control(
    ui: &mut egui::Ui,
    inputs: &mut super::Inputs,
    canvas: &mut GraphCanvas,
    reference: Option<&str>,
    project_root: &std::path::Path,
) {
    let Some(reference) = reference else { return };
    ui.horizontal(|ui| {
        ui.label(format!("Selected material: {reference}"));
        if ui.button("Open graph").clicked() {
            match open_selected_graph(project_root, reference, canvas) {
                Ok(name) => {
                    inputs.material_name = name;
                    inputs.material_open_reference = Some(reference.into());
                    inputs.material_canvas_state = super::MaterialCanvasState::Authored;
                    inputs.material_preview_source = None;
                    inputs.material_property_problem = None;
                    inputs.material_drag = None;
                }
                Err(problem) => inputs.material_property_problem = Some(problem),
            }
        }
    });
}

fn handle_palette_action(
    editor: &mut Editor,
    inputs: &mut super::Inputs,
    intents: &mut Vec<Intent>,
    canvas: &GraphCanvas,
    action: Option<PaletteAction>,
    geometry: &[&str],
) {
    match action {
        Some(PaletteAction::Save) => {
            let reference = inputs
                .material_open_reference
                .clone()
                .unwrap_or_else(|| format!("materials/{}.cygraph", inputs.material_name));
            match canvas_interchange(&inputs.material_name, canvas) {
                Ok(source) => {
                    let arguments = Arguments::new()
                        .with("reference", Value::Text(reference))
                        .with("source", Value::Text(source));
                    intents.push(Intent::Invoke("material.graph.save".into(), arguments));
                }
                Err(problem) => {
                    editor
                        .notifications
                        .post(cy_editor_services::Notification::error(
                            "Material save failed",
                            problem,
                        ));
                }
            }
        }
        Some(PaletteAction::Request(operation)) => {
            match canvas_interchange(&inputs.material_name, canvas).and_then(|payload| {
                editor.request_material_for_geometry(operation, payload.into_bytes(), geometry)
            }) {
                Ok(_) => {}
                Err(problem) => {
                    editor
                        .notifications
                        .post(cy_editor_services::Notification::error(
                            "The material request could not be submitted",
                            problem,
                        ));
                }
            }
        }
        Some(PaletteAction::Cancel) => {
            if let Err(problem) = editor.cancel_material_request() {
                editor
                    .notifications
                    .post(cy_editor_services::Notification::error(
                        "The material request could not be cancelled",
                        problem,
                    ));
            }
        }
        None => {}
    }
}

fn preview_changed_graph(editor: &mut Editor, inputs: &mut super::Inputs, canvas: &GraphCanvas) {
    let Some(reference) = inputs.material_open_reference.clone() else {
        return;
    };
    let Ok(source) = canvas_interchange(&inputs.material_name, canvas) else {
        return;
    };
    let semantic: String = source
        .lines()
        .filter(|line| !line.starts_with("# layout "))
        .flat_map(|line| [line, "\n"])
        .collect();
    if inputs.material_preview_source.as_ref() == Some(&(reference.clone(), semantic.clone())) {
        return;
    }
    if editor.preview_material_graph(&reference, &source).is_ok() {
        inputs.material_preview_source = Some((reference, semantic));
    }
}

fn selected_material_graph(editor: &Editor) -> Option<String> {
    let document = editor.documents.get(editor.workspace.active()?)?;
    let mut selected = editor.selection.get().nodes();
    let node = selected.next()?;
    if selected.next().is_some() {
        return None;
    }
    material_of(document, node).filter(|path| path.ends_with(".cygraph"))
}

fn open_selected_graph(
    root: &std::path::Path,
    reference: &str,
    canvas: &mut GraphCanvas,
) -> std::result::Result<String, String> {
    let source = std::path::Path::new(reference).with_extension("cymatcanvas");
    if !source
        .components()
        .all(|part| matches!(part, std::path::Component::Normal(_)))
    {
        return Err("material path must stay inside the project".to_owned());
    }
    let text = std::fs::read_to_string(root.join(source)).map_err(|error| error.to_string())?;
    load_canvas_interchange(&text, canvas).map_err(|problem| problem.to_string())
}

#[derive(Clone, Copy)]
enum PaletteAction {
    Request(MaterialOperation),
    Save,
    Cancel,
}

fn unavailable(panels: &Panels<'_>, ui: &mut egui::Ui, state: MaterialCatalogueState) {
    let (what, remedy) = match state {
        MaterialCatalogueState::Loading => (
            "Loading the engine material catalogue…",
            "The panel will populate when the asynchronous backend request completes.",
        ),
        MaterialCatalogueState::Failed => (
            "The engine material catalogue is unavailable.",
            "Open Problems for the backend diagnostic, then reconnect a compatible runtime.",
        ),
        MaterialCatalogueState::Unavailable | MaterialCatalogueState::Ready => (
            "No engine material catalogue is loaded.",
            "Start a hosted runtime; material definitions come from the engine, not this editor.",
        ),
    };
    nothing_here(ui, panels.shell, what, remedy);
}

fn palette(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    canvas: &mut GraphCanvas,
    inputs: &mut super::Inputs,
    intents: &mut Vec<Intent>,
    backend: PaletteBackendState<'_>,
    assets: &AssetCatalogueService,
) -> Option<PaletteAction> {
    let mut action = None;
    ui.heading("Engine catalogue");
    ui.label(secondary(
        shell,
        format!("{} stable node types", canvas.catalogue().len()),
    ));
    ui.add_space(shell.metrics().gap() * 0.5);
    let ready = backend.catalogue == MaterialCatalogueState::Ready;
    let pending = matches!(backend.request, MaterialRequestState::Pending { .. });
    ui.horizontal(|ui| {
        if ui
            .add_enabled(ready && !pending, egui::Button::new("Validate"))
            .clicked()
        {
            action = Some(PaletteAction::Request(MaterialOperation::Validate));
        }
        if ui
            .add_enabled(ready && !pending, egui::Button::new("Compile"))
            .clicked()
        {
            action = Some(PaletteAction::Request(MaterialOperation::Compile));
        }
        if ui
            .add_enabled(ready && !pending, egui::Button::new("Save .cygraph"))
            .clicked()
        {
            action = Some(PaletteAction::Save);
        }
        if pending && ui.button("Cancel").clicked() {
            action = Some(PaletteAction::Cancel);
        }
    });
    material_request_status(ui, shell, canvas, backend.request);
    material_preview_status(ui, shell, backend.preview);
    material_edit_controls(ui, canvas, inputs, intents, backend, assets);
    ui.horizontal(|ui| {
        ui.label("Stage");
        egui::ComboBox::from_id_salt("material-palette-stage")
            .selected_text(if inputs.material_stage == VERTEX_STAGE {
                "Vertex"
            } else {
                "Surface"
            })
            .show_ui(ui, |ui| {
                ui.selectable_value(&mut inputs.material_stage, SURFACE_STAGE, "Surface");
                ui.selectable_value(&mut inputs.material_stage, VERTEX_STAGE, "Vertex");
            });
    });
    ui.add_space(shell.metrics().gap() * 0.5);
    ui.add(
        egui::TextEdit::singleline(&mut inputs.material_filter)
            .hint_text("Search nodes")
            .desired_width(f32::INFINITY),
    );
    ui.add_space(shell.metrics().gap() * 0.5);

    let query = inputs.material_filter.trim().to_ascii_lowercase();
    let entries = palette_names(canvas, &query, inputs.material_stage)
        .into_iter()
        .filter_map(|name| {
            let identity = canvas.catalogue().get(&name)?.identity;
            Some(PaletteEntry {
                label: display_name(&name),
                hover: format!("{name} · NodeTypeId {identity}"),
                name,
            })
        })
        .collect();
    if let Some(name) = node_palette(ui, entries, !pending) {
        let at = palette_slot(canvas.nodes().count());
        add_palette_node(canvas, inputs, intents, backend.project_root, &name, at);
    }

    ui.with_layout(
        egui::Layout::bottom_up(egui::Align::Min),
        |ui| match backend.catalogue {
            MaterialCatalogueState::Ready => {
                status(ui, shell, Semantic::Live, "Runtime services ready");
            }
            _ => status(
                ui,
                shell,
                Semantic::Warning,
                "Offline snapshot — compile unavailable",
            ),
        },
    );
    action
}

fn palette_names(canvas: &GraphCanvas, query: &str, stage: u8) -> Vec<String> {
    canvas
        .catalogue()
        .type_names()
        .into_iter()
        .filter(|name| {
            matches_filter(name, query)
                && canvas
                    .catalogue()
                    .get(name)
                    .is_some_and(|node| node.supports_stage(stage))
        })
        .map(ToOwned::to_owned)
        .collect()
}

fn material_edit_controls(
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    inputs: &mut super::Inputs,
    intents: &mut Vec<Intent>,
    backend: PaletteBackendState<'_>,
    assets: &AssetCatalogueService,
) {
    let saved = saved_canvas_is_current(canvas, inputs, backend.project_root);
    let pending = matches!(backend.request, MaterialRequestState::Pending { .. });
    let draft = RefCell::new(None);
    let draft_name = inputs.material_name.clone();
    let draft_reference = inputs.material_open_reference.clone();
    ui.add_enabled_ui(!pending, |ui| {
        graph_properties_with(
            ui,
            canvas,
            assets,
            &mut inputs.material_property_problem,
            |canvas, key, property, value| {
                if saved && let Some(reference) = inputs.material_open_reference.as_ref() {
                    property.validate_literal(&value)?;
                    intents.push(Intent::Invoke(
                        "material.node.property.set".into(),
                        Arguments::new()
                            .with("reference", Value::Text(reference.clone()))
                            .with(
                                "node",
                                Value::Int(i64::try_from(key.ordinal()).unwrap_or(i64::MAX)),
                            )
                            .with("property", Value::Text(property.name.clone()))
                            .with("value", Value::Text(value)),
                    ));
                    Ok(())
                } else {
                    canvas.set_property_by_identity(key, property.identity, value)?;
                    *draft.borrow_mut() = Some(material_draft_intent(
                        canvas,
                        &draft_name,
                        draft_reference.as_deref(),
                    )?);
                    Ok(())
                }
            },
        );
    });
    if let Some((reference, intent)) = draft.into_inner() {
        inputs.material_open_reference = Some(reference);
        inputs.material_canvas_state = super::MaterialCanvasState::Draft;
        intents.push(intent);
    }
    selected_node_remove_control(ui, canvas, inputs, intents, saved, pending);
    link_disconnect_controls(ui, canvas, inputs, intents, saved, pending);
}

fn selected_node_remove_control(
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    inputs: &mut super::Inputs,
    intents: &mut Vec<Intent>,
    saved: bool,
    pending: bool,
) {
    let Some(node) = canvas.selection().first().copied() else {
        return;
    };
    if !ui
        .add_enabled(!pending, egui::Button::new("Remove selected node"))
        .clicked()
    {
        return;
    }
    if saved && let Some(reference) = inputs.material_open_reference.as_ref() {
        intents.push(Intent::Invoke(
            "material.node.remove".into(),
            Arguments::new()
                .with("reference", Value::Text(reference.clone()))
                .with(
                    "node",
                    Value::Int(i64::try_from(node.ordinal()).unwrap_or(i64::MAX)),
                ),
        ));
    } else {
        let result = canvas
            .remove(node)
            .and_then(|()| queue_material_draft(canvas, inputs, intents));
        if let Err(problem) = result {
            inputs.material_link_problem = Some(problem.to_string());
        }
    }
}

fn link_disconnect_controls(
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    inputs: &mut super::Inputs,
    intents: &mut Vec<Intent>,
    saved: bool,
    pending: bool,
) {
    let links: Vec<_> = canvas.links().cloned().collect();
    for link in links {
        ui.horizontal(|ui| {
            ui.label(format!(
                "{}:{} → {}:{}",
                link.from.ordinal(),
                link.from_pin,
                link.to.ordinal(),
                link.to_pin
            ));
            if !ui
                .add_enabled(!pending, egui::Button::new("Disconnect"))
                .clicked()
            {
                return;
            }
            if saved && let Some(reference) = inputs.material_open_reference.as_ref() {
                intents.push(Intent::Invoke(
                    "material.node.disconnect".into(),
                    Arguments::new()
                        .with("reference", Value::Text(reference.clone()))
                        .with(
                            "from",
                            Value::Int(i64::try_from(link.from.ordinal()).unwrap_or(i64::MAX)),
                        )
                        .with("from_pin", Value::Text(link.from_pin.clone()))
                        .with(
                            "to",
                            Value::Int(i64::try_from(link.to.ordinal()).unwrap_or(i64::MAX)),
                        )
                        .with("to_pin", Value::Text(link.to_pin.clone())),
                ));
            } else {
                let result = canvas
                    .disconnect(link.from, &link.from_pin, link.to, &link.to_pin)
                    .and_then(|()| queue_material_draft(canvas, inputs, intents));
                if let Err(problem) = result {
                    inputs.material_link_problem = Some(problem.to_string());
                }
            }
        });
    }
}

fn add_palette_node(
    canvas: &mut GraphCanvas,
    inputs: &mut super::Inputs,
    intents: &mut Vec<Intent>,
    project_root: &std::path::Path,
    node_type: &str,
    at: GraphLayout,
) {
    let saved = saved_canvas_is_current(canvas, inputs, project_root);
    if saved && let Some(reference) = inputs.material_open_reference.as_ref() {
        intents.push(Intent::Invoke(
            "material.node.add".into(),
            Arguments::new()
                .with("reference", Value::Text(reference.clone()))
                .with("node_type", Value::Text(node_type.into()))
                .with("x", Value::Float(at.x))
                .with("y", Value::Float(at.y)),
        ));
    } else {
        let result = canvas
            .add(node_type, at)
            .and_then(|_node| queue_material_draft(canvas, inputs, intents));
        if let Err(problem) = result {
            inputs.material_link_problem = Some(problem.to_string());
        }
    }
}

fn material_connect_intent(reference: &str, link: &GraphConnection) -> Intent {
    Intent::Invoke(
        "material.node.connect".into(),
        Arguments::new()
            .with("reference", Value::Text(reference.into()))
            .with(
                "from",
                Value::Int(i64::try_from(link.from.ordinal()).unwrap_or(i64::MAX)),
            )
            .with("from_pin", Value::Text(link.from_name.clone()))
            .with(
                "to",
                Value::Int(i64::try_from(link.to.ordinal()).unwrap_or(i64::MAX)),
            )
            .with("to_pin", Value::Text(link.to_name.clone())),
    )
}

fn move_material_node_gesture(
    canvas: &mut GraphCanvas,
    saved_reference: Option<&str>,
    drag: &mut Option<MaterialDragState>,
    pending: bool,
    queued: &RefCell<Vec<Intent>>,
    movement: GraphMovement,
) -> Result<()> {
    if !movement.finished {
        if pending && saved_reference.is_some() {
            return Err(Problem::new(
                "move a material node",
                "engine authoring is pending",
            ));
        }
        if drag.is_none()
            && let Some(reference) = saved_reference
        {
            let initial = canvas.layout_of(movement.node).ok_or_else(|| {
                Problem::new(
                    "move a material node",
                    "the selected node has no canvas layout",
                )
            })?;
            *drag = Some(MaterialDragState {
                reference: reference.into(),
                node: movement.node,
                initial,
            });
        }
        return canvas.move_to(movement.node, movement.at);
    }
    let Some(state) = drag.take() else {
        return Ok(());
    };
    if state.node != movement.node {
        *drag = Some(state);
        return Ok(());
    }
    let at = canvas.layout_of(state.node).ok_or_else(|| {
        Problem::new(
            "move a material node",
            "the dragged node has no canvas layout",
        )
    })?;
    if at != state.initial {
        queued.borrow_mut().push(Intent::Invoke(
            "material.node.move".into(),
            Arguments::new()
                .with("reference", Value::Text(state.reference))
                .with(
                    "node",
                    Value::Int(i64::try_from(state.node.ordinal()).unwrap_or(i64::MAX)),
                )
                .with("x", Value::Float(at.x))
                .with("y", Value::Float(at.y)),
        ));
    }
    Ok(())
}

fn saved_canvas_is_current(
    canvas: &GraphCanvas,
    inputs: &super::Inputs,
    project_root: &std::path::Path,
) -> bool {
    if inputs.material_canvas_state == super::MaterialCanvasState::Draft {
        return false;
    }
    inputs
        .material_open_reference
        .as_ref()
        .is_some_and(|reference| {
            if !project_root.join(reference).is_file() {
                return false;
            }
            let path = project_root.join(reference).with_extension("cymatcanvas");
            canvas_interchange(&inputs.material_name, canvas).is_ok_and(|source| {
                std::fs::read_to_string(path).ok().as_deref() == Some(source.as_str())
            })
        })
}

fn material_draft_intent(
    canvas: &GraphCanvas,
    name: &str,
    reference: Option<&str>,
) -> Result<(String, Intent)> {
    let source = canvas_interchange(name, canvas)?;
    let reference =
        reference.map_or_else(|| format!("materials/{name}.cygraph"), ToOwned::to_owned);
    let intent = Intent::Invoke(
        "material.canvas.draft.save".into(),
        Arguments::new()
            .with("reference", Value::Text(reference.clone()))
            .with("source", Value::Text(source)),
    );
    Ok((reference, intent))
}

fn queue_material_draft(
    canvas: &GraphCanvas,
    inputs: &mut super::Inputs,
    intents: &mut Vec<Intent>,
) -> Result<()> {
    let (reference, intent) = material_draft_intent(
        canvas,
        &inputs.material_name,
        inputs.material_open_reference.as_deref(),
    )?;
    inputs.material_open_reference = Some(reference);
    inputs.material_canvas_state = super::MaterialCanvasState::Draft;
    intents.push(intent);
    Ok(())
}

#[derive(Clone, Copy)]
struct PaletteBackendState<'a> {
    catalogue: MaterialCatalogueState,
    request: &'a MaterialRequestState,
    preview: &'a MaterialPreviewState,
    project_root: &'a std::path::Path,
}

fn material_preview_status(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    preview: &MaterialPreviewState,
) {
    let (role, text) = match preview {
        MaterialPreviewState::Idle => return,
        MaterialPreviewState::Pending { request, operation } => (
            Semantic::Active,
            format!("Preview {operation} · request #{}", request.as_u64()),
        ),
        MaterialPreviewState::Applied { preview, artefact } => (
            Semantic::Live,
            format!("Preview {preview:016x} · applied {artefact:016x}"),
        ),
        MaterialPreviewState::Failed { problem } => (
            Semantic::Error,
            format!("Preview kept previous artefact · {}", problem.because),
        ),
    };
    ui.label(
        egui::RichText::new(text)
            .color(theme::role(shell.theme, role))
            .size(shell.metrics().text(TextRole::Secondary)),
    );
}

fn material_request_status(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    canvas: &mut GraphCanvas,
    request: &MaterialRequestState,
) {
    if let MaterialRequestState::Failed {
        request,
        diagnostics,
    } = request
    {
        ui.label(
            egui::RichText::new(request.map_or_else(
                || "Backend unavailable".into(),
                |id| format!("Failed · request #{}", id.as_u64()),
            ))
            .color(theme::role(shell.theme, Semantic::Error))
            .size(shell.metrics().text(TextRole::Secondary)),
        );
        for diagnostic in diagnostics {
            let semantic = match diagnostic.severity {
                MaterialDiagnosticSeverity::Info => Semantic::Active,
                MaterialDiagnosticSeverity::Warning => Semantic::Warning,
                MaterialDiagnosticSeverity::Error => Semantic::Error,
            };
            let location = if diagnostic.primary.node == 0 {
                String::new()
            } else if diagnostic.primary.pin == 0 {
                format!(" · node {}", diagnostic.primary.node)
            } else {
                format!(
                    " · node {} / pin {}",
                    diagnostic.primary.node, diagnostic.primary.pin
                )
            };
            let response = ui.add_enabled(
                diagnostic.primary.node != 0,
                egui::Button::new(
                    egui::RichText::new(format!(
                        "{}{}\n{}",
                        diagnostic.code, location, diagnostic.message
                    ))
                    .color(theme::role(shell.theme, semantic))
                    .size(shell.metrics().text(TextRole::Secondary)),
                )
                .frame(false),
            );
            if response.clicked() {
                select_backend_location(canvas, diagnostic.primary.node);
            }
        }
        return;
    }
    let (semantic, text) = match request {
        MaterialRequestState::Idle => (Semantic::SecondaryText, "Not yet validated".to_owned()),
        MaterialRequestState::Pending { request, operation } => (
            Semantic::Active,
            format!("{operation:?} request #{}…", request.as_u64()),
        ),
        MaterialRequestState::Validated { request } => (
            Semantic::Live,
            format!("Validated · request #{}", request.as_u64()),
        ),
        MaterialRequestState::Authored { request, .. } => (
            Semantic::Live,
            format!("Graph authored · request #{}", request.as_u64()),
        ),
        MaterialRequestState::Previewed { request } => (
            Semantic::Live,
            format!("Live preview · request #{}", request.as_u64()),
        ),
        MaterialRequestState::Compiled {
            request,
            artefact,
            graph,
            programs,
            dependencies,
            geometry_sources,
        } => (
            Semantic::Live,
            format!(
                "Compiled {artefact:016x}\ngraph {graph:016x} · {programs} programs · {} dependencies · geometry {}\nrequest #{}",
                dependencies.len(),
                if geometry_sources.is_empty() {
                    "unspecified".to_owned()
                } else {
                    geometry_sources.join(", ")
                },
                request.as_u64()
            ),
        ),
        MaterialRequestState::Failed { .. } => unreachable!(),
        MaterialRequestState::Cancelled { request } => (
            Semantic::Warning,
            format!("Cancelled · request #{}", request.as_u64()),
        ),
    };
    ui.label(
        egui::RichText::new(text)
            .color(theme::role(shell.theme, semantic))
            .size(shell.metrics().text(TextRole::Secondary)),
    );
}

fn select_backend_location(canvas: &mut GraphCanvas, node: u64) {
    if let Ok(node) = NodeKey::new(node) {
        let _ = canvas.select([node]);
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_interface::specialised::graph::{Catalogue, NodeType};
    use cy_editor_interface::specialised::material::VERTEX_STAGE;

    use super::*;

    #[test]
    fn desktop_material_save_uses_the_mcp_command_and_undo_path() {
        let mut editor = Editor::default();
        let mut inputs = super::super::Inputs {
            material_name: "sway".into(),
            ..Default::default()
        };
        let canvas = GraphCanvas::new(1);
        let mut intents = Vec::new();
        handle_palette_action(
            &mut editor,
            &mut inputs,
            &mut intents,
            &canvas,
            Some(PaletteAction::Save),
            &[],
        );
        let Some(Intent::Invoke(command, arguments)) = intents.first() else {
            panic!("desktop save must use the registered material graph command");
        };
        assert_eq!(command, "material.graph.save");
        assert_eq!(arguments.text("reference"), Some("materials/sway.cygraph"));
        assert!(
            arguments
                .text("source")
                .unwrap()
                .starts_with("cymatcanvas 1\n")
        );
    }

    #[test]
    fn saved_material_palette_node_uses_the_shared_edit_command() {
        let root = std::env::temp_dir().join(format!("cy-material-node-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("materials")).unwrap();
        let mut canvas = GraphCanvas::new(1);
        canvas.load(
            Catalogue::new(vec![NodeType::identified(
                1,
                1,
                "material.constant".into(),
                vec![],
            )])
            .unwrap(),
        );
        let mut inputs = super::super::Inputs {
            material_name: "sway".into(),
            material_open_reference: Some("materials/sway.cygraph".into()),
            ..Default::default()
        };
        std::fs::write(
            root.join("materials/sway.cymatcanvas"),
            canvas_interchange(&inputs.material_name, &canvas).unwrap(),
        )
        .unwrap();
        std::fs::write(root.join("materials/sway.cygraph"), "cygraph 1\n").unwrap();
        let mut intents = Vec::new();
        let at = GraphLayout { x: 12.0, y: 30.0 };
        add_palette_node(
            &mut canvas,
            &mut inputs,
            &mut intents,
            &root,
            "material.constant",
            at,
        );
        let Some(Intent::Invoke(command, arguments)) = intents.first() else {
            panic!("saved node placement must use the registry");
        };
        assert_eq!(command, "material.node.add");
        assert_eq!(arguments.text("node_type"), Some("material.constant"));
        assert_eq!(canvas.nodes().count(), 0);

        std::fs::remove_file(root.join("materials/sway.cymatcanvas")).unwrap();
        add_palette_node(
            &mut canvas,
            &mut inputs,
            &mut intents,
            &root,
            "material.constant",
            at,
        );
        assert_eq!(canvas.nodes().count(), 1);
        assert_eq!(intents.len(), 2);
        assert!(
            matches!(intents.last(), Some(Intent::Invoke(command, _)) if command == "material.canvas.draft.save")
        );
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn saved_material_drag_queues_one_move_on_release() {
        let mut canvas = GraphCanvas::new(1);
        canvas.load(
            Catalogue::new(vec![NodeType::identified(
                1,
                1,
                "material.constant".into(),
                vec![],
            )])
            .unwrap(),
        );
        let node = canvas
            .add("material.constant", GraphLayout { x: 12.0, y: 30.0 })
            .unwrap();
        let queued = RefCell::new(Vec::new());
        let mut drag = None;
        let moved = GraphLayout { x: 15.0, y: 40.0 };
        move_material_node_gesture(
            &mut canvas,
            Some("materials/sway.cygraph"),
            &mut drag,
            false,
            &queued,
            GraphMovement {
                node,
                at: moved,
                finished: false,
            },
        )
        .unwrap();
        assert_eq!(canvas.layout_of(node), Some(moved));
        assert!(queued.borrow().is_empty());
        move_material_node_gesture(
            &mut canvas,
            None,
            &mut drag,
            false,
            &queued,
            GraphMovement {
                node,
                at: moved,
                finished: true,
            },
        )
        .unwrap();
        let queued = queued.into_inner();
        let Some(Intent::Invoke(command, arguments)) = queued.first() else {
            panic!("a saved drag must queue a command");
        };
        assert_eq!(queued.len(), 1);
        assert_eq!(command, "material.node.move");
        assert_eq!(arguments.text("reference"), Some("materials/sway.cygraph"));
        assert_eq!(arguments.get("x").and_then(Value::as_float), Some(15.0));
    }

    #[test]
    fn active_scene_meshes_supply_static_geometry_for_primary_and_imported_slots() {
        use cy_editor_core::Actor;
        use cy_editor_documents::operation::Operation;
        use cy_editor_documents::selection::Selection;
        use cy_editor_services::primitives::{
            MaterialBinding, create_mesh_instance, set_material_slots,
        };
        use cy_editor_viewport::gizmo::Transform3;

        let mut editor = Editor::default();
        let document_id = editor.open_document("worlds/city.cyworld").unwrap();
        let unrelated_node = editor
            .documents
            .get_mut(document_id)
            .unwrap()
            .with_transaction("Assign material", Actor::human("designer"), |document| {
                let node = create_mesh_instance(
                    document,
                    None,
                    "meshes/box.cyprim",
                    Transform3::default(),
                )?;
                let binding = MaterialBinding::of_schema(document.schema()).unwrap();
                document.record(Operation::SetAssetReference {
                    node,
                    component: binding.component,
                    field: binding.material,
                    before: String::new(),
                    after: "materials/sway.cygraph".into(),
                })?;
                let slotted = create_mesh_instance(
                    document,
                    None,
                    "meshes/other.cyprim",
                    Transform3::default(),
                )?;
                set_material_slots(
                    document,
                    slotted,
                    &[
                        "materials/other.cygraph".into(),
                        "materials/secondary.cygraph".into(),
                    ],
                )?;
                Ok(slotted)
            })
            .unwrap();
        let mut selection = Selection::new();
        selection.set_nodes([unrelated_node]);
        editor.selection.set(selection);

        assert_eq!(
            editor.assigned_material_geometry(Some("materials/sway.cygraph")),
            ["StaticMesh"]
        );
        assert_eq!(
            editor.assigned_material_geometry(Some("materials/secondary.cygraph")),
            ["StaticMesh"]
        );
        assert_eq!(
            editor.assigned_material_geometry(Some("materials/missing.cygraph")),
            Vec::<&str>::new()
        );
        assert!(editor.assigned_material_geometry(None).is_empty());
    }

    #[test]
    fn visible_cards_follow_engine_type_and_pin_identities() {
        let mut pin = Pin::new("out", PinDirection::Output, "value");
        pin.identity = 9;
        let catalogue = Catalogue::new(vec![NodeType::identified(
            42,
            3,
            "material.future".into(),
            vec![pin],
        )])
        .unwrap();
        let mut canvas = GraphCanvas::new(1);
        canvas.load(catalogue);
        canvas
            .add("material.future", GraphLayout { x: 10.0, y: 20.0 })
            .unwrap();

        let cards = node_cards(&canvas, egui::Pos2::ZERO);
        assert_eq!(cards.len(), 1);
        assert_eq!(cards[0].type_identity, 42);
        assert_eq!(cards[0].outputs[0].identity, 9);
        assert_eq!(cards[0].key.ordinal(), 1);
    }

    #[test]
    fn engine_names_become_labels_without_becoming_identities() {
        assert_eq!(display_name("material.texture_sample"), "Texture Sample");
        assert_eq!(display_name("plugin.rainbow"), "Plugin.rainbow");
    }

    #[test]
    fn display_labels_with_spaces_are_searchable() {
        assert!(matches_filter("material.add_closures", "add closures"));
    }

    #[test]
    fn material_palette_uses_engine_stage_compatibility() {
        let catalogue = Catalogue::new(vec![
            NodeType::identified(1, 1, "material.output".into(), vec![])
                .with_stage_mask(SURFACE_STAGE),
            NodeType::identified(2, 1, "material.offset".into(), vec![])
                .with_stage_mask(VERTEX_STAGE),
            NodeType::identified(4, 1, "material.vertex_interpolant".into(), vec![])
                .with_stage_mask(VERTEX_STAGE),
            NodeType::identified(3, 1, "material.sin".into(), vec![])
                .with_stage_mask(SURFACE_STAGE | VERTEX_STAGE),
        ])
        .unwrap();
        let mut canvas = GraphCanvas::new(1);
        canvas.load(catalogue);

        assert_eq!(
            palette_names(&canvas, "", SURFACE_STAGE),
            ["material.output", "material.sin"]
        );
        assert_eq!(
            palette_names(&canvas, "", VERTEX_STAGE),
            [
                "material.offset",
                "material.sin",
                "material.vertex_interpolant"
            ]
        );
    }

    #[test]
    fn pin_actions_create_one_stable_identity_link() {
        let (mut canvas, source, sink) = connection_canvas("value");
        let mut pending = None;
        let mut problem = None;

        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
            None,
            PinAction {
                node: source,
                pin: identified_pin(41, "out", PinDirection::Output, "value"),
            },
        );
        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
            None,
            PinAction {
                node: sink,
                pin: identified_pin(73, "in", PinDirection::Input, "value"),
            },
        );

        assert!(problem.is_none());
        assert!(pending.is_none());
        let link = canvas.links().next().expect("one visible link");
        assert_eq!((link.from_pin_identity, link.to_pin_identity), (41, 73));
    }

    #[test]
    fn pin_action_can_route_a_connection_without_mutating_the_canvas() {
        let (mut canvas, source, sink) = connection_canvas("value");
        let mut pending = None;
        let mut problem = None;
        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
            None,
            PinAction {
                node: source,
                pin: identified_pin(41, "out", PinDirection::Output, "value"),
            },
        );
        let mut routed = None;
        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
            Some(&mut |_, connection| {
                routed = Some((connection.from, connection.from_name.clone(), connection.to));
                Ok(())
            }),
            PinAction {
                node: sink,
                pin: identified_pin(73, "in", PinDirection::Input, "value"),
            },
        );
        assert_eq!(routed, Some((source, "out".into(), sink)));
        assert_eq!(canvas.links().count(), 0);
        assert!(problem.is_none());
    }

    #[test]
    fn incompatible_pin_action_is_visible_and_non_mutating() {
        let (mut canvas, source, sink) = connection_canvas("colour");
        let mut pending = None;
        let mut problem = None;
        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
            None,
            PinAction {
                node: source,
                pin: identified_pin(41, "out", PinDirection::Output, "value"),
            },
        );
        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
            None,
            PinAction {
                node: sink,
                pin: identified_pin(73, "in", PinDirection::Input, "colour"),
            },
        );

        assert_eq!(canvas.links().count(), 0);
        assert!(
            problem
                .as_deref()
                .is_some_and(|message| { message.contains("value") && message.contains("colour") }),
            "the visible refusal should name both pin types: {problem:?}"
        );
    }

    #[test]
    fn a_backend_diagnostic_location_selects_its_stable_node() {
        let (mut canvas, _source, sink) = connection_canvas("value");

        select_backend_location(&mut canvas, sink.ordinal());

        assert_eq!(canvas.selection(), vec![sink]);
        assert_eq!(
            canvas.nodes().count(),
            2,
            "navigation must not mutate the graph"
        );
    }

    fn connection_canvas(target_type: &str) -> (GraphCanvas, NodeKey, NodeKey) {
        let catalogue = Catalogue::new(vec![
            NodeType::identified(
                7,
                1,
                "material.source".into(),
                vec![identified_pin(41, "out", PinDirection::Output, "value")],
            ),
            NodeType::identified(
                8,
                1,
                "material.sink".into(),
                vec![identified_pin(73, "in", PinDirection::Input, target_type)],
            ),
        ])
        .expect("two node types");
        let mut canvas = GraphCanvas::new(1);
        canvas.load(catalogue);
        let source = canvas
            .add("material.source", GraphLayout::default())
            .expect("source");
        let sink = canvas
            .add("material.sink", GraphLayout::default())
            .expect("sink");
        (canvas, source, sink)
    }

    fn identified_pin(identity: u32, name: &str, direction: PinDirection, data_type: &str) -> Pin {
        let mut pin = Pin::new(name, direction, data_type);
        pin.identity = identity;
        pin
    }
}
