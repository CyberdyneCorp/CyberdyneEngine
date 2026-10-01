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

use std::path::PathBuf;

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::shell::Shell;
use cy_editor_interface::specialised::Session;
use cy_editor_interface::specialised::graph::{GraphCanvas, Layout, NodeKey};
use cy_editor_interface::specialised::script;
use cy_editor_services::script_graph::{
    CompileReport, DEFAULT_EVENT, PlayState, ScriptGraph, Severity, validate_reference,
};
use cy_editor_services::{AssetCatalogueService, MaterialCatalogueState};
use cy_editor_visual::colour::Semantic;

use super::graph_canvas::{self, CanvasFeedback, GraphConnection, GraphMovement};
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
        }
    }
}

/// What the panel edits this frame.
pub(crate) struct Target {
    reference: String,
    graph: ScriptGraph,
    source: String,
    report: Option<(bool, CompileReport)>,
    state: Option<PlayState>,
    /// The first selected entity, its name, and the graph it runs.
    selected: Option<(NodeId, String, Option<String>)>,
    /// Engine identity to the authored name, for Play's rows.
    names: Vec<(u64, String)>,
    problem: Option<String>,
    connected: bool,
    pending: bool,
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
    ];

    type Target = Target;

    /// The canvas edits live beside the shared canvas (`script_authoring_commands`); the rest are
    /// built in, in `cy_editor_services::script_commands`.
    fn register(registry: &mut cy_editor_commands::Registry) -> cy_editor_core::problem::Result<()> {
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
        let project = &panels.editor.project;
        if !project.source_exists(&reference) {
            ui.label(secondary(
                panels.shell,
                "There is no graph here yet. A new one answers one event; add its response from \
                 the palette.",
            ));
            if ui.button("Create graph").clicked() {
                panels.intents.push(Intent::Invoke(
                    "script.graph.create".into(),
                    Arguments::new().with("reference", Value::Text(reference)),
                ));
            }
            return None;
        }
        let source = match project.read_source(&reference) {
            Ok(source) => source,
            Err(problem) => {
                status(ui, panels.shell, Semantic::Error, &problem.to_string());
                return None;
            }
        };
        let graph = match ScriptGraph::decode(&source) {
            Ok(graph) => graph,
            Err(problem) => {
                status(ui, panels.shell, Semantic::Error, &problem.to_string());
                return None;
            }
        };
        let requests = &panels.editor.backend.script;
        let report = requests
            .report(&reference)
            .map(|(compiled, report)| (compiled == &source, report.clone()));
        let connected = panels.editor.runtime.is_connected();
        ask_for_compile(panels, &reference, &source, report.as_ref());
        let document = panels.editor.documents.get(document_id);
        let selected = panels
            .editor
            .selection
            .get()
            .nodes()
            .next()
            .and_then(|node| {
                let document = document?;
                let name = document.content().node(node)?.name.clone();
                Some((
                    node,
                    name,
                    cy_editor_services::script_commands::attached_graph(document, node),
                ))
            });
        let names = document.map_or_else(Vec::new, |document| {
            let content = document.content();
            content
                .nodes()
                .filter_map(|node| {
                    Some((
                        cy_editor_services::mirror::engine_identity(node),
                        content.node(node)?.name.clone(),
                    ))
                })
                .collect()
        });
        let requests = &panels.editor.backend.script;
        Some(Target {
            reference,
            graph,
            source,
            report,
            state: requests.state().cloned(),
            selected,
            names,
            problem: requests.problem().map(str::to_owned),
            connected,
            pending: requests.pending(),
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
        let height = (ui.available_height() * 0.7).max(300.0);
        ui.allocate_ui(egui::vec2(ui.available_width(), height), |ui| {
            canvas_area(frame, ui, canvas, &target);
        });
        egui::ScrollArea::vertical().show(ui, |ui| {
            compile_rows(frame, ui, canvas, &target);
            play_rows(frame, ui, &target);
        });
    }
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
    if !target.connected {
        return (
            Semantic::Warning,
            "Not compiled: no runtime is attached, and the editor does not compile graphs".into(),
        );
    }
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
    ui.allocate_ui(egui::vec2(ui.available_width(), ui.available_height() * 0.55), |ui| {
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
    });
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
    let mut on_connect = |_: &mut GraphCanvas, connection: &GraphConnection| {
        requested.push(
            with_reference(target)
                .with("from", ordinal(connection.from))
                .with("from_pin", Value::Text(connection.from_name.clone()))
                .with("to", ordinal(connection.to))
                .with("to_pin", Value::Text(connection.to_name.clone())),
        );
        Ok(())
    };
    let mut moved = None;
    let mut on_move = |canvas: &mut GraphCanvas, movement: GraphMovement| {
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
        },
    );
    for arguments in requested {
        invoke(frame, "script.node.connect", arguments);
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
            .add(egui::Label::new(egui::RichText::new(format!("{} {line}", role.glyph())).color(
                crate::theme::role(frame.shell.theme, role),
            ))
            .sense(egui::Sense::click()))
            .on_hover_text("Select the node this is about");
        if response.clicked()
            && let Ok(key) = NodeKey::new(diagnostic.node)
        {
            let _ = canvas.select([key]);
        }
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
            invoke(
                frame,
                "script.event.raise",
                Arguments::new()
                    .with("entity", Value::Text(entity.to_string()))
                    .with("event", Value::Text(frame.inputs.script.event.clone()))
                    .with("x", Value::Float(x))
                    .with("y", Value::Float(y))
                    .with("z", Value::Float(z)),
            );
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
    let name_of = |node: u64| {
        target
            .names
            .iter()
            .find(|(identity, _)| *identity == node)
            .map_or_else(|| format!("{node:x}"), |(_, name)| name.clone())
    };
    egui::Grid::new("script-play-instances")
        .striped(true)
        .num_columns(5)
        .show(ui, |ui| {
            for title in ["Entity", "Graph", "Status", "Position", "Runs"] {
                ui.label(secondary(frame.shell, title));
            }
            ui.end_row();
            for instance in &state.instances {
                ui.label(name_of(instance.node));
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
    for cue in &state.cues {
        ui.label(secondary(
            frame.shell,
            format!(
                "♪ {} played {} at tick {} ({:.2}, {:.2}, {:.2})",
                name_of(cue.node),
                cue.cue,
                cue.tick,
                cue.position[0],
                cue.position[1],
                cue.position[2]
            ),
        ));
    }
}
