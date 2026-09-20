//! Provider-neutral source-control status and actions.

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;
use cy_editor_viewmodels::source_control::Availability;

use super::{Intent, Panels, nothing_here, secondary};

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.strong(panels.source_control.provider());
        if ui.button("Refresh").clicked() {
            panels.intents.push(Intent::Invoke(
                "source-control.refresh".into(),
                Arguments::new(),
            ));
        }
        if panels.source_control.pending() {
            ui.spinner();
            ui.label(secondary(panels.shell, "Refreshing"));
        }
    });
    if let Some(problem) = panels.source_control.problem() {
        super::status(
            ui,
            panels.shell,
            cy_editor_visual::colour::Semantic::Error,
            problem,
        );
    }
    if let Some(path) = panels.source_control.history_path() {
        ui.collapsing(format!("History · {path}"), |ui| {
            if panels.source_control.history_rows().is_empty() {
                ui.label(secondary(panels.shell, "No submitted revisions."));
            }
            for revision in panels.source_control.history_rows() {
                ui.horizontal_wrapped(|ui| {
                    ui.label(super::numeric(panels.shell, &revision.revision));
                    ui.label(secondary(
                        panels.shell,
                        format!("{} — {}", revision.author, revision.description),
                    ));
                });
            }
        });
    }
    if panels.source_control.rows().is_empty() {
        nothing_here(
            ui,
            panels.shell,
            "No source-control status yet.",
            "Open a document or choose Refresh.",
        );
        return;
    }

    let rows = panels.source_control.rows().to_vec();
    egui::ScrollArea::vertical()
        .id_salt("source-control")
        .auto_shrink([false, false])
        .show(ui, |ui| {
            for row in &rows {
                row_ui(ui, panels, row);
                ui.separator();
            }
        });
}

fn row_ui(
    ui: &mut egui::Ui,
    panels: &mut Panels<'_>,
    row: &cy_editor_viewmodels::SourceControlRow,
) {
    ui.horizontal_wrapped(|ui| {
        ui.strong(&row.path);
        ui.label(secondary(panels.shell, &row.state_label));
        if let Some(owner) = &row.locked_by {
            ui.label(secondary(panels.shell, format!("Locked by {owner}")));
        }
    });
    ui.horizontal_wrapped(|ui| {
        for (label, command, availability) in [
            (
                "History",
                "source-control.history",
                panels.source_control.history(),
            ),
            (
                "Check Out",
                "source-control.checkout",
                panels.source_control.check_out(),
            ),
            (
                "Revert",
                "source-control.revert",
                panels.source_control.revert(),
            ),
            (
                if row.locked_by.is_some() {
                    "Unlock"
                } else {
                    "Lock"
                },
                if row.locked_by.is_some() {
                    "source-control.unlock"
                } else {
                    "source-control.lock"
                },
                panels.source_control.lock(),
            ),
        ] {
            action(ui, label, command, availability, &row.path, panels.intents);
        }
    });
    ui.horizontal(|ui| {
        ui.text_edit_singleline(&mut panels.inputs.source_control_description);
        let response = ui
            .add_enabled(
                panels.source_control.submit().is_available()
                    && !panels.inputs.source_control_description.trim().is_empty(),
                egui::Button::new("Submit"),
            )
            .on_disabled_hover_text(unavailable(
                panels.source_control.submit(),
                "Enter a change description",
            ));
        if response.clicked() {
            panels.intents.push(Intent::Invoke(
                "source-control.submit".into(),
                Arguments::new()
                    .with("path", Value::Text(row.path.clone()))
                    .with(
                        "description",
                        Value::Text(panels.inputs.source_control_description.clone()),
                    ),
            ));
        }
    });
}

fn action(
    ui: &mut egui::Ui,
    label: &str,
    command: &str,
    availability: &Availability,
    path: &str,
    intents: &mut Vec<Intent>,
) {
    let response = ui
        .add_enabled(availability.is_available(), egui::Button::new(label))
        .on_disabled_hover_text(unavailable(availability, "Unavailable"));
    if response.clicked() {
        intents.push(Intent::Invoke(
            command.into(),
            Arguments::new().with("path", Value::Text(path.to_string())),
        ));
    }
}

fn unavailable<'a>(availability: &'a Availability, fallback: &'a str) -> &'a str {
    match availability {
        Availability::Available => fallback,
        Availability::Unavailable(reason) => reason,
    }
}
