// SPDX-License-Identifier: MIT
//! The one node-graph canvas every graph panel draws, and the palette that feeds it.
//!
//! `editor-architecture` asks for one canvas shared by every node-graph editor, and
//! `cy_editor_interface::specialised` already enforces that for the *model*: every graph domain
//! borrows the same [`GraphCanvas`]. This module is the matching guarantee for the *view*. The
//! material graph and the VFX graph both draw through [`draw_canvas`] and [`node_palette`], and a
//! later graph editor (gameplay, abilities, animation blend graphs) hosts its engine-declared
//! vocabulary — [`cy_editor_interface::Domain::node_types`] or a backend catalogue — the same way,
//! with nothing domain-specific in here.
//!
//! What a host contributes is routing, not drawing: [`CanvasFeedback::on_connect`] and
//! [`CanvasFeedback::on_move`] turn a gesture into a registered command, which is what makes the
//! gesture one undoable transaction and gives it MCP parity. With no handler the canvas edits the
//! unsaved draft directly.

use std::collections::BTreeMap;

use cy_editor_interface::specialised::graph::{
    GraphCanvas, Layout as GraphLayout, NodeKey, NodeType, Pin, PinDirection, Property,
    PropertyKind, Severity,
};
use cy_editor_services::{AssetCatalogueService, MaterialCatalogueState};
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::TextRole;

use crate::theme;

/// Width of one node card, in points.
pub(super) const NODE_WIDTH: f32 = 190.0;
const NODE_HEADER: f32 = 48.0;
const PIN_ROW: f32 = 18.0;
/// Columns in the grid the palette places new nodes on.
const PALETTE_COLUMNS: usize = 3;

/// One row of a node palette: the engine's type name, the label a person reads, and its hover text.
#[derive(Clone, PartialEq, Debug)]
pub(super) struct PaletteEntry {
    /// The engine's type name, which is what an add command names.
    pub name: String,
    /// What the button says.
    pub label: String,
    /// What hovering the button explains.
    pub hover: String,
}

/// Draw a palette and answer the type name whose button was clicked this frame.
///
/// Presentation only: the host decides whether a click adds to the draft or invokes the saved
/// asset's add command, and where the node goes ([`palette_slot`]).
pub(super) fn node_palette(
    ui: &mut egui::Ui,
    entries: Vec<PaletteEntry>,
    enabled: bool,
) -> Option<String> {
    let mut chosen = None;
    egui::ScrollArea::vertical().show(ui, |ui| {
        for entry in entries {
            if ui
                .add_enabled(enabled, egui::Button::new(format!("＋ {}", entry.label)))
                .on_hover_text(&entry.hover)
                .clicked()
            {
                chosen = Some(entry.name);
            }
        }
    });
    chosen
}

/// The palette for a catalogue with no panel-specific presentation: every type whose name or
/// label contains `query`, labelled by [`palette_label`].
///
/// This is what a graph domain whose vocabulary is [`cy_editor_interface::Domain::node_types`]
/// gets without writing a palette of its own.
pub(super) fn catalogue_palette(canvas: &GraphCanvas, query: &str) -> Vec<PaletteEntry> {
    let query = query.trim().to_ascii_lowercase();
    canvas
        .catalogue()
        .type_names()
        .into_iter()
        .filter_map(|name| canvas.catalogue().get(name))
        .filter(|node_type| {
            let label = palette_label(&node_type.name).to_ascii_lowercase();
            query.is_empty()
                || node_type.name.to_ascii_lowercase().contains(&query)
                || label.contains(&query)
        })
        .map(|node_type: &NodeType| PaletteEntry {
            name: node_type.name.clone(),
            label: palette_label(&node_type.name),
            hover: format!("{} · NodeTypeId {}", node_type.name, node_type.identity),
        })
        .collect()
}

/// A readable label for any engine type name: the domain prefix dropped, words capitalised.
///
/// `script.emit_event` reads "Emit Event". The name itself stays the identity; this is only what a
/// button shows.
pub(super) fn palette_label(type_name: &str) -> String {
    let local = type_name
        .split_once('.')
        .map_or(type_name, |(_, rest)| rest);
    local
        .split('_')
        .filter(|part| !part.is_empty())
        .map(|part| {
            let mut chars = part.chars();
            chars.next().map_or_else(String::new, |first| {
                first.to_uppercase().collect::<String>() + chars.as_str()
            })
        })
        .collect::<Vec<_>>()
        .join(" ")
}

/// Where the palette places the `index`-th node: a three-column grid clear of the canvas header.
pub(super) fn palette_slot(index: usize) -> GraphLayout {
    let column = index % PALETTE_COLUMNS;
    let row = index / PALETTE_COLUMNS;
    GraphLayout {
        x: 28.0 + display_index(column) * (NODE_WIDTH + 34.0),
        y: 34.0 + display_index(row) * 150.0,
    }
}

/// The selected node's engine-declared properties as typed controls; `apply` routes each edit.
pub(super) fn graph_properties_with(
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    assets: &AssetCatalogueService,
    problem: &mut Option<String>,
    mut apply: impl FnMut(
        &mut GraphCanvas,
        NodeKey,
        &Property,
        String,
    ) -> cy_editor_core::problem::Result<()>,
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
            match apply(canvas, key, &property, value) {
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

type ConnectionHandler<'a> =
    dyn FnMut(&mut GraphCanvas, &GraphConnection) -> cy_editor_core::problem::Result<()> + 'a;
type MoveHandler<'a> =
    dyn FnMut(&mut GraphCanvas, GraphMovement) -> cy_editor_core::problem::Result<()> + 'a;

/// What the host panel learns from, and routes through, one frame of the canvas.
///
/// `on_connect` and `on_move` let a panel turn a gesture into a registered command (and so into
/// one undoable transaction) instead of mutating the canvas; `None` edits the canvas directly.
pub(super) struct CanvasFeedback<'a> {
    pub link_problem: &'a mut Option<String>,
    pub node_alerts: &'a [(u64, String)],
    /// Whether an unwired input is worth a warning. False for a domain where it reads a defined
    /// value, so the canvas carries the engine's diagnostics instead.
    pub unwired_inputs: bool,
    pub on_connect: Option<&'a mut ConnectionHandler<'a>>,
    pub on_move: Option<&'a mut MoveHandler<'a>>,
}

/// A node drag: `finished` is set on the frame the pointer is released.
#[derive(Clone, Copy)]
pub(super) struct GraphMovement {
    pub node: NodeKey,
    pub at: GraphLayout,
    pub finished: bool,
}

/// A connection gesture between two engine pins, by stable identity.
pub(super) struct GraphConnection {
    pub from: NodeKey,
    pub from_pin: u32,
    pub from_name: String,
    pub to: NodeKey,
    pub to_pin: u32,
    pub to_name: String,
}

/// Draw the canvas and turn this frame's pointer input into selection, link and move gestures.
pub(super) fn draw_canvas(
    ui: &mut egui::Ui,
    shell: &cy_editor_interface::shell::Shell,
    canvas: &mut GraphCanvas,
    state: MaterialCatalogueState,
    empty_message: &str,
    pending_source: &mut Option<(u64, u32, String, String)>,
    feedback: &mut CanvasFeedback<'_>,
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
            egui::Id::new(("graph-node", card.key.ordinal())),
            egui::Sense::click_and_drag(),
        );
        if response.clicked() {
            select = Some(card.key);
        }
        if response.dragged() {
            movement = Some(GraphMovement {
                node: card.key,
                at: GraphLayout {
                    x: card.layout.x + response.drag_delta().x,
                    y: card.layout.y + response.drag_delta().y,
                },
                finished: false,
            });
        } else if response.drag_stopped() && feedback.on_move.is_some() {
            movement = Some(GraphMovement {
                node: card.key,
                at: card.layout,
                finished: true,
            });
        }
        draw_node(
            &painter,
            shell,
            card,
            selected.contains(&card.key),
            response.hovered(),
        );
        draw_node_alert(&painter, shell, card, response, feedback.node_alerts);
    }
    let pin_action = interact_with_pins(ui, shell, &cards, pending_source.as_ref());
    if let Some(pin) = pin_action {
        apply_pin_action(
            canvas,
            pending_source,
            feedback.link_problem,
            feedback.on_connect.take(),
            pin,
        );
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
    apply_movement(canvas, feedback, movement);

    if cards.is_empty() {
        painter.text(
            rect.center(),
            egui::Align2::CENTER_CENTER,
            empty_message,
            egui::FontId::proportional(shell.metrics().text(TextRole::Body)),
            theme::role(shell.theme, Semantic::SecondaryText),
        );
    }
    draw_catalogue_status(&painter, shell, rect, state);
    if let Some(key) = draw_diagnostics(
        ui,
        &painter,
        shell,
        canvas,
        rect,
        feedback.link_problem.as_deref(),
        feedback.unwired_inputs,
    ) {
        let _ = canvas.select([key]);
    }
}

fn apply_movement(
    canvas: &mut GraphCanvas,
    feedback: &mut CanvasFeedback<'_>,
    movement: Option<GraphMovement>,
) {
    let Some(movement) = movement else { return };
    let result = if let Some(move_node) = feedback.on_move.take() {
        move_node(canvas, movement)
    } else if movement.finished {
        Ok(())
    } else {
        canvas.move_to(movement.node, movement.at)
    };
    if let Err(problem) = result {
        *feedback.link_problem = Some(problem.to_string());
    }
}

fn draw_catalogue_status(
    painter: &egui::Painter,
    shell: &cy_editor_interface::shell::Shell,
    rect: egui::Rect,
    state: MaterialCatalogueState,
) {
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
}

fn draw_node_alert(
    painter: &egui::Painter,
    shell: &cy_editor_interface::shell::Shell,
    card: &NodeCard,
    response: egui::Response,
    alerts: &[(u64, String)],
) {
    if let Some((_, message)) = alerts.iter().find(|(node, _)| *node == card.key.ordinal()) {
        painter.rect_stroke(
            card.rect,
            egui::CornerRadius::same(5),
            egui::Stroke::new(2.0, theme::role(shell.theme, Semantic::Error)),
            egui::StrokeKind::Outside,
        );
        response.on_hover_text(message);
    }
}

/// One node as the canvas lays it out: engine identities beside the rectangle it occupies.
#[derive(Clone)]
pub(super) struct NodeCard {
    pub key: NodeKey,
    pub type_name: String,
    pub type_identity: u32,
    pub layout: GraphLayout,
    pub rect: egui::Rect,
    pub inputs: Vec<Pin>,
    pub outputs: Vec<Pin>,
}

/// A click on one pin, resolved to the node and the engine pin it belongs to.
#[derive(Clone)]
pub(super) struct PinAction {
    pub node: NodeKey,
    pub pin: Pin,
}

pub(super) fn node_cards(canvas: &GraphCanvas, origin: egui::Pos2) -> Vec<NodeCard> {
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
            egui::Id::new(("graph-pin", card.key.ordinal(), pin.identity, pin.direction)),
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

pub(super) fn apply_pin_action(
    canvas: &mut GraphCanvas,
    pending: &mut Option<(u64, u32, String, String)>,
    problem: &mut Option<String>,
    on_connect: Option<&mut ConnectionHandler<'_>>,
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
            let connection = GraphConnection {
                from,
                from_pin: source.1,
                from_name: source.2,
                to: action.node,
                to_pin: action.pin.identity,
                to_name: action.pin.name,
            };
            let result = if let Some(connect) = on_connect {
                connect(canvas, &connection)
            } else {
                canvas.connect_identified(
                    connection.from,
                    connection.from_pin,
                    connection.to,
                    connection.to_pin,
                )
            };
            match result {
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
    unwired_inputs: bool,
) -> Option<NodeKey> {
    let diagnostics = canvas.diagnostics_reporting(unwired_inputs);
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
            egui::Id::new(("graph-diagnostic", key.ordinal(), location)),
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

pub(super) fn display_name(type_name: &str) -> String {
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

pub(super) fn matches_filter(type_name: &str, query: &str) -> bool {
    query.is_empty()
        || type_name.to_ascii_lowercase().contains(query)
        || display_name(type_name).to_ascii_lowercase().contains(query)
}

pub(super) fn display_index(index: usize) -> f32 {
    f32::from(u16::try_from(index).unwrap_or(u16::MAX))
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use cy_editor_commands::Registry;
    use cy_editor_interface::shell::Shell;
    use cy_editor_interface::specialised::Surface as EditorSurface;
    use cy_editor_interface::specialised::graph::Catalogue;
    use cy_editor_interface::{Domain, SpecialisedEditors};

    use super::*;

    #[test]
    fn palette_slots_fill_a_three_column_grid_clear_of_the_header() {
        let at = |index| {
            let slot = palette_slot(index);
            (slot.x, slot.y)
        };
        assert_eq!(at(0), (28.0, 34.0));
        assert_eq!(at(1), (28.0 + NODE_WIDTH + 34.0, 34.0));
        assert_eq!(at(2), (28.0 + 2.0 * (NODE_WIDTH + 34.0), 34.0));
        assert_eq!(at(3), (28.0, 184.0), "the fourth node starts a new row");
    }

    #[test]
    fn palette_labels_drop_the_domain_prefix_and_keep_the_name_as_identity() {
        assert_eq!(palette_label("script.emit_event"), "Emit Event");
        assert_eq!(palette_label("pose.blend_mask"), "Blend Mask");
        assert_eq!(palette_label("unprefixed"), "Unprefixed");
    }

    /// Every graph domain the host can open with a vocabulary declared in this tree gets that
    /// vocabulary as its palette, one entry per `Domain::node_types` name and nothing else, and a
    /// node picked from it lands on the canvas where the palette said it would.
    #[test]
    fn every_openable_graph_domain_hosts_its_declared_node_palette() {
        let mut editors = SpecialisedEditors::new().expect("host");
        let hosted: Vec<Domain> = editors
            .openable()
            .into_iter()
            .filter(|domain| {
                domain.surfaces().contains(&EditorSurface::Graph) && !domain.node_types().is_empty()
            })
            .collect();
        assert!(
            hosted.contains(&Domain::GameplayAndUtilityGraphs)
                && hosted.contains(&Domain::AbilitiesAndEffects),
            "the script and ability palettes are declared locally: {hosted:?}"
        );
        for domain in hosted {
            let session = editors.open(domain).expect("an openable domain opens");
            let canvas = session.graph.expect("a graph domain has the canvas");
            let entries = catalogue_palette(canvas, "");
            let mut names: Vec<&str> = entries.iter().map(|entry| entry.name.as_str()).collect();
            names.sort_unstable();
            let mut declared = domain.node_types().to_vec();
            declared.sort_unstable();
            assert_eq!(names, declared, "{domain:?} palette");

            let first = entries[0].name.clone();
            let slot = palette_slot(canvas.nodes().count());
            let key = canvas.add(&first, slot).expect("a palette type is addable");
            let cards = node_cards(canvas, egui::Pos2::ZERO);
            let card = cards.iter().find(|card| card.key == key).expect("drawn");
            assert_eq!(card.rect.min, egui::pos2(slot.x, slot.y));
            assert_eq!(card.type_name, first);
        }
    }

    #[test]
    fn a_catalogue_palette_searches_names_and_labels() {
        let mut canvas = GraphCanvas::new(1);
        canvas.load(
            Catalogue::new(vec![
                NodeType::new("script.emit_event", Vec::new()),
                NodeType::new("script.branch", Vec::new()),
            ])
            .unwrap(),
        );
        let names = |query: &str| -> Vec<String> {
            catalogue_palette(&canvas, query)
                .into_iter()
                .map(|entry| entry.name)
                .collect()
        };
        assert_eq!(names("emit event"), ["script.emit_event"]);
        assert_eq!(names("BRANCH"), ["script.branch"]);
        assert_eq!(names("").len(), 2);
        assert!(names("material").is_empty());
    }

    // --- The canvas driven through real egui frames -------------------------------------------

    struct Frames {
        ctx: egui::Context,
        shell: Shell,
    }

    impl Frames {
        fn new() -> Self {
            Self {
                ctx: egui::Context::default(),
                shell: Shell::new(&Registry::new()).expect("shell"),
            }
        }

        /// One frame of the canvas; answers the canvas origin it was laid out at.
        fn run(
            &self,
            events: Vec<egui::Event>,
            canvas: &mut GraphCanvas,
            pending: &mut Option<(u64, u32, String, String)>,
            feedback: &mut CanvasFeedback<'_>,
        ) -> egui::Pos2 {
            let raw = egui::RawInput {
                screen_rect: Some(egui::Rect::from_min_size(
                    egui::Pos2::ZERO,
                    egui::vec2(900.0, 600.0),
                )),
                events,
                ..Default::default()
            };
            let mut origin = egui::Pos2::ZERO;
            let mut output = self.ctx.run_ui(raw, |ui| {
                egui::CentralPanel::default().show(ui, |ui| {
                    origin = ui.available_rect_before_wrap().min;
                    draw_canvas(
                        ui,
                        &self.shell,
                        canvas,
                        MaterialCatalogueState::Ready,
                        "empty",
                        pending,
                        feedback,
                    );
                });
            });
            output.textures_delta.clear();
            origin
        }
    }

    fn pointer(at: egui::Pos2, pressed: Option<bool>) -> Vec<egui::Event> {
        let mut events = vec![egui::Event::PointerMoved(at)];
        if let Some(pressed) = pressed {
            events.push(egui::Event::PointerButton {
                pos: at,
                button: egui::PointerButton::Primary,
                pressed,
                modifiers: egui::Modifiers::NONE,
            });
        }
        events
    }

    fn pinned_catalogue() -> Catalogue {
        let mut out = Pin::new("out", PinDirection::Output, "float");
        out.identity = 41;
        let mut input = Pin::new("in", PinDirection::Input, "float");
        input.identity = 73;
        Catalogue::new(vec![
            NodeType::identified(7, 1, "script.const_float".into(), vec![out]),
            NodeType::identified(8, 1, "script.return".into(), vec![input]),
        ])
        .unwrap()
    }

    #[test]
    fn dragging_a_node_header_reports_moves_and_one_finished_gesture() {
        let frames = Frames::new();
        let mut canvas = GraphCanvas::new(1);
        canvas.load(pinned_catalogue());
        let node = canvas
            .add("script.const_float", GraphLayout { x: 40.0, y: 60.0 })
            .unwrap();
        let movements = RefCell::new(Vec::new());
        let mut on_move = |canvas: &mut GraphCanvas, movement: GraphMovement| {
            movements.borrow_mut().push(movement);
            if movement.finished {
                Ok(())
            } else {
                canvas.move_to(movement.node, movement.at)
            }
        };
        let mut problem = None;
        let mut pending = None;
        let mut frame = |events| {
            frames.run(
                events,
                &mut canvas,
                &mut pending,
                &mut CanvasFeedback {
                    unwired_inputs: true,
                    link_problem: &mut problem,
                    node_alerts: &[],
                    on_connect: None,
                    on_move: Some(&mut on_move),
                },
            )
        };
        let origin = frame(Vec::new());
        let grab = origin + egui::vec2(60.0, 70.0);
        frame(pointer(grab, Some(true)));
        frame(pointer(grab + egui::vec2(15.0, 10.0), None));
        frame(pointer(grab + egui::vec2(30.0, 20.0), None));
        frame(pointer(grab + egui::vec2(30.0, 20.0), Some(false)));
        frame(Vec::new());

        let movements = movements.into_inner();
        let finished: Vec<_> = movements.iter().filter(|m| m.finished).collect();
        assert_eq!(
            finished.len(),
            1,
            "one release is one gesture: {:?}",
            movements.len()
        );
        assert!(
            movements.iter().any(|m| !m.finished),
            "the drag was reported"
        );
        assert!(movements.iter().all(|m| m.node == node));
        assert_eq!(
            canvas.layout_of(node),
            Some(GraphLayout { x: 70.0, y: 80.0 }),
            "the node follows the pointer"
        );
    }

    #[test]
    fn clicking_an_output_then_an_input_pin_routes_one_connection_to_the_host() {
        let frames = Frames::new();
        let mut canvas = GraphCanvas::new(1);
        canvas.load(pinned_catalogue());
        let source = canvas
            .add("script.const_float", GraphLayout { x: 20.0, y: 40.0 })
            .unwrap();
        let sink = canvas
            .add("script.return", GraphLayout { x: 360.0, y: 40.0 })
            .unwrap();
        let routed = RefCell::new(Vec::new());
        let mut on_connect = |_: &mut GraphCanvas, link: &GraphConnection| {
            routed
                .borrow_mut()
                .push((link.from, link.from_pin, link.to, link.to_pin));
            Ok(())
        };
        let mut problem = None;
        let mut pending = None;
        let mut frame = |events| {
            frames.run(
                events,
                &mut canvas,
                &mut pending,
                &mut CanvasFeedback {
                    unwired_inputs: true,
                    link_problem: &mut problem,
                    node_alerts: &[],
                    on_connect: Some(&mut on_connect),
                    on_move: None,
                },
            )
        };
        let origin = frame(Vec::new());
        let pin_y = 40.0 + NODE_HEADER + 0.5 * PIN_ROW;
        let output = origin + egui::vec2(20.0 + NODE_WIDTH, pin_y);
        let input = origin + egui::vec2(360.0, pin_y);
        frame(pointer(output, Some(true)));
        frame(pointer(output, Some(false)));
        frame(pointer(input, Some(true)));
        frame(pointer(input, Some(false)));

        assert_eq!(routed.into_inner(), [(source, 41, sink, 73)]);
        assert!(pending.is_none(), "the gesture completed");
        assert!(problem.is_none());
        assert_eq!(
            canvas.links().count(),
            0,
            "a routed connection is the host's to commit"
        );
    }
}
