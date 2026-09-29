// SPDX-License-Identifier: MIT
//! The Navigation panel: navigation worlds, their settings, the bake, overlays, the Nav*
//! components and the test path. Issue #28, tasks 5.2 and 5.3.
//!
//! --- A CLIENT OF THE COMMANDS, AND THROUGH THEM OF THE ENGINE ------------------------------------
//!
//! The panel reads two things: the document through `NavmeshSettings::read`, and the answers the
//! engine's navigation service gave through `Editor::navmesh`. Everything it changes is an
//! `Intent::Invoke` of a registered `navigation.*` command, so a button here and the same tool
//! over MCP are one registry call and one transaction. It never bakes, hashes, raycasts or edits
//! a field itself.
//!
//! It is a [`SpecialisedTool`]: the frame in `panels/specialised.rs` draws the title, Undo/Redo
//! and the diagnostics area, and refuses startup if a command in [`NavigationTool::COMMANDS`] is
//! not an undoable or read-only MCP tool.
//!
//! --- THE ARMED PICK --------------------------------------------------------------------------------
//!
//! "Pick start", "Pick end" and "Place link" arm [`NavigationInputs::armed`]. The viewport's next
//! click then asks the engine for the navmesh point under the pixel (`navigation.point.pick`)
//! instead of selecting ([`armed_pick`]), and the answer lands here on a later frame
//! ([`settle_pick`]). A link needs two answers and records ONE `navigation.link.add`.

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::Session;
use cy_editor_services::Editor;
use cy_editor_services::nav_bake::{NavBackend, NavPathResult, NavSettingsBlock, NavStatusReport};
use cy_editor_services::navmesh::{NavmeshSettings, OVERLAYS, PLACEABLE};
use cy_editor_services::navmesh_service::{NavBakeState, NavmeshService};
use cy_editor_viewport::transport::{FrameImage, PresentedFrame};
use cy_editor_viewport::{PickIntent, PickRequest};
use cy_editor_visual::colour::Semantic;

use super::specialised::{SpecialisedTool, ToolDiagnostic, ToolFrame};
use super::{Inputs, Intent, Panels, heading, nothing_here, numeric, secondary, status};

/// What an armed viewport click is for.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PickTarget {
    /// The test path's start point.
    PathStart,
    /// The test path's end point.
    PathEnd,
    /// The first endpoint of a NavLink being placed.
    LinkFrom,
    /// The second endpoint; its answer records the link.
    LinkTo,
}

impl PickTarget {
    /// What the viewport's next click will do, for the panel to say.
    #[must_use]
    pub const fn prompt(self) -> &'static str {
        match self {
            PickTarget::PathStart => "Click the viewport to pick the path start.",
            PickTarget::PathEnd => "Click the viewport to pick the path end.",
            PickTarget::LinkFrom => "Click the viewport to pick where the link starts.",
            PickTarget::LinkTo => "Click the viewport to pick where the link ends.",
        }
    }
}

/// The panel's own presentation state. None of it is document state.
#[derive(Clone, PartialEq, Debug)]
pub struct NavigationInputs {
    /// The navigation world the panel shows; `None` follows the first one.
    pub world: Option<u32>,
    /// Settings being edited and not yet applied, with the world they belong to.
    pub draft: Option<(u32, NavSettingsBlock)>,
    /// The viewport's next click picks a navmesh point for this, rather than selecting.
    pub armed: Option<PickTarget>,
    /// A pick sent to the engine: what it is for, and `NavmeshService::pick_answers` when sent.
    pub awaiting: Option<(PickTarget, u64)>,
    /// The test path's start point.
    pub start: Option<[f32; 3]>,
    /// The test path's end point.
    pub end: Option<[f32; 3]>,
    /// The first endpoint of the link being placed.
    pub link_from: Option<[f32; 3]>,
    /// Half the side of the flow-field region around its target, metres.
    pub flow_half_extent: f32,
    /// Flow-field cell size, metres.
    pub flow_cell: f32,
    /// The last thing that went wrong in this panel.
    pub problem: Option<String>,
}

impl Default for NavigationInputs {
    fn default() -> Self {
        Self {
            world: None,
            draft: None,
            armed: None,
            awaiting: None,
            start: None,
            end: None,
            link_from: None,
            flow_half_extent: 8.0,
            flow_cell: 0.5,
            problem: None,
        }
    }
}

/// The editable settings, as `navigation.settings.set` names them.
const SETTINGS: [(&str, &str); 7] = [
    ("agent_radius", "Agent radius"),
    ("agent_height", "Agent height"),
    ("max_slope", "Max slope"),
    ("step_height", "Step height"),
    ("cell_size", "Cell size"),
    ("cell_height", "Cell height"),
    ("tile_size", "Tile size"),
];

fn setting<'b>(block: &'b mut NavSettingsBlock, name: &str) -> Option<&'b mut f32> {
    match name {
        "agent_radius" => Some(&mut block.agent_radius),
        "agent_height" => Some(&mut block.agent_height),
        "max_slope" => Some(&mut block.max_slope),
        "step_height" => Some(&mut block.step_height),
        "cell_size" => Some(&mut block.cell_size),
        "cell_height" => Some(&mut block.cell_height),
        "tile_size" => Some(&mut block.tile_size),
        _ => None,
    }
}

fn world_value(world: u32) -> Value {
    Value::Int(i64::from(world))
}

fn invoke(id: &str, arguments: Arguments) -> Intent {
    Intent::Invoke(id.into(), arguments)
}

fn vec3_text([x, y, z]: [f32; 3]) -> String {
    format!("{x}, {y}, {z}")
}

/// The one transaction a two-point link placement records.
fn link_add(world: u32, from: [f32; 3], to: [f32; 3]) -> Intent {
    invoke(
        "navigation.link.add",
        Arguments::new().with("world", world_value(world)).with(
            "values",
            Value::Text(format!("from={}; to={}", vec3_text(from), vec3_text(to))),
        ),
    )
}

// --- The viewport half of the armed pick -----------------------------------------------------------

/// The pixel of the rendered frame under a click made in the panel's own pixels. The runtime
/// renders at its own size, so a shared texture's extent rescales the click; a scale is not a
/// projection, and the ray is still the engine's.
fn frame_pixel(request: &PickRequest, image: Option<&FrameImage>, x: f32, y: f32) -> [f32; 2] {
    let Some(FrameImage::SharedTexture { image, .. }) = image else {
        return [x, y];
    };
    if request.display_width == 0 || request.display_height == 0 {
        return [x, y];
    }
    #[allow(
        clippy::cast_precision_loss,
        reason = "frame and panel extents are a few thousand pixels"
    )]
    let scale = [
        image.width as f32 / request.display_width as f32,
        image.height as f32 / request.display_height as f32,
    ];
    [x * scale[0], y * scale[1]]
}

/// What an armed pick made of a viewport click.
#[derive(Debug)]
pub(super) enum ArmedClick {
    /// No pick is armed: the click selects as usual.
    NotArmed,
    /// The click was taken but nothing was sent; the panel says why and the pick stays armed.
    Held,
    /// The `navigation.point.pick` to invoke.
    Pick(Intent),
}

/// The image of the frame the click was made on: the stream keeps only its newest frame, so a
/// click on an older one cannot be rescaled honestly and is refused instead.
fn clicked_image<'a>(
    request: &PickRequest,
    latest: Option<&'a PresentedFrame>,
) -> Option<&'a FrameImage> {
    latest
        .filter(|frame| frame.frame == request.frame)
        .map(|frame| &frame.image)
}

/// When a navmesh pick is armed, the `navigation.point.pick` a viewport click becomes instead of
/// a selection.
pub(super) fn armed_pick(
    inputs: &mut NavigationInputs,
    editor: &Editor,
    request: &PickRequest,
) -> ArmedClick {
    let Some(target) = inputs.armed else {
        return ArmedClick::NotArmed;
    };
    let PickIntent::Click { x, y } = request.intent else {
        return ArmedClick::NotArmed;
    };
    let latest = editor.viewports.focused().stream.latest();
    let Some(image) = clicked_image(request, latest) else {
        inputs.problem = Some(
            "The viewport showed a newer frame before the click was read; click again.".into(),
        );
        return ArmedClick::Held;
    };
    let [x, y] = frame_pixel(request, Some(image), x, y);
    inputs.armed = None;
    inputs.awaiting = Some((target, editor.navmesh.pick_answers()));
    let arguments = Arguments::new()
        .with("world", world_value(inputs.world.unwrap_or(1)))
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
    ArmedClick::Pick(invoke(NAVIGATION_PICK_COMMAND, arguments))
}

/// The command an armed click invokes.
pub(crate) const NAVIGATION_PICK_COMMAND: &str = "navigation.point.pick";

/// The pick's invoke was refused (another navigation request pending, no runtime): nothing is
/// awaited any more, so a later, unrelated pick answer cannot place a point the user did not
/// click. The pick is armed again for another click.
pub(crate) fn pick_refused(inputs: &mut NavigationInputs, because: &str) {
    if let Some((target, _)) = inputs.awaiting.take() {
        inputs.armed = Some(target);
        inputs.problem = Some(format!("The pick was not sent: {because}"));
    }
}

/// Take the engine's answer to an awaited pick, once it has arrived.
pub(super) fn settle_pick(
    inputs: &mut NavigationInputs,
    service: &NavmeshService,
    intents: &mut Vec<Intent>,
) {
    let Some((target, sent)) = inputs.awaiting else {
        return;
    };
    if service.pick_answers() <= sent {
        return;
    }
    inputs.awaiting = None;
    match service.pick().filter(|pick| pick.hit) {
        Some(pick) => place(inputs, target, pick.point, intents),
        None => {
            inputs.problem = Some("The click missed the navmesh, so nothing was placed.".into());
        }
    }
}

fn place(
    inputs: &mut NavigationInputs,
    target: PickTarget,
    point: [f32; 3],
    intents: &mut Vec<Intent>,
) {
    inputs.problem = None;
    match target {
        PickTarget::PathStart => inputs.start = Some(point),
        PickTarget::PathEnd => inputs.end = Some(point),
        PickTarget::LinkFrom => {
            inputs.link_from = Some(point);
            inputs.armed = Some(PickTarget::LinkTo);
        }
        PickTarget::LinkTo => {
            if let Some(from) = inputs.link_from.take() {
                intents.push(link_add(inputs.world.unwrap_or(1), from, point));
            }
        }
    }
}

// --- The panel --------------------------------------------------------------------------------------

/// The Navigation panel, drawn in the specialised-editor frame (`panels/specialised.rs`), which
/// gives it the title, Undo/Redo over the document's history, the diagnostics area and the check
/// that every command below is an undoable MCP peer.
pub(crate) struct NavigationTool;

/// What the body shows this frame: the document's navigation worlds and a copy of the engine's
/// last answers, taken while the tool can still see the editor.
pub(crate) struct NavigationView {
    worlds: Vec<NavmeshSettings>,
    bake: NavBakeState,
    busy: bool,
    status: Option<NavStatusReport>,
    /// Terrain regions whose navigation the engine flagged stale since the last bake.
    terrain_stale: usize,
    path: Option<String>,
    flow: Option<String>,
    query_failure: Option<String>,
}

impl NavigationView {
    fn capture(
        worlds: Vec<NavmeshSettings>,
        service: &NavmeshService,
        terrain: &cy_editor_services::terrain_engine::TerrainEngine,
    ) -> Self {
        Self {
            worlds,
            bake: service.bake_state().clone(),
            busy: service.pending_request().is_some(),
            status: service.status_report().cloned(),
            terrain_stale: terrain.stale_navigation().len(),
            path: service.path().map(path_summary),
            flow: service.flow_field().map(|flow| {
                format!(
                    "{} by {} cells, {} unreachable",
                    flow.width, flow.depth, flow.unreachable
                )
            }),
            query_failure: service
                .query_failure()
                .map(|(query, failure)| format!("{}: {}", query.operation(), failure.summary())),
        }
    }
}

fn path_summary(path: &NavPathResult) -> String {
    let verdict = match (path.found, path.partial) {
        (false, _) => "No path",
        (true, true) => "Partial path",
        (true, false) => "Complete path",
    };
    format!(
        "{verdict}: cost {:.2}, {} points",
        path.cost,
        path.points.len()
    )
}

impl SpecialisedTool for NavigationTool {
    const DOMAIN: Domain = Domain::NavigationBaking;
    const TITLE: &'static str = "Navigation";
    const COMMANDS: &'static [&'static str] = &[
        "navigation.world.create",
        "navigation.settings.set",
        "navigation.bake",
        "navigation.bake.status",
        "navigation.overlay.set",
        "navigation.surface.add",
        "navigation.obstacle.add",
        "navigation.area.add",
        "navigation.link.add",
        "navigation.path.query",
        "navigation.flowfield.query",
        "navigation.point.pick",
    ];

    type Target = NavigationView;

    fn target(panels: &mut Panels<'_>, ui: &mut egui::Ui) -> Option<Self::Target> {
        settle_pick(
            &mut panels.inputs.navigation,
            &panels.editor.navmesh,
            panels.intents,
        );
        let Some(document) = panels
            .editor
            .workspace
            .active()
            .and_then(|id| panels.editor.documents.get(id))
        else {
            nothing_here(
                ui,
                panels.shell,
                "No world is open.",
                "Open a world to author and bake its navigation.",
            );
            return None;
        };
        let worlds = NavmeshSettings::read(document);
        if worlds.is_empty() {
            ui.label(secondary(
                panels.shell,
                "This world has no navigation world yet. Create one, add a Nav Mesh Surface, then bake.",
            ));
            if ui.button("Create navigation world").clicked() {
                panels.intents.push(create_world());
            }
            return None;
        }
        Some(NavigationView::capture(
            worlds,
            &panels.editor.navmesh,
            &panels.editor.terrain,
        ))
    }

    fn diagnostics(inputs: &Inputs) -> Vec<ToolDiagnostic> {
        inputs
            .navigation
            .problem
            .iter()
            .map(ToolDiagnostic::error)
            .collect()
    }

    fn body(
        frame: &mut ToolFrame<'_>,
        _session: Session<'_>,
        view: Self::Target,
        ui: &mut egui::Ui,
    ) {
        let shell = frame.shell;
        let inputs = &mut frame.inputs.navigation;
        let intents = &mut frame.intents;
        egui::ScrollArea::vertical().show(ui, |ui| {
            let current = world_picker(ui, inputs, &view.worlds, intents);
            settings(ui, shell, inputs, current, intents);
            bake(ui, shell, &view, current, intents);
            overlays(ui, shell, current, intents);
            components(ui, shell, inputs, current.world, intents);
            test_path(ui, shell, inputs, &view, current.world, intents);
            flow_field(ui, shell, inputs, &view, current.world, intents);
        });
    }
}

fn create_world() -> Intent {
    invoke("navigation.world.create", Arguments::new())
}

/// The world picker. Returns the world shown; `worlds` is never empty here.
fn world_picker<'w>(
    ui: &mut egui::Ui,
    inputs: &mut NavigationInputs,
    worlds: &'w [NavmeshSettings],
    intents: &mut Vec<Intent>,
) -> &'w NavmeshSettings {
    let current = worlds
        .iter()
        .find(|world| Some(world.world) == inputs.world)
        .unwrap_or(&worlds[0]);
    inputs.world = Some(current.world);
    ui.horizontal(|ui| {
        let label = ui.label("Navigation world");
        egui::ComboBox::from_id_salt("navigation-world")
            .selected_text(format!("World {}", current.world))
            .show_ui(ui, |ui| {
                for world in worlds {
                    ui.selectable_value(
                        &mut inputs.world,
                        Some(world.world),
                        format!("World {}", world.world),
                    );
                }
            })
            .response
            .labelled_by(label.id);
        if ui.button("New world").clicked() {
            intents.push(create_world());
        }
    });
    current
}

fn settings(
    ui: &mut egui::Ui,
    shell: &Shell,
    inputs: &mut NavigationInputs,
    current: &NavmeshSettings,
    intents: &mut Vec<Intent>,
) {
    heading(ui, shell, "Agent and build settings");
    if inputs.draft.is_none_or(|(world, _)| world != current.world) {
        inputs.draft = Some((current.world, current.settings));
    }
    let Some((_, draft)) = inputs.draft.as_mut() else {
        return;
    };
    egui::Grid::new("navigation-settings")
        .num_columns(2)
        .show(ui, |ui| {
            for (name, label) in SETTINGS {
                let label = ui.label(label);
                if let Some(value) = setting(draft, name) {
                    ui.add(egui::DragValue::new(value).speed(0.01).range(0.0..=1000.0))
                        .labelled_by(label.id);
                }
                ui.end_row();
            }
            let label = ui.label("Voxeliser");
            egui::ComboBox::from_id_salt("navigation-backend")
                .selected_text(draft.backend.keyword())
                .show_ui(ui, |ui| {
                    for backend in [NavBackend::Engine, NavBackend::Recast] {
                        ui.selectable_value(&mut draft.backend, backend, backend.keyword());
                    }
                })
                .response
                .labelled_by(label.id);
            ui.end_row();
        });
    let changes = changed_settings(&current.settings, draft);
    ui.horizontal(|ui| {
        let apply = ui.add_enabled(!changes.is_empty(), egui::Button::new("Apply settings"));
        if apply.clicked() {
            intents.push(invoke(
                "navigation.settings.set",
                Arguments::new()
                    .with("world", world_value(current.world))
                    .with("values", Value::Text(changes.join("; "))),
            ));
        }
        if ui
            .add_enabled(!changes.is_empty(), egui::Button::new("Revert"))
            .clicked()
        {
            *draft = current.settings;
        }
    });
}

/// `field=value` for every setting the draft changes: one gesture, one transaction.
fn changed_settings(stored: &NavSettingsBlock, draft: &NavSettingsBlock) -> Vec<String> {
    let mut stored = *stored;
    let mut draft = *draft;
    let mut changes: Vec<String> = SETTINGS
        .iter()
        .filter_map(|(name, _)| {
            let before = *setting(&mut stored, name)?;
            let after = *setting(&mut draft, name)?;
            (before.to_bits() != after.to_bits()).then(|| format!("{name}={after}"))
        })
        .collect();
    if stored.backend != draft.backend {
        changes.push(format!("backend={}", draft.backend.keyword()));
    }
    changes
}

fn bake(
    ui: &mut egui::Ui,
    shell: &Shell,
    view: &NavigationView,
    current: &NavmeshSettings,
    intents: &mut Vec<Intent>,
) {
    heading(ui, shell, "Bake");
    let world = world_value(current.world);
    ui.horizontal(|ui| {
        let busy = view.busy;
        if ui.add_enabled(!busy, egui::Button::new("Bake")).clicked() {
            intents.push(invoke(
                "navigation.bake",
                Arguments::new().with("world", world.clone()),
            ));
        }
        if ui
            .add_enabled(!busy, egui::Button::new("Check for changes"))
            .clicked()
        {
            intents.push(invoke(
                "navigation.bake.status",
                Arguments::new()
                    .with("world", world)
                    .with("refresh", Value::Bool(true)),
            ));
        }
    });
    bake_state(ui, shell, &view.bake);
    let (role, text) = freshness(view.status.as_ref(), view.terrain_stale, current);
    status(ui, shell, role, &text);
}

fn bake_state(ui: &mut egui::Ui, shell: &Shell, state: &NavBakeState) {
    match state {
        NavBakeState::Idle => {}
        NavBakeState::Pending { progress, .. } => {
            let (done, total) = progress.map_or((0, 0), |progress| (progress.done, progress.total));
            #[allow(
                clippy::cast_precision_loss,
                reason = "tile counts are far below f32's exact integer range"
            )]
            let fraction = if total == 0 {
                0.0
            } else {
                done as f32 / total as f32
            };
            ui.add(
                egui::ProgressBar::new(fraction.clamp(0.0, 1.0))
                    .text(format!("Baking: {done} of {total} tiles")),
            );
        }
        NavBakeState::Completed { report, .. } => {
            let counters = &report.counters;
            ui.label(numeric(
                shell,
                format!(
                    "{} tiles built, {} empty, {} polygons, {} link failures",
                    counters.tiles_built,
                    counters.tiles_empty,
                    counters.polys,
                    report.link_failures
                ),
            ));
        }
        NavBakeState::Failed { failure, .. } => {
            status(ui, shell, Semantic::Error, &failure.summary());
        }
        NavBakeState::Cancelled { .. } => {
            status(ui, shell, Semantic::Warning, "The bake was cancelled.");
        }
    }
}

/// The stale badge: the engine's answer for this world, or what the document records. Terrain
/// edits are the same stale flag: the engine flags their regions and the next bake consumes them.
fn freshness(
    report: Option<&NavStatusReport>,
    terrain_stale: usize,
    current: &NavmeshSettings,
) -> (Semantic, String) {
    if current.bake_identity == 0 {
        return (Semantic::Neutral, "Not baked".into());
    }
    let report = report.filter(|report| report.world == current.world);
    match report {
        Some(report) if report.sidecar_missing => (
            Semantic::Warning,
            "Sidecar missing: bake again to rebuild the navmesh".into(),
        ),
        Some(report) if report.stale => (
            Semantic::Warning,
            "Stale: the sources changed since this bake".into(),
        ),
        _ if terrain_stale > 0 => (
            Semantic::Warning,
            "Stale: the terrain changed since this bake".into(),
        ),
        Some(_) => (Semantic::Live, "Up to date".into()),
        None => (
            Semantic::Neutral,
            format!("Baked: {} tiles; not checked yet", current.tile_count),
        ),
    }
}

fn overlays(
    ui: &mut egui::Ui,
    shell: &Shell,
    current: &NavmeshSettings,
    intents: &mut Vec<Intent>,
) {
    heading(ui, shell, "Overlays");
    let enabled = i64::from(current.overlay);
    ui.horizontal_wrapped(|ui| {
        for (name, bit) in OVERLAYS {
            let mut on = enabled & bit != 0;
            if ui.checkbox(&mut on, name).changed() {
                let flags = if on { enabled | bit } else { enabled & !bit };
                intents.push(invoke(
                    "navigation.overlay.set",
                    Arguments::new()
                        .with("world", world_value(current.world))
                        .with("overlays", Value::Text(overlay_words(flags))),
                ));
            }
        }
    });
}

fn overlay_words(flags: i64) -> String {
    let words: Vec<&str> = OVERLAYS
        .iter()
        .filter(|(_, bit)| flags & bit != 0)
        .map(|(name, _)| *name)
        .collect();
    if words.is_empty() {
        "none".into()
    } else {
        words.join(", ")
    }
}

fn components(
    ui: &mut egui::Ui,
    shell: &Shell,
    inputs: &mut NavigationInputs,
    world: u32,
    intents: &mut Vec<Intent>,
) {
    heading(ui, shell, "Components");
    ui.horizontal_wrapped(|ui| {
        for spec in &PLACEABLE {
            if ui.button(format!("Add {}", spec.label)).clicked() {
                intents.push(invoke(
                    &format!("navigation.{}.add", spec.noun),
                    Arguments::new().with("world", world_value(world)),
                ));
            }
        }
    });
    if ui.button("Place link in viewport").clicked() {
        arm(inputs, PickTarget::LinkFrom);
    }
    ui.label(secondary(
        shell,
        "Select a component to edit its fields in the Inspector.",
    ));
}

fn arm(inputs: &mut NavigationInputs, target: PickTarget) {
    inputs.armed = Some(target);
    inputs.awaiting = None;
    inputs.problem = None;
    if target == PickTarget::LinkFrom {
        inputs.link_from = None;
    }
}

fn point_text(point: Option<[f32; 3]>) -> String {
    point.map_or_else(|| "not picked".into(), vec3_text)
}

fn test_path(
    ui: &mut egui::Ui,
    shell: &Shell,
    inputs: &mut NavigationInputs,
    view: &NavigationView,
    world: u32,
    intents: &mut Vec<Intent>,
) {
    heading(ui, shell, "Test path");
    pick_prompt(ui, shell, inputs);
    ui.horizontal(|ui| {
        if ui.button("Pick start").clicked() {
            arm(inputs, PickTarget::PathStart);
        }
        ui.label(numeric(shell, point_text(inputs.start)));
    });
    ui.horizontal(|ui| {
        if ui.button("Pick end").clicked() {
            arm(inputs, PickTarget::PathEnd);
        }
        ui.label(numeric(shell, point_text(inputs.end)));
    });
    let endpoints = inputs.start.zip(inputs.end);
    if ui
        .add_enabled(endpoints.is_some(), egui::Button::new("Find path"))
        .clicked()
        && let Some((start, end)) = endpoints
    {
        intents.push(invoke(
            "navigation.path.query",
            Arguments::new()
                .with("world", world_value(world))
                .with("start", Value::Vec3(start))
                .with("end", Value::Vec3(end)),
        ));
    }
    if let Some(path) = &view.path {
        ui.label(numeric(shell, path));
    }
}

fn pick_prompt(ui: &mut egui::Ui, shell: &Shell, inputs: &mut NavigationInputs) {
    let prompt = match (inputs.armed, inputs.awaiting) {
        (Some(target), _) => target.prompt(),
        (None, Some(_)) => "Waiting for the engine to resolve the click.",
        (None, None) => return,
    };
    ui.horizontal(|ui| {
        status(ui, shell, Semantic::Active, prompt);
        if ui.button("Cancel pick").clicked() {
            inputs.armed = None;
            inputs.awaiting = None;
            inputs.link_from = None;
        }
    });
}

fn flow_field(
    ui: &mut egui::Ui,
    shell: &Shell,
    inputs: &mut NavigationInputs,
    view: &NavigationView,
    world: u32,
    intents: &mut Vec<Intent>,
) {
    heading(ui, shell, "Flow field");
    ui.horizontal(|ui| {
        let label = ui.label("Region half size");
        ui.add(egui::DragValue::new(&mut inputs.flow_half_extent).range(0.5..=1000.0))
            .labelled_by(label.id);
        let label = ui.label("Cell size");
        ui.add(
            egui::DragValue::new(&mut inputs.flow_cell)
                .range(0.05..=100.0)
                .speed(0.01),
        )
        .labelled_by(label.id);
    });
    let target = inputs.end;
    if ui
        .add_enabled(
            target.is_some(),
            egui::Button::new("Build flow field to end"),
        )
        .clicked()
        && let Some(target) = target
    {
        let extent = inputs.flow_half_extent;
        let [x, y, z] = target;
        intents.push(invoke(
            "navigation.flowfield.query",
            Arguments::new()
                .with("world", world_value(world))
                .with("target", Value::Vec3(target))
                .with("min", Value::Vec3([x - extent, y - extent, z - extent]))
                .with("max", Value::Vec3([x + extent, y + extent, z + extent]))
                .with("cell", Value::Float(inputs.flow_cell)),
        ));
    }
    if let Some(flow) = &view.flow {
        ui.label(numeric(shell, flow));
    }
    if let Some(failure) = &view.query_failure {
        ui.label(secondary(shell, failure));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_label_this_panel_draws_uses_the_engines_vocabulary() {
        let mut labels: Vec<String> = [
            "Navigation",
            "Navigation world",
            "Create navigation world",
            "New world",
            "Agent and build settings",
            "Voxeliser",
            "Apply settings",
            "Revert",
            "Bake",
            "Check for changes",
            "Not baked",
            "Up to date",
            "Stale: the sources changed since this bake",
            "Stale: the terrain changed since this bake",
            "Sidecar missing: bake again to rebuild the navmesh",
            "Overlays",
            "Components",
            "Place link in viewport",
            "Test path",
            "Pick start",
            "Pick end",
            "Find path",
            "Cancel pick",
            "Flow field",
            "Region half size",
            "Build flow field to end",
            "No world is open.",
            "Open a world to author and bake its navigation.",
        ]
        .map(ToString::to_string)
        .to_vec();
        labels.extend(SETTINGS.iter().map(|(_, label)| (*label).to_string()));
        labels.extend(PLACEABLE.iter().map(|spec| format!("Add {}", spec.label)));
        for target in [
            PickTarget::PathStart,
            PickTarget::PathEnd,
            PickTarget::LinkFrom,
            PickTarget::LinkTo,
        ] {
            labels.push(target.prompt().to_string());
        }
        for label in labels {
            cy_editor_visual::vocabulary::check_label(&label)
                .unwrap_or_else(|problem| panic!("{label:?}: {problem}"));
        }
    }

    #[test]
    fn only_the_changed_settings_are_sent_in_one_gesture() {
        let stored = NavSettingsBlock::DEFAULT;
        let mut draft = stored;
        assert!(changed_settings(&stored, &draft).is_empty());
        draft.agent_radius = 0.25;
        draft.tile_size = 8.0;
        draft.backend = NavBackend::Recast;
        assert_eq!(
            changed_settings(&stored, &draft),
            ["agent_radius=0.25", "tile_size=8", "backend=recast"]
        );
    }

    #[test]
    fn a_click_is_rescaled_to_the_rendered_frame_and_left_alone_without_one() {
        use cy_editor_viewport::transport::SharedImage;
        use cy_editor_viewport::{PickIntent, ViewportId};
        let request = PickRequest::for_frame(
            ViewportId::from_raw(0),
            cy_editor_protocol::FrameId::from_raw(3),
            PickIntent::Click { x: 100.0, y: 50.0 },
        )
        .with_display_extent(800, 400);
        let shared = FrameImage::SharedTexture {
            handle: 1,
            image: SharedImage {
                width: 1600,
                height: 1200,
                ..SharedImage::default()
            },
        };
        let bits = |pixel: [f32; 2]| pixel.map(f32::to_bits);
        assert_eq!(
            bits(frame_pixel(&request, Some(&shared), 100.0, 50.0)),
            bits([200.0, 150.0])
        );
        assert_eq!(
            bits(frame_pixel(
                &request,
                Some(&FrameImage::Surface(1)),
                100.0,
                50.0
            )),
            bits([100.0, 50.0])
        );
        assert_eq!(
            bits(frame_pixel(&request, None, 100.0, 50.0)),
            bits([100.0, 50.0])
        );
    }

    #[test]
    fn a_refused_pick_is_not_settled_by_a_later_unrelated_answer() {
        let mut inputs = NavigationInputs {
            awaiting: Some((PickTarget::PathStart, 0)),
            ..NavigationInputs::default()
        };
        pick_refused(&mut inputs, "navigation.path.query is still pending");
        assert_eq!(inputs.awaiting, None);
        assert_eq!(inputs.armed, Some(PickTarget::PathStart));
        assert!(inputs.problem.as_deref().unwrap().contains("still pending"));
        // A pick answered for someone else (an MCP client) advances the count; with nothing
        // awaited it places nothing.
        let mut intents = Vec::new();
        settle_pick(&mut inputs, &NavmeshService::new(), &mut intents);
        assert_eq!(inputs.start, None);
        assert!(intents.is_empty());
        // Nothing awaited: a refusal of some other pick leaves the panel alone.
        let mut idle = NavigationInputs::default();
        pick_refused(&mut idle, "refused");
        assert_eq!(idle, NavigationInputs::default());
    }

    #[test]
    fn a_click_is_rescaled_only_with_the_frame_it_was_made_on() {
        use cy_editor_protocol::FrameId;
        use cy_editor_viewport::{PickIntent, ViewportId};
        let frame = PresentedFrame::new(
            FrameId::from_raw(3),
            cy_editor_viewport::ViewState::default(),
            FrameImage::Surface(1),
            0,
        );
        let on = PickRequest::for_frame(
            ViewportId::from_raw(0),
            FrameId::from_raw(3),
            PickIntent::Click { x: 1.0, y: 1.0 },
        );
        let older = PickRequest::for_frame(
            ViewportId::from_raw(0),
            FrameId::from_raw(2),
            PickIntent::Click { x: 1.0, y: 1.0 },
        );
        assert!(clicked_image(&on, Some(&frame)).is_some());
        assert!(clicked_image(&older, Some(&frame)).is_none());
        assert!(clicked_image(&on, None).is_none());
    }

    #[test]
    fn terrain_edits_since_a_bake_show_as_the_same_stale_badge() {
        let baked = NavmeshSettings {
            node: cy_editor_core::ids::NodeId::from_u128(1),
            world: 1,
            settings: NavSettingsBlock::DEFAULT,
            overlay: 0,
            bake_identity: 0x11,
            source_fingerprint: 0x22,
            tile_count: 4,
            sidecar: String::new(),
        };
        let answer = |stale| NavStatusReport {
            world: 1,
            baked: true,
            stale,
            current_fingerprint: 0x22,
            saved_fingerprint: 0x22,
            identity: 0x11,
            resident_tiles: 4,
            counters: cy_editor_services::nav_bake::NavBuildCounters::default(),
            link_failures: 0,
            sidecar_missing: false,
        };
        let current = answer(false);
        assert_eq!(
            freshness(Some(&current), 0, &baked),
            (Semantic::Live, "Up to date".into())
        );
        let terrain = (
            Semantic::Warning,
            "Stale: the terrain changed since this bake".to_string(),
        );
        assert_eq!(freshness(Some(&current), 2, &baked), terrain);
        assert_eq!(freshness(None, 2, &baked), terrain);
        assert_eq!(
            freshness(Some(&answer(true)), 2, &baked).1,
            "Stale: the sources changed since this bake"
        );
        let unbaked = NavmeshSettings {
            bake_identity: 0,
            ..baked
        };
        assert_eq!(freshness(None, 2, &unbaked).1, "Not baked");
    }

    #[test]
    fn overlay_words_round_trip_every_flag() {
        assert_eq!(overlay_words(0), "none");
        assert_eq!(overlay_words(1 | (1 << 8)), "polygons, obstacles");
    }
}
