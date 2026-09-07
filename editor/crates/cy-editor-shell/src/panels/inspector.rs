//! The inspector, generated from reflection. Task 1.3.
//!
//! --- THE REQUIREMENT, AND WHAT WOULD VIOLATE IT --------------------------------------------------
//!
//! `editor-ui-ux` requires the inspector to be *generated* — a type the editor has never heard of
//! gets a usable form because it was described, not because somebody wrote a panel for it. The
//! violation is not subtle and it is always locally justified: one `match` on a type's name to give
//! `Transform` a nicer layout, and from then on every type either has bespoke code or looks
//! second-class, and the generation is decoration.
//!
//! **There is no type name in this file.** The whole of the drawing is a walk over
//! `GeneratedInspector::sections()` and a `match` on [`Control`], which is derived from a field's
//! *kind*. `Control::Custom` is the registered override, and it is drawn as a named placeholder here
//! rather than dispatched, because a custom editor is a toolkit object and registering one is a
//! plugin capability that arrives with plugins.
//!
//! --- WHEN THERE IS NOTHING TO GENERATE FROM --------------------------------------------------------
//!
//! It says so, in those words. A catalogue comes from a document's own schema
//! (`Catalogue::of_document`) or from the engine's registered component types over the C ABI
//! (`Catalogue::of_world`). With no runtime attached and a world whose schema declares no types,
//! there is nothing to describe — and the honest answer is a sentence naming both sources, not a
//! hand-written Transform section that would make the panel look finished and the requirement look
//! satisfied.
//!
//! --- THE AXIS LANGUAGE ------------------------------------------------------------------------------
//!
//! X red, Y green, Z blue, from `cy_editor_interface::inspector::lanes`, which calls
//! `cy_editor_visual::axis::colour` — the same function the viewport's gizmo calls. A value in a
//! vector field and a handle in the viewport are the same hue because they are the same call, not
//! because two tables agree.

use cy_editor_core::value::Value;
use cy_editor_documents::selection::CommonValue;
use cy_editor_interface::inspector::{Control, InspectorRow, InspectorSection, lanes};
use cy_editor_visual::colour::Semantic;
use cy_editor_visual::density::TextRole;

use super::{Panels, nothing_here, numeric, secondary, status};
use crate::theme;

/// Draw the inspector.
pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let metrics = panels.metrics();

    // The selection's count and composition, always — never one arbitrary member's values presented
    // as the selection's. `editor-visual-language` requires the sentence; `SelectionSummary` writes
    // it, in the engine's vocabulary.
    ui.label(
        egui::RichText::new(panels.shell.inspector.summary().line())
            .size(metrics.text(TextRole::Title)),
    );
    ui.add_space(metrics.gap() * 0.5);

    if panels.shell.inspector.catalogue().is_none() {
        nothing_here(
            ui,
            panels.shell,
            "No component types are described, so there is nothing to generate a form from.",
            "Types are described by the open world's own schema, or by the engine's registered \
             components when a runtime is attached. The inspector writes no form by hand.",
        );
        return;
    }
    if panels.shell.inspector.sections().is_empty() {
        // Two different states, and telling them apart matters: an empty selection is something the
        // user fixes by clicking, and a selection whose components nothing described is something
        // only the schema or the runtime can fix. One message for both would send them to the wrong
        // one every other time.
        if panels.shell.inspector.summary().count == 0 {
            nothing_here(
                ui,
                panels.shell,
                "Nothing selected.",
                "Select an entity in the Hierarchy.",
            );
        } else {
            nothing_here(
                ui,
                panels.shell,
                "The selection carries no described components.",
                "The inspector generates its rows from reflection and writes none by hand, so a \
                 component appears here once the world's schema or an attached runtime describes \
                 its type.",
            );
        }
        return;
    }

    let mut edits: Vec<Edit> = Vec::new();
    let mut expansions: Vec<(String, bool)> = Vec::new();
    egui::ScrollArea::vertical()
        .auto_shrink([false, false])
        .show(ui, |ui| {
            for section in panels.shell.inspector.sections() {
                draw_section(panels.shell, ui, section, &mut edits, &mut expansions);
            }
        });

    for (type_name, expanded) in expansions {
        panels.shell.inspector.set_expanded(&type_name, expanded);
    }
    apply(panels, edits);
}

/// A committed change a row asked for, applied once the section walk has finished.
///
/// Deferred because committing moves the document's revision, and the inspector rebuilds from the
/// revision — mutating while iterating `sections()` would draw the rest of the frame against rows
/// that no longer exist.
struct Edit {
    component: cy_editor_core::ids::TypeId,
    field: cy_editor_core::ids::FieldId,
    value: Value,
}

fn apply(panels: &mut Panels<'_>, edits: Vec<Edit>) {
    for edit in edits {
        panels
            .shell
            .inspector
            .begin_edit(edit.component, edit.field, edit.value);
        // One transaction per commit, across the whole selection — which is what
        // `GeneratedInspector::commit_edit` does and why the panel does not write fields itself.
        if let Err(problem) = panels.shell.inspector.commit_edit(panels.editor) {
            panels.editor.notifications.post(
                cy_editor_services::notifications::Notification::error(
                    problem.what.clone(),
                    problem,
                ),
            );
        }
    }
}

fn draw_section(
    shell: &cy_editor_interface::shell::Shell,
    ui: &mut egui::Ui,
    section: &InspectorSection,
    edits: &mut Vec<Edit>,
    expansions: &mut Vec<(String, bool)>,
) {
    let metrics = shell.metrics();
    let header = egui::CollapsingHeader::new(
        egui::RichText::new(&section.title).size(metrics.text(TextRole::Section)),
    )
    .id_salt(("inspector-section", section.component))
    .default_open(section.expanded)
    .show(ui, |ui| {
        if let Some(editor) = &section.custom_editor {
            ui.label(secondary(
                shell,
                format!("A registered editor named {editor} replaces this generated form."),
            ));
            return;
        }
        if section.partially_described {
            // "WHEN a type is only partially described THEN the inspector SHALL show what it can and
            // say what it could not." Said, rather than silently shortened.
            ui.label(secondary(
                shell,
                "The source described fewer fields than this type has members.",
            ));
        }
        for row in &section.rows {
            draw_row(shell, ui, row, edits);
        }
        if !section.advanced.is_empty() {
            egui::CollapsingHeader::new(secondary(shell, "Advanced"))
                .id_salt(("inspector-advanced", section.component))
                .default_open(false)
                .show(ui, |ui| {
                    for row in &section.advanced {
                        draw_row(shell, ui, row, edits);
                    }
                });
        }
    });
    let open = header.openness > 0.5;
    if open != section.expanded {
        expansions.push((section.title.clone(), open));
    }
}

fn draw_row(
    shell: &cy_editor_interface::shell::Shell,
    ui: &mut egui::Ui,
    row: &InspectorRow,
    edits: &mut Vec<Edit>,
) {
    let metrics = shell.metrics();
    ui.horizontal(|ui| {
        // A modified field is marked, and the mark is a character rather than a colour alone.
        let name = if row.modified {
            format!("{} {}", "\u{2022}", row.name)
        } else {
            row.name.clone()
        };
        let label = ui.add_sized(
            [metrics.row() * 5.0, metrics.hit_target()],
            egui::Label::new(egui::RichText::new(name).size(metrics.text(TextRole::Body)))
                .truncate(),
        );
        label.on_hover_text(&row.tooltip);
        ui.add_enabled_ui(row.writable, |ui| {
            control(shell, ui, row, edits);
        });
        if let Some(unit) = &row.unit {
            ui.label(secondary(shell, unit.suffix()));
        }
    });
    if let Some(problem) = &row.problem {
        // "Problems SHALL be surfaced at the object that has them — in the ... inspector row."
        ui.indent(("inspector-problem", row.field), |ui| {
            status(ui, shell, Semantic::Error, &problem.because);
            if let Some(remedy) = &problem.remedy {
                ui.label(secondary(shell, remedy.clone()));
            }
        });
    }
}

/// One field's control, chosen by kind and by nothing else.
fn control(
    shell: &cy_editor_interface::shell::Shell,
    ui: &mut egui::Ui,
    row: &InspectorRow,
    edits: &mut Vec<Edit>,
) {
    match &row.control {
        Control::Toggle => {
            let mut held = matches!(row.committed, CommonValue::Same(Value::Bool(true)));
            let mixed = matches!(row.committed, CommonValue::Mixed);
            if ui
                .add(egui::Checkbox::new(
                    &mut held,
                    if mixed { "Mixed" } else { "" },
                ))
                .changed()
            {
                edits.push(Edit {
                    component: row.component,
                    field: row.field,
                    value: Value::Bool(held),
                });
            }
        }
        Control::Number => scalar(ui, row, edits),
        Control::Text => {
            let mut text = match &row.committed {
                CommonValue::Same(Value::Text(value)) => value.clone(),
                CommonValue::Mixed => String::new(),
                other => display(other),
            };
            let response = ui.add(
                egui::TextEdit::singleline(&mut text)
                    .desired_width(metrics_width(shell))
                    .hint_text(if matches!(row.committed, CommonValue::Mixed) {
                        "Mixed"
                    } else {
                        ""
                    }),
            );
            if response.lost_focus() && ui.input(|input| input.key_pressed(egui::Key::Enter)) {
                edits.push(Edit {
                    component: row.component,
                    field: row.field,
                    value: Value::Text(text),
                });
            }
        }
        control @ (Control::Vector(_) | Control::Rotation) => {
            vector(shell, ui, row, control, edits);
        }
        Control::EntityReference => {
            ui.label(numeric(shell, display(&row.committed)))
                .on_hover_text("A reference to another node.");
        }
        Control::Opaque => {
            ui.label(secondary(shell, display(&row.committed)));
        }
        Control::Custom(name) => {
            // A named placeholder, not a dispatch: registering a custom editor is a plugin
            // capability, and pretending to have one here would be per-type editor code by another
            // route.
            ui.label(secondary(shell, format!("Custom editor: {name}")));
        }
    }
}

/// A single number.
fn scalar(ui: &mut egui::Ui, row: &InspectorRow, edits: &mut Vec<Edit>) {
    let mut value = as_f64(&row.committed).unwrap_or_default();
    let response = ui.add(
        egui::DragValue::new(&mut value)
            .speed(0.01)
            .custom_formatter(|value, _| format!("{value:.3}")),
    );
    if response.changed() {
        edits.push(Edit {
            component: row.component,
            field: row.field,
            value: retype(&row.committed, value),
        });
    }
}

/// A vector or a rotation: one lane per axis, labelled and coloured by the axis language.
fn vector(
    shell: &cy_editor_interface::shell::Shell,
    ui: &mut egui::Ui,
    row: &InspectorRow,
    control: &Control,
    edits: &mut Vec<Edit>,
) {
    let mut lanes_out = lane_values(&row.committed);
    let mut changed = false;
    for (index, (label, colour)) in lanes(control, shell.theme).into_iter().enumerate() {
        let Some(value) = lanes_out.get_mut(index) else {
            continue;
        };
        ui.label(
            egui::RichText::new(label)
                .family(egui::FontFamily::Monospace)
                .color(colour.map_or_else(
                    || theme::role(shell.theme, Semantic::SecondaryText),
                    theme::colour,
                )),
        );
        if ui
            .add(
                egui::DragValue::new(value)
                    .speed(0.01)
                    .custom_formatter(|value, _| format!("{value:.3}")),
            )
            .changed()
        {
            changed = true;
        }
    }
    if changed {
        edits.push(Edit {
            component: row.component,
            field: row.field,
            value: rebuild_lanes(&row.committed, &lanes_out),
        });
    }
}

fn metrics_width(shell: &cy_editor_interface::shell::Shell) -> f32 {
    shell.metrics().row() * 8.0
}

/// A value as text, for the controls that show rather than edit.
fn display(value: &CommonValue) -> String {
    match value {
        CommonValue::None => "—".to_string(),
        CommonValue::Mixed => "Mixed".to_string(),
        CommonValue::Same(value) => format!("{value:?}"),
    }
}

#[expect(
    clippy::cast_precision_loss,
    reason = "an integer field beyond 2^53 is not a number a drag control could edit anyway; the \
              committed value is only ever replaced through `retype`, which puts it back as an i64"
)]
fn as_f64(value: &CommonValue) -> Option<f64> {
    match value {
        CommonValue::Same(Value::Int(number)) => Some(*number as f64),
        CommonValue::Same(Value::Float(number)) => Some(f64::from(*number)),
        CommonValue::Same(Value::Double(number)) => Some(*number),
        _ => None,
    }
}

/// A number back in the kind the field holds, so an edit never changes a field's type.
#[expect(
    clippy::cast_possible_truncation,
    reason = "the field's declared kind decides the width, and narrowing to it is the whole point: \
              an integer field must receive an integer and a Float field an f32"
)]
fn retype(committed: &CommonValue, value: f64) -> Value {
    match committed {
        CommonValue::Same(Value::Int(_)) => Value::Int(value as i64),
        CommonValue::Same(Value::Double(_)) => Value::Double(value),
        _ => Value::Float(value as f32),
    }
}

fn lane_values(value: &CommonValue) -> Vec<f64> {
    match value {
        CommonValue::Same(Value::Vec2(lanes)) => {
            lanes.iter().map(|lane| f64::from(*lane)).collect()
        }
        CommonValue::Same(Value::Vec3(lanes)) => {
            lanes.iter().map(|lane| f64::from(*lane)).collect()
        }
        CommonValue::Same(Value::Vec4(lanes) | Value::Quat(lanes)) => {
            lanes.iter().map(|lane| f64::from(*lane)).collect()
        }
        _ => vec![0.0; 4],
    }
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "every vector kind in `Value` is f32; the drag control works in f64 because that is \
              what egui's `DragValue` offers, and this is where it goes back"
)]
fn rebuild_lanes(committed: &CommonValue, lanes: &[f64]) -> Value {
    let lane = |index: usize| lanes.get(index).copied().unwrap_or_default() as f32;
    match committed {
        CommonValue::Same(Value::Vec2(_)) => Value::Vec2([lane(0), lane(1)]),
        CommonValue::Same(Value::Vec4(_)) => Value::Vec4([lane(0), lane(1), lane(2), lane(3)]),
        CommonValue::Same(Value::Quat(_)) => Value::Quat([lane(0), lane(1), lane(2), lane(3)]),
        _ => Value::Vec3([lane(0), lane(1), lane(2)]),
    }
}
