// SPDX-License-Identifier: MIT
//! The Animation editor: pose graphs on the shared canvas, a clip and its events on the shared
//! timeline, and a preview the engine evaluates and draws. Issue #29, animation.
//!
//! `Domain::AnimationGraphsAndClips` on the specialised scaffold. The palette is the engine's
//! (`animation.catalogue.get`): clips, blends, states and transitions, with the preview character's
//! clips to choose from. Every change is a registered `animation.*` command — one undoable
//! transaction, and an MCP tool of the same name. The engine compiles the graph whenever the saved
//! text differs from what it last compiled, and its diagnostics are drawn ON THE NODE they name.
//!
//! THE PREVIEW IS THE ENGINE'S. Play, Pause, a scrub on the timeline's ruler and a parameter change
//! are `animation.preview.*` commands; the engine evaluates the pose on its character and draws it
//! skinned in the viewport, and the panel shows what the engine says it did: the state, the blend,
//! the time, the pose's digest and the events playback crossed. The timeline shows one clip — the
//! one previewed, or the clip node selected on the canvas — with one event track per event name;
//! adding, dragging and deleting a key there are `animation.event.add`, `.move` and `.remove`.
//! Nothing here evaluates a pose.

use std::collections::BTreeMap;
use std::path::PathBuf;

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::Session;
use cy_editor_interface::specialised::animation::{self, ClipTimeline};
use cy_editor_interface::specialised::graph::{GraphCanvas, Layout, NodeKey};
use cy_editor_interface::specialised::script;
use cy_editor_interface::specialised::timeline::{TimelineSurface, TrackKind};
use cy_editor_services::animation_graph::{
    CLIP_NODE, CompileReport, NO_STATE, PreviewState, validate_reference,
};
use cy_editor_services::script_graph::{CompileDiagnostic, ScriptGraph, Severity};
use cy_editor_services::{AssetCatalogueService, MaterialCatalogueState};
use cy_editor_visual::colour::Semantic;

use super::graph_canvas::{self, CanvasFeedback, GraphConnection, GraphMovement};
use super::specialised::{SpecialisedTool, ToolDiagnostic, ToolFrame};
use super::timeline::{self, TimelineEdit, TimelineView};
use super::{Inputs, Intent, Panels, heading, nothing_here, secondary, status};

/// The panel's presentation state.
#[derive(Clone, Debug)]
pub struct AnimationInputs {
    /// Whether the panel drew this frame; a playing preview is followed while it does.
    pub seen: bool,
    /// The graph being edited.
    pub reference: String,
    /// The palette's search.
    pub filter: String,
    /// A wire being drawn from an output pin.
    pub link_source: Option<(u64, u32, String, String)>,
    /// What the canvas, the timeline or a property refused.
    pub problem: Option<String>,
    /// The source the canvas was laid out from, so an edit, an undo or a redo relays it.
    pub loaded: Option<String>,
    /// The source a compile was last asked for, so one change asks once.
    pub compile_asked: Option<String>,
    /// The graph the panel last asked the engine to preview, so opening it previews it once.
    pub preview_asked: Option<String>,
    /// Whether the preview runs the state machine rather than the timeline's clip alone.
    pub machine: bool,
    /// The clip node the timeline shows when nothing is previewed or selected.
    pub clip_node: Option<u64>,
    /// The event an "Add event" places at the playhead.
    pub new_event: String,
    /// The author's parameters as the panel last sent them.
    pub parameters: BTreeMap<String, f32>,
    /// The timeline's zoom, scroll, selection and gesture.
    pub(super) view: TimelineView,
    /// What the timeline was last fitted to: a clip node (zero for the state machine) and its
    /// length's bits. Showing something else fits the zoom to it once; after that the zoom is the
    /// author's.
    pub fitted: Option<(u64, u32)>,
}

impl Default for AnimationInputs {
    fn default() -> Self {
        Self {
            seen: false,
            reference: "game/animation/locomotion.cyanimgraph".into(),
            filter: String::new(),
            link_source: None,
            problem: None,
            loaded: None,
            compile_asked: None,
            preview_asked: None,
            machine: false,
            clip_node: None,
            new_event: "footstep".into(),
            parameters: BTreeMap::new(),
            view: TimelineView::default(),
            fitted: None,
        }
    }
}

/// How long a state machine preview runs before it starts again: the engine's
/// `kAnimationGraphPreviewSeconds`, until the engine has said so itself.
const GRAPH_PREVIEW_SECONDS: f32 = 4.0;

/// What the panel edits this frame.
pub(crate) struct Target {
    reference: String,
    graph: ScriptGraph,
    source: String,
    report: Option<(bool, CompileReport)>,
    preview: Option<PreviewState>,
    problem: Option<String>,
    connected: bool,
    pending: bool,
}

impl Target {
    /// The engine's preview, when it shows this graph.
    fn previewing(&self) -> Option<&PreviewState> {
        self.preview.as_ref().filter(|state| state.active)
    }
}

/// The Animation editor, drawn in the specialised-editor frame.
pub(crate) struct AnimationTool;

impl SpecialisedTool for AnimationTool {
    const DOMAIN: Domain = Domain::AnimationGraphsAndClips;
    const TITLE: &'static str = "Animation";
    const COMMANDS: &'static [&'static str] = &[
        "animation.graph.create",
        "animation.node.add",
        "animation.node.move",
        "animation.node.connect",
        "animation.node.disconnect",
        "animation.node.remove",
        "animation.node.property.set",
        "animation.event.add",
        "animation.event.move",
        "animation.event.remove",
        "animation.graph.compile",
        "animation.preview.scrub",
        "animation.preview.play",
        "animation.preview.pause",
        "animation.preview.parameter",
        "animation.preview.stop",
    ];

    type Target = Target;

    /// The canvas and timeline edits live beside the shared canvas
    /// (`animation_authoring_commands`); the rest are built in, in
    /// `cy_editor_services::animation_commands`.
    fn register(
        registry: &mut cy_editor_commands::Registry,
    ) -> cy_editor_core::problem::Result<()> {
        cy_editor_interface::specialised::animation_authoring_commands::register(registry)
    }

    fn target(panels: &mut Panels<'_>, ui: &mut egui::Ui) -> Option<Self::Target> {
        panels.inputs.animation.seen = true;
        reference_row(panels.shell, ui, &mut panels.inputs.animation.reference);
        let reference = panels.inputs.animation.reference.trim().to_owned();
        if panels.editor.workspace.active().is_none() {
            nothing_here(
                ui,
                panels.shell,
                "No world is open.",
                "Open a world; animation graph edits undo in its history.",
            );
            return None;
        }
        if let Err(problem) = validate_reference(&reference) {
            status(ui, panels.shell, Semantic::Error, &problem.to_string());
            return None;
        }
        if !panels.specialised.animation_catalogue_ready() {
            panels.editor.backend.animation.want();
            nothing_here(
                ui,
                panels.shell,
                "Waiting for the engine's animation vocabulary.",
                "The palette and the preview character are the engine's own: attach a runtime \
                 (`just run-editor-live`) and they arrive.",
            );
            return None;
        }
        let (source, graph) = read_graph(panels, ui, &reference)?;
        let requests = &panels.editor.backend.animation;
        let report = requests
            .report(&reference)
            .map(|(compiled, report)| (compiled == &source, report.clone()));
        let previewed = requests
            .settings()
            .is_some_and(|settings| settings.reference == reference);
        let preview = requests.preview_state().cloned().filter(|_| previewed);
        let connected = panels.editor.runtime.is_connected();
        ask_for_compile(panels, &reference, &source, report.as_ref());
        ask_for_preview(panels, &reference, &graph, report.as_ref(), previewed);
        let requests = &panels.editor.backend.animation;
        Some(Target {
            graph,
            report,
            preview,
            problem: requests.problem().map(str::to_owned),
            connected,
            pending: requests.pending(),
            reference,
            source,
        })
    }

    fn diagnostics(inputs: &Inputs) -> Vec<ToolDiagnostic> {
        inputs
            .animation
            .problem
            .iter()
            .map(|problem| ToolDiagnostic::error(problem.clone()))
            .collect()
    }

    fn body(frame: &mut ToolFrame<'_>, session: Session<'_>, target: Target, ui: &mut egui::Ui) {
        let (Some(canvas), Some(surface)) = (session.graph, session.timeline) else {
            return;
        };
        if frame.inputs.animation.loaded.as_deref() != Some(target.source.as_str()) {
            match script::open(&target.graph, canvas) {
                Ok(()) => frame.inputs.animation.loaded = Some(target.source.clone()),
                Err(problem) => frame.inputs.animation.problem = Some(problem.to_string()),
            }
        }
        if let Some(problem) = &target.problem {
            status(ui, frame.shell, Semantic::Error, problem);
        }
        toolbar(frame, ui, &target);
        preview_bar(frame, ui, &target, canvas);
        let height = (ui.available_height() * 0.5).max(260.0);
        ui.allocate_ui(egui::vec2(ui.available_width(), height), |ui| {
            canvas_area(frame, ui, canvas, &target);
        });
        timeline_area(frame, ui, canvas, surface, &target);
        egui::ScrollArea::vertical().show(ui, |ui| {
            parameter_rows(frame, ui, &target);
            event_rows(frame, ui, &target);
            compile_rows(frame, ui, canvas, &target);
        });
    }
}

/// The graph's text and its reading, or the empty state that offers to create it.
fn read_graph(
    panels: &mut Panels<'_>,
    ui: &mut egui::Ui,
    reference: &str,
) -> Option<(String, ScriptGraph)> {
    let project = &panels.editor.project;
    if !project.source_exists(reference) {
        ui.label(secondary(
            panels.shell,
            "There is no animation graph here yet. A new one has one state playing the preview \
             character's first clip; add clips, blends and transitions from the palette.",
        ));
        if ui.button("Create graph").clicked() {
            panels.intents.push(Intent::Invoke(
                "animation.graph.create".into(),
                Arguments::new().with("reference", Value::Text(reference.to_owned())),
            ));
        }
        return None;
    }
    let read = project
        .read_source(reference)
        .and_then(|source| ScriptGraph::decode(&source).map(|graph| (source, graph)));
    match read {
        Ok(read) => Some(read),
        Err(problem) => {
            status(ui, panels.shell, Semantic::Error, &problem.to_string());
            None
        }
    }
}

fn reference_row(shell: &Shell, ui: &mut egui::Ui, reference: &mut String) {
    ui.horizontal(|ui| {
        ui.label(secondary(shell, "Graph"));
        ui.add(egui::TextEdit::singleline(reference).desired_width(f32::INFINITY))
            .on_hover_text("The project-relative .cyanimgraph this panel edits");
    });
}

/// Compile whenever the saved text is not what the engine last compiled — once per change.
fn ask_for_compile(
    panels: &mut Panels<'_>,
    reference: &str,
    source: &str,
    report: Option<&(bool, CompileReport)>,
) {
    let current = report.is_some_and(|(current, _)| *current);
    let asked = panels.inputs.animation.compile_asked.as_deref() == Some(source);
    if current
        || asked
        || !panels.editor.runtime.is_connected()
        || panels.editor.backend.animation.pending()
    {
        return;
    }
    panels.inputs.animation.compile_asked = Some(source.to_owned());
    panels.intents.push(Intent::Invoke(
        "animation.graph.compile".into(),
        Arguments::new().with("reference", Value::Text(reference.to_owned())),
    ));
}

/// Opening a compiled graph shows it: its first clip at the start, once per graph. Everything
/// after that is the author's gesture.
fn ask_for_preview(
    panels: &mut Panels<'_>,
    reference: &str,
    graph: &ScriptGraph,
    report: Option<&(bool, CompileReport)>,
    previewed: bool,
) {
    let compiled = report.is_some_and(|(current, report)| *current && report.compiled);
    let asked = panels.inputs.animation.preview_asked.as_deref() == Some(reference);
    if previewed || asked || !compiled || !panels.editor.runtime.is_connected() {
        return;
    }
    panels.inputs.animation.preview_asked = Some(reference.to_owned());
    let node = if panels.inputs.animation.machine {
        0
    } else {
        animation::first_clip(graph).unwrap_or(0)
    };
    panels.intents.push(Intent::Invoke(
        "animation.preview.scrub".into(),
        scrub_arguments(reference, node, 0.0),
    ));
}

fn invoke(frame: &mut ToolFrame<'_>, command: &str, arguments: Arguments) {
    frame
        .intents
        .push(Intent::Invoke(command.to_owned(), arguments));
}

fn with_reference(target: &Target) -> Arguments {
    Arguments::new().with("reference", Value::Text(target.reference.clone()))
}

fn ordinal(key: u64) -> Value {
    Value::Int(i64::try_from(key).unwrap_or(i64::MAX))
}

fn scrub_arguments(reference: &str, node: u64, time: f64) -> Arguments {
    Arguments::new()
        .with("reference", Value::Text(reference.to_owned()))
        .with("node", ordinal(node))
        .with("time", Value::Float(seconds(time)))
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "a timeline time is seconds within a clip; f32 is the engine's clock"
)]
fn seconds(time: f64) -> f32 {
    time as f32
}

/// One line on what the engine made of the graph, and the compile action.
fn toolbar(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    ui.horizontal(|ui| {
        let (role, line) = compile_line(target);
        status(ui, frame.shell, role, &line);
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            if ui
                .add_enabled(target.connected, egui::Button::new("Compile"))
                .on_disabled_hover_text("The engine compiles graphs; attach a runtime")
                .clicked()
            {
                frame.inputs.animation.compile_asked = Some(target.source.clone());
                invoke(frame, "animation.graph.compile", with_reference(target));
            }
        });
    });
}

fn compile_line(target: &Target) -> (Semantic, String) {
    match &target.report {
        Some((true, report)) if report.compiled => (
            Semantic::Live,
            format!(
                "Compiled: {} state(s) ({}), {} transition(s), {} clip(s), {} instruction(s)",
                report.states.len(),
                report
                    .states
                    .iter()
                    .map(|state| state.name.as_str())
                    .collect::<Vec<_>>()
                    .join(", "),
                report.transitions.len(),
                report.clips.len(),
                report.instructions
            ),
        ),
        Some((true, report)) => (
            Semantic::Error,
            format!(
                "Does not compile: {} error(s), each on its node",
                report.errors().count()
            ),
        ),
        _ if !target.connected => (
            Semantic::Warning,
            "Not compiled: no runtime is attached, and the editor does not compile graphs".into(),
        ),
        _ if target.pending => (Semantic::Active, "Compiling…".into()),
        _ => (Semantic::Neutral, "Not compiled yet".into()),
    }
}

/// The clip node the timeline shows: the previewed one, else the clip node selected on the canvas,
/// else the one chosen last, else the graph's first.
fn timeline_clip(frame: &ToolFrame<'_>, canvas: &GraphCanvas, target: &Target) -> Option<u64> {
    let is_clip = |key: u64| {
        target
            .graph
            .nodes
            .get(&key)
            .is_some_and(|node| node.type_name == CLIP_NODE)
    };
    if let Some(state) = target.previewing().filter(|state| state.focus != 0) {
        return Some(state.focus);
    }
    canvas
        .selection()
        .first()
        .map(|key| key.ordinal())
        .filter(|key| is_clip(*key))
        .or_else(|| frame.inputs.animation.clip_node.filter(|key| is_clip(*key)))
        .or_else(|| animation::first_clip(&target.graph))
}

/// Play, pause and stop, what is previewed, and what the engine says the character is doing.
fn preview_bar(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    target: &Target,
    canvas: &GraphCanvas,
) {
    let clip = timeline_clip(frame, canvas, target);
    let previewing = target.previewing();
    // What the engine previews decides the mode shown, whoever asked for it: the panel, a script or
    // an agent. Choosing the other mode is a request, sent below.
    if let Some(state) = previewing {
        frame.inputs.animation.machine = state.focus == 0;
    }
    let ready = target.connected
        && target
            .report
            .as_ref()
            .is_some_and(|(current, report)| *current && report.compiled);
    ui.horizontal(|ui| {
        ui.label(secondary(frame.shell, "Preview"));
        let was_machine = frame.inputs.animation.machine;
        ui.selectable_value(&mut frame.inputs.animation.machine, false, "Clip")
            .on_hover_text("Preview the timeline's clip alone");
        ui.selectable_value(&mut frame.inputs.animation.machine, true, "State machine")
            .on_hover_text("Preview the graph from its entry state, with the parameters below");
        let focus = if frame.inputs.animation.machine {
            0
        } else {
            clip.unwrap_or(0)
        };
        if frame.inputs.animation.machine != was_machine && ready {
            // The other mode, from its start.
            invoke(
                frame,
                "animation.preview.scrub",
                scrub_arguments(&target.reference, focus, 0.0),
            );
        }
        if previewing.is_some_and(|state| state.playing) {
            if ui.button("Pause").clicked() {
                invoke(frame, "animation.preview.pause", Arguments::new());
            }
        } else if ui
            .add_enabled(ready, egui::Button::new("Play"))
            .on_disabled_hover_text("The engine previews a graph that compiles")
            .clicked()
        {
            invoke(
                frame,
                "animation.preview.play",
                with_reference(target).with("node", ordinal(focus)),
            );
        }
        if ui
            .add_enabled(previewing.is_some(), egui::Button::new("Stop"))
            .on_hover_text("Take the preview character out of the viewport")
            .clicked()
        {
            invoke(frame, "animation.preview.stop", Arguments::new());
        }
    });
    match previewing {
        Some(state) => {
            let role = if state.playing {
                Semantic::Live
            } else {
                Semantic::Active
            };
            let line = format!(
                "Engine: {} · pose {:016x} · {} joint(s)",
                state.describe(),
                state.pose_digest,
                state.joints.len()
            );
            status(ui, frame.shell, role, &line);
        }
        None => {
            ui.label(secondary(
                frame.shell,
                "Not previewing: scrub the timeline or press Play to show the character in the \
                 viewport",
            ));
        }
    }
}

/// The engine's diagnostics, as the canvas outlines them.
fn node_alerts(target: &Target) -> Vec<(u64, String)> {
    let Some((true, report)) = &target.report else {
        return Vec::new();
    };
    report
        .diagnostics
        .iter()
        .filter(|diagnostic| diagnostic.node != 0)
        .map(|diagnostic| {
            (
                diagnostic.node,
                format!("{}: {}", diagnostic.code, diagnostic.describe()),
            )
        })
        .collect()
}

fn canvas_area(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    target: &Target,
) {
    let available = ui.available_size();
    ui.horizontal(|ui| {
        ui.allocate_ui_with_layout(
            egui::vec2(230.0_f32.min(available.x * 0.36), available.y),
            egui::Layout::top_down(egui::Align::Min),
            |ui| side_column(frame, ui, canvas, target),
        );
        ui.separator();
        ui.allocate_ui(egui::vec2(ui.available_width(), available.y), |ui| {
            draw(frame, ui, canvas, target);
        });
    });
}

/// The palette, then the selected node's properties and actions.
fn side_column(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    target: &Target,
) {
    super::search_field(
        ui,
        frame.shell,
        "Search clips, blends and states",
        &mut frame.inputs.animation.filter,
    );
    let entries = graph_canvas::catalogue_palette(canvas, &frame.inputs.animation.filter);
    ui.allocate_ui(
        egui::vec2(ui.available_width(), ui.available_height() * 0.5),
        |ui| {
            if let Some(node_type) = graph_canvas::node_palette(ui, entries, true) {
                let at = graph_canvas::palette_slot(canvas.nodes().count());
                invoke(
                    frame,
                    "animation.node.add",
                    with_reference(target)
                        .with("node_type", Value::Text(node_type))
                        .with("x", Value::Float(at.x))
                        .with("y", Value::Float(at.y)),
                );
            }
        },
    );
    let assets = AssetCatalogueService::new(PathBuf::new());
    let mut edits = Vec::new();
    graph_canvas::graph_properties_with(
        ui,
        canvas,
        &assets,
        &mut frame.inputs.animation.problem,
        |_, key, property, value| {
            edits.push((key, property.name.clone(), value));
            Ok(())
        },
    );
    for (key, property, value) in edits {
        invoke(
            frame,
            "animation.node.property.set",
            with_reference(target)
                .with("node", ordinal(key.ordinal()))
                .with("property", Value::Text(property))
                .with("value", Value::Text(value)),
        );
    }
    let Some(node) = canvas.selection().first().copied() else {
        return;
    };
    if canvas
        .node(node)
        .is_some_and(|authored| authored.type_name == CLIP_NODE)
    {
        frame.inputs.animation.clip_node = Some(node.ordinal());
    }
    if ui.button("Remove node").clicked() {
        invoke(
            frame,
            "animation.node.remove",
            with_reference(target).with("node", ordinal(node.ordinal())),
        );
    }
    let wires: Vec<_> = canvas
        .links()
        .filter(|link| link.from == node || link.to == node)
        .cloned()
        .collect();
    for wire in wires {
        if ui
            .small_button(format!(
                "Unwire {}.{} → {}.{}",
                wire.from.ordinal(),
                wire.from_pin,
                wire.to.ordinal(),
                wire.to_pin
            ))
            .clicked()
        {
            invoke(
                frame,
                "animation.node.disconnect",
                with_reference(target)
                    .with("from", ordinal(wire.from.ordinal()))
                    .with("from_pin", Value::Text(wire.from_pin.clone()))
                    .with("to", ordinal(wire.to.ordinal()))
                    .with("to_pin", Value::Text(wire.to_pin.clone())),
            );
        }
    }
}

/// The canvas, with each gesture routed to its command: one gesture, one undo entry.
fn draw(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, canvas: &mut GraphCanvas, target: &Target) {
    let alerts = node_alerts(target);
    let mut requested = Vec::new();
    let mut on_connect = |_: &mut GraphCanvas,
                          connection: &GraphConnection|
     -> cy_editor_core::problem::Result<()> {
        requested.push(
            with_reference(target)
                .with("from", ordinal(connection.from.ordinal()))
                .with("from_pin", Value::Text(connection.from_name.clone()))
                .with("to", ordinal(connection.to.ordinal()))
                .with("to_pin", Value::Text(connection.to_name.clone())),
        );
        Ok(())
    };
    let mut moved = None;
    let mut on_move = |canvas: &mut GraphCanvas,
                       movement: GraphMovement|
     -> cy_editor_core::problem::Result<()> {
        if movement.finished {
            moved = Some((movement.node, movement.at));
            Ok(())
        } else {
            canvas.move_to(movement.node, movement.at)
        }
    };
    graph_canvas::draw_canvas(
        ui,
        frame.shell,
        canvas,
        MaterialCatalogueState::Ready,
        "Add a clip and a state from the palette",
        &mut frame.inputs.animation.link_source,
        &mut CanvasFeedback {
            link_problem: &mut frame.inputs.animation.problem,
            node_alerts: &alerts,
            unwired_inputs: false,
            on_connect: Some(&mut on_connect),
            on_move: Some(&mut on_move),
            node_marks: &[],
            on_gutter: None,
        },
    );
    for arguments in requested {
        invoke(frame, "animation.node.connect", arguments);
    }
    if let Some((node, at)) = moved {
        invoke(
            frame,
            "animation.node.move",
            move_arguments(target, node, at),
        );
    }
}

fn move_arguments(target: &Target, node: NodeKey, at: Layout) -> Arguments {
    with_reference(target)
        .with("node", ordinal(node.ordinal()))
        .with("x", Value::Float(at.x))
        .with("y", Value::Float(at.y))
}

/// The clip's length: the engine's, while it previews that clip; else the node's own record.
fn clip_length(target: &Target, node: u64) -> f32 {
    if let Some(state) = target.previewing().filter(|state| state.focus == node) {
        return state.length;
    }
    target
        .graph
        .nodes
        .get(&node)
        .and_then(|found| found.property("duration"))
        .map(|property| property.literal.display(&property.literal_type))
        .and_then(|text| text.parse::<f32>().ok())
        .filter(|length| *length > 0.0)
        .unwrap_or(1.0)
}

/// The timeline: the clip and its events, scrubbed on the ruler and edited key by key; or, while
/// the state machine is previewed, its run from the entry state, scrubbed the same way.
fn timeline_area(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    canvas: &GraphCanvas,
    surface: &mut TimelineSurface,
    target: &Target,
) {
    if frame.inputs.animation.machine {
        machine_timeline(frame, ui, surface, target);
        return;
    }
    let Some(node) = timeline_clip(frame, canvas, target) else {
        ui.label(secondary(
            frame.shell,
            "The timeline shows a clip: add a pose.clip node to the graph.",
        ));
        return;
    };
    let drawn = animation::clip_of(&target.graph, node).and_then(|(clip, events)| {
        animation::clip_timeline(surface, &clip, clip_length(target, node), &events)
            .map(|drawn| (clip, drawn))
    });
    let (clip, drawn) = match drawn {
        Ok(drawn) => drawn,
        Err(problem) => {
            status(ui, frame.shell, Semantic::Error, &problem.to_string());
            return;
        }
    };
    if let Some(state) = target.previewing().filter(|state| state.focus == node) {
        surface.scrub(f64::from(state.time));
    }
    heading(ui, frame.shell, &format!("Clip {clip} · node {node}"));
    fit(frame, ui, node, clip_length(target, node));
    let response = timeline::show(ui, frame.shell, surface, &mut frame.inputs.animation.view);
    if let Some(time) = response.scrub {
        invoke(
            frame,
            "animation.preview.scrub",
            scrub_arguments(&target.reference, node, time),
        );
    }
    for edit in response.edits {
        if let Some((command, arguments)) = event_command(target, node, &drawn, edit) {
            invoke(frame, command, arguments);
        }
    }
}

/// The state machine's run on the timeline: one track over the preview's length.
fn machine_timeline(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    surface: &mut TimelineSurface,
    target: &Target,
) {
    let state = target.previewing().filter(|state| state.focus == 0);
    let length = state.map_or(GRAPH_PREVIEW_SECONDS, |state| state.length);
    surface.load(f64::from(length));
    let track = surface.add_track(TrackKind::Animation, "state machine");
    let _ = surface.add_section(track, 0.0, f64::from(length), "from the entry state");
    if let Some(state) = state {
        surface.scrub(f64::from(state.time));
    }
    heading(ui, frame.shell, "State machine");
    fit(frame, ui, 0, length);
    let response = timeline::show(ui, frame.shell, surface, &mut frame.inputs.animation.view);
    if let Some(time) = response.scrub {
        invoke(
            frame,
            "animation.preview.scrub",
            scrub_arguments(&target.reference, 0, time),
        );
    }
}

/// Zoom the timeline so `length` seconds fill its lanes, once per clip shown.
fn fit(frame: &mut ToolFrame<'_>, ui: &egui::Ui, node: u64, length: f32) {
    let shown = (node, length.to_bits());
    if frame.inputs.animation.fitted == Some(shown) || length.is_nan() || length <= 0.0 {
        return;
    }
    frame.inputs.animation.fitted = Some(shown);
    let lanes = (ui.available_width() - timeline::LABEL_WIDTH).max(1.0);
    let view = &mut frame.inputs.animation.view;
    view.pixels_per_second = (lanes / length).clamp(timeline::ZOOM_RANGE.0, timeline::ZOOM_RANGE.1);
    view.scroll = 0.0;
}

/// The `animation.event.*` command one timeline gesture is.
fn event_command(
    target: &Target,
    node: u64,
    drawn: &ClipTimeline,
    edit: TimelineEdit,
) -> Option<(&'static str, Arguments)> {
    let arguments = |event: &str| {
        with_reference(target)
            .with("node", ordinal(node))
            .with("event", Value::Text(event.to_owned()))
    };
    match edit {
        TimelineEdit::AddKey { track, time, .. } => {
            let name = drawn.track_event(track)?;
            Some((
                "animation.event.add",
                arguments(name).with("time", Value::Float(seconds(time))),
            ))
        }
        TimelineEdit::MoveKey { key, to, .. } => {
            let event = drawn.event(key)?;
            Some((
                "animation.event.move",
                arguments(&event.name)
                    .with("from", Value::Float(event.time))
                    .with("to", Value::Float(seconds(to.max(0.0)))),
            ))
        }
        TimelineEdit::RemoveKey { key, .. } => {
            let event = drawn.event(key)?;
            Some((
                "animation.event.remove",
                arguments(&event.name).with("time", Value::Float(event.time)),
            ))
        }
        // The clip's length is the clip's, and a removed key comes back through undo.
        TimelineEdit::RestoreKey { .. } | TimelineEdit::TrimSection { .. } => None,
    }
}

/// Place a new event at the playhead, and the events the engine's playback crossed.
fn event_rows(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    heading(ui, frame.shell, "Events");
    let node = frame
        .inputs
        .animation
        .clip_node
        .or_else(|| animation::first_clip(&target.graph));
    ui.horizontal(|ui| {
        ui.label("Event");
        ui.add(
            egui::TextEdit::singleline(&mut frame.inputs.animation.new_event).desired_width(120.0),
        );
        let time = target
            .previewing()
            .filter(|state| Some(state.focus) == node)
            .map_or(0.0, |state| state.time);
        if ui
            .add_enabled(
                node.is_some(),
                egui::Button::new(format!("Add at {time:.3} s")),
            )
            .on_hover_text("Place the event on the timeline's clip at the playhead")
            .clicked()
            && let Some(node) = node
        {
            let name = frame.inputs.animation.new_event.trim().to_owned();
            invoke(
                frame,
                "animation.event.add",
                with_reference(target)
                    .with("node", ordinal(node))
                    .with("event", Value::Text(name))
                    .with("time", Value::Float(time)),
            );
        }
    });
    ui.label(secondary(
        frame.shell,
        "Double-click an event's row on the timeline to place another, drag a key to move it, \
         and press Delete to remove the selected keys.",
    ));
    let Some(state) = target.previewing() else {
        return;
    };
    for fired in state.events.iter().rev().take(6) {
        ui.label(secondary(
            frame.shell,
            format!(
                "⚑ {} fired at {:.3} s ({:.0}% through its clip)",
                fired.name,
                fired.at,
                fired.normalised_time * 100.0
            ),
        ));
    }
}

/// The author's parameters: each one the state machine reads, set for the preview.
fn parameter_rows(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let Some((true, report)) = &target.report else {
        return;
    };
    let names: Vec<String> = report.author_parameters().map(str::to_owned).collect();
    if names.is_empty() {
        return;
    }
    heading(ui, frame.shell, "Parameters");
    for name in names {
        ui.horizontal(|ui| {
            let value = frame
                .inputs
                .animation
                .parameters
                .entry(name.clone())
                .or_insert(0.0);
            ui.label(&name);
            let response = ui.add(egui::DragValue::new(value).speed(0.05).range(0.0..=1.0));
            let open = value.abs() > 0.0;
            let toggled = ui
                .small_button(if open { "Close" } else { "Open" })
                .on_hover_text("A condition opens its transitions while it is not zero")
                .clicked();
            if toggled {
                *value = if open { 0.0 } else { 1.0 };
            }
            if response.drag_stopped() || response.lost_focus() || toggled {
                let arguments = Arguments::new()
                    .with("name", Value::Text(name.clone()))
                    .with("value", Value::Float(*value));
                frame.intents.push(Intent::Invoke(
                    "animation.preview.parameter".into(),
                    arguments,
                ));
            }
        });
    }
    if let Some(state) = target.previewing().filter(|state| state.focus == 0) {
        let target_line = if state.target == NO_STATE {
            String::new()
        } else {
            format!(" → {}", state.target_name)
        };
        ui.label(secondary(
            frame.shell,
            format!("State {}{target_line}", state.state_name),
        ));
    }
}

/// Every engine diagnostic as a row that selects its node.
fn compile_rows(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    target: &Target,
) {
    let Some((current, report)) = &target.report else {
        return;
    };
    if !current {
        ui.label(secondary(
            frame.shell,
            "The graph changed since the engine last compiled it.",
        ));
    }
    if !report.diagnostics.is_empty() {
        heading(ui, frame.shell, "Diagnostics");
    }
    for diagnostic in &report.diagnostics {
        diagnostic_row(frame, ui, canvas, diagnostic);
    }
    if report.compiled {
        egui::CollapsingHeader::new("What the graph became")
            .id_salt("animation-program")
            .show(ui, |ui| {
                ui.label(secondary(
                    frame.shell,
                    format!(
                        "Program digest {:016x} · {} joint(s)",
                        report.program_digest, report.joints
                    ),
                ));
                for transition in &report.transitions {
                    let name = |state: u32| {
                        report
                            .states
                            .get(state as usize)
                            .map_or("?", |found| found.name.as_str())
                    };
                    ui.label(secondary(
                        frame.shell,
                        format!(
                            "node {}: {} → {} when {} · blend {} s · priority {} · {}",
                            transition.node,
                            name(transition.from),
                            name(transition.to),
                            transition.condition,
                            transition.duration,
                            transition.priority,
                            transition.interruption
                        ),
                    ));
                }
                for clip in &report.clips {
                    ui.label(secondary(
                        frame.shell,
                        format!(
                            "clip {} · {} · {} event(s){}",
                            clip.name,
                            if clip.looping { "loops" } else { "holds" },
                            clip.events,
                            if clip.known {
                                ""
                            } else {
                                " · not on the preview character"
                            }
                        ),
                    ));
                }
            });
    }
}

/// One engine diagnostic as a row that selects the node it is about.
fn diagnostic_row(
    frame: &ToolFrame<'_>,
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    diagnostic: &CompileDiagnostic,
) {
    let role = match diagnostic.severity {
        Severity::Info => Semantic::Active,
        Severity::Warning => Semantic::Warning,
        Severity::Error => Semantic::Error,
    };
    let place = if diagnostic.node == 0 {
        "graph".to_owned()
    } else {
        format!("node {}", diagnostic.node)
    };
    let line = format!("{place} — {}: {}", diagnostic.code, diagnostic.describe());
    let response = ui
        .add(
            egui::Button::new(
                egui::RichText::new(format!("{} {line}", role.glyph()))
                    .color(crate::theme::role(frame.shell.theme, role)),
            )
            .frame(false),
        )
        .on_hover_text("Select the node this is about");
    if response.clicked()
        && let Ok(key) = NodeKey::new(diagnostic.node)
    {
        let _ = canvas.select([key]);
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_services::animation_graph::ClipEvent;

    use super::*;

    fn target(graph: ScriptGraph) -> Target {
        Target {
            reference: "game/animation/locomotion.cyanimgraph".into(),
            source: graph.encode(),
            graph,
            report: None,
            preview: None,
            problem: None,
            connected: true,
            pending: false,
        }
    }

    #[test]
    fn each_timeline_gesture_is_the_event_command_for_the_event_it_touched() {
        let mut graph = ScriptGraph::new("locomotion");
        graph.capabilities.clear();
        let mut surface = TimelineSurface::new(1, 30.0).unwrap();
        let events = vec![
            ClipEvent {
                name: "footstep".into(),
                time: 0.25,
            },
            ClipEvent {
                name: "footstep".into(),
                time: 0.75,
            },
        ];
        let drawn = animation::clip_timeline(&mut surface, "walk", 1.0, &events).unwrap();
        let track = drawn.event_tracks[0].0;
        let (key, _) = drawn.keys[1].clone();
        let target = target(graph);

        let (command, arguments) = event_command(
            &target,
            3,
            &drawn,
            TimelineEdit::MoveKey {
                track,
                key,
                to: 0.5,
            },
        )
        .unwrap();
        assert_eq!(command, "animation.event.move");
        assert_eq!(arguments.get("from").and_then(Value::as_float), Some(0.75));
        assert_eq!(arguments.get("to").and_then(Value::as_float), Some(0.5));
        assert_eq!(arguments.text("event"), Some("footstep"));

        let (command, arguments) =
            event_command(&target, 3, &drawn, TimelineEdit::RemoveKey { track, key }).unwrap();
        assert_eq!(command, "animation.event.remove");
        assert_eq!(arguments.get("time").and_then(Value::as_float), Some(0.75));

        let (command, arguments) = event_command(
            &target,
            3,
            &drawn,
            TimelineEdit::AddKey {
                track,
                time: 0.5,
                value: 0.0,
            },
        )
        .unwrap();
        assert_eq!(command, "animation.event.add");
        assert_eq!(arguments.get("node").and_then(Value::as_int), Some(3));

        // The clip's own track is not an event track: nothing to add there.
        let clip_track = drawn.clip_track.unwrap();
        assert!(
            event_command(
                &target,
                3,
                &drawn,
                TimelineEdit::AddKey {
                    track: clip_track,
                    time: 0.5,
                    value: 0.0
                }
            )
            .is_none()
        );
        assert_eq!(
            surface.track(clip_track).unwrap().kind,
            TrackKind::Animation
        );
    }
}
