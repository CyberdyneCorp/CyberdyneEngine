// SPDX-License-Identifier: MIT
//! The Lighting & Lightmaps editor's global illumination half (issue #35): the world's GI
//! settings, GI volumes with their overrides, the selected object's illumination class, every
//! emitter's class with its material's rule, the nineteen GI debug views as the viewport's view
//! modes, and the engine's GI report with a clicked point explained.
//!
//! Drawing only, like the rest of [`super::lighting`]. Every edit is a registered `gi.*` command
//! in one transaction (`cy_editor_services::illumination`), so undo reverses it and an agent
//! reaches it the same way; a volume is placed by the move gizmo and sized by the scale gizmo,
//! because its translation is its centre and its scale its size. The report and the views are the
//! ENGINE's: `gi.report` asks for `illumination.report` and the view buttons are
//! `viewport.view-mode.*`, which the runtime draws over its own frame.

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_services::illumination::{
    CLASSES, EMISSION_MODES, GiVolume, IlluminationScene, MODES, Override, QUALITIES, Settings,
    TIERS,
};
use cy_editor_services::illumination_requests::IlluminationReport;
use cy_editor_services::mirror::engine_identity;
use cy_editor_viewport::viewmode::ViewMode;
use cy_editor_visual::colour::Semantic;

use super::specialised::ToolFrame;
use super::{Intent, heading, numeric, secondary, status};

/// The commands this half invokes, which [`super::lighting::LightingTool::COMMANDS`] lists; a test
/// holds the two together.
#[cfg(test)]
pub(super) const COMMANDS: [&str; 7] = [
    "gi.settings.set",
    "gi.volume.create",
    "gi.volume.set",
    "gi.object.set-class",
    "gi.emission.set",
    "gi.report",
    "edit.select",
];

/// The nineteen GI debug views, each the engine's `DebugViewMode` of the same name and drawn by
/// the runtime over its frame.
pub(super) const GI_VIEWS: [ViewMode; 19] = [
    ViewMode::GiProbes,
    ViewMode::GiProbeIrradiance,
    ViewMode::GiProbeAge,
    ViewMode::GiProbeError,
    ViewMode::GiProbeVisibility,
    ViewMode::GiRadianceClipmap,
    ViewMode::GiSurfaceCache,
    ViewMode::GiDistanceField,
    ViewMode::GiCards,
    ViewMode::GiReflectionProbes,
    ViewMode::GiScreenTrace,
    ViewMode::GiConfidence,
    ViewMode::GiTier,
    ViewMode::GiRayCount,
    ViewMode::GiDenoiserConfidence,
    ViewMode::GiDisocclusion,
    ViewMode::GiIndirectDiffuse,
    ViewMode::GiIndirectSpecular,
    ViewMode::GiEmissive,
];

/// A volume's overrides as the panel edits them.
#[derive(Clone, PartialEq, Debug)]
pub struct VolumeEdit {
    /// Higher wins.
    pub priority: i64,
    /// Metres.
    pub blend_distance: f32,
    /// 0 to 1.
    pub weight: f32,
    /// `Override::bit`s.
    pub sets: i64,
    /// `QUALITIES`.
    pub quality: String,
    /// `TIERS`.
    pub tier: String,
    /// The numeric overrides, in `Override::ALL`'s order where they are numbers.
    pub probe_density: f32,
    /// How fast its probes age.
    pub update_rate: f32,
    /// Indirect diffuse scale.
    pub diffuse_intensity: f32,
    /// Indirect specular scale.
    pub specular_intensity: f32,
    /// Metres.
    pub far_field: f32,
}

impl VolumeEdit {
    fn of(volume: &GiVolume) -> Self {
        Self {
            priority: volume.priority,
            blend_distance: volume.blend_distance,
            weight: volume.weight,
            sets: volume.sets,
            quality: volume.quality.clone(),
            tier: volume.tier.clone(),
            probe_density: volume.probe_density,
            update_rate: volume.update_rate,
            diffuse_intensity: volume.diffuse_intensity,
            specular_intensity: volume.specular_intensity,
            far_field: volume.far_field,
        }
    }

    fn arguments(&self, volume: NodeId) -> Arguments {
        Arguments::new()
            .with("volume", Value::Text(volume.to_string()))
            .with("priority", Value::Int(self.priority))
            .with("blend_distance", Value::Float(self.blend_distance))
            .with("weight", Value::Float(self.weight))
            .with("sets", Value::Int(self.sets))
            .with("quality", Value::Text(self.quality.clone()))
            .with("tier", Value::Text(self.tier.clone()))
            .with("probe_density", Value::Float(self.probe_density))
            .with("update_rate", Value::Float(self.update_rate))
            .with("diffuse_intensity", Value::Float(self.diffuse_intensity))
            .with("specular_intensity", Value::Float(self.specular_intensity))
            .with("far_field", Value::Float(self.far_field))
    }
}

/// The GI half's staged edits, kept between frames.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct GiInputs {
    /// The settings being edited, and the document's they were staged from: restaged when the
    /// document's change under them (an undo, an agent's edit).
    pub settings: Option<(Settings, Settings)>,
    /// The selected volume's overrides being edited, and the document's.
    pub volume: Option<(NodeId, VolumeEdit, VolumeEdit)>,
    /// The viewport's next click explains the point under it instead of selecting.
    pub explain_armed: bool,
}

/// What the GI half draws this frame.
pub(crate) struct GiTarget {
    /// The open world's GI, when a world is open.
    pub scene: Option<IlluminationScene>,
    /// The engine's last GI report, whether one is coming, and why the last failed.
    pub report: Option<IlluminationReport>,
    pub pending: bool,
    pub problem: Option<String>,
    /// The view mode the focused viewport is in.
    pub view_mode: ViewMode,
    /// The selected node, when it is a placed object, and every placed node's engine identity.
    pub selected: Option<NodeId>,
    pub placed: bool,
    pub nodes: Vec<(u64, NodeId, String)>,
}

fn invoke(intents: &mut Vec<Intent>, id: &str, arguments: Arguments) {
    intents.push(Intent::Invoke(id.into(), arguments));
}

fn combo(ui: &mut egui::Ui, id: &str, current: &mut String, options: &[&str]) {
    egui::ComboBox::from_id_salt(id)
        .selected_text(current.clone())
        .show_ui(ui, |ui| {
            for option in options {
                ui.selectable_value(current, (*option).to_string(), *option);
            }
        });
}

/// The controls: settings, volumes, the selected object's class.
pub(super) fn controls(frame: &mut ToolFrame<'_>, target: &GiTarget, ui: &mut egui::Ui) {
    let Some(scene) = &target.scene else {
        return;
    };
    settings(frame, scene, ui);
    volumes(frame, scene, target.selected, ui);
    if let Some(node) = target.selected.filter(|_| target.placed) {
        class_row(frame, scene, node, ui);
    }
}

fn settings(frame: &mut ToolFrame<'_>, scene: &IlluminationScene, ui: &mut egui::Ui) {
    let shell = frame.shell;
    heading(ui, shell, "Global illumination");
    ui.label(secondary(
        shell,
        "The world's dynamic GI, as a game's options menu sets it: the engine applies a change in \
         place, without rebuilding what the world's geometry did not change.",
    ));
    let inputs = &mut frame.inputs.lighting.gi;
    if inputs
        .settings
        .as_ref()
        .is_none_or(|(base, _)| *base != scene.settings)
    {
        inputs.settings = Some((scene.settings.clone(), scene.settings.clone()));
    }
    let Some((base, edit)) = inputs.settings.as_mut() else {
        return;
    };
    egui::Grid::new("gi-settings")
        .num_columns(2)
        .show(ui, |ui| {
            ui.label("Mode");
            combo(ui, "gi-mode", &mut edit.mode, &MODES);
            ui.end_row();
            ui.label("Quality");
            combo(ui, "gi-quality", &mut edit.quality, &QUALITIES);
            ui.end_row();
            ui.label("Tier");
            combo(ui, "gi-tier", &mut edit.tier, &TIERS);
            ui.end_row();
            ui.label("Hardware ray tracing");
            ui.checkbox(&mut edit.hardware_ray_tracing, "where the device has it");
            ui.end_row();
            ui.label("Budget (ms)");
            ui.add(
                egui::DragValue::new(&mut edit.budget_ms)
                    .range(0.1..=33.0)
                    .speed(0.05),
            );
            ui.end_row();
        });
    if ui
        .add_enabled(edit != base, egui::Button::new("Apply GI settings"))
        .clicked()
    {
        let arguments = Arguments::new()
            .with("mode", Value::Text(edit.mode.clone()))
            .with("quality", Value::Text(edit.quality.clone()))
            .with("tier", Value::Text(edit.tier.clone()))
            .with(
                "hardware_ray_tracing",
                Value::Bool(edit.hardware_ray_tracing),
            )
            .with("budget_ms", Value::Float(edit.budget_ms));
        invoke(&mut frame.intents, "gi.settings.set", arguments);
    }
}

fn volumes(
    frame: &mut ToolFrame<'_>,
    scene: &IlluminationScene,
    selected: Option<NodeId>,
    ui: &mut egui::Ui,
) {
    heading(ui, frame.shell, "GI volumes");
    if scene.volumes.is_empty() {
        ui.label(secondary(
            frame.shell,
            "No volume yet. A volume overrides the GI's quality, density, update rate, \
             intensities, tier or far field inside its box.",
        ));
    }
    for volume in &scene.volumes {
        let label = format!(
            "{}  {:.1} x {:.1} x {:.1} m",
            volume.name, volume.size[0], volume.size[1], volume.size[2]
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
    if ui.button("Add GI volume").clicked() {
        invoke(&mut frame.intents, "gi.volume.create", Arguments::new());
    }
    if let Some(volume) = scene
        .volumes
        .iter()
        .find(|volume| selected == Some(volume.id))
    {
        volume_editor(frame, volume, ui);
    }
}

fn override_value(ui: &mut egui::Ui, parameter: Override, edit: &mut VolumeEdit) {
    match parameter {
        Override::Quality => combo(ui, "gi-volume-quality", &mut edit.quality, &QUALITIES),
        Override::Tier => combo(ui, "gi-volume-tier", &mut edit.tier, &TIERS),
        Override::ProbeDensity => {
            ui.add(
                egui::DragValue::new(&mut edit.probe_density)
                    .range(0.1..=8.0)
                    .speed(0.05),
            );
        }
        Override::UpdateRate => {
            ui.add(
                egui::DragValue::new(&mut edit.update_rate)
                    .range(0.1..=16.0)
                    .speed(0.05),
            );
        }
        Override::DiffuseIntensity => {
            ui.add(
                egui::DragValue::new(&mut edit.diffuse_intensity)
                    .range(0.0..=8.0)
                    .speed(0.02),
            );
        }
        Override::SpecularIntensity => {
            ui.add(
                egui::DragValue::new(&mut edit.specular_intensity)
                    .range(0.0..=8.0)
                    .speed(0.02),
            );
        }
        Override::FarField => {
            ui.add(
                egui::DragValue::new(&mut edit.far_field)
                    .range(1.0..=4000.0)
                    .speed(0.5),
            );
        }
    }
}

/// The selected volume's overrides, staged and applied as one command.
fn volume_editor(frame: &mut ToolFrame<'_>, volume: &GiVolume, ui: &mut egui::Ui) {
    let authored = VolumeEdit::of(volume);
    let inputs = &mut frame.inputs.lighting.gi;
    if inputs
        .volume
        .as_ref()
        .is_none_or(|(node, base, _)| *node != volume.id || *base != authored)
    {
        inputs.volume = Some((volume.id, authored.clone(), authored.clone()));
    }
    let Some((node, base, edit)) = inputs.volume.as_mut() else {
        return;
    };
    ui.label(secondary(
        frame.shell,
        "Move it with the move gizmo and size it with the scale gizmo: its scale is its size in \
         metres. Tick what it overrides; everything else is the world's.",
    ));
    egui::Grid::new("gi-volume").num_columns(2).show(ui, |ui| {
        ui.label("Priority");
        ui.add(egui::DragValue::new(&mut edit.priority).range(-64..=64));
        ui.end_row();
        ui.label("Blend (m)");
        ui.add(
            egui::DragValue::new(&mut edit.blend_distance)
                .range(0.0..=32.0)
                .speed(0.05),
        );
        ui.end_row();
        ui.label("Weight");
        ui.add(egui::Slider::new(&mut edit.weight, 0.0..=1.0));
        ui.end_row();
        for parameter in Override::ALL {
            let mut on = edit.sets & parameter.bit() != 0;
            if ui.checkbox(&mut on, parameter.label()).changed() {
                edit.sets ^= parameter.bit();
            }
            ui.add_enabled_ui(on, |ui| override_value(ui, parameter, edit));
            ui.end_row();
        }
    });
    if ui
        .add_enabled(edit != base, egui::Button::new("Apply volume"))
        .clicked()
    {
        let arguments = edit.arguments(*node);
        invoke(&mut frame.intents, "gi.volume.set", arguments);
    }
}

fn class_row(
    frame: &mut ToolFrame<'_>,
    scene: &IlluminationScene,
    node: NodeId,
    ui: &mut egui::Ui,
) {
    heading(ui, frame.shell, "Selected object");
    if let Some(chosen) = class_combo(ui, ("gi-class", node), scene.class_of(node)) {
        invoke(
            &mut frame.intents,
            "gi.object.set-class",
            class_arguments(node, &chosen),
        );
    }
}

/// An illumination class picker. Answers the new class on the frame it is chosen.
pub(super) fn class_combo(
    ui: &mut egui::Ui,
    id: impl std::hash::Hash + std::fmt::Debug,
    current: &str,
) -> Option<String> {
    let mut chosen = current.to_owned();
    ui.horizontal(|ui| {
        ui.label("Illumination class");
        egui::ComboBox::from_id_salt(id)
            .selected_text(current)
            .show_ui(ui, |ui| {
                for option in CLASSES {
                    ui.selectable_value(&mut chosen, option.to_owned(), option)
                        .on_hover_text(class_meaning(option));
                }
            });
    });
    (chosen != current).then_some(chosen)
}

const fn class_meaning(class: &str) -> &'static str {
    match class.as_bytes() {
        b"static" => "In the caches; every move invalidates the region it left and entered.",
        b"slow-dynamic" => {
            "In the caches; moves are coalesced into one invalidation a few frames apart."
        }
        b"fast-dynamic" => {
            "A proxy that occludes the light under it and shows in reflections; in no cache."
        }
        b"transient" => "Ignored by the GI: a projectile, a spark.",
        _ => "Derived from its mobility and size, as a game's object is.",
    }
}

pub(super) fn class_arguments(node: NodeId, class: &str) -> Arguments {
    Arguments::new()
        .with("entity", Value::Text(node.to_string()))
        .with("class", Value::Text(class.to_owned()))
}

// --- What the engine says --------------------------------------------------------------------------

/// The views, the emitters and the report.
pub(super) fn views(frame: &mut ToolFrame<'_>, target: &GiTarget, ui: &mut egui::Ui) {
    debug_views(frame, target, ui);
    report(frame, target, ui);
    emitters(frame, target, ui);
}

fn debug_views(frame: &mut ToolFrame<'_>, target: &GiTarget, ui: &mut egui::Ui) {
    heading(ui, frame.shell, "GI debug views");
    ui.label(secondary(
        frame.shell,
        "Each is the engine's view of the same name, drawn by the runtime over the viewport's \
         frame from the GI's own state.",
    ));
    ui.horizontal_wrapped(|ui| {
        for mode in std::iter::once(ViewMode::Off).chain(GI_VIEWS) {
            let label = mode.label();
            if ui
                .selectable_label(target.view_mode == mode, label)
                .on_hover_text(mode.shows())
                .clicked()
            {
                invoke(&mut frame.intents, &mode.command_id(), Arguments::new());
            }
        }
    });
}

fn report(frame: &mut ToolFrame<'_>, target: &GiTarget, ui: &mut egui::Ui) {
    let shell = frame.shell;
    heading(ui, shell, "GI report");
    ui.horizontal(|ui| {
        if ui
            .add_enabled(!target.pending, egui::Button::new("Refresh report"))
            .clicked()
        {
            invoke(&mut frame.intents, "gi.report", Arguments::new());
        }
        let armed = &mut frame.inputs.lighting.gi.explain_armed;
        ui.toggle_value(armed, "Explain a point")
            .on_hover_text("The viewport's next click asks the engine what lit the surface there.");
    });
    if let Some(problem) = &target.problem {
        status(ui, shell, Semantic::Error, problem);
    }
    let Some(report) = &target.report else {
        ui.label(secondary(shell, "No report yet."));
        return;
    };
    ui.label(numeric(
        shell,
        format!(
            "{} · {} quality · {} tier · rays through {} · converged {:.0}%",
            report.mode,
            report.quality,
            report.tier,
            report.world_tier,
            report.convergence * 100.0
        ),
    ));
    ui.label(numeric(
        shell,
        format!(
            "{} cards, {} probes, {} volumes · {:.2} of {:.2} ms",
            report.cards, report.probes, report.volumes, report.measured_ms, report.allocation_ms
        ),
    ));
    ui.label(numeric(
        shell,
        format!(
            "rays: {} screen, {} software, {} hardware · hit rates {:.0}% surface, {:.0}% probes",
            report.screen_rays,
            report.software_rays,
            report.hardware_rays,
            report.surface_hit_rate * 100.0,
            report.radiance_hit_rate * 100.0
        ),
    ));
    egui::Grid::new("gi-stages").num_columns(2).show(ui, |ui| {
        for stage in &report.stages {
            ui.label(&stage.name);
            ui.label(numeric(shell, format!("{:.3} ms", stage.ms)));
            ui.end_row();
        }
    });
    if let Some(explained) = &report.explained {
        heading(ui, shell, "Explained point");
        ui.label(numeric(
            shell,
            format!(
                "{} at ({:.2}, {:.2}, {:.2}) · indirect ({:.3}, {:.3}, {:.3}) · confidence {:.2}",
                explained.cause,
                explained.point[0],
                explained.point[1],
                explained.point[2],
                explained.diffuse[0],
                explained.diffuse[1],
                explained.diffuse[2],
                explained.confidence
            ),
        ));
        ui.label(numeric(
            shell,
            format!(
                "{} probes of {} in reach, {} leak suspect(s) ({:.0}% of the weight) · rays via {} \
                 · region {:.0}% converged",
                explained.probes_contributing,
                explained.probes_in_reach,
                explained.leak_suspects,
                explained.leak_weight * 100.0,
                explained.world_tier,
                explained.convergence * 100.0
            ),
        ));
        for source in &explained.sources {
            ui.label(secondary(
                shell,
                format!(
                    "{}{}: {:.0}% of the weight, confidence {:.2}",
                    source.name,
                    if source.excluded { " (excluded)" } else { "" },
                    source.weight_share * 100.0,
                    source.confidence
                ),
            ));
        }
    }
}

/// Every emitter the engine classified, each with its material's rule.
fn emitters(frame: &mut ToolFrame<'_>, target: &GiTarget, ui: &mut egui::Ui) {
    let shell = frame.shell;
    heading(ui, shell, "Emitters");
    let Some(report) = &target.report else {
        ui.label(secondary(
            shell,
            "Refresh the report to see how the engine classified the world's emitters.",
        ));
        return;
    };
    if report.emitters.is_empty() {
        ui.label(secondary(shell, "Nothing in this world emits light."));
        return;
    }
    ui.label(numeric(
        shell,
        format!(
            "power: {:.2} promoted to lights, {:.2} in the cache, {:.2} over the light budget, {:.2} off",
            report.promoted_power, report.cached_power, report.demoted_power, report.disabled_power
        ),
    ));
    let rules = target.scene.as_ref();
    egui::Grid::new("gi-emitters")
        .num_columns(4)
        .show(ui, |ui| {
            for emitter in &report.emitters {
                let object = target
                    .nodes
                    .iter()
                    .find(|(identity, _, _)| *identity == emitter.node)
                    .map_or_else(
                        || format!("{:016x}", emitter.node),
                        |(_, _, name)| name.clone(),
                    );
                ui.label(object);
                ui.label(&emitter.material);
                ui.label(numeric(
                    shell,
                    format!(
                        "{} · {:.2} m² · power {:.2}",
                        emitter.class, emitter.area, emitter.power
                    ),
                ));
                let current =
                    rules.map_or("automatic", |scene| scene.emission_of(&emitter.material));
                let mut chosen = current.to_owned();
                egui::ComboBox::from_id_salt(("gi-emission", &emitter.material, emitter.node))
                    .selected_text(current)
                    .show_ui(ui, |ui| {
                        for mode in EMISSION_MODES {
                            ui.selectable_value(&mut chosen, mode.to_owned(), mode);
                        }
                    });
                if chosen != current {
                    invoke(
                        &mut frame.intents,
                        "gi.emission.set",
                        Arguments::new()
                            .with("material", Value::Text(emitter.material.clone()))
                            .with("mode", Value::Text(chosen)),
                    );
                }
                ui.end_row();
            }
        });
}

/// What an armed explain made of a viewport click.
#[derive(Debug, PartialEq)]
pub(super) enum ArmedExplain {
    /// Nothing is armed, or it was not a click: the click selects as usual.
    NotArmed,
    /// The click was on a frame the viewport no longer shows: nothing is sent, and the explain
    /// stays armed for the next click.
    Held,
    /// The `gi.report` to invoke.
    Explain(Intent),
}

/// When an explain is armed, the `gi.report` a viewport click becomes instead of a selection: the
/// engine explains the surface under the clicked pixel of the frame on screen.
pub(super) fn armed_explain(
    inputs: &mut GiInputs,
    editor: &cy_editor_services::Editor,
    request: &cy_editor_viewport::PickRequest,
) -> ArmedExplain {
    if !inputs.explain_armed {
        return ArmedExplain::NotArmed;
    }
    let cy_editor_viewport::PickIntent::Click { x, y } = request.intent else {
        return ArmedExplain::NotArmed;
    };
    let latest = editor.viewports.focused().stream.latest();
    let Some(image) = super::navigation_baking::clicked_image(request, latest) else {
        return ArmedExplain::Held;
    };
    let [x, y] = super::navigation_baking::frame_pixel(request, Some(image), x, y);
    inputs.explain_armed = false;
    let arguments = Arguments::new()
        .with(
            "viewport",
            Value::Int(i64::try_from(request.viewport.as_u64()).unwrap_or(0)),
        )
        .with(
            "frame",
            Value::Int(i64::try_from(request.frame.as_u64()).unwrap_or(0)),
        )
        .with("x", Value::Float(x))
        .with("y", Value::Float(y));
    ArmedExplain::Explain(Intent::Invoke("gi.report".into(), arguments))
}

/// The engine identity, node and name of every node, for naming what the report names.
pub(super) fn node_names(document: &cy_editor_documents::Document) -> Vec<(u64, NodeId, String)> {
    let content = document.content();
    content
        .nodes()
        .map(|node| {
            let name = content
                .node(node)
                .map(|state| state.name.clone())
                .unwrap_or_default();
            (engine_identity(node), node, name)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_gi_views_are_the_engines_nineteen_and_each_has_a_command() {
        let names: Vec<&str> = GI_VIEWS.iter().map(|mode| mode.engine_name()).collect();
        assert_eq!(names.len(), 19);
        assert!(names.iter().all(|name| name.starts_with("Gi")));
        assert!(
            !names.contains(&"GiContribution"),
            "the frame's own term, not a GI view"
        );
        for mode in GI_VIEWS {
            assert!(mode.command_id().starts_with("viewport.view-mode.gi-"));
        }
    }

    #[test]
    fn an_armed_explain_turns_a_click_on_the_frame_on_screen_into_one_report() {
        use cy_editor_protocol::FrameId;
        use cy_editor_viewport::transport::{
            FrameImage, Mailbox, MailboxTransport, PresentedFrame, TransportKind,
        };
        use cy_editor_viewport::{PickIntent, PickRequest, ViewportId};

        let mut editor = cy_editor_services::Editor::default();
        let mailbox = Mailbox::new();
        let state = editor.viewports.focused().state.clone();
        mailbox.publish(PresentedFrame::new(
            FrameId::from_raw(7),
            state,
            FrameImage::Surface(0),
            0,
        ));
        let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox);
        editor.viewports.focused_mut().pump(&mut transport, 1_000);
        let click = |frame: u64| {
            PickRequest::for_frame(
                ViewportId::from_raw(0),
                FrameId::from_raw(frame),
                PickIntent::Click { x: 40.0, y: 30.0 },
            )
        };
        // Nothing armed: the click selects as usual.
        let mut inputs = GiInputs::default();
        assert_eq!(
            armed_explain(&mut inputs, &editor, &click(7)),
            ArmedExplain::NotArmed
        );
        // Armed, on a frame no longer on screen: held, and still armed.
        inputs.explain_armed = true;
        assert_eq!(
            armed_explain(&mut inputs, &editor, &click(6)),
            ArmedExplain::Held
        );
        assert!(inputs.explain_armed);
        // Armed, on the frame on screen: one `gi.report` naming that frame and pixel, and disarmed.
        let ArmedExplain::Explain(Intent::Invoke(command, arguments)) =
            armed_explain(&mut inputs, &editor, &click(7))
        else {
            panic!("an armed click explains the point");
        };
        assert_eq!(command, "gi.report");
        assert_eq!(arguments.get("frame"), Some(&Value::Int(7)));
        assert_eq!(arguments.get("x"), Some(&Value::Float(40.0)));
        assert!(!inputs.explain_armed);
    }

    #[test]
    fn a_volume_edit_carries_every_field_the_command_takes() {
        let edit = VolumeEdit {
            priority: 2,
            blend_distance: 0.5,
            weight: 0.75,
            sets: Override::Tier.bit(),
            quality: "high".into(),
            tier: "world-only".into(),
            probe_density: 2.0,
            update_rate: 3.0,
            diffuse_intensity: 0.5,
            specular_intensity: 1.5,
            far_field: 30.0,
        };
        let arguments = edit.arguments(NodeId::from_u128(7));
        assert_eq!(arguments.get("sets"), Some(&Value::Int(32)));
        assert_eq!(arguments.text("tier"), Some("world-only"));
        assert_eq!(arguments.get("far_field"), Some(&Value::Float(30.0)));
    }
}
