// SPDX-License-Identifier: MIT
//! Swift Workspace file tree, tabs, conflict-safe buffers, and build loop.

use cy_editor_commands::Arguments;

use super::{Intent, Panels, nothing_here, secondary};

mod swift_highlight;

pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let metrics = panels.metrics();
    match panels.editor.source_language.state() {
        cy_editor_services::LanguageServiceState::NotStarted => {
            ui.label(secondary(
                panels.shell,
                "SourceKit-LSP starts when a source is opened.",
            ));
        }
        cy_editor_services::LanguageServiceState::Starting => {
            ui.label(secondary(panels.shell, "Starting SourceKit-LSP…"));
        }
        cy_editor_services::LanguageServiceState::Available => {
            ui.label(secondary(panels.shell, "SourceKit-LSP connected"));
        }
        cy_editor_services::LanguageServiceState::Unavailable { reason } => {
            ui.colored_label(egui::Color32::YELLOW, reason);
        }
    }
    let mut open = None;
    let available = ui.available_size();
    let sources_width = 190.0_f32.min(available.x * 0.35);
    ui.horizontal(|ui| {
        ui.allocate_ui_with_layout(
            egui::vec2(sources_width, available.y),
            egui::Layout::top_down(egui::Align::Min),
            |ui| {
                ui.label(secondary(panels.shell, "Swift Sources"));
                egui::ScrollArea::vertical()
                    .id_salt("swift-files")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        for path in panels.source_workspace.files() {
                            let name = path.rsplit('/').next().unwrap_or(path);
                            if ui
                                .add_sized(
                                    [ui.available_width(), metrics.hit_target()],
                                    egui::Button::new(name).selected(false),
                                )
                                .on_hover_text(path)
                                .clicked()
                            {
                                open = Some(path.clone());
                            }
                        }
                    });
            },
        );
        ui.separator();
        ui.allocate_ui_with_layout(
            egui::vec2(ui.available_width(), available.y),
            egui::Layout::top_down(egui::Align::Min),
            |ui| editor(panels, ui),
        );
    });
    if let Some(path) = open {
        panels.intents.push(Intent::OpenSource(path));
    }
    ui.add_space(metrics.gap() * 0.25);
}

fn editor(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let tabs: Vec<_> = panels
        .source_workspace
        .buffers()
        .iter()
        .enumerate()
        .map(|(index, buffer)| {
            (
                index,
                buffer.path().to_string(),
                buffer.is_dirty(),
                panels
                    .source_workspace
                    .active()
                    .is_some_and(|active| active.path() == buffer.path()),
            )
        })
        .collect();
    let mut activate = None;
    ui.horizontal_wrapped(|ui| {
        for (index, path, dirty, active) in &tabs {
            let name = path.rsplit('/').next().unwrap_or(path);
            let label = if *dirty {
                format!("{name} ●")
            } else {
                name.to_string()
            };
            if ui.selectable_label(*active, label).clicked() {
                activate = Some(*index);
            }
        }
    });
    if let Some(index) = activate {
        panels.source_workspace.activate(index);
    }

    let Some(active) = panels.source_workspace.active() else {
        nothing_here(
            ui,
            panels.shell,
            "No Swift source is open.",
            "Choose a .swift file from the project tree.",
        );
        return;
    };
    let path = active.path().to_string();
    let dirty = active.is_dirty();
    let mut text = active.text().to_string();
    let diagnostics = active.diagnostics().to_vec();
    let conflict = active.conflict().cloned();
    let cursor_request = panels
        .source_workspace
        .active_mut()
        .and_then(cy_editor_viewmodels::SourceBufferViewModel::take_cursor_request);

    ui.horizontal(|ui| {
        if ui.add_enabled(dirty, egui::Button::new("Save")).clicked() {
            panels.intents.push(Intent::SaveSource);
        }
        if ui
            .add_enabled(!dirty, egui::Button::new("Build"))
            .on_hover_text(if dirty {
                "Save the source before building"
            } else {
                "Compile Swift sources and update the running Play session"
            })
            .clicked()
        {
            panels
                .intents
                .push(Intent::Invoke("project.build".into(), Arguments::new()));
        }
        if ui
            .button("Reload Module")
            .on_hover_text("Apply the latest successful build to the running Play session")
            .clicked()
        {
            panels
                .intents
                .push(Intent::Invoke("project.reload".into(), Arguments::new()));
        }
        ui.label(secondary(panels.shell, &path));
    });
    build_status(panels, ui);

    if let Some(conflict) = conflict {
        conflict_controls(panels, ui, &conflict);
    }
    let diagnostic_height = if diagnostics.is_empty() {
        0.0
    } else {
        panels.metrics().hit_target() * diagnostics.len().min(4) as f32
    };
    let code_height = (ui.available_height() - diagnostic_height).max(160.0);
    let rows = (code_height / ui.text_style_height(&egui::TextStyle::Monospace)) as usize;
    let response = egui::ScrollArea::both()
        .id_salt("swift-editor")
        .auto_shrink([false, false])
        .max_height(code_height)
        .show(ui, |ui| {
            let id = egui::Id::new(("swift-editor-text", &path));
            prepare_cursor(ui, id, &text, cursor_request);
            let mut layouter = |ui: &egui::Ui, source: &dyn egui::TextBuffer, wrap_width: f32| {
                let mut job = swift_highlight::highlight(source.as_str(), ui);
                job.wrap.max_width = wrap_width;
                ui.fonts_mut(|fonts| fonts.layout_job(job))
            };
            egui::TextEdit::multiline(&mut text)
                .id(id)
                .code_editor()
                .layouter(&mut layouter)
                .desired_width(f32::INFINITY)
                .desired_rows(rows.max(24))
                .hint_text("// Swift source")
                .show(ui)
        })
        .inner;
    if response.response.changed()
        && let Some(buffer) = panels.source_workspace.active_mut()
    {
        buffer.edit(text);
    }
    diagnostic_list(panels, ui, &path, diagnostics);
}

fn build_status(panels: &Panels<'_>, ui: &mut egui::Ui) {
    let build = panels.source_workspace.build();
    if build.state.is_empty() || build.state == "never-built" {
        return;
    }
    ui.group(|ui| {
        ui.horizontal(|ui| {
            ui.label(format!(
                "Build · {} · generation {}",
                build.state, build.generation
            ));
            if let Some(fraction) = build.fraction {
                ui.add(
                    egui::ProgressBar::new(fraction.clamp(0.0, 1.0))
                        .show_percentage()
                        .desired_width(120.0),
                );
            }
        });
        ui.label(secondary(panels.shell, &build.step));
        if let Some(diagnostic) = &build.diagnostic {
            ui.label(egui::RichText::new(format!("Build failed: {diagnostic}")).strong());
        }
        if let Some(library) = &build.library {
            ui.label(secondary(panels.shell, format!("Output: {library}")));
        }
    });
    if let Some(reload) = panels.source_workspace.reload() {
        ui.group(|ui| {
            ui.label(format!(
                "Reload · {} · {} generation {}",
                reload.state, reload.module, reload.generation
            ));
            ui.label(format!(
                "Preserved: {}",
                if reload.preserved.is_empty() {
                    "pending or not reported".to_string()
                } else {
                    reload.preserved.join(", ")
                }
            ));
            ui.label(format!(
                "Dropped: {}",
                if reload.dropped.is_empty() {
                    "none".to_string()
                } else {
                    reload.dropped.join(", ")
                }
            ));
            if let Some(diagnostic) = &reload.diagnostic {
                ui.label(egui::RichText::new(format!("Reload failed: {diagnostic}")).strong());
            }
        });
    }
}

fn prepare_cursor(
    ui: &mut egui::Ui,
    id: egui::Id,
    text: &str,
    request: Option<cy_editor_viewmodels::SourcePosition>,
) {
    let Some(position) = request else { return };
    let mut state = egui::TextEdit::load_state(ui.ctx(), id).unwrap_or_default();
    let character = text[..position.offset.min(text.len())].chars().count();
    state
        .cursor
        .set_char_range(Some(egui::text::CCursorRange::one(
            egui::text::CCursor::new(character),
        )));
    state.store(ui.ctx(), id);
    ui.memory_mut(|memory| memory.request_focus(id));
}

fn diagnostic_list(
    panels: &mut Panels<'_>,
    ui: &mut egui::Ui,
    path: &str,
    diagnostics: Vec<cy_editor_viewmodels::SourceDiagnostic>,
) {
    for diagnostic in diagnostics {
        let label = format!(
            "{}:{}: {}",
            diagnostic.line + 1,
            diagnostic.column + 1,
            diagnostic.message
        );
        if ui
            .add(
                egui::Button::new(label)
                    .fill(egui::Color32::TRANSPARENT)
                    .stroke(egui::Stroke::NONE),
            )
            .on_hover_text("Open this diagnostic location")
            .clicked()
        {
            panels.intents.push(Intent::NavigateSource {
                path: path.to_string(),
                line: diagnostic.line,
                column: diagnostic.column,
            });
        }
    }
}

fn conflict_controls(
    panels: &mut Panels<'_>,
    ui: &mut egui::Ui,
    conflict: &cy_editor_viewmodels::SourceConflict,
) {
    ui.group(|ui| {
        ui.label("This file changed outside the editor. Choose how to continue.");
        ui.horizontal(|ui| {
            if ui.button("Reload Disk").clicked()
                && let Some(buffer) = panels.source_workspace.active_mut()
            {
                buffer.resolve_reload();
            }
            if ui.button("Keep Buffer").clicked()
                && let Some(buffer) = panels.source_workspace.active_mut()
            {
                buffer.resolve_keep();
            }
            if ui.button("Merge with Markers").clicked() {
                let disk = conflict.disk.as_deref().unwrap_or_default();
                let merged = format!(
                    "<<<<<<< EDITOR\n{}\n=======\n{}\n>>>>>>> DISK\n",
                    conflict.buffer, disk
                );
                if let Some(buffer) = panels.source_workspace.active_mut() {
                    buffer.resolve_merge(merged);
                }
            }
        });
    });
}
