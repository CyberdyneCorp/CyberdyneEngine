//! The visible terrain editor built on the one shared painting surface.

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::painting::{PaintingSurface, Sample};
use cy_editor_services::terrain::TerrainStack;
use cy_editor_visual::colour::{Semantic, Surface};

use super::{Inputs, Intent, Panels, heading, nothing_here, secondary};
use crate::theme;

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let Some(document_id) = panels.editor.workspace.active() else {
        nothing_here(
            ui,
            panels.shell,
            "No world is open.",
            "Open a world before creating terrain authoring data.",
        );
        return;
    };
    let selected: Vec<NodeId> = panels.editor.selection.get().nodes().collect();
    let target = panels
        .editor
        .documents
        .get(document_id)
        .and_then(|document| {
            selected
                .iter()
                .find_map(|node| TerrainStack::read(document, *node).map(|stack| (*node, stack)))
        });
    let Some((terrain, stack)) = target else {
        ui.heading("Terrain");
        ui.label(secondary(
            panels.shell,
            "Create a terrain authoring root, then sculpt or paint it with stable modifiers.",
        ));
        if ui.button("Create terrain").clicked() {
            panels
                .intents
                .push(Intent::Invoke("terrain.create".into(), Arguments::new()));
        }
        return;
    };

    if panels
        .inputs
        .terrain_layer
        .is_some_and(|selected| !stack.layers.iter().any(|layer| layer.id == selected))
    {
        panels.inputs.terrain_layer = None;
    }
    if panels.inputs.terrain_layer.is_none() {
        panels.inputs.terrain_layer = stack.layers.first().map(|layer| layer.id);
    }

    let shell = &*panels.shell;
    let inputs = &mut *panels.inputs;
    let session = match panels.specialised.open(Domain::Terrain) {
        Ok(session) => session,
        Err(problem) => {
            nothing_here(
                ui,
                shell,
                "The terrain editor could not be opened.",
                &problem.to_string(),
            );
            return;
        }
    };
    let surface = session
        .painting
        .expect("terrain declares the shared painting surface");
    let mut intents = Vec::new();
    let available = ui.available_size();
    ui.horizontal(|ui| {
        ui.allocate_ui_with_layout(
            egui::vec2(250.0_f32.min(available.x * 0.42), available.y),
            egui::Layout::top_down(egui::Align::Min),
            |ui| {
                controls(inputs, shell, ui, terrain, &stack, surface, &mut intents);
            },
        );
        ui.separator();
        ui.allocate_ui(egui::vec2(ui.available_width(), available.y), |ui| {
            paint_field(inputs, shell, ui, terrain, surface, &mut intents);
        });
    });
    panels.intents.extend(intents);
}

fn controls(
    inputs: &mut Inputs,
    shell: &Shell,
    ui: &mut egui::Ui,
    terrain: NodeId,
    stack: &TerrainStack,
    surface: &mut PaintingSurface,
    intents: &mut Vec<Intent>,
) {
    ui.heading("Brush");
    ui.horizontal_wrapped(|ui| {
        for (tool, label) in [
            ("raise", "Raise"),
            ("lower", "Lower"),
            ("smooth", "Smooth"),
            ("flatten", "Flatten"),
            ("paint", "Paint"),
        ] {
            ui.selectable_value(&mut inputs.terrain_tool, tool.into(), label);
        }
    });

    let mut brush = surface.brush();
    brush_row(ui, "Radius", &mut brush.radius, 0.1..=10_000.0);
    brush_row(ui, "Strength", &mut brush.strength, 0.0..=1.0);
    brush_row(ui, "Falloff", &mut brush.falloff, 0.0..=1.0);
    brush_row(ui, "Spacing", &mut brush.spacing, 0.0..=1.0);
    if brush != surface.brush()
        && let Err(problem) = surface.set_brush(brush)
    {
        inputs.terrain_problem = Some(problem.to_string());
    }

    heading(ui, shell, "Material layers");
    egui::ComboBox::from_id_salt("terrain-material-layer")
        .selected_text(selected_layer_name(stack, inputs.terrain_layer))
        .show_ui(ui, |ui| {
            for layer in &stack.layers {
                ui.selectable_value(&mut inputs.terrain_layer, Some(layer.id), &layer.name)
                    .on_hover_text(&layer.material);
            }
        });
    ui.add(egui::TextEdit::singleline(&mut inputs.terrain_layer_name).hint_text("Layer name"));
    ui.add(
        egui::TextEdit::singleline(&mut inputs.terrain_layer_material)
            .hint_text("materials/ground.cymat"),
    );
    if ui.button("Add material layer").clicked() {
        intents.push(Intent::Invoke(
            "terrain.layer.add".into(),
            Arguments::new()
                .with("terrain", Value::Text(terrain.to_string()))
                .with("name", Value::Text(inputs.terrain_layer_name.clone()))
                .with(
                    "material",
                    Value::Text(inputs.terrain_layer_material.clone()),
                ),
        ));
    }

    heading(ui, shell, "Modifier stack");
    egui::ScrollArea::vertical().show(ui, |ui| {
        for (index, modifier) in stack.modifiers.iter().enumerate() {
            ui.horizontal(|ui| {
                let mut enabled = modifier.enabled;
                if ui.checkbox(&mut enabled, "").changed() {
                    intents.push(Intent::Invoke(
                        "terrain.modifier.set-enabled".into(),
                        Arguments::new()
                            .with("modifier", Value::Text(modifier.id.to_string()))
                            .with("enabled", Value::Bool(enabled)),
                    ));
                }
                ui.label(&modifier.name).on_hover_text(format!(
                    "{} · {} samples · {}",
                    modifier.kind, modifier.sample_count, modifier.id
                ));
                if ui.add_enabled(index > 0, egui::Button::new("↑")).clicked() {
                    intents.push(move_modifier(modifier.id, index - 1));
                }
                if ui
                    .add_enabled(index + 1 < stack.modifiers.len(), egui::Button::new("↓"))
                    .clicked()
                {
                    intents.push(move_modifier(modifier.id, index + 1));
                }
            });
        }
    });
}

fn brush_row(
    ui: &mut egui::Ui,
    label: &str,
    value: &mut f32,
    range: std::ops::RangeInclusive<f32>,
) {
    ui.horizontal(|ui| {
        ui.label(label);
        ui.add(egui::DragValue::new(value).range(range).speed(0.02));
    });
}

fn selected_layer_name(stack: &TerrainStack, selected: Option<NodeId>) -> String {
    selected
        .and_then(|id| stack.layers.iter().find(|layer| layer.id == id))
        .map_or_else(|| "No material layer".into(), |layer| layer.name.clone())
}

fn move_modifier(modifier: NodeId, order: usize) -> Intent {
    Intent::Invoke(
        "terrain.modifier.move".into(),
        Arguments::new()
            .with("modifier", Value::Text(modifier.to_string()))
            .with(
                "order",
                Value::Int(i64::try_from(order).unwrap_or(i64::MAX)),
            ),
    )
}

fn paint_field(
    inputs: &mut Inputs,
    shell: &Shell,
    ui: &mut egui::Ui,
    terrain: NodeId,
    surface: &mut PaintingSurface,
    intents: &mut Vec<Intent>,
) {
    ui.heading("Terrain surface");
    ui.label(secondary(
        shell,
        "Drag to author one undoable non-destructive modifier. Escape cancels the gesture.",
    ));
    let desired = egui::vec2(
        ui.available_width(),
        (ui.available_height() - 24.0).max(180.0),
    );
    let (rect, response) = ui.allocate_exact_size(desired, egui::Sense::drag());
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, 4.0, theme::surface(shell.theme, Surface::Sunken));
    draw_grid(
        &painter,
        rect,
        theme::role(shell.theme, Semantic::SecondaryText),
    );

    if ui.input(|input| input.key_pressed(egui::Key::Escape)) {
        surface.cancel();
        inputs.terrain_problem = None;
    }
    if response.drag_started()
        && let Some(position) = response.interact_pointer_pos()
        && let Err(problem) = surface.begin(sample(rect, position))
    {
        inputs.terrain_problem = Some(problem.to_string());
    }
    if response.dragged()
        && let Some(position) = response.interact_pointer_pos()
    {
        let _ = surface.sample(sample(rect, position));
    }
    if response.drag_stopped() && surface.is_active() {
        if let Some(position) = response.interact_pointer_pos() {
            let _ = surface.sample(sample(rect, position));
        }
        match surface.finish() {
            Ok(_stroke) if inputs.terrain_tool == "paint" && inputs.terrain_layer.is_none() => {
                inputs.terrain_problem = Some("Paint requires a material layer.".into());
            }
            Ok(stroke) => {
                let mut arguments = Arguments::new()
                    .with("terrain", Value::Text(terrain.to_string()))
                    .with("tool", Value::Text(inputs.terrain_tool.clone()))
                    .with("stroke", Value::Bytes(stroke.encode()));
                if let Some(layer) = inputs.terrain_layer {
                    arguments = arguments.with("layer", Value::Text(layer.to_string()));
                }
                intents.push(Intent::Invoke("terrain.stroke.commit".into(), arguments));
                inputs.terrain_problem = None;
            }
            Err(problem) => inputs.terrain_problem = Some(problem.to_string()),
        }
    }

    let points: Vec<egui::Pos2> = surface
        .active_samples()
        .map(|sample| {
            egui::pos2(
                rect.left() + sample.x * rect.width(),
                rect.top() + sample.y * rect.height(),
            )
        })
        .collect();
    if points.len() > 1 {
        painter.add(egui::Shape::line(
            points,
            egui::Stroke::new(2.0, theme::role(shell.theme, Semantic::Selection)),
        ));
    }
    if let Some(problem) = &inputs.terrain_problem {
        painter.text(
            rect.left_top() + egui::vec2(10.0, 10.0),
            egui::Align2::LEFT_TOP,
            problem,
            egui::FontId::proportional(12.0),
            theme::role(shell.theme, Semantic::Error),
        );
    }
}

fn sample(rect: egui::Rect, position: egui::Pos2) -> Sample {
    Sample::new(
        ((position.x - rect.left()) / rect.width()).clamp(0.0, 1.0),
        ((position.y - rect.top()) / rect.height()).clamp(0.0, 1.0),
        1.0,
    )
    .expect("a clamped finite pointer position")
}

fn draw_grid(painter: &egui::Painter, rect: egui::Rect, colour: egui::Color32) {
    for fraction in [0.125_f32, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875] {
        let x = egui::lerp(rect.x_range(), fraction);
        let y = egui::lerp(rect.y_range(), fraction);
        painter.line_segment(
            [egui::pos2(x, rect.top()), egui::pos2(x, rect.bottom())],
            egui::Stroke::new(1.0, colour),
        );
        painter.line_segment(
            [egui::pos2(rect.left(), y), egui::pos2(rect.right(), y)],
            egui::Stroke::new(1.0, colour),
        );
    }
}
