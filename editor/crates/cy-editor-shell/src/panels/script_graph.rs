// SPDX-License-Identifier: MIT
//! The Gameplay Graph editor: visual scripting on the shared canvas. Issue #29, Wave 2.
//!
//! `Domain::GameplayAndUtilityGraphs` on the specialised scaffold. The palette is the engine's
//! (`script.catalogue.get`), so it offers events and responses and never a per-frame tick. Every
//! change is a registered `script.*` command — one undoable transaction, and an MCP tool of the same
//! name. The engine compiles the graph whenever the saved text differs from what it last compiled,
//! and its diagnostics are drawn ON THE NODE they name: an outline and a hover on the canvas, and a
//! row below it that selects the node. During Play the panel raises events on an entity's graphs
//! and shows what the engine's compiled program did: each instance, what it waits for, and the cues
//! it played. Nothing here interprets a graph.
//!
//! THE PLAY DEBUGGER (#84). A dot at the right of a node's header is its breakpoint gutter; the
//! header row holds Pause, Continue, Step Over and Step Into; the node Play is paused before is
//! outlined, and the last nodes the engine's trace says ran glow, newest brightest. Below the
//! canvas the watch list shows the inspected entity's graph variables and the watched pins. Each
//! of these is a registered `script.debug.*` command, so an agent does the same over MCP. Saving
//! a graph Play runs reloads it there, and the panel says what the reload kept or why it was
//! refused, on the node.

use std::path::PathBuf;

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::Session;
use cy_editor_interface::specialised::graph::{GraphCanvas, Layout, NodeKey};
use cy_editor_interface::specialised::script;
use cy_editor_services::script_debug::{Breakpoint, DebugState, ReloadReply};
use cy_editor_services::script_graph::{
    CompileDiagnostic, CompileReport, DEFAULT_EVENT, PlayState, ScriptGraph, Severity, graph_name,
    validate_reference,
};
use cy_editor_services::{AssetCatalogueService, MaterialCatalogueState};
use cy_editor_visual::colour::Semantic;

use super::graph_canvas::{self, CanvasFeedback, GraphConnection, GraphMovement, NodeMark};
use super::specialised::{SpecialisedTool, ToolDiagnostic, ToolFrame};
use super::{Inputs, Intent, Panels, heading, nothing_here, secondary, status};

/// The panel's presentation state.
#[derive(Clone, Debug)]
pub struct ScriptInputs {
    /// Whether the panel drew this frame; Play's graphs are polled while it does.
    pub seen: bool,
    /// The graph being edited.
    pub reference: String,
    /// The palette's search.
    pub filter: String,
    /// A wire being drawn from an output pin.
    pub link_source: Option<(u64, u32, String, String)>,
    /// What the canvas or a property refused.
    pub problem: Option<String>,
    /// The source the canvas was laid out from, so an edit, an undo or a redo relays it.
    pub loaded: Option<String>,
    /// The source a compile was last asked for, so one change asks once.
    pub compile_asked: Option<String>,
    /// The event to raise in Play, and its three arguments.
    pub event: String,
    /// The event's arguments: for `unit.command`, the target.
    pub arguments: [f32; 3],
    /// Whether a breakpoint set from the gutter stops only the selected entity.
    pub break_selected_only: bool,
}

impl Default for ScriptInputs {
    fn default() -> Self {
        Self {
            seen: false,
            reference: "game/scripts/unit_command.cyscript".into(),
            filter: String::new(),
            link_source: None,
            problem: None,
            loaded: None,
            compile_asked: None,
            event: DEFAULT_EVENT.into(),
            arguments: [6.0, 0.0, 8.0],
            break_selected_only: false,
        }
    }
}

/// A selected entity: its identity, its name, and the graph it runs.
type Selected = (NodeId, String, Option<String>);

/// What the panel edits this frame.
pub(crate) struct Target {
    reference: String,
    graph: ScriptGraph,
    source: String,
    report: Option<(bool, CompileReport)>,
    state: Option<PlayState>,
    /// The first selected entity, its name, and the graph it runs.
    selected: Option<Selected>,
    /// Engine identity to the authored name, for Play's rows.
    names: Vec<(u64, String)>,
    problem: Option<String>,
    connected: bool,
    pending: bool,
    /// The graph's name as Play knows it: its file's stem.
    graph_name: String,
    /// The debugger's last state.
    debug: Option<DebugState>,
    /// The breakpoints the editor wants on this graph.
    breakpoints: Vec<Breakpoint>,
    /// The engine's last answer to a reload of this graph, and whether it was of this text.
    reload: Option<(bool, ReloadReply)>,
}

/// The Gameplay Graph editor, drawn in the specialised-editor frame.
pub(crate) struct ScriptGraphTool;

impl SpecialisedTool for ScriptGraphTool {
    const DOMAIN: Domain = Domain::GameplayAndUtilityGraphs;
    const TITLE: &'static str = "Gameplay Graph";
    const COMMANDS: &'static [&'static str] = &[
        "script.graph.create",
        "script.node.add",
        "script.node.move",
        "script.node.connect",
        "script.node.disconnect",
        "script.node.remove",
        "script.node.property.set",
        "script.graph.attach",
        "script.graph.compile",
        "script.event.raise",
        "script.refresh",
        "script.debug.breakpoint",
        "script.debug.pause",
        "script.debug.continue",
        "script.debug.step",
        "script.debug.watch",
        "script.debug.inspect",
    ];

    type Target = Target;

    /// The canvas edits live beside the shared canvas (`script_authoring_commands`); the rest are
    /// built in, in `cy_editor_services::script_commands`.
    fn register(
        registry: &mut cy_editor_commands::Registry,
    ) -> cy_editor_core::problem::Result<()> {
        cy_editor_interface::specialised::script_authoring_commands::register(registry)
    }

    fn target(panels: &mut Panels<'_>, ui: &mut egui::Ui) -> Option<Self::Target> {
        panels.inputs.script.seen = true;
        reference_row(panels.shell, ui, &mut panels.inputs.script.reference);
        let reference = panels.inputs.script.reference.trim().to_owned();
        let Some(document_id) = panels.editor.workspace.active() else {
            nothing_here(
                ui,
                panels.shell,
                "No world is open.",
                "Open a world; gameplay graph edits undo in its history.",
            );
            return None;
        };
        if let Err(problem) = validate_reference(&reference) {
            status(ui, panels.shell, Semantic::Error, &problem.to_string());
            return None;
        }
        if !panels.specialised.script_catalogue_ready() {
            nothing_here(
                ui,
                panels.shell,
                "Waiting for the engine's gameplay graph vocabulary.",
                "The palette is the engine's own: attach a runtime (`just run-editor-live`) and it \
                 arrives.",
            );
            return None;
        }
        let (source, graph) = read_graph(panels, ui, &reference)?;
        let requests = &panels.editor.backend.script;
        let report = requests
            .report(&reference)
            .map(|(compiled, report)| (compiled == &source, report.clone()));
        let connected = panels.editor.runtime.is_connected();
        ask_for_compile(panels, &reference, &source, report.as_ref());
        let (selected, names) = selection_and_names(panels, document_id);
        let requests = &panels.editor.backend.script;
        let name = graph_name(&reference);
        Some(Target {
            graph,
            report,
            state: requests.state().cloned(),
            selected,
            names,
            problem: requests.problem().map(str::to_owned),
            connected,
            pending: requests.pending(),
            debug: requests.debug().cloned(),
            breakpoints: breakpoints_of(requests, &name),
            reload: requests
                .reload_reply(&reference)
                .map(|(sent, reply)| (sent == &source, reply.clone())),
            graph_name: name,
            reference,
            source,
        })
    }

    fn diagnostics(inputs: &Inputs) -> Vec<ToolDiagnostic> {
        inputs
            .script
            .problem
            .iter()
            .map(|problem| ToolDiagnostic::error(problem.clone()))
            .collect()
    }

    fn body(frame: &mut ToolFrame<'_>, session: Session<'_>, target: Target, ui: &mut egui::Ui) {
        let Some(canvas) = session.graph else {
            return;
        };
        if frame.inputs.script.loaded.as_deref() != Some(target.source.as_str()) {
            match script::open(&target.graph, canvas) {
                Ok(()) => frame.inputs.script.loaded = Some(target.source.clone()),
                Err(problem) => frame.inputs.script.problem = Some(problem.to_string()),
            }
        }
        if let Some(problem) = &target.problem {
            status(ui, frame.shell, Semantic::Error, problem);
        }
        toolbar(frame, ui, &target);
        debug_bar(frame, ui, &target);
        let height = (ui.available_height() * 0.62).max(300.0);
        ui.allocate_ui(egui::vec2(ui.available_width(), height), |ui| {
            canvas_area(frame, ui, canvas, &target);
        });
        egui::ScrollArea::vertical().show(ui, |ui| {
            compile_rows(frame, ui, canvas, &target);
            reload_rows(frame, ui, canvas, &target);
            watch_rows(frame, ui, canvas, &target);
            play_rows(frame, ui, &target);
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
            "There is no graph here yet. A new one answers one event; add its response from the \
             palette.",
        ));
        if ui.button("Create graph").clicked() {
            panels.intents.push(Intent::Invoke(
                "script.graph.create".into(),
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

/// The first selected entity with its name and graph, and every entity's engine identity and name.
fn selection_and_names(
    panels: &Panels<'_>,
    document_id: cy_editor_core::ids::DocumentId,
) -> (Option<Selected>, Vec<(u64, String)>) {
    let Some(document) = panels.editor.documents.get(document_id) else {
        return (None, Vec::new());
    };
    let content = document.content();
    let selected = panels
        .editor
        .selection
        .get()
        .nodes()
        .next()
        .and_then(|node| {
            let name = content.node(node)?.name.clone();
            Some((
                node,
                name,
                cy_editor_services::script_commands::attached_graph(document, node),
            ))
        });
    let names = content
        .nodes()
        .filter_map(|node| {
            Some((
                cy_editor_services::mirror::engine_identity(node),
                content.node(node)?.name.clone(),
            ))
        })
        .collect();
    (selected, names)
}

fn reference_row(shell: &Shell, ui: &mut egui::Ui, reference: &mut String) {
    ui.horizontal(|ui| {
        ui.label(secondary(shell, "Graph"));
        ui.add(egui::TextEdit::singleline(reference).desired_width(f32::INFINITY))
            .on_hover_text("The project-relative .cyscript this panel edits");
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
    let asked = panels.inputs.script.compile_asked.as_deref() == Some(source);
    if current
        || asked
        || !panels.editor.runtime.is_connected()
        || panels.editor.backend.script.pending()
    {
        return;
    }
    panels.inputs.script.compile_asked = Some(source.to_owned());
    panels.intents.push(Intent::Invoke(
        "script.graph.compile".into(),
        Arguments::new().with("reference", Value::Text(reference.to_owned())),
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

fn ordinal(key: NodeKey) -> Value {
    Value::Int(i64::try_from(key.ordinal()).unwrap_or(i64::MAX))
}

/// One line on what the engine made of the graph, and the actions on it.
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
                frame.inputs.script.compile_asked = Some(target.source.clone());
                invoke(frame, "script.graph.compile", with_reference(target));
            }
            if let Some((entity, name, attached)) = &target.selected
                && attached.as_deref() != Some(target.reference.as_str())
                && ui
                    .button(format!("Run on {name}"))
                    .on_hover_text("Attach this graph to the selected entity: it runs during Play")
                    .clicked()
            {
                invoke(
                    frame,
                    "script.graph.attach",
                    with_reference(target).with("entity", Value::Text(entity.to_string())),
                );
            }
        });
    });
}

fn compile_line(target: &Target) -> (Semantic, String) {
    // The engine's answer about this exact text stays true without a runtime; only the absence
    // of one is worth saying when there is no answer.
    match &target.report {
        Some((true, report)) if report.compiled => (
            Semantic::Live,
            format!(
                "Compiled: {} instruction(s), {} handler(s) ({}), {} register(s) kept across a wait",
                report.instructions,
                report.handlers.len(),
                report
                    .handlers
                    .iter()
                    .map(|handler| handler.event.as_str())
                    .collect::<Vec<_>>()
                    .join(", "),
                report.state_slots
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
        "Search events and responses",
        &mut frame.inputs.script.filter,
    );
    let entries = graph_canvas::catalogue_palette(canvas, &frame.inputs.script.filter);
    ui.allocate_ui(
        egui::vec2(ui.available_width(), ui.available_height() * 0.55),
        |ui| {
            if let Some(node_type) = graph_canvas::node_palette(ui, entries, true) {
                let at = graph_canvas::palette_slot(canvas.nodes().count());
                invoke(
                    frame,
                    "script.node.add",
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
        &mut frame.inputs.script.problem,
        |_, key, property, value| {
            edits.push((key, property.name.clone(), value));
            Ok(())
        },
    );
    for (key, property, value) in edits {
        invoke(
            frame,
            "script.node.property.set",
            with_reference(target)
                .with("node", ordinal(key))
                .with("property", Value::Text(property))
                .with("value", Value::Text(value)),
        );
    }
    if let Some(node) = canvas.selection().first().copied() {
        if ui.button("Remove node").clicked() {
            invoke(
                frame,
                "script.node.remove",
                with_reference(target).with("node", ordinal(node)),
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
                    "script.node.disconnect",
                    with_reference(target)
                        .with("from", ordinal(wire.from))
                        .with("from_pin", Value::Text(wire.from_pin.clone()))
                        .with("to", ordinal(wire.to))
                        .with("to_pin", Value::Text(wire.to_pin.clone())),
                );
            }
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
                .with("from", ordinal(connection.from))
                .with("from_pin", Value::Text(connection.from_name.clone()))
                .with("to", ordinal(connection.to))
                .with("to_pin", Value::Text(connection.to_name.clone())),
        );
        Ok(())
    };
    let marks = node_marks(target);
    let mut toggled = None;
    let mut on_gutter = |key: NodeKey| toggled = Some(key);
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
        "Add an event's response from the palette",
        &mut frame.inputs.script.link_source,
        &mut CanvasFeedback {
            link_problem: &mut frame.inputs.script.problem,
            node_alerts: &alerts,
            unwired_inputs: false,
            on_connect: Some(&mut on_connect),
            on_move: Some(&mut on_move),
            node_marks: &marks,
            on_gutter: Some(&mut on_gutter),
        },
    );
    for arguments in requested {
        invoke(frame, "script.node.connect", arguments);
    }
    if let Some(key) = toggled {
        toggle_breakpoint(frame, target, key);
    }
    if let Some((node, at)) = moved {
        invoke(frame, "script.node.move", move_arguments(target, node, at));
    }
}

fn move_arguments(target: &Target, node: NodeKey, at: Layout) -> Arguments {
    with_reference(target)
        .with("node", ordinal(node))
        .with("x", Value::Float(at.x))
        .with("y", Value::Float(at.y))
}

/// Every engine diagnostic as a row that selects its node, and what the graph became.
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
            .id_salt("script-listing")
            .show(ui, |ui| {
                ui.label(secondary(
                    frame.shell,
                    format!(
                        "Program digest {:016x}; reads and writes: {}",
                        report.program_digest,
                        report
                            .accesses
                            .iter()
                            .map(|(resource, mode)| format!(
                                "{} {resource}",
                                if *mode == 0 { "reads" } else { "writes" }
                            ))
                            .collect::<Vec<_>>()
                            .join(", ")
                    ),
                ));
                ui.add(egui::Label::new(
                    egui::RichText::new(&report.listing).monospace(),
                ));
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
    } else if diagnostic.pin.is_empty() {
        format!("node {}", diagnostic.node)
    } else {
        format!("node {} · pin {}", diagnostic.node, diagnostic.pin)
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

/// Raise an event on the selected entity, and what Play's graphs did.
fn play_rows(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    heading(ui, frame.shell, "Play");
    let playing = target.state.as_ref().is_some_and(|state| state.playing);
    if !playing {
        ui.label(secondary(
            frame.shell,
            "Graphs run during Play, on every entity they are attached to. Enter Play to raise \
             events and watch them.",
        ));
    }
    ui.horizontal(|ui| {
        ui.label("Event");
        ui.add(egui::TextEdit::singleline(&mut frame.inputs.script.event).desired_width(120.0));
        for (index, axis) in ["x", "y", "z"].iter().enumerate() {
            ui.label(*axis);
            ui.add(egui::DragValue::new(&mut frame.inputs.script.arguments[index]).speed(0.1));
        }
        let raise = target.selected.as_ref().filter(|_| playing);
        if ui
            .add_enabled(raise.is_some(), egui::Button::new("Raise"))
            .on_disabled_hover_text("Select an entity during Play")
            .clicked()
            && let Some((entity, _, _)) = raise
        {
            let [x, y, z] = frame.inputs.script.arguments;
            let arguments = Arguments::new()
                .with("entity", Value::Text(entity.to_string()))
                .with("event", Value::Text(frame.inputs.script.event.clone()))
                .with("x", Value::Float(x))
                .with("y", Value::Float(y))
                .with("z", Value::Float(z));
            invoke(frame, "script.event.raise", arguments);
        }
    });
    let Some(state) = &target.state else {
        return;
    };
    if !state.playing {
        return;
    }
    ui.label(secondary(
        frame.shell,
        format!(
            "Tick {} · {} instance(s) of compiled programs",
            state.tick,
            state.instances.len()
        ),
    ));
    instances_table(frame.shell, ui, target, state);
    for cue in &state.cues {
        ui.label(secondary(
            frame.shell,
            format!(
                "♪ {} played {} at tick {} ({:.2}, {:.2}, {:.2})",
                name_of(target, cue.node),
                cue.cue,
                cue.tick,
                cue.position[0],
                cue.position[1],
                cue.position[2]
            ),
        ));
    }
}

/// The authored name of an engine identity, or the identity.
fn name_of(target: &Target, node: u64) -> String {
    target
        .names
        .iter()
        .find(|(identity, _)| *identity == node)
        .map_or_else(|| format!("{node:x}"), |(_, name)| name.clone())
}

/// One row per graph instance Play is running.
fn instances_table(shell: &Shell, ui: &mut egui::Ui, target: &Target, state: &PlayState) {
    egui::Grid::new("script-play-instances")
        .striped(true)
        .num_columns(5)
        .show(ui, |ui| {
            for title in ["Entity", "Graph", "Status", "Position", "Runs"] {
                ui.label(secondary(shell, title));
            }
            ui.end_row();
            for instance in &state.instances {
                ui.label(name_of(target, instance.node));
                ui.label(&instance.graph);
                ui.label(if instance.waiting.is_empty() {
                    instance.status.to_owned()
                } else {
                    format!("{} on {}", instance.status, instance.waiting)
                });
                ui.label(format!(
                    "{:.2}, {:.2}, {:.2}",
                    instance.position[0], instance.position[1], instance.position[2]
                ));
                ui.label(instance.runs.to_string());
                ui.end_row();
            }
        });
}

// --- The Play debugger (#84) ----------------------------------------------------------------------

/// How many of the trace's most recent nodes glow on the canvas.
const HIGHLIGHTED: usize = 6;

/// The breakpoints on graph `name`: the ones the editor wants, and any the engine holds besides.
fn breakpoints_of(
    requests: &cy_editor_services::script_requests::ScriptRequests,
    name: &str,
) -> Vec<Breakpoint> {
    let held = requests
        .debug()
        .filter(|debug| debug.playing)
        .map(|debug| debug.breakpoints.as_slice())
        .unwrap_or_default();
    let mut all: Vec<Breakpoint> = requests
        .breakpoints()
        .iter()
        .chain(held)
        .filter(|breakpoint| breakpoint.graph == name)
        .cloned()
        .collect();
    all.sort();
    all.dedup();
    all
}

/// The debugger's marks on this graph's nodes: breakpoints, the paused node and recent execution.
fn node_marks(target: &Target) -> Vec<NodeMark> {
    let mut marks: Vec<NodeMark> = Vec::new();
    for breakpoint in &target.breakpoints {
        mark_of(&mut marks, breakpoint.node).breakpoint = true;
    }
    if let Some(debug) = target.debug.as_ref().filter(|debug| debug.playing) {
        for (node, heat) in debug.recent_nodes(&target.graph_name, HIGHLIGHTED) {
            mark_of(&mut marks, node).heat = heat;
        }
        if debug.paused && debug.paused_graph == target.graph_name {
            mark_of(&mut marks, debug.paused_node).paused = true;
        }
    }
    marks
}

/// `node`'s mark, added when it has none yet.
fn mark_of(marks: &mut Vec<NodeMark>, node: u64) -> &mut NodeMark {
    let index = marks
        .iter()
        .position(|mark| mark.node == node)
        .unwrap_or_else(|| {
            marks.push(NodeMark {
                node,
                ..NodeMark::default()
            });
            marks.len() - 1
        });
    &mut marks[index]
}

/// The entity a breakpoint set from the gutter stops for: the selected one when the panel is
/// scoped to it, else every entity (empty).
fn scoped_entity(frame: &ToolFrame<'_>, target: &Target) -> String {
    match &target.selected {
        Some((entity, _, _)) if frame.inputs.script.break_selected_only => entity.to_string(),
        _ => String::new(),
    }
}

/// A gutter click: remove every breakpoint on the node, or set one.
fn toggle_breakpoint(frame: &mut ToolFrame<'_>, target: &Target, key: NodeKey) {
    let existing: Vec<&Breakpoint> = target
        .breakpoints
        .iter()
        .filter(|breakpoint| breakpoint.node == key.ordinal())
        .collect();
    if existing.is_empty() {
        let entity = scoped_entity(frame, target);
        invoke(
            frame,
            "script.debug.breakpoint",
            breakpoint_arguments(target, key, entity, true),
        );
        return;
    }
    for breakpoint in existing {
        let entity = if breakpoint.entity == 0 {
            String::new()
        } else {
            format!("{:x}", breakpoint.entity)
        };
        invoke(
            frame,
            "script.debug.breakpoint",
            breakpoint_arguments(target, key, entity, false),
        );
    }
}

fn breakpoint_arguments(target: &Target, key: NodeKey, entity: String, enabled: bool) -> Arguments {
    with_reference(target)
        .with("node", ordinal(key))
        .with("entity", Value::Text(entity))
        .with("enabled", Value::Bool(enabled))
}

/// Pause, continue and the two steps, and one line on where Play is.
fn debug_bar(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let debug = target.debug.as_ref().filter(|debug| debug.playing);
    let paused = debug.is_some_and(|debug| debug.paused);
    let running = debug.is_some_and(|debug| debug.debugging && !debug.paused);
    ui.horizontal(|ui| {
        let (role, line) = debug_line(target);
        status(ui, frame.shell, role, &line);
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            let controls = [
                ("Step Into", "script.debug.step", Some("into"), paused),
                ("Step Over", "script.debug.step", Some("over"), paused),
                ("Continue", "script.debug.continue", None, paused),
                ("Pause", "script.debug.pause", None, running),
            ];
            for (label, command, mode, enabled) in controls {
                let clicked = ui
                    .add_enabled(enabled, egui::Button::new(label))
                    .on_disabled_hover_text(if paused || running {
                        "Not while Play is in this state"
                    } else {
                        "The debugger works on Play's graphs; enter Play"
                    })
                    .clicked();
                if clicked {
                    let arguments = mode.map_or_else(Arguments::new, |mode| {
                        Arguments::new().with("mode", Value::Text(mode.into()))
                    });
                    invoke(frame, command, arguments);
                }
            }
            if let Some((_, name, _)) = &target.selected {
                ui.checkbox(
                    &mut frame.inputs.script.break_selected_only,
                    format!("Break only for {name}"),
                )
                .on_hover_text("New breakpoints stop the selected entity's graph and no other");
            }
        });
    });
}

fn debug_line(target: &Target) -> (Semantic, String) {
    let Some(debug) = target.debug.as_ref().filter(|debug| debug.playing) else {
        return (
            Semantic::Neutral,
            format!(
                "Debugger: {} breakpoint(s), sent when Play starts",
                target.breakpoints.len()
            ),
        );
    };
    if !debug.debugging {
        return (
            Semantic::Warning,
            "Debugger: this runtime was built without it (Profile or Shipping)".into(),
        );
    }
    if debug.paused {
        return (
            Semantic::Warning,
            format!(
                "Paused before {} node {} on {} ({}, tick {}) — the whole simulation waits",
                debug.paused_graph,
                debug.paused_node,
                name_of(target, debug.paused_entity),
                debug.reason,
                debug.paused_tick
            ),
        );
    }
    (
        Semantic::Live,
        format!(
            "Running · tick {} · {} breakpoint(s)",
            debug.tick,
            debug.breakpoints.len()
        ),
    )
}

/// What the last reload of this graph did, or why it was refused, each refusal on its node.
fn reload_rows(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    target: &Target,
) {
    let Some((current, reply)) = &target.reload else {
        return;
    };
    if reply.accepted {
        let applied = target
            .debug
            .as_ref()
            .map(|debug| &debug.last_reload)
            .filter(|reload| {
                reload.graph == target.graph_name && reload.generation == reply.generation
            });
        let line = applied.map_or_else(
            || format!("Reloaded into Play: generation {}, swapped at the next tick", reply.generation),
            |reload| {
                format!(
                    "Reloaded into Play: generation {}, {} instance(s), {} variable(s) kept, {} added, \
                     {} dropped",
                    reload.generation, reload.instances, reload.kept, reload.added, reload.dropped
                )
            },
        );
        status(ui, frame.shell, Semantic::Live, &line);
        return;
    }
    status(
        ui,
        frame.shell,
        Semantic::Error,
        if *current {
            "Not reloaded: Play keeps running the previous program"
        } else {
            "The last reload was refused; the graph has changed since"
        },
    );
    for diagnostic in &reply.diagnostics {
        diagnostic_row(frame, ui, canvas, diagnostic);
    }
}

/// The inspected entity's variables and the watched pins, and the buttons that change them.
fn watch_rows(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    canvas: &mut GraphCanvas,
    target: &Target,
) {
    heading(ui, frame.shell, "Watches");
    ui.horizontal(|ui| {
        if let Some(node) = canvas.selection().first().copied()
            && ui
                .button(format!("Watch node {}", node.ordinal()))
                .on_hover_text("Add the selected node's value to the watch list")
                .clicked()
        {
            invoke(
                frame,
                "script.debug.watch",
                with_reference(target)
                    .with("node", ordinal(node))
                    .with("pin", Value::Text("value".into())),
            );
        }
        if let Some((entity, name, _)) = &target.selected
            && ui
                .button(format!("Inspect {name}"))
                .on_hover_text(
                    "Read the selected entity's variables and pins rather than the paused one's",
                )
                .clicked()
        {
            invoke(
                frame,
                "script.debug.inspect",
                with_reference(target).with("entity", Value::Text(entity.to_string())),
            );
        }
    });
    let Some(debug) = target.debug.as_ref().filter(|debug| debug.playing) else {
        ui.label(secondary(
            frame.shell,
            "During Play the paused entity's graph variables and watched pins show here.",
        ));
        return;
    };
    if debug.inspected_entity == 0 {
        ui.label(secondary(
            frame.shell,
            "Nothing to inspect: pause at a breakpoint, or inspect the selected entity.",
        ));
        return;
    }
    ui.label(secondary(
        frame.shell,
        format!(
            "{} on {}",
            debug.inspected_graph,
            name_of(target, debug.inspected_entity)
        ),
    ));
    let mut remove = None;
    egui::Grid::new("script-debug-watches")
        .striped(true)
        .num_columns(3)
        .show(ui, |ui| {
            for variable in &debug.variables {
                ui.label(format!("var {}", variable.name));
                ui.label(egui::RichText::new(variable.value.display()).monospace());
                ui.label("");
                ui.end_row();
            }
            for watch in &debug.watches {
                ui.label(format!("node {} · {}", watch.node, watch.pin));
                ui.label(
                    egui::RichText::new(if watch.found {
                        watch.value.display()
                    } else {
                        "—".into()
                    })
                    .monospace(),
                );
                if ui
                    .small_button("✕")
                    .on_hover_text("Stop watching")
                    .clicked()
                {
                    remove = Some((watch.node, watch.pin.clone()));
                }
                ui.end_row();
            }
        });
    if let Some((node, pin)) = remove {
        invoke(
            frame,
            "script.debug.watch",
            with_reference(target)
                .with("node", Value::Int(i64::try_from(node).unwrap_or(i64::MAX)))
                .with("pin", Value::Text(pin))
                .with("enabled", Value::Bool(false)),
        );
    }
}
