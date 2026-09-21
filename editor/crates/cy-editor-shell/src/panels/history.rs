// SPDX-License-Identifier: MIT
//! Attributed undo history for the active document.

use cy_editor_commands::Arguments;
use cy_editor_visual::colour::Semantic;

use super::{Intent, Panels, heading, nothing_here, numeric, secondary, status};

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        if ui
            .add_enabled(panels.history.can_undo(), egui::Button::new("Undo"))
            .on_disabled_hover_text("Nothing in this document can be undone")
            .clicked()
        {
            panels
                .intents
                .push(Intent::Invoke("edit.undo".into(), Arguments::new()));
        }
        if ui
            .add_enabled(panels.history.can_redo(), egui::Button::new("Redo"))
            .on_disabled_hover_text("Nothing in this document can be redone")
            .clicked()
        {
            panels
                .intents
                .push(Intent::Invoke("edit.redo".into(), Arguments::new()));
        }
        ui.label(secondary(
            panels.shell,
            format!("Cursor {}", panels.history.cursor()),
        ));
    });

    if panels.history.document().is_none() {
        nothing_here(
            ui,
            panels.shell,
            "No document history.",
            "Open a world and make a change.",
        );
        return;
    }

    if let Some(message) = panels.history.truncation_message() {
        status(ui, panels.shell, Semantic::Warning, &message);
    }
    if panels.history.rows().is_empty() {
        nothing_here(
            ui,
            panels.shell,
            "Nothing has changed in this document.",
            "Authoring changes appear here with their attribution.",
        );
        return;
    }

    heading(ui, panels.shell, "Oldest to newest");
    egui::ScrollArea::vertical()
        .id_salt("undo-history")
        .auto_shrink([false, false])
        .show(ui, |ui| {
            for row in panels.history.rows() {
                let state = if row.applied { "Applied" } else { "Undone" };
                ui.horizontal(|ui| {
                    ui.label(numeric(panels.shell, format!("#{}", row.transaction)));
                    ui.strong(&row.name);
                    ui.label(secondary(panels.shell, state));
                });
                ui.horizontal_wrapped(|ui| {
                    ui.label(secondary(
                        panels.shell,
                        format!(
                            "{} · {} operation{}",
                            row.attribution.actor,
                            row.operations,
                            if row.operations == 1 { "" } else { "s" }
                        ),
                    ));
                    if let Some(session) = &row.attribution.session {
                        ui.label(secondary(panels.shell, format!("· session {session}")));
                    }
                });
                if let Some(intent) = &row.attribution.intent {
                    ui.label(secondary(panels.shell, format!("Intent: {intent}")));
                }
                ui.separator();
            }
        });
}
