//! The hierarchy: the scene's tree, virtualised, with a permanent search. Task 1.3.
//!
//! --- WHY IT IS VIRTUALISED AT THIRTY ROWS ---------------------------------------------------------
//!
//! `editor-ui-ux` requires lists whose cost scales with what is visible rather than with what
//! exists, and the M5.5 design note named this as the price of choosing an immediate-mode toolkit:
//! *"a 30,000-row outliner needs deliberate virtualisation."* Deliberate is the word that matters. An
//! immediate-mode list is written the same way whether it builds thirty rows or thirty thousand, so
//! the version that is quadratic in the size of the world looks exactly like the version that is
//! not, and it is discovered on somebody's real project rather than on ours.
//!
//! So the visible window comes from `cy_editor_interface::virtualise::Window`, which is the model
//! that was written and tested for this at M5 and whose own test asserts the property that matters —
//! a list of a hundred and a list of a hundred thousand produce the same row count.
//!
//! --- WHAT IS NOT DRAWN, AND WHY THAT IS NOT AN OMISSION ---------------------------------------------
//!
//! The reference images show a per-row visibility toggle at the right edge. The document model has
//! no notion of a node being hidden — `NodeState` is parent, children, layer, prefab, components and
//! overrides — so a toggle here would be a control that does nothing, which `editor-ui-ux` treats as
//! worse than an absent one. It arrives with the property, not before it.

use cy_editor_interface::virtualise::{Viewport, Window, content_height};
use cy_editor_visual::colour::Semantic;
use cy_editor_visual::density::TextRole;

use super::{Panels, nothing_here, row_background, search_field, secondary};
use crate::theme;

/// What a click on a row landed on.
enum Hit {
    /// The row itself: select the node.
    Select,
    /// The disclosure triangle: expand or collapse it.
    Disclose,
}

/// One row: its background, its disclosure, its problem marker and its label.
fn draw_row(
    panels: &Panels<'_>,
    ui: &mut egui::Ui,
    row: &cy_editor_viewmodels::HierarchyRow,
    rect: egui::Rect,
) -> Option<Hit> {
    let metrics = panels.shell.metrics();
    let body = egui::FontId::proportional(metrics.text(TextRole::Body));
    let response = ui.interact(
        rect,
        egui::Id::new(("hierarchy", row.node)),
        egui::Sense::click(),
    );
    row_background(ui, panels.shell, rect, row.selected, response.hovered());

    let mut cursor = rect.left() + metrics.gap() * 0.5;
    cursor += metrics.icon() * theme::points(row.depth);
    let mut hit = None;

    if row.has_children {
        let triangle = egui::Rect::from_min_size(
            egui::pos2(cursor, rect.top()),
            egui::vec2(metrics.icon(), rect.height()),
        );
        let disclosure = ui.interact(
            triangle,
            egui::Id::new(("hierarchy-disclosure", row.node)),
            egui::Sense::click(),
        );
        ui.painter().text(
            triangle.center(),
            egui::Align2::CENTER_CENTER,
            // The disclosure is a character rather than an asset, so it is legible at compact
            // density before an icon set exists.
            if panels.hierarchy.is_expanded(row.node) {
                "\u{25be}"
            } else {
                "\u{25b8}"
            },
            body.clone(),
            theme::role(panels.shell.theme, Semantic::SecondaryText),
        );
        if disclosure.clicked() {
            hit = Some(Hit::Disclose);
        }
    }
    cursor += metrics.icon() + metrics.gap() * 0.5;

    let has_problem = panels.shell.problems.at_node(row.node).next().is_some();
    let colour = if has_problem {
        theme::role(panels.shell.theme, Semantic::Error)
    } else {
        theme::role(panels.shell.theme, Semantic::PrimaryText)
    };
    ui.painter().text(
        egui::pos2(cursor, rect.center().y),
        egui::Align2::LEFT_CENTER,
        if has_problem {
            format!("{} {}", Semantic::Error.glyph(), row.label)
        } else {
            row.label.clone()
        },
        body,
        colour,
    );

    if hit.is_none() && response.clicked() {
        hit = Some(Hit::Select);
    }
    response.on_hover_text(row.node.to_string());
    hit
}

/// Draw the outliner.
pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let metrics = panels.metrics();

    if search_field(
        ui,
        panels.shell,
        "Search the scene",
        &mut panels.inputs.hierarchy_filter,
    ) {
        panels
            .hierarchy
            .set_filter(panels.inputs.hierarchy_filter.clone());
    }
    ui.add_space(metrics.gap() * 0.5);

    panels.hierarchy.refresh(panels.editor);
    let total = panels.hierarchy.rows().len();
    if total == 0 {
        nothing_here(
            ui,
            panels.shell,
            if panels.inputs.hierarchy_filter.trim().is_empty() {
                "No entities in the active world."
            } else {
                "No entity matches this search."
            },
            "Create Entity (Ctrl+Shift+N), or open a world.",
        );
        return;
    }

    // The count sits above the list rather than below it: a summary drawn after a scroll area that
    // has taken the panel's whole height is a summary clipped off the bottom edge, which is exactly
    // what happened the first time this panel was run.
    ui.label(secondary(
        panels.shell,
        format!(
            "{total} entities · {} rebuilds",
            panels.hierarchy.rebuilds()
        ),
    ));
    ui.add_space(metrics.gap() * 0.5);

    let row_height = metrics.row();
    let mut clicked = None;
    let mut toggled = None;

    egui::ScrollArea::vertical()
        .auto_shrink([false, false])
        .show_viewport(ui, |ui, visible| {
            // The whole list's height is allocated so the scrollbar is honest about a list that was
            // never built, and only the window inside it is drawn.
            ui.set_height(content_height(total, row_height));
            let window = Window::of(
                total,
                Viewport::new(visible.min.y, visible.height(), row_height),
            );
            let top = ui.min_rect().top() + row_height * theme::points(window.first);

            for (offset, row) in window.slice(panels.hierarchy.rows()).iter().enumerate() {
                let rect = egui::Rect::from_min_size(
                    egui::pos2(
                        ui.min_rect().left(),
                        top + row_height * theme::points(offset),
                    ),
                    egui::vec2(ui.available_width(), row_height),
                );
                match draw_row(panels, ui, row, rect) {
                    Some(Hit::Select) => clicked = Some(row.node),
                    Some(Hit::Disclose) => toggled = Some(row.node),
                    None => {}
                }
            }
        });

    // Applied after the list is drawn: a selection change moves a revision, and rebuilding the rows
    // underneath the loop that is reading them is how an outliner draws a frame of the wrong tree.
    if let Some(node) = toggled {
        // The view model owns which nodes are expanded; this asks it to flip one, and the rows are
        // rebuilt on the next refresh. Nothing is written to the document — expansion is
        // presentation state, and `cy-editor-viewmodels` has a test that says so.
        let expanded = panels.hierarchy.is_expanded(node);
        panels.hierarchy.set_expanded(node, !expanded);
    }
    if let Some(node) = clicked {
        panels.hierarchy.select(panels.editor, node);
    }
}
