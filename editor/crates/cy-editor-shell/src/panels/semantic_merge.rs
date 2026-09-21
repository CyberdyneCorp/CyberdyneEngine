// SPDX-License-Identifier: MIT
//! Identity-keyed semantic Diff and Merge panels.

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;

use super::{Intent, Panels, heading, nothing_here, secondary};

pub(super) fn show_diff(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    revision_inputs(panels, ui);
    if panels.diff.local().is_empty() && panels.diff.incoming().is_empty() {
        nothing_here(
            ui,
            panels.shell,
            "No comparison is active.",
            "Enter a common base and incoming source-control revision, then choose Compare.",
        );
        return;
    }
    heading(ui, panels.shell, "Local from base");
    rows(ui, panels.diff.local());
    heading(ui, panels.shell, "Incoming from base");
    rows(ui, panels.diff.incoming());
}

pub(super) fn show_merge(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    revision_inputs(panels, ui);
    if panels.merge.path().is_empty() {
        nothing_here(
            ui,
            panels.shell,
            "No merge is pending.",
            "Compare source-control revisions in this panel or Semantic Diff.",
        );
        return;
    }
    ui.label(format!(
        "{} · {} -> {}",
        panels.merge.path(),
        panels.merge.base_revision(),
        panels.merge.incoming_revision()
    ));
    ui.label(secondary(
        panels.shell,
        format!(
            "{} non-overlapping changes apply automatically",
            panels.merge.automatic_count()
        ),
    ));
    let conflicts = panels.merge.conflicts().to_vec();
    egui::ScrollArea::vertical()
        .id_salt("semantic-merge-conflicts")
        .show(ui, |ui| {
            for conflict in conflicts {
                ui.group(|ui| {
                    ui.label(format!(
                        "Conflict {} · {}",
                        conflict.index + 1,
                        conflict.target
                    ));
                    ui.label(format!("Local: {}", conflict.local));
                    ui.label(format!("Incoming: {}", conflict.incoming));
                    ui.label(secondary(panels.shell, &conflict.reason));
                    if let Some(decision) = &conflict.decision {
                        ui.label(format!("Decision: {decision}"));
                    }
                    ui.horizontal(|ui| {
                        if ui.button("Keep Local").clicked() {
                            resolve(panels, conflict.index, "local", None);
                        }
                        if ui.button("Accept Incoming").clicked() {
                            resolve(panels, conflict.index, "incoming", None);
                        }
                    });
                    ui.horizontal(|ui| {
                        let replacement = panels
                            .inputs
                            .merge_replacements
                            .entry(conflict.index)
                            .or_default();
                        ui.add(
                            egui::TextEdit::singleline(replacement)
                                .hint_text("typed literal, e.g. float:1.5")
                                .desired_width(190.0),
                        );
                        let submit = ui.button("Use Replacement").clicked();
                        let replacement = submit.then(|| replacement.clone());
                        if let Some(replacement) = replacement {
                            resolve(panels, conflict.index, "replacement", Some(replacement));
                        }
                    });
                });
            }
        });
}

fn revision_inputs(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.label("Base");
        ui.text_edit_singleline(&mut panels.inputs.merge_base_revision);
        ui.label("Incoming");
        ui.text_edit_singleline(&mut panels.inputs.merge_incoming_revision);
        let enabled = !panels.inputs.merge_base_revision.trim().is_empty()
            && !panels.inputs.merge_incoming_revision.trim().is_empty();
        if ui
            .add_enabled(enabled, egui::Button::new("Compare"))
            .clicked()
        {
            panels.intents.push(Intent::Invoke(
                "document.merge-start".into(),
                Arguments::new()
                    .with(
                        "base",
                        Value::Text(panels.inputs.merge_base_revision.trim().to_string()),
                    )
                    .with(
                        "incoming",
                        Value::Text(panels.inputs.merge_incoming_revision.trim().to_string()),
                    ),
            ));
        }
    });
}

fn resolve(panels: &mut Panels<'_>, index: usize, choice: &str, replacement: Option<String>) {
    let mut arguments = Arguments::new()
        .with(
            "index",
            Value::Int(i64::try_from(index).unwrap_or(i64::MAX)),
        )
        .with("choice", Value::Text(choice.to_string()));
    if let Some(replacement) = replacement {
        arguments = arguments.with("replacement", Value::Text(replacement));
    }
    panels
        .intents
        .push(Intent::Invoke("document.merge-resolve".into(), arguments));
}

fn rows(ui: &mut egui::Ui, rows: &[cy_editor_viewmodels::DiffRow]) {
    egui::ScrollArea::vertical().show(ui, |ui| {
        for row in rows {
            ui.horizontal(|ui| {
                ui.monospace(&row.target);
                ui.label(&row.summary);
            });
        }
    });
}

#[cfg(test)]
mod tests {
    #[test]
    fn diff_and_merge_panels_only_render_models_and_emit_commands() {
        let source = include_str!("semantic_merge.rs");
        for forbidden in [
            concat!("world", "file"),
            concat!("Document", "Content"),
            concat!(".rec", "ord("),
            concat!(".app", "ly("),
            concat!("std::fs", "::"),
        ] {
            assert!(
                !source.contains(forbidden),
                "semantic panels crossed the presentation boundary through {forbidden}"
            );
        }
        assert!(source.contains("document.merge-start"));
        assert!(source.contains("document.merge-resolve"));
    }
}
