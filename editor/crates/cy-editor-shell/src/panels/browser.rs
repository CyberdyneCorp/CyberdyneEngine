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
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::TextRole;

use super::{Intent, Panels, nothing_here, search_field, secondary};
use crate::theme;

/// Draw the content browser.
pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let metrics = panels.metrics();
    search_field(
        ui,
        panels.shell,
        "Search content",
        &mut panels.inputs.browser_filter,
    );
    ui.add_space(metrics.gap() * 0.5);

    let filter = panels.inputs.browser_filter.trim().to_lowercase();
    let mut assets: Vec<String> = panels
        .editor
        .documents
        .ids()
        .filter_map(|id| panels.editor.documents.get(id))
        .flat_map(|document| document.assets().to_vec())
        .filter(|path| filter.is_empty() || path.to_lowercase().contains(&filter))
        .collect();
    assets.sort_unstable();
    assets.dedup();

    if assets.is_empty() {
        nothing_here(
            ui,
            panels.shell,
            if filter.is_empty() {
                "The open worlds name no assets."
            } else {
                "No asset matches this search."
            },
            "Open a world; its assets appear here.",
        );
        return;
    }

    ui.label(secondary(
        panels.shell,
        format!(
            "{} items · {} previews requested",
            assets.len(),
            panels.thumbnails.requests()
        ),
    ));
    ui.add_space(metrics.gap() * 0.5);

    let tile = metrics.row() * 3.5;
    let mut open = None;
    egui::ScrollArea::vertical()
        .auto_shrink([false, false])
        .show(ui, |ui| {
            ui.horizontal_wrapped(|ui| {
                for path in &assets {
                    if entry(panels, ui, path, tile) {
                        open = Some(path.clone());
                    }
                }
            });
        });
    if let Some(path) = open {
        panels.intents.push(Intent::OpenAsset(path));
    }
}

/// One asset tile: its preview or its typed placeholder, and its name.
fn entry(panels: &mut Panels<'_>, ui: &mut egui::Ui, path: &str, tile: f32) -> bool {
    let metrics = panels.metrics();
    let kind = Kind::of_path(path);
    let thumbnail = panels.thumbnails.thumbnail(path);
    let theme = panels.shell.theme;

    let response = ui
        .allocate_ui(egui::vec2(tile, tile + metrics.row()), |ui| {
            let (rect, response) =
                ui.allocate_exact_size(egui::vec2(tile, tile), egui::Sense::click());
            ui.painter().rect_filled(
                rect,
                egui::CornerRadius::same(3),
                if response.hovered() {
                    theme::lifted(theme, Surface::Sunken, 0.08)
                } else {
                    theme::surface(theme, Surface::Sunken)
                },
            );
            // The placeholder says the kind, in a word. A picture that has not been rendered is a
            // fact about the engine, not about the asset, and the entry stays identifiable meanwhile.
            let (text, colour) = match thumbnail {
                Thumbnail::Render(_) => (
                    "Preview".to_string(),
                    theme::role(theme, Semantic::PrimaryText),
                ),
                Thumbnail::Pending(kind) | Thumbnail::Typed(kind) => (
                    kind.label().to_string(),
                    theme::role(theme, Semantic::SecondaryText),
                ),
            };
            ui.painter().text(
                rect.center(),
                egui::Align2::CENTER_CENTER,
                text,
                egui::FontId::proportional(metrics.text(TextRole::Secondary)),
                colour,
            );
            ui.add_sized(
                [tile, metrics.row()],
                egui::Label::new(
                    egui::RichText::new(name_of(path)).size(metrics.text(TextRole::Body)),
                )
                .truncate(),
            );
            response
        })
        .inner;

    response
        .on_hover_text(format!("{path}\n{}", kind.label()))
        .double_clicked()
}

/// The file name, which is what a tile has room for.
fn name_of(path: &str) -> &str {
    path.rsplit('/').next().unwrap_or(path)
}
