//! The content browser. Task 1.3.
//!
//! Named "Content Browser" and not "Content Drawer", which is `editor-visual-language`'s vocabulary
//! table and the first of the four things the reference images get wrong. The label is checked, not
//! merely intended: [`crate::vocabulary`] runs every static string this crate can draw through
//! `cy_editor_visual::vocabulary::check_label`.
//!
//! --- THUMBNAILS, AND WHAT A PLACEHOLDER IS FOR ------------------------------------------------------
//!
//! `editor-ui-ux` requires the browser to show what an asset *is* — "every harvester, insectoid and
//! structure distinguishable by its thumbnail" is what makes a large library navigable. Those
//! renders come from the engine, and the engine renders nothing into this editor until a runtime is
//! attached. `cy_editor_interface::thumbnails` already answers that exactly: a `Thumbnail` is either
//! a `Rendered` handle or a typed `Placeholder`, and the placeholder carries the asset's **kind**,
//! so an entry that has no picture still says "Mesh" or "Material" rather than showing a generic
//! file icon that says nothing.
//!
//! --- WHY THE LIST IS THE OPEN DOCUMENTS' ASSETS -----------------------------------------------------
//!
//! Because that is what the editor actually knows. There is no project asset index in this
//! milestone — `editor-content-pipeline` owns that and arrives later — and inventing a directory
//! listing here would produce a browser that shows files the editor cannot open. What it shows is
//! every asset the open documents name, which is true, and it says so when that is empty.

use cy_editor_interface::thumbnails::{Kind, Thumbnail};
use cy_editor_interface::virtualise::{Viewport, Window, content_height};
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::TextRole;

use super::{Intent, Panels, nothing_here, search_field, secondary};
use crate::theme;

/// Draw the content browser.
pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let metrics = panels.metrics();
    controls(panels, ui);
    ui.add_space(metrics.gap() * 0.5);
    panels.asset_browser.refresh(&panels.editor.asset_catalogue);
    import_dialog(panels, ui.ctx());
    let total = panels.asset_browser.rows().len();

    if total == 0 {
        nothing_here(
            ui,
            panels.shell,
            if panels.inputs.browser_filter.trim().is_empty() {
                "This folder contains no matching assets."
            } else {
                "No asset matches this search."
            },
            if panels.inputs.browser_filter.trim().is_empty() {
                "Navigate up or refresh after adding project files."
            } else {
                "Clear the search to see all referenced assets."
            },
        );
        return;
    }

    ui.label(secondary(panels.shell, format!("{total} items")))
        .on_hover_text(format!(
            "{} previews requested",
            panels.thumbnails.requests()
        ));
    ui.add_space(metrics.gap() * 0.5);

    if let Some(row) = virtual_rows(panels, ui, total, metrics.row() * 1.75) {
        if row.folder {
            panels.asset_browser.navigate(&row.path);
        } else {
            panels.intents.push(Intent::OpenAsset(row.path));
        }
    }
}

fn controls(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    if search_field(
        ui,
        panels.shell,
        "Search content",
        &mut panels.inputs.browser_filter,
    ) {
        panels
            .asset_browser
            .set_name_filter(panels.inputs.browser_filter.clone());
    }
    ui.horizontal(|ui| {
        if ui.button("Import…").clicked() {
            panels.inputs.browser_import_open = true;
        }
        if ui
            .add_enabled(
                !panels.asset_browser.folder().is_empty(),
                egui::Button::new("Up"),
            )
            .clicked()
        {
            panels.asset_browser.up();
        }
        ui.label(if panels.asset_browser.folder().is_empty() {
            "Project"
        } else {
            panels.asset_browser.folder()
        });
        egui::ComboBox::from_id_salt("asset-kind")
            .selected_text(if panels.inputs.browser_kind.is_empty() {
                "All types"
            } else {
                panels.inputs.browser_kind.as_str()
            })
            .show_ui(ui, |ui| {
                for kind in [
                    "", "world", "prefab", "mesh", "material", "texture", "audio", "swift", "file",
                ] {
                    if ui
                        .selectable_value(
                            &mut panels.inputs.browser_kind,
                            kind.to_string(),
                            if kind.is_empty() { "All types" } else { kind },
                        )
                        .changed()
                    {
                        panels.asset_browser.set_kind_filter(if kind.is_empty() {
                            None
                        } else {
                            Some(kind.to_string())
                        });
                    }
                }
            });
        if ui.button("Refresh").clicked()
            && let Err(problem) = panels.editor.asset_catalogue.refresh()
        {
            panels
                .editor
                .notifications
                .post(cy_editor_services::Notification::error(
                    problem.what.clone(),
                    problem,
                ));
        }
    });
}

fn import_dialog(panels: &mut Panels<'_>, ctx: &egui::Context) {
    if !panels.inputs.browser_import_open {
        return;
    }
    let mut open = true;
    egui::Window::new("Import assets")
        .collapsible(false)
        .resizable(true)
        .open(&mut open)
        .show(ctx, |ui| {
            ui.label(
                "Choose files in Finder and drop them here, or enter native paths (one per line).",
            );
            ui.add(
                egui::TextEdit::multiline(&mut panels.inputs.browser_import_paths)
                    .hint_text("/Users/me/Models/robot.fbx")
                    .desired_rows(5)
                    .desired_width(460.0),
            );
            ui.label(secondary(
                panels.shell,
                format!(
                    "Destination: {}",
                    if panels.asset_browser.folder().is_empty() {
                        "Imported"
                    } else {
                        panels.asset_browser.folder()
                    }
                ),
            ));
            ui.horizontal(|ui| {
                let paths: Vec<std::path::PathBuf> = panels
                    .inputs
                    .browser_import_paths
                    .lines()
                    .map(str::trim)
                    .filter(|path| !path.is_empty())
                    .map(std::path::PathBuf::from)
                    .collect();
                if ui
                    .add_enabled(!paths.is_empty(), egui::Button::new("Import"))
                    .clicked()
                {
                    panels.intents.push(Intent::ImportExternal {
                        paths,
                        destination: panels.asset_browser.folder().to_string(),
                    });
                    panels.inputs.browser_import_paths.clear();
                    panels.inputs.browser_import_open = false;
                }
                if ui.button("Cancel").clicked() {
                    panels.inputs.browser_import_open = false;
                }
            });
        });
    panels.inputs.browser_import_open &= open;
}

fn virtual_rows(
    panels: &mut Panels<'_>,
    ui: &mut egui::Ui,
    total: usize,
    row_height: f32,
) -> Option<cy_editor_viewmodels::AssetRow> {
    let mut activated = None;
    egui::ScrollArea::vertical()
        .auto_shrink([false, false])
        .show_viewport(ui, |ui, visible| {
            ui.set_height(content_height(total, row_height));
            let window = Window::of(
                total,
                Viewport::new(visible.min.y, visible.height(), row_height),
            );
            let rows = window.slice(panels.asset_browser.rows()).to_vec();
            let top = ui.min_rect().top() + row_height * crate::theme::points(window.first);
            for (offset, row) in rows.iter().enumerate() {
                let rect = egui::Rect::from_min_size(
                    egui::pos2(
                        ui.min_rect().left(),
                        top + row_height * crate::theme::points(offset),
                    ),
                    egui::vec2(ui.available_width(), row_height),
                );
                if entry(panels, ui, row, rect) {
                    activated = Some(row.clone());
                }
            }
        });
    activated
}

/// One virtualised asset row: its preview or typed placeholder, name, kind, and path.
fn entry(
    panels: &mut Panels<'_>,
    ui: &mut egui::Ui,
    row: &cy_editor_viewmodels::AssetRow,
    rect: egui::Rect,
) -> bool {
    let metrics = panels.metrics();
    let kind = Kind::of_label(&row.kind);
    let thumbnail = if row.folder {
        Thumbnail::Typed(Kind::Other)
    } else {
        panels.thumbnails.thumbnail(&row.path)
    };
    let theme = panels.shell.theme;
    let response = ui.interact(
        rect,
        egui::Id::new(("asset", &row.path)),
        egui::Sense::click(),
    );
    ui.painter().rect_filled(
        rect,
        egui::CornerRadius::same(3),
        if response.hovered() {
            theme::lifted(theme, Surface::Sunken, 0.08)
        } else {
            theme::surface(theme, Surface::Sunken)
        },
    );
    let preview = if row.folder {
        "Folder"
    } else {
        match thumbnail {
            Thumbnail::Render(_) => "Preview",
            Thumbnail::Pending(kind) | Thumbnail::Typed(kind) => kind.label(),
        }
    };
    ui.painter().text(
        egui::pos2(rect.left() + metrics.gap(), rect.center().y),
        egui::Align2::LEFT_CENTER,
        format!("{preview}   {}   · {}", row.name, row.kind),
        egui::FontId::proportional(metrics.text(TextRole::Body)),
        theme::role(theme, Semantic::PrimaryText),
    );
    response
        .on_hover_text(format!("{}\n{}", row.path, kind.label()))
        .double_clicked()
}
