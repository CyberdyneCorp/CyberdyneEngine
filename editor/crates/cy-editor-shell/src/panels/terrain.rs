// SPDX-License-Identifier: MIT
//! The visible terrain editor built on the one shared painting surface.
//!
//! Every stroke is one `terrain.stroke.commit` transaction; nothing here computes a height. The
//! brush field draws what the engine's terrain module evaluated for the document's stack
//! (`Editor::terrain`), with the holes it cut and the regions whose navigation it flagged stale.

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::Session;
use cy_editor_interface::specialised::painting::{PaintingSurface, Sample};
use cy_editor_services::terrain::{TOOLS, TerrainStack};
use cy_editor_services::terrain_engine::{StaleRegion, TerrainEvaluation};
use cy_editor_visual::colour::{Semantic, Surface};

use super::specialised::{SpecialisedTool, ToolDiagnostic, ToolFrame};
use super::{Inputs, Intent, Panels, heading, nothing_here, secondary, status};
use crate::theme;

/// The terrain editor, drawn in the specialised-editor frame.
pub(crate) struct TerrainTool;

impl SpecialisedTool for TerrainTool {
    const DOMAIN: Domain = Domain::Terrain;
    const TITLE: &'static str = "Terrain";
    const COMMANDS: &'static [&'static str] = &[
        "terrain.create",
        "terrain.layer.add",
        "terrain.stroke.commit",
        "terrain.modifier.set-enabled",
        "terrain.modifier.move",
        "terrain.status",
    ];

    /// The selected terrain root, its authored stack, and what the engine made of it.
    type Target = (NodeId, TerrainStack, EngineView);

    fn target(panels: &mut Panels<'_>, ui: &mut egui::Ui) -> Option<Self::Target> {
        let Some(document_id) = panels.editor.workspace.active() else {
            nothing_here(
                ui,
                panels.shell,
                "No world is open.",
                "Open a world before creating terrain authoring data.",
            );
            return None;
        };
        let selected: Vec<NodeId> = panels.editor.selection.get().nodes().collect();
        let target = panels
            .editor
            .documents
            .get(document_id)
            .and_then(|document| {
                selected.iter().find_map(|node| {
                    TerrainStack::read(document, *node).map(|stack| (*node, stack))
                })
            });
        let Some((terrain, stack)) = target else {
            ui.label(secondary(
                panels.shell,
                "Create a terrain authoring root, then sculpt or paint it with stable modifiers.",
            ));
            if ui.button("Create terrain").clicked() {
                panels
                    .intents
                    .push(Intent::Invoke("terrain.create".into(), Arguments::new()));
            }
            return None;
        };
        keep_selected_layer(panels.inputs, &stack);
        let view = engine_view(panels, ui.ctx(), terrain);
        Some((terrain, stack, view))
    }

    fn diagnostics(inputs: &Inputs) -> Vec<ToolDiagnostic> {
        inputs
            .terrain_problem
            .iter()
            .chain(inputs.terrain_engine_problem.iter())
            .map(ToolDiagnostic::error)
            .collect()
    }

    fn body(
        frame: &mut ToolFrame<'_>,
        session: Session<'_>,
        (terrain, stack, view): Self::Target,
        ui: &mut egui::Ui,
    ) {
        let surface = session
            .painting
            .expect("terrain declares the shared painting surface");
        let ToolFrame {
            shell,
            inputs,
            intents,
        } = frame;
        let available = ui.available_size();
        ui.horizontal(|ui| {
            ui.allocate_ui_with_layout(
                egui::vec2(250.0_f32.min(available.x * 0.42), available.y),
                egui::Layout::top_down(egui::Align::Min),
                |ui| {
                    controls(inputs, shell, ui, terrain, &stack, surface, intents);
                },
            );
            ui.separator();
            // Top-down explicitly: `allocate_ui` inherits the row's horizontal layout, which put
            // the heading, the hint and the field side by side and squeezed the field.
            ui.allocate_ui_with_layout(
                egui::vec2(ui.available_width(), available.y),
                egui::Layout::top_down(egui::Align::Min),
                |ui| {
                    paint_field(inputs, shell, ui, terrain, surface, &view, intents);
                },
            );
        });
    }
}

/// Keep the paint layer on one the stack still has, defaulting to its first.
fn keep_selected_layer(inputs: &mut Inputs, stack: &TerrainStack) {
    if inputs
        .terrain_layer
        .is_some_and(|selected| !stack.layers.iter().any(|layer| layer.id == selected))
    {
        inputs.terrain_layer = None;
    }
    if inputs.terrain_layer.is_none() {
        inputs.terrain_layer = stack.layers.first().map(|layer| layer.id);
    }
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
        for (tool, label) in TOOLS.into_iter().zip(TOOL_LABELS) {
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
    view: &EngineView,
    intents: &mut Vec<Intent>,
) {
    ui.heading("Terrain surface");
    ui.label(secondary(
        shell,
        "Drag to author one undoable non-destructive modifier. Escape cancels the gesture.",
    ));
    status(ui, shell, view.role, &view.summary);
    let desired = egui::vec2(
        ui.available_width(),
        (ui.available_height() - 24.0).max(180.0),
    );
    let (rect, response) = ui.allocate_exact_size(desired, egui::Sense::drag());
    response.widget_info(|| {
        egui::WidgetInfo::labeled(egui::WidgetType::Other, true, "Terrain brush field")
    });
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, 4.0, theme::surface(shell.theme, Surface::Sunken));
    if let Some(texture) = &view.texture {
        painter.image(
            texture.id(),
            rect,
            egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
            egui::Color32::WHITE,
        );
    }
    draw_grid(
        &painter,
        rect,
        theme::role(shell.theme, Semantic::SecondaryText),
    );
    draw_stale(
        &painter,
        rect,
        view,
        theme::role(shell.theme, Semantic::Warning),
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
                // Only paint names a layer: `terrain.stroke.commit` refuses a layer on a sculpt or
                // hole stroke, and the panel keeps a layer selected whenever the stack has one.
                if inputs.terrain_tool == "paint"
                    && let Some(layer) = inputs.terrain_layer
                {
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
}

/// The panel's words for [`TOOLS`], in its order.
const TOOL_LABELS: [&str; 6] = ["Raise", "Lower", "Smooth", "Flatten", "Paint", "Hole"];

/// What the engine made of the edited terrain, ready to draw.
pub(crate) struct EngineView {
    /// The engine's surface as an image, when it has evaluated this terrain.
    texture: Option<egui::TextureHandle>,
    /// One line: what the engine last evaluated, or why nothing is shown.
    summary: String,
    /// How that line reads.
    role: Semantic,
    /// Regions whose navigation is stale, in metres, and the extent they are over.
    stale: Vec<StaleRegion>,
    extent: f32,
}

/// Resolve the engine's answer for `terrain`, uploading its image once per evaluation.
fn engine_view(panels: &mut Panels<'_>, ctx: &egui::Context, terrain: NodeId) -> EngineView {
    let engine = &panels.editor.terrain;
    panels.inputs.terrain_engine_problem = engine.problem().map(str::to_owned);
    let evaluation = engine
        .evaluation()
        .filter(|_| engine.terrain() == Some(terrain));
    let wanted = panels
        .editor
        .edited_terrain_request()
        .filter(|(edited, _)| *edited == terrain)
        .and_then(|(_, request)| request.ok());
    let current = engine.is_current(wanted.as_deref());
    let connected = panels.editor.runtime.is_connected();
    let (summary, role) = engine_summary(evaluation, connected, current);
    let Some(evaluation) = evaluation else {
        panels.inputs.terrain_surface = None;
        return EngineView {
            texture: None,
            summary,
            role,
            stale: Vec::new(),
            extent: 1.0,
        };
    };
    let key = (terrain, evaluation.generation);
    let texture = match &panels.inputs.terrain_surface {
        Some((cached, texture)) if *cached == key => texture.clone(),
        _ => {
            let texture = ctx.load_texture(
                "terrain-engine-surface",
                surface_image(evaluation),
                egui::TextureOptions::NEAREST,
            );
            panels.inputs.terrain_surface = Some((key, texture.clone()));
            texture
        }
    };
    EngineView {
        texture: Some(texture),
        summary,
        role,
        stale: evaluation.stale.clone(),
        extent: evaluation.extent,
    }
}

/// The status line: what the engine evaluated, whether it is still the document's stack, and why
/// nothing is drawn when nothing is.
pub(crate) fn engine_summary(
    evaluation: Option<&TerrainEvaluation>,
    connected: bool,
    current: bool,
) -> (String, Semantic) {
    let Some(evaluation) = evaluation else {
        return if connected {
            (
                "Waiting for the engine to evaluate the terrain.".into(),
                Semantic::SecondaryText,
            )
        } else {
            (
                "No engine attached. Strokes are recorded; the engine evaluates them when it connects."
                    .into(),
                Semantic::SecondaryText,
            )
        };
    };
    let holes = evaluation.hole_count();
    let mut summary = format!(
        "Engine surface {}: {} triangles, {holes} holes cut from rendering and collision",
        evaluation.generation, evaluation.rendered_triangles
    );
    if !current {
        summary.push_str(" (updating)");
    }
    if evaluation.stale.is_empty() {
        (summary, Semantic::Live)
    } else {
        summary.push_str(&format!(
            ". Navigation stale in {} regions until it is rebaked",
            evaluation.stale.len()
        ));
        (summary, Semantic::Warning)
    }
}

/// Layer tints, by layer order. Muted and distinct from the selection gold.
const LAYER_TINTS: [[u8; 3]; 4] = [
    [58, 157, 143],
    [139, 127, 209],
    [192, 105, 78],
    [127, 163, 90],
];

/// The engine's lattice as an image, one pixel per quad, rows along x: a hillshade lit from the
/// north-west, tinted by each texel's painted layers, and transparent where a hole is cut so the
/// field's sunken well shows through.
pub(crate) fn surface_image(evaluation: &TerrainEvaluation) -> egui::ColorImage {
    let quads = (evaluation.edge - 1) as usize;
    let spacing = evaluation.extent / quads as f32;
    let mut pixels = Vec::with_capacity(quads * quads);
    for z in 0..quads {
        for x in 0..quads {
            let at = z * quads + x;
            if evaluation.holes[at] != 0 {
                pixels.push(egui::Color32::TRANSPARENT);
                continue;
            }
            let (x32, z32) = (x as u32, z as u32);
            let dx = evaluation.height(x32 + 1, z32) - evaluation.height(x32, z32);
            let dz = evaluation.height(x32, z32 + 1) - evaluation.height(x32, z32);
            let normal = [-dx / spacing, 1.0, -dz / spacing];
            let length = (normal[0] * normal[0] + 1.0 + normal[2] * normal[2]).sqrt();
            let light = [-0.5_f32, 0.707, -0.5];
            let lit = ((normal[0] * light[0] + normal[1] * light[1] + normal[2] * light[2])
                / length)
                .clamp(0.0, 1.0);
            let grey = 70.0 + 150.0 * lit;
            let mut colour = [grey; 3];
            let texel = evaluation.texels[at];
            for slot in 0..4 {
                let (layer, weight) = (texel[slot], f32::from(texel[4 + slot]) / 255.0);
                if layer == 0 || weight == 0.0 {
                    continue;
                }
                let tint = LAYER_TINTS[usize::from(layer - 1) % LAYER_TINTS.len()];
                for channel in 0..3 {
                    let tinted = f32::from(tint[channel]) * (grey / 220.0) * 1.3;
                    colour[channel] += (tinted - colour[channel]) * weight * 0.7;
                }
            }
            pixels.push(egui::Color32::from_rgb(
                colour[0].clamp(0.0, 255.0) as u8,
                colour[1].clamp(0.0, 255.0) as u8,
                colour[2].clamp(0.0, 255.0) as u8,
            ));
        }
    }
    egui::ColorImage::new([quads, quads], pixels)
}

/// A thin outline around each region whose navigation is stale.
fn draw_stale(painter: &egui::Painter, rect: egui::Rect, view: &EngineView, colour: egui::Color32) {
    let to_screen = |x: f32, z: f32| {
        egui::pos2(
            rect.left() + (x / view.extent) * rect.width(),
            rect.top() + (z / view.extent) * rect.height(),
        )
    };
    for region in &view.stale {
        painter.rect_stroke(
            egui::Rect::from_two_pos(
                to_screen(region.min_x, region.min_z),
                to_screen(region.max_x, region.max_z),
            ),
            0.0,
            egui::Stroke::new(1.0, colour),
            egui::StrokeKind::Inside,
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

#[cfg(test)]
mod tests {
    use super::*;

    fn evaluation(holes: Vec<u8>, texels: Vec<[u8; 8]>, stale: usize) -> TerrainEvaluation {
        TerrainEvaluation {
            generation: 4,
            edge: 3,
            extent: 128.0,
            height_min: -512.0,
            height_max: 1536.0,
            rendered_triangles: 6,
            rendered_hole_quads: 1,
            collision_holes: 1,
            stale: vec![
                StaleRegion {
                    min_x: 0.0,
                    min_z: 0.0,
                    max_x: 64.0,
                    max_z: 64.0,
                };
                stale
            ],
            heights: vec![16384; 9],
            texels,
            holes,
        }
    }

    #[test]
    fn the_engine_surface_is_transparent_where_a_hole_is_cut_and_tinted_where_painted() {
        let base = [0, 0, 0, 0, 255, 0, 0, 0];
        let painted = [2, 0, 0, 0, 255, 0, 0, 0];
        let image = surface_image(&evaluation(
            vec![0, 1, 0, 0],
            vec![base, base, painted, base],
            0,
        ));
        assert_eq!(image.size, [2, 2]);
        assert_eq!(image.pixels[1], egui::Color32::TRANSPARENT);
        assert_eq!(image.pixels[0].a(), 255);
        // Flat ground is one grey; the painted quad is not that grey.
        let grey = image.pixels[0];
        assert_eq!(grey.r(), grey.g());
        assert_ne!(image.pixels[2], grey);
        assert_eq!(image.pixels[3], grey);
    }

    #[test]
    fn the_status_line_says_what_the_engine_evaluated_and_what_is_stale() {
        let (summary, role) = engine_summary(None, false, false);
        assert!(summary.contains("No engine attached"), "{summary}");
        assert_eq!(role, Semantic::SecondaryText);
        let (summary, _) = engine_summary(None, true, false);
        assert!(summary.contains("Waiting for the engine"), "{summary}");

        let base = [0, 0, 0, 0, 255, 0, 0, 0];
        let clean = evaluation(vec![0, 1, 0, 0], vec![base; 4], 0);
        let (summary, role) = engine_summary(Some(&clean), true, true);
        assert!(summary.contains("6 triangles, 1 holes"), "{summary}");
        assert!(!summary.contains("updating"), "{summary}");
        assert_eq!(role, Semantic::Live);

        let stale = evaluation(vec![0; 4], vec![base; 4], 2);
        let (summary, role) = engine_summary(Some(&stale), true, false);
        assert!(summary.contains("(updating)"), "{summary}");
        assert!(
            summary.contains("Navigation stale in 2 regions"),
            "{summary}"
        );
        assert_eq!(role, Semantic::Warning);
    }
}
