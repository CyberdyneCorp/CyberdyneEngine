// SPDX-License-Identifier: MIT
//! The visible material graph: engine palette at the left, the shared graph canvas at the right.
//!
//! This module is a renderer and interaction adapter only. Node identity, catalogue membership,
//! selection, layout, links, type checking, and diagnostics remain in `GraphCanvas`; material code
//! contributes no second graph model.

use std::collections::BTreeMap;

use cy_editor_interface::Domain;
use cy_editor_interface::specialised::graph::{
    GraphCanvas, Layout as GraphLayout, NodeKey, Pin, PinDirection, Property, PropertyKind,
    Severity,
};
use cy_editor_interface::specialised::material::{canvas_interchange, load_canvas_interchange};
use cy_editor_services::primitives::material_of;
use cy_editor_services::{
    AssetCatalogueService, Editor, MaterialCatalogueState, MaterialDiagnosticSeverity,
    MaterialOperation, MaterialPreviewState, MaterialRequestState,
};
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::TextRole;

use super::{Panels, nothing_here, secondary, status};
use crate::theme;

const PALETTE_WIDTH: f32 = 220.0;
const NODE_WIDTH: f32 = 190.0;
const NODE_HEADER: f32 = 48.0;
const PIN_ROW: f32 = 18.0;

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let selected_graph = selected_material_graph(panels.editor);
    let project_root = panels.editor.project.root().to_path_buf();
    let state = panels.editor.backend.material_catalogue_state();
    let request_state = panels.editor.backend.material_request_state().clone();
    let preview_state = panels.editor.backend.material_preview_state().clone();
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
    if let Some(reference) = &selected_graph {
        ui.horizontal(|ui| {
            ui.label(format!("Selected material: {reference}"));
            if ui.button("Open graph").clicked() {
                match open_selected_graph(&project_root, reference, canvas) {
                    Ok(name) => {
                        panels.inputs.material_name = name;
                        panels.inputs.material_open_reference = Some(reference.clone());
                        panels.inputs.material_preview_source = None;
                        panels.inputs.material_property_problem = None;
                    }
                    Err(problem) => panels.inputs.material_property_problem = Some(problem),
                }
            }
        });
    }
    let available = ui.available_size();
    let mut action = None;
    ui.horizontal(|ui| {
        ui.allocate_ui_with_layout(
            egui::vec2(PALETTE_WIDTH.min(available.x * 0.38), available.y),
            egui::Layout::top_down(egui::Align::Min),
            |ui| {
                action = palette(
                    ui,
                    panels.shell,
                    canvas,
                    &mut panels.inputs.material_filter,
                    PaletteBackendState {
                        catalogue: state,
                        request: &request_state,
                        preview: &preview_state,
                    },
                    &panels.editor.asset_catalogue,
                    &mut panels.inputs.material_property_problem,
                );
            },
        );
        ui.separator();
        ui.allocate_ui(egui::vec2(ui.available_width(), available.y), |ui| {
            draw_canvas(
                ui,
                panels.shell,
                canvas,
                state,
                &mut panels.inputs.material_link_source,
                &mut panels.inputs.material_link_problem,
            );
        });
    });
    match action {
        Some(PaletteAction::Save) => {
            let reference = panels
                .inputs
                .material_open_reference
                .clone()
                .unwrap_or_else(|| format!("materials/{}.cygraph", panels.inputs.material_name));
            match canvas_interchange(&panels.inputs.material_name, canvas).and_then(|source| {
                let request = panels
                    .editor
                    .request_material(MaterialOperation::Author, source.as_bytes().to_vec())?;
                panels.inputs.material_save = Some((request.as_u64(), reference, source));
                Ok(())
            }) {
                Ok(()) => {}
                Err(problem) => {
                    panels
                        .editor
                        .notifications
                        .post(cy_editor_services::Notification::error(
                            "Material save failed",
                            problem,
                        ))
                }
            }
        }
        Some(PaletteAction::Request(operation)) => {
            match canvas_interchange(&panels.inputs.material_name, canvas).and_then(|payload| {
                panels
                    .editor
                    .request_material(operation, payload.into_bytes())
            }) {
                Ok(_) => {}
                Err(problem) => {
                    panels
                        .editor
                        .notifications
                        .post(cy_editor_services::Notification::error(
                            "The material request could not be submitted",
                            problem,
                        ));
                }
            }
        }
        Some(PaletteAction::Cancel) => {
            if let Err(problem) = panels.editor.cancel_material_request() {
                panels
                    .editor
                    .notifications
                    .post(cy_editor_services::Notification::error(
                        "The material request could not be cancelled",
                        problem,
                    ));
            }
        }
        None => {}
    }
    if action.is_none()
        && state == MaterialCatalogueState::Ready
        && !matches!(request_state, MaterialRequestState::Pending { .. })
    {
        preview_changed_graph(panels.editor, panels.inputs, canvas);
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

pub(crate) fn finish_save(editor: &mut Editor, inputs: &mut super::Inputs) {
    let Some((request, _, _)) = inputs.material_save.as_ref() else {
        return;
    };
    let result = match editor.backend.material_request_state() {
        MaterialRequestState::Authored {
            request: done,
            graph,
        } if done.as_u64() == *request => Some(Ok(graph.clone())),
        MaterialRequestState::Failed {
            request: Some(done),
            ..
        } if done.as_u64() == *request => Some(Err(
            "the engine rejected this graph; see Problems".to_owned()
        )),
        MaterialRequestState::Cancelled { request: done } if done.as_u64() == *request => {
            Some(Err("the graph save was cancelled".to_owned()))
        }
        _ => None,
    };
    let Some(result) = result else { return };
    let (_, reference, source) = inputs.material_save.take().expect("pending save");
    let previous = std::fs::read_to_string(
        editor
            .project
            .root()
            .join(std::path::Path::new(&reference).with_extension("cymatcanvas")),
    )
    .ok();
    let outcome =
        result.and_then(|graph| save_graph(editor.project.root(), &reference, &source, &graph));
    match outcome {
        Ok(()) => {
            inputs.material_open_reference = Some(reference.clone());
            editor
                .notifications
                .post(cy_editor_services::Notification::info(format!(
                    "Saved {reference}"
                )));
            if let Err(message) =
                super::material_parameters::sync(editor, &reference, previous.as_deref())
            {
                editor
                    .notifications
                    .post(cy_editor_services::Notification::error(
                        "Material saved; scene properties could not be synced",
                        cy_editor_core::problem::Problem::new("sync graph properties", message),
                    ));
            }
        }
        Err(message) => editor
            .notifications
            .post(cy_editor_services::Notification::error(
                "Material save failed",
                cy_editor_core::problem::Problem::new("save material graph", message),
            )),
    }
}

fn save_graph(
    root: &std::path::Path,
    reference: &str,
    source: &str,
    graph: &str,
) -> Result<(), String> {
    let path = std::path::Path::new(reference);
    if path
        .extension()
        .is_none_or(|extension| extension != "cygraph")
        || !path
            .components()
            .all(|part| matches!(part, std::path::Component::Normal(_)))
    {
        return Err("material graph path must be a project-relative .cygraph".into());
    }
    let graph_path = root.join(path);
    let source_path = graph_path.with_extension("cymatcanvas");
    let parent = graph_path
        .parent()
        .ok_or("material graph has no directory")?;
    std::fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    let graph_stage = graph_path.with_extension("cygraph.tmp");
    let source_stage = source_path.with_extension("cymatcanvas.tmp");
    std::fs::write(&graph_stage, graph).map_err(|error| error.to_string())?;
    std::fs::write(&source_stage, source).map_err(|error| error.to_string())?;
    let previous_source = std::fs::read(&source_path).ok();
    std::fs::rename(&source_stage, &source_path).map_err(|error| error.to_string())?;
    if let Err(error) = std::fs::rename(&graph_stage, &graph_path) {
        match previous_source {
            Some(bytes) => {
                let _ = std::fs::write(&source_path, bytes);
            }
            None => {
                let _ = std::fs::remove_file(&source_path);
            }
        }
        return Err(error.to_string());
    }
    Ok(())
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
    filter: &mut String,
    backend: PaletteBackendState<'_>,
    assets: &AssetCatalogueService,
    property_problem: &mut Option<String>,
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
    material_properties(ui, canvas, assets, property_problem);
    ui.add_space(shell.metrics().gap() * 0.5);
    ui.add(
        egui::TextEdit::singleline(filter)
            .hint_text("Search nodes")
            .desired_width(f32::INFINITY),
    );
    ui.add_space(shell.metrics().gap() * 0.5);

    let query = filter.trim().to_ascii_lowercase();
    let names: Vec<String> = canvas
        .catalogue()
        .type_names()
        .into_iter()
        .filter(|name| matches_filter(name, &query))
        .map(ToOwned::to_owned)
        .collect();
    egui::ScrollArea::vertical().show(ui, |ui| {
        for name in names {
            let Some(node_type) = canvas.catalogue().get(&name) else {
                continue;
            };
            let identity = node_type.identity;
            let label = display_name(&name);
            let response = ui
                .button(format!("＋ {label}"))
                .on_hover_text(format!("{name} · NodeTypeId {identity}"));
            if response.clicked() {
                let index = canvas.nodes().count();
                let column = index % 3;
                let row = index / 3;
                let _ = canvas.add(
                    &name,
                    GraphLayout {
                        x: 28.0 + display_index(column) * (NODE_WIDTH + 34.0),
                        y: 34.0 + display_index(row) * 150.0,
                    },
                );
            }
        }
    });

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

#[derive(Clone, Copy)]
struct PaletteBackendState<'a> {
    catalogue: MaterialCatalogueState,
    request: &'a MaterialRequestState,
    preview: &'a MaterialPreviewState,
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

fn material_properties(
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    assets: &AssetCatalogueService,
    problem: &mut Option<String>,
) {
    let Some(key) = canvas.selection().first().copied() else {
        return;
    };
    let Some(node) = canvas.node(key) else {
        return;
    };
    let type_name = node.type_name.clone();
    let properties = canvas
        .catalogue()
        .get(&type_name)
        .map_or_else(Vec::new, |node_type| node_type.properties.clone());
    if properties.is_empty() {
        return;
    }
    ui.separator();
    ui.strong("Properties");
    for property in properties {
        let mut value = canvas
            .property_value(key, &property)
            .map_or_else(|| property.default.clone(), ToOwned::to_owned);
        let changed = property_control(ui, &property, &mut value, assets);
        if changed {
            match canvas.set_property_by_identity(key, property.identity, value) {
                Ok(()) => *problem = None,
                Err(refused) => *problem = Some(refused.to_string()),
            }
        }
    }
    if let Some(problem) = problem {
        ui.colored_label(egui::Color32::from_rgb(232, 96, 96), problem.as_str());
    }
}

fn property_control(
    ui: &mut egui::Ui,
    property: &Property,
    value: &mut String,
    assets: &AssetCatalogueService,
) -> bool {
    let mut changed = false;
    ui.push_id(property.identity, |ui| {
        ui.label(display_name(&property.name))
            .on_hover_text(&property.tooltip);
        match property.kind {
            PropertyKind::Bool => {
                let mut checked = value == "true" || value == "1";
                if ui.checkbox(&mut checked, "").changed() {
                    *value = checked.to_string();
                    changed = true;
                }
            }
            PropertyKind::Enumeration => {
                egui::ComboBox::from_id_salt("value")
                    .selected_text(value.as_str())
                    .show_ui(ui, |ui| {
                        for choice in &property.choices {
                            changed |= ui.selectable_value(value, choice.clone(), choice).changed();
                        }
                    });
            }
            PropertyKind::Asset => {
                let label = if value.is_empty() {
                    "None"
                } else {
                    value.as_str()
                };
                egui::ComboBox::from_id_salt("asset")
                    .selected_text(label)
                    .show_ui(ui, |ui| {
                        changed |= ui.selectable_value(value, String::new(), "None").changed();
                        for asset in assets.entries().iter().filter(|asset| {
                            asset.kind == property.asset_kind && asset.identity.is_some()
                        }) {
                            let identity = asset.identity.clone().expect("filtered above");
                            changed |= ui
                                .selectable_value(value, identity, asset.path.as_str())
                                .changed();
                        }
                    });
            }
            PropertyKind::Scalar => {
                let mut number = value.parse::<f64>().unwrap_or_default();
                let mut control =
                    egui::DragValue::new(&mut number).speed(property.step.unwrap_or(0.01));
                if property.minimum.is_some() || property.maximum.is_some() {
                    control = control.range(
                        property.minimum.unwrap_or(f64::NEG_INFINITY)
                            ..=property.maximum.unwrap_or(f64::INFINITY),
                    );
                }
                if ui
                    .add(control)
                    .on_hover_text(property_metadata(property))
                    .changed()
                {
                    *value = number.to_string();
                    changed = true;
                }
            }
            PropertyKind::Text | PropertyKind::Vector => {
                changed = ui
                    .add(egui::TextEdit::singleline(value).desired_width(f32::INFINITY))
                    .on_hover_text(property_metadata(property))
                    .changed();
            }
        }
    });
    changed
}

fn property_metadata(property: &Property) -> String {
    let mut metadata = vec![format!("{} · {}", property.domain, property.stage)];
    if !property.semantic.is_empty() {
        metadata.push(property.semantic.clone());
    }
    if property.required_capabilities != 0 {
        metadata.push(format!(
            "capabilities 0x{:x}",
            property.required_capabilities
        ));
    }
    if property.minimum.is_some() || property.maximum.is_some() || property.step.is_some() {
        metadata.push(format!(
            "range {:?}…{:?}, step {:?}",
            property.minimum, property.maximum, property.step
        ));
    }
    metadata.join("\n")
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
        } => (
            Semantic::Live,
            format!(
                "Compiled {artefact:016x}\ngraph {graph:016x} · {programs} programs · {} dependencies\nrequest #{}",
                dependencies.len(),
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

fn draw_canvas(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    canvas: &mut GraphCanvas,
    state: MaterialCatalogueState,
    pending_source: &mut Option<(u64, u32, String, String)>,
    link_problem: &mut Option<String>,
) {
    let rect = ui.available_rect_before_wrap();
    let background = ui.allocate_rect(rect, egui::Sense::click());
    let painter = ui.painter_at(rect);
    painter.rect_filled(
        rect,
        egui::CornerRadius::same(2),
        theme::surface(shell.theme, Surface::Sunken),
    );
    draw_grid(&painter, rect, theme::role(shell.theme, Semantic::Neutral));

    let cards = node_cards(canvas, rect.min);
    let pins = pin_positions(&cards);
    draw_links(
        &painter,
        canvas,
        &pins,
        theme::role(shell.theme, Semantic::Active),
    );
    let selected = canvas.selection();
    let mut select = None;
    let mut movement = None;
    for card in &cards {
        let header = egui::Rect::from_min_max(
            card.rect.min,
            egui::pos2(card.rect.right(), card.rect.top() + NODE_HEADER),
        );
        let response = ui.interact(
            header,
            egui::Id::new(("material-node", card.key.ordinal())),
            egui::Sense::click_and_drag(),
        );
        if response.clicked() {
            select = Some(card.key);
        }
        if response.dragged() {
            movement = Some((
                card.key,
                GraphLayout {
                    x: card.layout.x + response.drag_delta().x,
                    y: card.layout.y + response.drag_delta().y,
                },
            ));
        }
        draw_node(
            &painter,
            shell,
            card,
            selected.contains(&card.key),
            response.hovered(),
        );
    }
    let pin_action = interact_with_pins(ui, shell, &cards, pending_source.as_ref());
    if let Some(pin) = pin_action {
        apply_pin_action(canvas, pending_source, link_problem, pin);
    } else if background.clicked() {
        *pending_source = None;
    }
    if ui.input(|input| input.key_pressed(egui::Key::Escape)) {
        *pending_source = None;
    }
    draw_pending_link(ui, &painter, &pins, pending_source.as_ref(), shell);
    if let Some(key) = select {
        let _ = canvas.select([key]);
    }
    if let Some((key, layout)) = movement {
        let _ = canvas.move_to(key, layout);
    }

    if cards.is_empty() {
        painter.text(
            rect.center(),
            egui::Align2::CENTER_CENTER,
            "Empty material graph\nChoose a node from the engine catalogue",
            egui::FontId::proportional(shell.metrics().text(TextRole::Body)),
            theme::role(shell.theme, Semantic::SecondaryText),
        );
    }
    let service = match state {
        MaterialCatalogueState::Ready => "ENGINE CATALOGUE · LIVE",
        _ => "ENGINE CATALOGUE · OFFLINE SNAPSHOT",
    };
    painter.text(
        rect.left_top() + egui::vec2(12.0, 10.0),
        egui::Align2::LEFT_TOP,
        service,
        egui::FontId::monospace(shell.metrics().text(TextRole::Secondary)),
        theme::role(
            shell.theme,
            if state == MaterialCatalogueState::Ready {
                Semantic::Live
            } else {
                Semantic::Warning
            },
        ),
    );
    if let Some(key) = draw_diagnostics(ui, &painter, shell, canvas, rect, link_problem.as_deref())
    {
        let _ = canvas.select([key]);
    }
}

#[derive(Clone)]
struct NodeCard {
    key: NodeKey,
    type_name: String,
    type_identity: u32,
    layout: GraphLayout,
    rect: egui::Rect,
    inputs: Vec<Pin>,
    outputs: Vec<Pin>,
}

#[derive(Clone)]
struct PinAction {
    node: NodeKey,
    pin: Pin,
}

fn node_cards(canvas: &GraphCanvas, origin: egui::Pos2) -> Vec<NodeCard> {
    canvas
        .nodes()
        .filter_map(|node| {
            let node_type = canvas.catalogue().get(&node.type_name)?;
            let layout = canvas.layout_of(node.key).unwrap_or_default();
            let inputs: Vec<Pin> = node_type
                .pins
                .iter()
                .filter(|pin| pin.direction == PinDirection::Input)
                .cloned()
                .collect();
            let outputs: Vec<Pin> = node_type
                .pins
                .iter()
                .filter(|pin| pin.direction == PinDirection::Output)
                .cloned()
                .collect();
            let rows = inputs.len().max(outputs.len()).max(1);
            let size = egui::vec2(
                NODE_WIDTH,
                NODE_HEADER + display_index(rows) * PIN_ROW + 10.0,
            );
            let rect = egui::Rect::from_min_size(origin + egui::vec2(layout.x, layout.y), size);
            Some(NodeCard {
                key: node.key,
                type_name: node.type_name.clone(),
                type_identity: node_type.identity,
                layout,
                rect,
                inputs,
                outputs,
            })
        })
        .collect()
}

type PinPositionKey = (NodeKey, u32, String, PinDirection);

fn pin_positions(cards: &[NodeCard]) -> BTreeMap<PinPositionKey, egui::Pos2> {
    let mut positions = BTreeMap::new();
    for card in cards {
        for (index, pin) in card.inputs.iter().enumerate() {
            positions.insert(
                (
                    card.key,
                    pin.identity,
                    pin.name.clone(),
                    PinDirection::Input,
                ),
                pin_position(card, index, PinDirection::Input),
            );
        }
        for (index, pin) in card.outputs.iter().enumerate() {
            positions.insert(
                (
                    card.key,
                    pin.identity,
                    pin.name.clone(),
                    PinDirection::Output,
                ),
                pin_position(card, index, PinDirection::Output),
            );
        }
    }
    positions
}

fn draw_links(
    painter: &egui::Painter,
    canvas: &GraphCanvas,
    pins: &BTreeMap<PinPositionKey, egui::Pos2>,
    colour: egui::Color32,
) {
    for link in canvas.links() {
        let Some(from) = pins.get(&(
            link.from,
            link.from_pin_identity,
            link.from_pin.clone(),
            PinDirection::Output,
        )) else {
            continue;
        };
        let Some(to) = pins.get(&(
            link.to,
            link.to_pin_identity,
            link.to_pin.clone(),
            PinDirection::Input,
        )) else {
            continue;
        };
        let middle = (from.x + to.x) * 0.5;
        painter.line(
            vec![
                *from,
                egui::pos2(middle, from.y),
                egui::pos2(middle, to.y),
                *to,
            ],
            egui::Stroke::new(2.0, colour),
        );
    }
}

fn interact_with_pins(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    cards: &[NodeCard],
    pending: Option<&(u64, u32, String, String)>,
) -> Option<PinAction> {
    let mut action = None;
    for card in cards {
        for (index, pin) in card.inputs.iter().enumerate() {
            if interact_with_pin(ui, shell, card, pin, index, pending) {
                action = Some(PinAction {
                    node: card.key,
                    pin: pin.clone(),
                });
            }
        }
        for (index, pin) in card.outputs.iter().enumerate() {
            if interact_with_pin(ui, shell, card, pin, index, pending) {
                action = Some(PinAction {
                    node: card.key,
                    pin: pin.clone(),
                });
            }
        }
    }
    action
}

fn interact_with_pin(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    card: &NodeCard,
    pin: &Pin,
    index: usize,
    pending: Option<&(u64, u32, String, String)>,
) -> bool {
    let position = pin_position(card, index, pin.direction);
    let hit = egui::Rect::from_center_size(position, egui::vec2(18.0, 18.0));
    let response = ui
        .interact(
            hit,
            egui::Id::new((
                "material-pin",
                card.key.ordinal(),
                pin.identity,
                pin.direction,
            )),
            egui::Sense::click(),
        )
        .on_hover_text(format!(
            "{} · {} · PinId {}",
            pin.name, pin.data_type, pin.identity
        ));
    let selected =
        pending.is_some_and(|source| source.0 == card.key.ordinal() && source.1 == pin.identity);
    let compatible = pending
        .is_some_and(|source| pin.direction == PinDirection::Input && source.3 == pin.data_type);
    if response.hovered() || selected || compatible {
        let role = if selected || compatible {
            Semantic::Live
        } else {
            Semantic::Active
        };
        ui.painter().circle_stroke(
            position,
            7.0,
            egui::Stroke::new(2.0, theme::role(shell.theme, role)),
        );
    }
    response.clicked()
}

fn apply_pin_action(
    canvas: &mut GraphCanvas,
    pending: &mut Option<(u64, u32, String, String)>,
    problem: &mut Option<String>,
    action: PinAction,
) {
    match action.pin.direction {
        PinDirection::Output => {
            let next = (
                action.node.ordinal(),
                action.pin.identity,
                action.pin.name,
                action.pin.data_type,
            );
            if pending
                .as_ref()
                .is_some_and(|source| source.0 == next.0 && source.1 == next.1)
            {
                *pending = None;
            } else {
                *pending = Some(next);
            }
            *problem = None;
        }
        PinDirection::Input => {
            let Some(source) = pending.take() else {
                *problem = Some(format!(
                    "{} [{}] is an input; choose an output pin first",
                    action.pin.name, action.pin.identity
                ));
                return;
            };
            let from = NodeKey::new(source.0).expect("a pending graph node is never zero");
            match canvas.connect_identified(from, source.1, action.node, action.pin.identity) {
                Ok(()) => *problem = None,
                Err(refused) => *problem = Some(refused.to_string()),
            }
        }
    }
}

fn draw_pending_link(
    ui: &egui::Ui,
    painter: &egui::Painter,
    pins: &BTreeMap<PinPositionKey, egui::Pos2>,
    pending: Option<&(u64, u32, String, String)>,
    shell: &cy_editor_interface::shell::Shell,
) {
    let Some(source) = pending else {
        return;
    };
    let Ok(node) = NodeKey::new(source.0) else {
        return;
    };
    let Some(from) = pins.get(&(node, source.1, source.2.clone(), PinDirection::Output)) else {
        return;
    };
    let Some(cursor) = ui.input(|input| input.pointer.hover_pos()) else {
        return;
    };
    let middle = (from.x + cursor.x) * 0.5;
    painter.line(
        vec![
            *from,
            egui::pos2(middle, from.y),
            egui::pos2(middle, cursor.y),
            cursor,
        ],
        egui::Stroke::new(2.0, theme::role(shell.theme, Semantic::Live)),
    );
}

fn draw_diagnostics(
    ui: &mut egui::Ui,
    painter: &egui::Painter,
    shell: &cy_editor_interface::shell::Shell,
    canvas: &GraphCanvas,
    canvas_rect: egui::Rect,
    gesture_problem: Option<&str>,
) -> Option<NodeKey> {
    let diagnostics = canvas.diagnostics();
    let shown = diagnostics
        .len()
        .min(if gesture_problem.is_some() { 2 } else { 3 });
    let rows = shown + usize::from(gesture_problem.is_some());
    if rows == 0 {
        return None;
    }
    let width = canvas_rect.width().min(520.0);
    let row_height = 34.0;
    let mut y = canvas_rect.bottom() - display_index(rows) * (row_height + 4.0) - 8.0;
    if let Some(message) = gesture_problem {
        diagnostic_row(
            ui,
            painter,
            shell,
            egui::Rect::from_min_size(
                egui::pos2(canvas_rect.left() + 8.0, y),
                egui::vec2(width - 16.0, row_height),
            ),
            Semantic::Error,
            "Connection refused",
            message,
            None,
        );
        y += row_height + 4.0;
    }
    let mut selected = None;
    for diagnostic in diagnostics.iter().take(shown) {
        let pin = diagnostic.pin.as_deref().map_or_else(
            || format!("node {}", diagnostic.node.ordinal()),
            |name| {
                let identity = diagnostic_pin_identity(canvas, diagnostic.node, name);
                format!(
                    "node {} · pin {} [{}]",
                    diagnostic.node.ordinal(),
                    name,
                    identity
                )
            },
        );
        let role = match diagnostic.severity {
            Severity::Info => Semantic::Active,
            Severity::Warning => Semantic::Warning,
            Severity::Error => Semantic::Error,
        };
        let clicked = diagnostic_row(
            ui,
            painter,
            shell,
            egui::Rect::from_min_size(
                egui::pos2(canvas_rect.left() + 8.0, y),
                egui::vec2(width - 16.0, row_height),
            ),
            role,
            &pin,
            &diagnostic.message,
            Some(diagnostic.node),
        );
        if clicked {
            selected = Some(diagnostic.node);
        }
        y += row_height + 4.0;
    }
    selected
}

#[allow(clippy::too_many_arguments)]
fn diagnostic_row(
    ui: &mut egui::Ui,
    painter: &egui::Painter,
    shell: &cy_editor_interface::shell::Shell,
    rect: egui::Rect,
    role: Semantic,
    location: &str,
    message: &str,
    node: Option<NodeKey>,
) -> bool {
    painter.rect_filled(
        rect,
        egui::CornerRadius::same(3),
        theme::surface(shell.theme, Surface::Raised),
    );
    painter.rect_filled(
        egui::Rect::from_min_max(rect.min, egui::pos2(rect.left() + 3.0, rect.bottom())),
        egui::CornerRadius::same(2),
        theme::role(shell.theme, role),
    );
    painter.text(
        rect.left_center() + egui::vec2(10.0, -7.0),
        egui::Align2::LEFT_CENTER,
        location,
        egui::FontId::monospace(shell.metrics().text(TextRole::Secondary)),
        theme::role(shell.theme, role),
    );
    painter.text(
        rect.left_center() + egui::vec2(10.0, 8.0),
        egui::Align2::LEFT_CENTER,
        shorten(message, 72),
        egui::FontId::proportional(shell.metrics().text(TextRole::Secondary)),
        theme::role(shell.theme, Semantic::SecondaryText),
    );
    node.is_some_and(|key| {
        ui.interact(
            rect,
            egui::Id::new(("material-diagnostic", key.ordinal(), location)),
            egui::Sense::click(),
        )
        .on_hover_text("Select the responsible node")
        .clicked()
    })
}

fn diagnostic_pin_identity(canvas: &GraphCanvas, node: NodeKey, pin_name: &str) -> u32 {
    canvas
        .node(node)
        .and_then(|node| canvas.catalogue().get(&node.type_name))
        .and_then(|node_type| node_type.pins.iter().find(|pin| pin.name == pin_name))
        .map_or(0, |pin| pin.identity)
}

fn shorten(message: &str, limit: usize) -> String {
    let mut characters = message.chars();
    let shortened: String = characters.by_ref().take(limit).collect();
    if characters.next().is_some() {
        shortened + "…"
    } else {
        shortened
    }
}

fn draw_node(
    painter: &egui::Painter,
    shell: &cy_editor_interface::shell::Shell,
    card: &NodeCard,
    selected: bool,
    hovered: bool,
) {
    let fill = if selected {
        theme::selected_fill(shell.theme)
    } else if hovered {
        theme::lifted(shell.theme, Surface::Raised, 0.08)
    } else {
        theme::surface(shell.theme, Surface::Raised)
    };
    painter.rect_filled(card.rect, egui::CornerRadius::same(5), fill);
    painter.rect_filled(
        egui::Rect::from_min_max(
            card.rect.min,
            egui::pos2(card.rect.right(), card.rect.top() + 4.0),
        ),
        egui::CornerRadius::same(3),
        theme::role(
            shell.theme,
            if selected {
                Semantic::Selection
            } else {
                Semantic::Active
            },
        ),
    );
    painter.text(
        card.rect.left_top() + egui::vec2(10.0, 11.0),
        egui::Align2::LEFT_TOP,
        display_name(&card.type_name),
        egui::FontId::proportional(shell.metrics().text(TextRole::Body)),
        theme::role(shell.theme, Semantic::PrimaryText),
    );
    painter.text(
        card.rect.left_top() + egui::vec2(10.0, 29.0),
        egui::Align2::LEFT_TOP,
        format!("type {} · node {}", card.type_identity, card.key.ordinal()),
        egui::FontId::monospace(shell.metrics().text(TextRole::Secondary)),
        theme::role(shell.theme, Semantic::SecondaryText),
    );
    for (index, pin) in card.inputs.iter().enumerate() {
        draw_pin(painter, shell, card, pin, index, false);
    }
    for (index, pin) in card.outputs.iter().enumerate() {
        draw_pin(painter, shell, card, pin, index, true);
    }
}

fn draw_pin(
    painter: &egui::Painter,
    shell: &cy_editor_interface::shell::Shell,
    card: &NodeCard,
    pin: &Pin,
    index: usize,
    output: bool,
) {
    let direction = if output {
        PinDirection::Output
    } else {
        PinDirection::Input
    };
    let position = pin_position(card, index, direction);
    let x = position.x;
    let y = position.y;
    painter.circle_filled(
        egui::pos2(x, y),
        4.0,
        theme::role(shell.theme, Semantic::Active),
    );
    painter.text(
        egui::pos2(if output { x - 8.0 } else { x + 8.0 }, y),
        if output {
            egui::Align2::RIGHT_CENTER
        } else {
            egui::Align2::LEFT_CENTER
        },
        format!("{}  [{}]", pin.name, pin.identity),
        egui::FontId::proportional(shell.metrics().text(TextRole::Secondary)),
        theme::role(shell.theme, Semantic::SecondaryText),
    );
}

fn pin_position(card: &NodeCard, index: usize, direction: PinDirection) -> egui::Pos2 {
    egui::pos2(
        if direction == PinDirection::Output {
            card.rect.right()
        } else {
            card.rect.left()
        },
        card.rect.top() + NODE_HEADER + (display_index(index) + 0.5) * PIN_ROW,
    )
}

fn draw_grid(painter: &egui::Painter, rect: egui::Rect, colour: egui::Color32) {
    let spacing = 24.0;
    let stroke = egui::Stroke::new(0.5, colour.gamma_multiply(0.45));
    let mut x = rect.left();
    while x < rect.right() {
        painter.line_segment(
            [egui::pos2(x, rect.top()), egui::pos2(x, rect.bottom())],
            stroke,
        );
        x += spacing;
    }
    let mut y = rect.top();
    while y < rect.bottom() {
        painter.line_segment(
            [egui::pos2(rect.left(), y), egui::pos2(rect.right(), y)],
            stroke,
        );
        y += spacing;
    }
}

fn display_name(type_name: &str) -> String {
    type_name
        .strip_prefix("material.")
        .unwrap_or(type_name)
        .split('_')
        .map(|part| {
            let mut chars = part.chars();
            chars.next().map_or_else(String::new, |first| {
                first.to_uppercase().collect::<String>() + chars.as_str()
            })
        })
        .collect::<Vec<_>>()
        .join(" ")
}

fn matches_filter(type_name: &str, query: &str) -> bool {
    query.is_empty()
        || type_name.to_ascii_lowercase().contains(query)
        || display_name(type_name).to_ascii_lowercase().contains(query)
}

fn display_index(index: usize) -> f32 {
    f32::from(u16::try_from(index).unwrap_or(u16::MAX))
}

#[cfg(test)]
mod tests {
    use cy_editor_interface::specialised::graph::{Catalogue, NodeType};

    use super::*;

    #[test]
    fn graph_save_writes_both_formats_and_rejects_paths_outside_project() {
        let root = std::env::temp_dir().join(format!("cy-material-save-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        save_graph(
            &root,
            "materials/paint.cygraph",
            "cymatcanvas 1\n",
            "cygraph 1\n",
        )
        .expect("save material assets");
        assert_eq!(
            std::fs::read_to_string(root.join("materials/paint.cygraph")).unwrap(),
            "cygraph 1\n"
        );
        assert_eq!(
            std::fs::read_to_string(root.join("materials/paint.cymatcanvas")).unwrap(),
            "cymatcanvas 1\n"
        );
        assert!(save_graph(&root, "../outside.cygraph", "", "").is_err());
        std::fs::remove_dir_all(root).unwrap();
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
    fn pin_actions_create_one_stable_identity_link() {
        let (mut canvas, source, sink) = connection_canvas("value");
        let mut pending = None;
        let mut problem = None;

        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
            PinAction {
                node: source,
                pin: identified_pin(41, "out", PinDirection::Output, "value"),
            },
        );
        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
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
    fn incompatible_pin_action_is_visible_and_non_mutating() {
        let (mut canvas, source, sink) = connection_canvas("colour");
        let mut pending = None;
        let mut problem = None;
        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
            PinAction {
                node: source,
                pin: identified_pin(41, "out", PinDirection::Output, "value"),
            },
        );
        apply_pin_action(
            &mut canvas,
            &mut pending,
            &mut problem,
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
