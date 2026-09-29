// SPDX-License-Identifier: MIT
//! The physics tools: the viewport's physics debug layers, the selected entity's joint, and the
//! ragdoll profile's status. Issue #29.
//!
//! Not one of the sixteen specialised domains — `editor-architecture` names no physics editor —
//! but drawn in the same frame: the scaffold's header with Undo and Redo, its diagnostics area,
//! and its MCP and undo parity check over [`COMMANDS`] at start-up. The panel changes nothing
//! itself. Every control is one registered command: a layer is `viewport.physics.<layer>` (a view,
//! so a read), and every joint edit is one `physics.joint.*` transaction, committed when a drag or
//! a typed edit finishes so one gesture is one undo entry.
//!
//! The viewport drawing is the ENGINE's: the layers come from its physics world while a world plays
//! or is paused, and the selected joint's gizmo — anchors, axis and limits — from the authored world
//! through the function the solver draws a simulated constraint with. Nothing here paints a
//! collider or a joint.

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_documents::Document;
use cy_editor_interface::shell::Shell;
use cy_editor_services::bodies::body_of;
use cy_editor_services::joints::{self, JointField, JointKind, JointSpec};
use cy_editor_viewport::PhysicsLayer;
use cy_editor_visual::colour::Semantic;

use super::specialised::{ToolDiagnostic, diagnostics_area, header};
use super::{Inputs, Intent, Panels, heading, nothing_here, secondary};

/// The header's title.
pub(crate) const TITLE: &str = "Physics";
/// How the parity check names this panel.
pub(crate) const PANEL: &str = "the physics panel";

/// Every registered command the panel invokes. `register_specialised_tools` refuses start-up if
/// one is unregistered, hidden from agents, or not an undoable mutation or a read.
pub(crate) const COMMANDS: &[&str] = &[
    "viewport.physics.colliders",
    "viewport.physics.contacts",
    "viewport.physics.constraints",
    "viewport.physics.sleep-state",
    "viewport.physics.velocities",
    "viewport.physics.centres-of-mass",
    "viewport.physics.broad-phase-bounds",
    "viewport.physics.hide-all",
    "scene.add-body",
    "physics.joint.add",
    "physics.joint.set",
    "physics.joint.remove",
];

/// Why a ragdoll profile cannot be set up from here yet, in the words the panel shows.
pub(crate) const RAGDOLL_SCOPE: &str = "Ragdoll profiles are generated from a skeleton \
    (cy::physics::ragdoll::Profile::generate), and this editor cannot load one yet: model import \
    stops before step 7, importing skeletons. Profile setup arrives with skeleton import.";

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    header(panels, ui, TITLE);
    diagnostics_area(ui, panels.shell, &diagnostics());
    egui::ScrollArea::vertical()
        .id_salt("physics-panel")
        .auto_shrink([false, false])
        .show(ui, |ui| {
            layers(panels, ui);
            ui.separator();
            joint_section(panels, ui);
        });
}

/// What the panel holds for the diagnostics area: the one tool it cannot offer yet.
fn diagnostics() -> Vec<ToolDiagnostic> {
    vec![ToolDiagnostic {
        role: Semantic::Warning,
        message: RAGDOLL_SCOPE.to_string(),
    }]
}

// --- The viewport's layers ----------------------------------------------------------------------------

fn layers(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    heading(ui, panels.shell, "Viewport physics layers");
    ui.label(secondary(
        panels.shell,
        "Drawn by the engine from its physics world while the world plays or is paused.",
    ));
    let current = panels.editor.viewports.focused().physics;
    ui.horizontal_wrapped(|ui| {
        for layer in PhysicsLayer::ALL {
            let mut on = current.contains(layer);
            let response = ui.checkbox(&mut on, layer.label()).on_hover_text(format!(
                "Shows {}. {}",
                layer.shows(),
                layer.how_to_read()
            ));
            if response.changed() {
                panels.intents.push(Intent::Invoke(
                    layer.command_id(),
                    Arguments::new().with("state", Value::Text(on_off(on).to_string())),
                ));
            }
        }
    });
    if ui
        .add_enabled(!current.is_empty(), egui::Button::new("Hide all layers"))
        .clicked()
    {
        panels.intents.push(Intent::Invoke(
            "viewport.physics.hide-all".into(),
            Arguments::new(),
        ));
    }
}

const fn on_off(on: bool) -> &'static str {
    if on { "on" } else { "off" }
}

// --- The selected entity's joint ------------------------------------------------------------------------

/// What the joint section is looking at this frame.
enum Subject {
    /// Nothing to author on, with the sentence that says why and what would help.
    Nothing(&'static str, &'static str),
    /// A single entity with no body.
    Bodiless(NodeId),
    /// A body with no joint.
    Unjoined(NodeId),
    /// A body and its joint.
    Joined(NodeId, JointSpec),
}

fn subject(panels: &Panels<'_>) -> Subject {
    let Some(document) = active_document(panels) else {
        return Subject::Nothing("No world is open.", "Open a world to author joints.");
    };
    let mut selected = panels.editor.selection.get().nodes();
    let (Some(node), None) = (selected.next(), selected.next()) else {
        return Subject::Nothing(
            "Select one entity.",
            "Joints are authored on the entity carrying body A.",
        );
    };
    if body_of(document, node).is_none() {
        return Subject::Bodiless(node);
    }
    joints::joint_of(document, node)
        .map_or(Subject::Unjoined(node), |spec| Subject::Joined(node, spec))
}

fn active_document<'a>(panels: &'a Panels<'_>) -> Option<&'a Document> {
    let id = panels.editor.workspace.active()?;
    panels.editor.documents.get(id)
}

fn joint_section(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    heading(ui, panels.shell, "Joint");
    match subject(panels) {
        Subject::Nothing(what, remedy) => nothing_here(ui, panels.shell, what, remedy),
        Subject::Bodiless(node) => {
            ui.label(secondary(
                panels.shell,
                "This entity has no physics body. A joint joins two bodies.",
            ));
            if ui.button("Add physics body").clicked() {
                panels.intents.push(Intent::Invoke(
                    "scene.add-body".into(),
                    Arguments::new().with("entity", Value::Text(node.to_string())),
                ));
            }
        }
        Subject::Unjoined(node) => add_form(panels, ui, node),
        Subject::Joined(node, spec) => joint_form(panels, ui, node, spec),
    }
}

/// Every other entity with a body, named, for the target picker.
fn body_choices(document: &Document, subject: NodeId) -> Vec<(NodeId, String)> {
    document
        .content()
        .nodes()
        .filter(|node| *node != subject && body_of(document, *node).is_some())
        .map(|node| (node, display_name(document, node)))
        .collect()
}

fn display_name(document: &Document, node: NodeId) -> String {
    let name = document
        .content()
        .node(node)
        .map(|state| state.name.clone())
        .unwrap_or_default();
    if name.is_empty() {
        format!("Entity {}", &node.to_string()[..8])
    } else {
        name
    }
}

fn add_form(panels: &mut Panels<'_>, ui: &mut egui::Ui, node: NodeId) {
    let choices =
        active_document(panels).map_or_else(Vec::new, |document| body_choices(document, node));
    let inputs = &mut *panels.inputs;
    let kind = JointKind::from_keyword(&inputs.physics_new_kind).unwrap_or(JointKind::Hinge);
    ui.horizontal(|ui| {
        ui.label("Kind");
        kind_picker(ui, "physics-new-kind", kind, &mut inputs.physics_new_kind);
    });
    ui.horizontal(|ui| {
        ui.label("Joined to");
        target_picker(
            ui,
            "physics-new-target",
            &choices,
            &mut inputs.physics_new_target,
        );
    });
    if ui.button("Add joint").clicked() {
        let target = inputs
            .physics_new_target
            .map_or_else(String::new, |target| target.to_string());
        panels.intents.push(Intent::Invoke(
            "physics.joint.add".into(),
            Arguments::new()
                .with("entity", Value::Text(node.to_string()))
                .with("kind", Value::Text(kind.keyword().to_string()))
                .with("target", Value::Text(target)),
        ));
    }
}

fn kind_picker(ui: &mut egui::Ui, id: &str, current: JointKind, word: &mut String) -> bool {
    let mut changed = false;
    egui::ComboBox::from_id_salt(id)
        .selected_text(current.label())
        .show_ui(ui, |ui| {
            for kind in JointKind::ALL {
                if ui.selectable_label(kind == current, kind.label()).clicked() {
                    *word = kind.keyword().to_string();
                    changed = kind != current;
                }
            }
        });
    changed
}

fn target_picker(
    ui: &mut egui::Ui,
    id: &str,
    choices: &[(NodeId, String)],
    selected: &mut Option<NodeId>,
) -> bool {
    let before = *selected;
    let text = selected
        .and_then(|node| choices.iter().find(|(choice, _)| *choice == node))
        .map_or_else(|| "The world".to_string(), |(_, name)| name.clone());
    egui::ComboBox::from_id_salt(id)
        .selected_text(text)
        .show_ui(ui, |ui| {
            ui.selectable_value(selected, None, "The world");
            for (node, name) in choices {
                ui.selectable_value(selected, Some(*node), name);
            }
        });
    *selected != before
}

fn joint_form(panels: &mut Panels<'_>, ui: &mut egui::Ui, node: NodeId, spec: JointSpec) {
    let (choices, target) = active_document(panels).map_or_else(
        || (Vec::new(), None),
        |document| {
            (
                body_choices(document, node),
                joints::node_of_identity(document, spec.target),
            )
        },
    );
    let shell = &*panels.shell;
    let inputs = &mut *panels.inputs;
    let mut commits: Vec<(JointField, String)> = Vec::new();

    ui.horizontal(|ui| {
        ui.label("Kind");
        let mut word = spec.kind.keyword().to_string();
        if kind_picker(ui, "physics-kind", spec.kind, &mut word) {
            commits.push((JointField::Kind, word));
        }
    });
    ui.horizontal(|ui| {
        ui.label("Joined to");
        let mut selected = target;
        if target_picker(ui, "physics-target", &choices, &mut selected) {
            commits.push((
                JointField::Target,
                selected.map_or_else(String::new, |node| node.to_string()),
            ));
        }
    });
    if spec.target != 0 && target.is_none() {
        status(
            ui,
            shell,
            "The entity this joint names is gone; it joins the world at play.",
        );
    }
    ui.label(secondary(
        shell,
        "The engine draws this joint in the viewport: its anchors, its axis and its limits.",
    ));
    for field in JointField::ALL {
        if field == JointField::Kind || field == JointField::Target || !spec.kind.uses(field) {
            continue;
        }
        if let Some(text) = field_row(ui, inputs, spec.kind, field, &spec.value(field)) {
            commits.push((field, text));
        }
    }
    let remove = ui.button("Remove joint").clicked();

    for (field, text) in commits {
        panels.intents.push(set_intent(node, field, text));
    }
    if remove {
        panels.intents.push(Intent::Invoke(
            "physics.joint.remove".into(),
            Arguments::new().with("entity", Value::Text(node.to_string())),
        ));
    }
}

fn status(ui: &mut egui::Ui, shell: &Shell, text: &str) {
    super::status(ui, shell, Semantic::Warning, text);
}

fn set_intent(node: NodeId, field: JointField, text: String) -> Intent {
    Intent::Invoke(
        "physics.joint.set".into(),
        Arguments::new()
            .with("entity", Value::Text(node.to_string()))
            .with("field", Value::Text(field.name().to_string()))
            .with("value", Value::Text(text)),
    )
}

/// What a person reads beside a field.
fn field_label(kind: JointKind, field: JointField) -> String {
    match field {
        JointField::Anchor => "Anchor".into(),
        JointField::Axis => "Axis".into(),
        JointField::LimitMin => format!("Minimum {}", kind.range_meaning()),
        JointField::LimitMax => format!("Maximum {}", kind.range_meaning()),
        JointField::SwingY => "Swing about Y".into(),
        JointField::SwingZ => "Swing about Z".into(),
        JointField::LinearMin => "Travel minimum".into(),
        JointField::LinearMax => "Travel maximum".into(),
        JointField::AngularMin => "Rotation minimum".into(),
        JointField::AngularMax => "Rotation maximum".into(),
        JointField::MotorVelocity => "Motor speed".into(),
        JointField::MotorMaxForce => "Motor force cap".into(),
        JointField::Ratio => "Ratio".into(),
        JointField::BreakForce => "Break force".into(),
        JointField::BreakTorque => "Break torque".into(),
        JointField::CollideConnected => "Bodies collide".into(),
        JointField::Kind | JointField::Target => field.name().into(),
    }
}

/// One field's control. Answers the text to commit when a gesture on it finished.
///
/// A drag or a typed edit writes into `inputs.physics_pending` while it lasts and is committed once,
/// when the pointer is released or the field loses focus: one gesture, one transaction, one undo.
fn field_row(
    ui: &mut egui::Ui,
    inputs: &mut Inputs,
    kind: JointKind,
    field: JointField,
    committed: &Value,
) -> Option<String> {
    let shown = match &inputs.physics_pending {
        Some((pending, value)) if *pending == field => value.clone(),
        _ => committed.clone(),
    };
    let mut commit = None;
    ui.horizontal(|ui| {
        ui.label(field_label(kind, field))
            .on_hover_text(field.help());
        let (edited, finished) = edit_value(ui, &shown);
        if let Some(value) = edited {
            inputs.physics_pending = Some((field, value));
        }
        if finished
            && let Some((pending, value)) = inputs.physics_pending.take()
            && pending == field
            && &value != committed
        {
            commit = Some(value_text(&value));
        }
    });
    commit
}

/// Draw one value's control: `(the edited value, whether the gesture finished)`.
fn edit_value(ui: &mut egui::Ui, value: &Value) -> (Option<Value>, bool) {
    match value {
        Value::Float(number) => {
            let mut number = *number;
            let response = ui.add(
                egui::DragValue::new(&mut number)
                    .speed(0.01)
                    .max_decimals(3),
            );
            (
                response.changed().then_some(Value::Float(number)),
                response.drag_stopped() || response.lost_focus(),
            )
        }
        Value::Vec3(lanes) => {
            let mut lanes = *lanes;
            let mut changed = false;
            let mut finished = false;
            for lane in &mut lanes {
                let response = ui.add(egui::DragValue::new(lane).speed(0.01).max_decimals(3));
                changed |= response.changed();
                finished |= response.drag_stopped() || response.lost_focus();
            }
            (changed.then_some(Value::Vec3(lanes)), finished)
        }
        Value::Bool(flag) => {
            let mut flag = *flag;
            let changed = ui.checkbox(&mut flag, "").changed();
            (changed.then_some(Value::Bool(flag)), changed)
        }
        _ => (None, false),
    }
}

/// A value as the text `physics.joint.set` reads.
fn value_text(value: &Value) -> String {
    match value {
        Value::Float(number) => number.to_string(),
        Value::Vec3([x, y, z]) => format!("{x} {y} {z}"),
        Value::Bool(flag) => flag.to_string(),
        Value::Text(text) => text.clone(),
        other => other.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_committed_value_is_the_text_the_set_command_reads() {
        assert_eq!(value_text(&Value::Float(-0.5)), "-0.5");
        assert_eq!(value_text(&Value::Vec3([1.0, 0.0, 2.5])), "1 0 2.5");
        assert_eq!(value_text(&Value::Bool(true)), "true");
    }

    /// Frames of one `field_row`, with the pointer events a drag produces.
    struct Frames {
        ctx: egui::Context,
        inputs: Inputs,
        /// Where the drag value was drawn in the last frame, from the accessibility tree.
        drag: Option<egui::Pos2>,
    }

    impl Frames {
        fn new() -> Self {
            let ctx = egui::Context::default();
            ctx.enable_accesskit();
            Self {
                ctx,
                inputs: Inputs::default(),
                drag: None,
            }
        }

        fn run(&mut self, events: Vec<egui::Event>, committed: &Value) -> Option<String> {
            let raw = egui::RawInput {
                screen_rect: Some(egui::Rect::from_min_size(
                    egui::Pos2::ZERO,
                    egui::vec2(600.0, 200.0),
                )),
                events,
                ..Default::default()
            };
            let mut commit = None;
            let Self { ctx, inputs, drag } = self;
            let mut output = ctx.run_ui(raw, |ui| {
                egui::CentralPanel::default().show(ui, |ui| {
                    commit = field_row(
                        ui,
                        inputs,
                        JointKind::Hinge,
                        JointField::LimitMax,
                        committed,
                    );
                });
            });
            output.textures_delta.clear();
            if let Some(update) = output.platform_output.accesskit_update.take() {
                *drag = update
                    .nodes
                    .iter()
                    .find(|(_, node)| node.role() == egui::accesskit::Role::SpinButton)
                    .and_then(|(_, node)| node.bounds())
                    .map(|bounds| {
                        #[expect(
                            clippy::cast_possible_truncation,
                            reason = "a widget's bounds are a few hundred points"
                        )]
                        let centre = egui::pos2(
                            ((bounds.x0 + bounds.x1) / 2.0) as f32,
                            ((bounds.y0 + bounds.y1) / 2.0) as f32,
                        );
                        centre
                    });
            }
            commit
        }

        fn pointer(
            &mut self,
            at: egui::Pos2,
            pressed: Option<bool>,
            committed: &Value,
        ) -> Option<String> {
            let mut events = vec![egui::Event::PointerMoved(at)];
            if let Some(pressed) = pressed {
                events.push(egui::Event::PointerButton {
                    pos: at,
                    button: egui::PointerButton::Primary,
                    pressed,
                    modifiers: egui::Modifiers::NONE,
                });
            }
            self.run(events, committed)
        }
    }

    #[test]
    fn a_dragged_field_is_committed_once_when_the_drag_is_released() {
        let committed = Value::Float(0.0);
        let mut frames = Frames::new();
        assert_eq!(frames.run(Vec::new(), &committed), None);
        let grab = frames.drag.expect("the row draws a drag value");
        let mut commits = Vec::new();
        commits.extend(frames.pointer(grab, None, &committed));
        commits.extend(frames.pointer(grab, Some(true), &committed));
        for step in 1_u8..=5 {
            let to = grab + egui::vec2(12.0 * f32::from(step), 0.0);
            commits.extend(frames.pointer(to, None, &committed));
        }
        assert!(
            commits.is_empty(),
            "nothing is committed mid-drag: {commits:?}"
        );
        assert!(
            frames.inputs.physics_pending.is_some(),
            "the drag is held as pending while it lasts"
        );
        let release = grab + egui::vec2(60.0, 0.0);
        commits.extend(frames.pointer(release, Some(false), &committed));
        commits.extend(frames.run(Vec::new(), &committed));
        let [text] = commits.as_slice() else {
            panic!("one release is one commit: {commits:?}");
        };
        let value: f32 = text.parse().expect("a number");
        assert!(value > 0.0, "the drag moved the value: {value}");
        assert!(frames.inputs.physics_pending.is_none());
    }

    #[test]
    fn every_field_a_kind_uses_has_a_label_a_person_can_read() {
        for kind in JointKind::ALL {
            for field in JointField::ALL {
                if kind.uses(field) {
                    let label = field_label(kind, field);
                    assert!(!label.contains('_'), "{kind:?} {field:?}: {label}");
                }
            }
        }
    }
}
