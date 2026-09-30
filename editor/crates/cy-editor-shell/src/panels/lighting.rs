// SPDX-License-Identifier: MIT
//! The lighting and lightmap baking editor: irradiance volumes, light mobility, the bake form with
//! its progress and cancel, and what the bake made — the density view and each captured probe in
//! its light.
//!
//! A [`SpecialisedTool`] like every other specialised panel: the scaffold draws the header, opens
//! the domain and checks the commands. Drawing only. Every authored edit is a registered command in
//! one transaction (`cy_editor_services::lighting`), so undo reverses it and an agent reaches it the
//! same way; the bake is `cy_editor_interface::specialised::lighting::LightingForm`'s invocation,
//! and the progress bar is the operation service's own state for the bake's request. The bake is
//! the scaffold's one kind of [`SpecialisedTool::OPERATIONS`]: it writes the world's description
//! and a cooked lightmap, not a document, so there is no transaction for undo to restore.
//!
//! The Inspector's contextual lighting rows — a light's mobility, an object's lightmap resolution —
//! are [`inspector_rows`], and push the same commands.

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::progress::OperationState;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::Session;
use cy_editor_interface::specialised::lighting::{
    BAKE_COMMAND, BakeStatus, Baked, CANCEL_COMMAND, LightingForm,
};
use cy_editor_services::lighting::{LightingScene, Mobility, Volume};
use cy_editor_services::lightmap_description::MODES;
use cy_editor_services::mirror::engine_identity;
use cy_editor_visual::colour::{Semantic, Surface};

use super::specialised::{SpecialisedTool, ToolDiagnostic, ToolFrame};
use super::{Inputs, Intent, Panels, heading, nothing_here, numeric, secondary, status};
use crate::theme;

/// The command that writes the open world's description without baking it.
const WRITE_COMMAND: &str = "lighting.write-lightmap-description";

/// The staged edits and the last bake's result, kept between frames.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct LightingInputs {
    /// The volume grid being edited, and the document's grid it was staged from. Applied as one
    /// command; restaged when the document's grid changes under it (an undo, an agent's edit).
    pub grid: Option<(NodeId, Grid, Grid)>,
    /// The Inspector's lightmap resolution while a drag or typing is in progress on it.
    pub resolution: Option<(NodeId, f32)>,
    /// What the last bake that finished made, read once when it ended.
    pub baked: Option<Baked>,
    /// The last bake that failed, and why.
    pub problem: Option<String>,
}

/// A probe to draw: where it is, and — once captured — its light and whether it is valid.
type ProbePoint = ([f32; 3], Option<([f32; 3], bool)>);

/// A volume's grid as the panel edits it: spacing, probes per axis, rays per probe.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Grid {
    /// Metres between probes.
    pub spacing: f32,
    /// Probes along x, y and z.
    pub counts: [i64; 3],
    /// Capture rays per probe.
    pub rays: i64,
}

impl Grid {
    fn of(volume: &Volume) -> Self {
        Self {
            spacing: volume.spacing,
            counts: volume.counts.map(i64::from),
            rays: i64::from(volume.rays),
        }
    }
}

/// What the editor draws this frame.
pub(crate) struct LightingTarget {
    /// The open world's lighting, when a world is open.
    scene: Option<LightingScene>,
    /// The bake the editor most recently started, if any.
    status: Option<BakeStatus>,
    selected: Option<NodeId>,
}

/// The lighting and lightmap baking editor.
pub(crate) struct LightingTool;

impl SpecialisedTool for LightingTool {
    const DOMAIN: Domain = Domain::LightingAndLightmapBaking;
    const TITLE: &'static str = "Lighting & Lightmaps";
    // `LightingForm::density_view()` and `probe_view()` are these modes' command ids; a test holds
    // them together.
    const COMMANDS: &'static [&'static str] = &[
        "lighting.volume.create",
        "lighting.volume.set",
        "lighting.light.set-mobility",
        "lighting.object.set-resolution",
        "edit.select",
        CANCEL_COMMAND,
        "viewport.view-mode.lightmap-density",
        "viewport.view-mode.gi-probes",
    ];
    const OPERATIONS: &'static [&'static str] = &[BAKE_COMMAND, WRITE_COMMAND];

    type Target = LightingTarget;

    fn target(panels: &mut Panels<'_>, _ui: &mut egui::Ui) -> Option<Self::Target> {
        collect_finished_bakes(panels);
        let scene = panels
            .editor
            .workspace
            .active()
            .and_then(|active| panels.editor.documents.get(active))
            .map(LightingScene::read);
        Some(LightingTarget {
            scene,
            status: LightingForm::status(panels.editor),
            selected: panels.editor.selection.get().nodes().next(),
        })
    }

    fn diagnostics(inputs: &Inputs) -> Vec<ToolDiagnostic> {
        inputs
            .lighting
            .problem
            .iter()
            .map(ToolDiagnostic::error)
            .collect()
    }

    fn body(
        frame: &mut ToolFrame<'_>,
        session: Session<'_>,
        target: Self::Target,
        ui: &mut egui::Ui,
    ) {
        let form = session
            .lighting
            .expect("lighting and lightmap baking opens onto its form");
        // Controls on the left, what the bake made on the right, as the terrain tool lays out its
        // brush field beside its controls.
        let available = ui.available_size();
        ui.horizontal_top(|ui| {
            ui.allocate_ui_with_layout(
                egui::vec2(360.0_f32.min(available.x * 0.5), available.y),
                egui::Layout::top_down(egui::Align::Min),
                |ui| {
                    egui::ScrollArea::vertical()
                        .id_salt("lighting-controls")
                        .show(ui, |ui| controls(frame, form, &target, ui));
                },
            );
            ui.separator();
            ui.allocate_ui_with_layout(
                egui::vec2(ui.available_width(), available.y),
                egui::Layout::top_down(egui::Align::Min),
                |ui| views(frame, target.scene.as_ref(), ui),
            );
        });
    }
}

fn controls(
    frame: &mut ToolFrame<'_>,
    form: &mut LightingForm,
    target: &LightingTarget,
    ui: &mut egui::Ui,
) {
    bake_form(frame, form, target, ui);
    match &target.scene {
        Some(scene) => {
            volumes(frame, scene, target.selected, ui);
            lights(frame, scene, ui);
        }
        None => nothing_here(
            ui,
            frame.shell,
            "No world is open.",
            "Open a world to place irradiance volumes, set light mobility and bake what was \
             authored.",
        ),
    }
}

/// Take the bakes that ended since the last frame: keep what the last good one made, and why the
/// last bad one failed. Probes are read from disk here, once per bake, not every frame.
fn collect_finished_bakes(panels: &mut Panels<'_>) {
    for completion in panels.editor.lightmaps.take_completed() {
        let lighting = &mut panels.inputs.lighting;
        match &completion.result {
            Ok(Some(_)) => {
                lighting.baked = LightingForm::baked(panels.editor, &completion);
                lighting.problem = None;
            }
            Ok(None) => {}
            Err(problem) => lighting.problem = Some(problem.to_string()),
        }
    }
}

fn invoke(intents: &mut Vec<Intent>, id: &str, arguments: Arguments) {
    intents.push(Intent::Invoke(id.into(), arguments));
}

// --- The bake ------------------------------------------------------------------------------------

fn bake_form(
    frame: &mut ToolFrame<'_>,
    form: &mut LightingForm,
    target: &LightingTarget,
    ui: &mut egui::Ui,
) {
    let shell = frame.shell;
    heading(ui, shell, "Bake lightmaps");
    ui.label(secondary(
        shell,
        "Bakes the open world as authored — its lights with their mobility, its meshes with their \
         lightmap resolution, its irradiance volumes — with the engine's path tracer, off the \
         interface thread. An unchanged world is not baked again; a cancelled bake writes nothing.",
    ));
    egui::Grid::new("lighting-bake-form")
        .num_columns(2)
        .show(ui, |ui| {
            level_settings(form, ui);
            ui.label("Level description");
            ui.text_edit_singleline(&mut form.description)
                .on_hover_text(
                    "Empty: the open world, whose .cylightmap the editor writes beside it on every \
                 bake. Or a project-relative .cylightmap to bake as it is.",
                );
            ui.end_row();
            ui.label("Output");
            ui.text_edit_singleline(&mut form.output)
                .on_hover_text("Project-relative; .cy/cooked/lightmaps/<level>.lightmap if empty.");
            ui.end_row();
        });
    let running = target.status.as_ref().is_some_and(BakeStatus::cancellable);
    let intents = &mut frame.intents;
    ui.horizontal(|ui| {
        let bake = form.bake(target.scene.is_some());
        let clicked = ui
            .add_enabled(
                !running && bake.is_ok(),
                egui::Button::new("Bake lightmaps"),
            )
            .clicked();
        if clicked && let Ok((command, arguments)) = bake {
            intents.push(Intent::Invoke(command.into(), arguments));
        }
        if let Some(status) = target.status.as_ref().filter(|status| status.cancellable())
            && ui.button("Cancel bake").clicked()
        {
            let (command, arguments) = LightingForm::cancel(status);
            intents.push(Intent::Invoke(command.into(), arguments));
        }
    });
    if let Some(status) = &target.status {
        bake_status(shell, status, ui);
    }
    if let Some(baked) = &frame.inputs.lighting.baked {
        let outcome = &baked.outcome;
        ui.label(numeric(
            shell,
            format!(
                "{} object(s) on {} page(s), {} texels, {} volume(s) of {} probes{}",
                outcome.objects,
                outcome.pages,
                outcome.texels,
                outcome.volumes,
                outcome.probes,
                if outcome.cached {
                    "; unchanged, not baked again"
                } else {
                    ""
                }
            ),
        ));
        if outcome.padding_short > 0 {
            status(
                ui,
                shell,
                Semantic::Warning,
                &format!(
                    "{} object(s) have chart padding below the bilinear and mip bound",
                    outcome.padding_short
                ),
            );
        }
    }
}

fn level_settings(form: &mut LightingForm, ui: &mut egui::Ui) {
    let settings = &mut form.settings;
    ui.label("Encoding");
    egui::ComboBox::from_id_salt("lighting-mode")
        .selected_text(settings.mode)
        .show_ui(ui, |ui| {
            for mode in MODES {
                ui.selectable_value(&mut settings.mode, mode, mode);
            }
        });
    ui.end_row();
    ui.label("Texel density (/m)");
    ui.add(
        egui::DragValue::new(&mut settings.density)
            .range(0.25..=64.0)
            .speed(0.1),
    );
    ui.end_row();
    ui.label("Page size");
    egui::ComboBox::from_id_salt("lighting-page")
        .selected_text(settings.page.to_string())
        .show_ui(ui, |ui| {
            for size in [128, 256, 512, 1024, 2048, 4096] {
                ui.selectable_value(&mut settings.page, size, size.to_string());
            }
        });
    ui.end_row();
    ui.label("Samples");
    ui.add(egui::DragValue::new(&mut settings.samples).range(1..=4096));
    ui.end_row();
    ui.label("Bounces");
    ui.add(egui::DragValue::new(&mut settings.bounces).range(0..=8));
    ui.end_row();
}

fn bake_status(shell: &Shell, status: &BakeStatus, ui: &mut egui::Ui) {
    let line = match &status.state {
        OperationState::Pending => "Queued".to_string(),
        OperationState::Running { .. } => status.step.clone(),
        OperationState::Completed => "Baked".to_string(),
        OperationState::Cancelled => "Cancelled: nothing was written".to_string(),
        OperationState::Failed(problem) => format!("Failed: {}", problem.because),
    };
    if let Some(fraction) = status.fraction {
        ui.add(egui::ProgressBar::new(fraction).text(line));
    } else {
        ui.label(secondary(shell, line));
    }
}

// --- Volumes and lights --------------------------------------------------------------------------

fn volumes(
    frame: &mut ToolFrame<'_>,
    scene: &LightingScene,
    selected: Option<NodeId>,
    ui: &mut egui::Ui,
) {
    heading(ui, frame.shell, "Irradiance volumes");
    if scene.volumes.is_empty() {
        ui.label(secondary(
            frame.shell,
            "No volume yet. A volume is a grid of probes the bake captures beside the atlas.",
        ));
    }
    for volume in &scene.volumes {
        let label = format!(
            "{}  {}×{}×{} probes",
            volume.name, volume.counts[0], volume.counts[1], volume.counts[2]
        );
        if ui
            .selectable_label(selected == Some(volume.id), label)
            .clicked()
        {
            invoke(
                &mut frame.intents,
                "edit.select",
                Arguments::new().with("entity", Value::Text(volume.id.to_string())),
            );
        }
    }
    if ui.button("Add irradiance volume").clicked() {
        invoke(
            &mut frame.intents,
            "lighting.volume.create",
            Arguments::new(),
        );
    }
    if let Some(volume) = scene
        .volumes
        .iter()
        .find(|volume| selected == Some(volume.id))
    {
        grid_editor(frame, volume, ui);
    }
}

/// The selected volume's grid, staged in the panel and applied as one command.
fn grid_editor(frame: &mut ToolFrame<'_>, volume: &Volume, ui: &mut egui::Ui) {
    let authored = Grid::of(volume);
    let inputs = &mut frame.inputs.lighting;
    if inputs
        .grid
        .is_none_or(|(node, base, _)| node != volume.id || base != authored)
    {
        inputs.grid = Some((volume.id, authored, authored));
    }
    let Some((node, _, edit)) = inputs.grid.as_mut() else {
        return;
    };
    ui.label(secondary(
        frame.shell,
        "Drag the volume with the move gizmo; its position is the first probe.",
    ));
    egui::Grid::new("lighting-grid")
        .num_columns(2)
        .show(ui, |ui| {
            ui.label("Spacing (m)");
            ui.add(
                egui::DragValue::new(&mut edit.spacing)
                    .range(0.05..=100.0)
                    .speed(0.05),
            );
            ui.end_row();
            for (axis, count) in ["X", "Y", "Z"].into_iter().zip(edit.counts.iter_mut()) {
                ui.label(format!("Probes {axis}"));
                ui.add(egui::DragValue::new(count).range(1..=64));
                ui.end_row();
            }
            ui.label("Rays per probe");
            ui.add(egui::DragValue::new(&mut edit.rays).range(1..=1024));
            ui.end_row();
        });
    if ui
        .add_enabled(*edit != authored, egui::Button::new("Apply grid"))
        .clicked()
    {
        let arguments = Arguments::new()
            .with("volume", Value::Text(node.to_string()))
            .with("spacing", Value::Float(edit.spacing))
            .with("count_x", Value::Int(edit.counts[0]))
            .with("count_y", Value::Int(edit.counts[1]))
            .with("count_z", Value::Int(edit.counts[2]))
            .with("rays", Value::Int(edit.rays));
        invoke(&mut frame.intents, "lighting.volume.set", arguments);
    }
}

fn lights(frame: &mut ToolFrame<'_>, scene: &LightingScene, ui: &mut egui::Ui) {
    heading(ui, frame.shell, "Light mobility");
    if scene.lights.is_empty() {
        ui.label(secondary(frame.shell, "No lights in this world."));
    }
    for light in &scene.lights {
        ui.horizontal(|ui| {
            ui.label(&light.name);
            if let Some(chosen) =
                mobility_combo(ui, ("lighting-mobility", light.id), light.mobility)
            {
                invoke(
                    &mut frame.intents,
                    "lighting.light.set-mobility",
                    mobility(light.id, chosen),
                );
            }
        });
    }
}

/// A mobility picker. Answers the new mobility on the frame it is chosen.
pub(super) fn mobility_combo(
    ui: &mut egui::Ui,
    id: impl std::hash::Hash + std::fmt::Debug,
    current: Mobility,
) -> Option<Mobility> {
    let mut chosen = current;
    egui::ComboBox::from_id_salt(id)
        .selected_text(current.label())
        .show_ui(ui, |ui| {
            for option in Mobility::ALL {
                ui.selectable_value(&mut chosen, option, option.label())
                    .on_hover_text(mobility_meaning(option));
            }
        });
    (chosen != current).then_some(chosen)
}

const fn mobility_meaning(mobility: Mobility) -> &'static str {
    match mobility {
        Mobility::Static => "Direct light and bounce are baked; the frame shades it no further.",
        Mobility::Stationary => {
            "Bounce and a shadow mask are baked; the frame shades its direct light."
        }
        Mobility::Movable => "Nothing is baked; dynamic GI owns it.",
    }
}

pub(super) fn mobility(node: NodeId, mobility: Mobility) -> Arguments {
    Arguments::new()
        .with("entity", Value::Text(node.to_string()))
        .with("mobility", Value::Text(mobility.keyword().into()))
}

// --- What the bake made --------------------------------------------------------------------------

fn views(frame: &mut ToolFrame<'_>, scene: Option<&LightingScene>, ui: &mut egui::Ui) {
    let shell = frame.shell;
    heading(ui, shell, "Texel density");
    ui.label(secondary(
        shell,
        "Draws every lightmapped surface as a checker of its lightmap texels: green on the level's \
         density, blue below, red above; grey has no lightmap.",
    ));
    if ui.button("Show lightmap density").clicked() {
        invoke(
            &mut frame.intents,
            &LightingForm::density_view(),
            Arguments::new(),
        );
    }
    heading(ui, shell, "Probes");
    if ui.button("Show GI probes").clicked() {
        invoke(
            &mut frame.intents,
            &LightingForm::probe_view(),
            Arguments::new(),
        );
    }
    let points = probe_points(scene, frame.inputs.lighting.baked.as_ref());
    probes(shell, &points, ui);
}

/// Every volume's probes: as the last bake captured them, or where the bake will capture them.
fn probe_points(scene: Option<&LightingScene>, baked: Option<&Baked>) -> Vec<ProbePoint> {
    let mut points = Vec::new();
    for volume in scene
        .map(|scene| scene.volumes.as_slice())
        .unwrap_or_default()
    {
        let captured = baked.and_then(|baked| {
            baked
                .probes
                .iter()
                .find(|captured| captured.id == engine_identity(volume.id))
        });
        match captured {
            Some(found) => points.extend(
                found
                    .probes
                    .iter()
                    .map(|probe| (probe.position, Some((probe.radiance, probe.valid)))),
            ),
            None => points.extend(volume.probe_positions().into_iter().map(|at| (at, None))),
        }
    }
    points
}

fn linear(colour: [f32; 3]) -> egui::Color32 {
    egui::Color32::from(egui::Rgba::from_rgb(colour[0], colour[1], colour[2]))
}

/// Probes seen from above: captured ones in their light, the authored grid where none is captured.
fn probes(shell: &Shell, points: &[ProbePoint], ui: &mut egui::Ui) {
    if points.is_empty() {
        ui.label(secondary(
            shell,
            "Add an irradiance volume to place probes.",
        ));
        return;
    }
    if points.iter().all(|(_, light)| light.is_none()) {
        ui.label(secondary(
            shell,
            "Not captured yet: hollow probes are where the bake will capture.",
        ));
    }
    let size = egui::vec2(
        ui.available_width(),
        (ui.available_height() - 4.0).max(160.0),
    );
    let (rect, _) = ui.allocate_exact_size(size, egui::Sense::hover());
    ui.painter()
        .rect_filled(rect, 0.0, theme::surface(shell.theme, Surface::Sunken));
    let place = top_down(points, rect.shrink(16.0));
    let outline = theme::role(shell.theme, Semantic::PrimaryText);
    for (position, light) in points {
        let centre = place(*position);
        match light {
            Some((radiance, true)) => {
                ui.painter()
                    .circle_filled(centre, 5.0, tone_mapped(*radiance));
                ui.painter().circle_stroke(
                    centre,
                    5.0,
                    egui::Stroke::new(1.0, outline.gamma_multiply(0.7)),
                );
            }
            Some((_, false)) => {
                let error = theme::role(shell.theme, Semantic::Error);
                ui.painter()
                    .circle_stroke(centre, 5.0, egui::Stroke::new(1.0, error));
                ui.painter().text(
                    centre,
                    egui::Align2::CENTER_CENTER,
                    "×",
                    egui::FontId::monospace(10.0),
                    error,
                );
            }
            None => {
                ui.painter().circle_stroke(
                    centre,
                    4.0,
                    egui::Stroke::new(1.0, outline.gamma_multiply(0.5)),
                );
            }
        }
    }
}

/// World x and z onto the rectangle, keeping the aspect, with y ignored (seen from above).
fn top_down(points: &[ProbePoint], rect: egui::Rect) -> impl Fn([f32; 3]) -> egui::Pos2 {
    let (mut low, mut high) = ([f32::MAX; 2], [f32::MIN; 2]);
    for (position, _) in points {
        low = [low[0].min(position[0]), low[1].min(position[2])];
        high = [high[0].max(position[0]), high[1].max(position[2])];
    }
    let extent = (high[0] - low[0]).max(high[1] - low[1]).max(1.0e-3);
    let scale = rect.width().min(rect.height()) / extent;
    let centre = rect.center();
    let middle = [(low[0] + high[0]) * 0.5, (low[1] + high[1]) * 0.5];
    move |position| {
        egui::pos2(
            centre.x + (position[0] - middle[0]) * scale,
            centre.y + (position[2] - middle[1]) * scale,
        )
    }
}

/// A probe's radiance in a display range: Reinhard per channel. Presentation only.
fn tone_mapped(radiance: [f32; 3]) -> egui::Color32 {
    linear(radiance.map(|channel| channel.max(0.0) / (1.0 + channel.max(0.0))))
}

// --- The Inspector's lighting rows -----------------------------------------------------------------

/// Mobility for a selected light and lightmap resolution for a selected placed object, as the
/// Inspector's contextual lighting rows. Each pushes the same command the lighting editor does.
pub(super) fn inspector_rows(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let Some(node) = single_selection(panels) else {
        return;
    };
    let Some(document) = panels
        .editor
        .workspace
        .active()
        .and_then(|active| panels.editor.documents.get(active))
    else {
        return;
    };
    let scene = LightingScene::read(document);
    let placed = cy_editor_viewport::gizmo::TransformBinding::of_schema(document.schema())
        .is_some_and(|binding| document.content().has_component(node, binding.component));
    let light = scene.mobility_of(node);
    if !placed && light.is_none() {
        return;
    }
    heading(ui, panels.shell, "Lighting");
    if let Some(current) = light {
        ui.horizontal(|ui| {
            ui.label("Mobility");
            if let Some(chosen) = mobility_combo(ui, ("inspector-mobility", node), current) {
                panels.intents.push(Intent::Invoke(
                    "lighting.light.set-mobility".into(),
                    mobility(node, chosen),
                ));
            }
        });
    }
    if placed && light.is_none() && !scene.volumes.iter().any(|volume| volume.id == node) {
        resolution_row(panels, ui, node, scene.resolution_of(node));
    }
}

fn single_selection(panels: &Panels<'_>) -> Option<NodeId> {
    let mut nodes = panels.editor.selection.get().nodes();
    let node = nodes.next()?;
    nodes.next().is_none().then_some(node)
}

/// A resolution field that commits once, when the drag or the typing ends. Between gestures it
/// shows the document's value, so an undo is seen at once.
fn resolution_row(panels: &mut Panels<'_>, ui: &mut egui::Ui, node: NodeId, authored: f32) {
    let staged = &mut panels.inputs.lighting.resolution;
    let mut value = match staged {
        Some((held, value)) if *held == node => *value,
        _ => authored,
    };
    let response = ui
        .horizontal(|ui| {
            ui.label("Lightmap resolution");
            ui.add(
                egui::DragValue::new(&mut value)
                    .range(0.125..=64.0)
                    .speed(0.05)
                    .suffix("×"),
            )
        })
        .inner;
    let finished = response.drag_stopped() || response.lost_focus();
    *staged = (!finished && (response.dragged() || response.has_focus())).then_some((node, value));
    if finished && (value - authored).abs() > f32::EPSILON {
        panels.intents.push(Intent::Invoke(
            "lighting.object.set-resolution".into(),
            Arguments::new()
                .with("entity", Value::Text(node.to_string()))
                .with("scale", Value::Float(value)),
        ));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_tool_lists_every_command_its_form_invokes() {
        let form = LightingForm::default();
        let (bake, _) = form.bake(true).expect("the open world bakes");
        assert!(LightingTool::OPERATIONS.contains(&bake));
        assert!(LightingTool::COMMANDS.contains(&LightingForm::density_view().as_str()));
        assert!(LightingTool::COMMANDS.contains(&LightingForm::probe_view().as_str()));
        assert!(LightingTool::COMMANDS.contains(&CANCEL_COMMAND));
    }

    #[test]
    fn a_captured_volume_is_drawn_in_its_light_and_an_uncaptured_one_where_it_will_be() {
        use cy_editor_services::lightmaps::{CapturedProbe, CapturedVolume, LightmapBakeOutcome};

        let volume = Volume {
            id: NodeId::from_u128(7),
            name: "Irradiance Volume".into(),
            origin: [0.0; 3],
            spacing: 1.0,
            counts: [2, 1, 1],
            rays: 16,
        };
        let scene = LightingScene {
            volumes: vec![volume],
            ..LightingScene::default()
        };
        let authored = probe_points(Some(&scene), None);
        assert_eq!(authored.len(), 2);
        assert!(authored.iter().all(|(_, light)| light.is_none()));

        let baked = Baked {
            outcome: LightmapBakeOutcome {
                output: "out.lightmap".into(),
                objects: 0,
                pages: 0,
                texels: 0,
                device_bytes: 0,
                mips: 0,
                padding_short: 0,
                volumes: 1,
                probes: 1,
                seconds: 0.0,
                cached: false,
            },
            probes: vec![CapturedVolume {
                id: engine_identity(NodeId::from_u128(7)),
                counts: [1, 1, 1],
                probes: vec![CapturedProbe {
                    position: [0.5, 0.0, 0.0],
                    radiance: [1.0, 0.5, 0.25],
                    valid: true,
                }],
            }],
        };
        let captured = probe_points(Some(&scene), Some(&baked));
        assert_eq!(
            captured,
            vec![([0.5, 0.0, 0.0], Some(([1.0, 0.5, 0.25], true)))]
        );
    }
}
