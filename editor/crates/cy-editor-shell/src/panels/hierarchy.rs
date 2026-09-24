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

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_interface::virtualise::{Viewport, Window, content_height};
use cy_editor_visual::colour::Semantic;
use cy_editor_visual::density::TextRole;

use super::{Intent, Panels, nothing_here, row_background, search_field, secondary};
use crate::theme;

/// What a click on a row landed on.
enum Hit {
    /// The row itself: select the node.
    Select,
    /// The disclosure triangle: expand or collapse it.
    Disclose,
    /// Begin editing the author-facing name.
    Rename,
    /// Begin an identity-based reparent drag.
    StartDrag,
    /// Drop the held identity onto this node.
    Drop,
}

#[derive(Default)]
struct Interactions {
    clicked: Option<(NodeId, cy_editor_viewmodels::SelectionIntent)>,
    toggled: Option<NodeId>,
    rename: Option<(NodeId, String)>,
    drag_started: Option<NodeId>,
    dropped_on: Option<NodeId>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum RenameResolution {
    Continue,
    Commit,
    Cancel,
}

const fn rename_resolution(lost_focus: bool, enter: bool, escape: bool) -> RenameResolution {
    if escape {
        RenameResolution::Cancel
    } else if lost_focus && enter {
        RenameResolution::Commit
    } else if lost_focus {
        RenameResolution::Cancel
    } else {
        RenameResolution::Continue
    }
}

/// One row: its background, its disclosure, its problem marker and its label.
fn draw_row(
    panels: &Panels<'_>,
    ui: &mut egui::Ui,
    row: &cy_editor_viewmodels::HierarchyRow,
    rect: egui::Rect,
    dragging: bool,
) -> Option<Hit> {
    let metrics = panels.shell.metrics();
    let body = egui::FontId::proportional(metrics.text(TextRole::Body));
    let response = ui.interact(
        rect,
        egui::Id::new(("hierarchy", row.node)),
        egui::Sense::click_and_drag(),
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
    } else if row.selected {
        theme::role(panels.shell.theme, Semantic::Selection)
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

    if dragging && response.hovered() && ui.input(|input| input.pointer.any_released()) {
        hit = Some(Hit::Drop);
    } else if response.drag_started() {
        hit = Some(Hit::StartDrag);
    } else if response.double_clicked() {
        hit = Some(Hit::Rename);
    } else if hit.is_none() && response.clicked() {
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
    ui.horizontal(|ui| {
        show_create_menu(panels, ui);
        let selected = panels.editor.selection.get().nodes().collect::<Vec<_>>();
        if selected.len() == 1 && ui.button("Move to Root").clicked() {
            panels.intents.push(Intent::Invoke(
                "scene.reparent-entity".into(),
                Arguments::new()
                    .with("entity", Value::Text(selected[0].to_string()))
                    .with("parent", Value::Text(String::new())),
            ));
        }
        ui.label(secondary(
            panels.shell,
            "Double-click to rename · drag onto a parent",
        ));
    });
    ui.add_space(metrics.gap() * 0.5);

    panels.hierarchy.refresh(panels.editor);
    if panels.inputs.hierarchy_rename.is_none()
        && ui.input(|input| input.key_pressed(egui::Key::F2))
        && let Some(row) = panels.hierarchy.rows().iter().find(|row| row.selected)
    {
        panels.inputs.hierarchy_rename = Some((row.node, row.label.clone(), false));
    }
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
            if panels.inputs.hierarchy_filter.trim().is_empty() {
                "Open a world, then use Create Entity in the Scene menu."
            } else {
                "Clear the search to see the scene."
            },
        );
        return;
    }

    // The count sits above the list rather than below it: a summary drawn after a scroll area that
    // has taken the panel's whole height is a summary clipped off the bottom edge, which is exactly
    // what happened the first time this panel was run.
    ui.label(secondary(
        panels.shell,
        format!(
            "{total} visible · {} selected",
            panels.editor.selection.get().nodes().count()
        ),
    ))
    .on_hover_text(format!(
        "{} hierarchy rebuilds",
        panels.hierarchy.rebuilds()
    ));
    ui.add_space(metrics.gap() * 0.5);

    let interactions = draw_rows(panels, ui, total, metrics.row(), metrics.gap());
    apply_interactions(panels, ui, interactions);
}

fn show_create_menu(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    ui.menu_button("Create", |ui| {
        for (label, command, key, value) in [
            ("Empty Entity", "scene.create-entity", "template", "empty"),
            ("Plane", "scene.create-primitive", "shape", "plane"),
        ] {
            if ui.button(label).clicked() {
                panels.intents.push(Intent::Invoke(
                    command.into(),
                    Arguments::new().with(key, Value::Text(value.into())),
                ));
                ui.close();
            }
        }
        ui.separator();
        for (label, kind) in [
            ("Directional Light", "directional"),
            ("Point Light", "point"),
            ("Spot Light", "spot"),
        ] {
            if ui.button(label).clicked() {
                panels.intents.push(Intent::Invoke(
                    "scene.create-light".into(),
                    Arguments::new().with("kind", Value::Text(kind.into())),
                ));
                ui.close();
            }
        }
        if ui.button("Camera").clicked() {
            let camera = &panels.editor.viewports.focused().state.camera;
            panels.intents.push(Intent::Invoke(
                "scene.create-camera".into(),
                Arguments::new()
                    .with("at", Value::Vec3(camera.position.to_array()))
                    .with("rotation", Value::Quat(camera.rotation.to_array())),
            ));
            ui.close();
        }
    });
}

fn draw_rows(
    panels: &mut Panels<'_>,
    ui: &mut egui::Ui,
    total: usize,
    row_height: f32,
    gap: f32,
) -> Interactions {
    let mut interactions = Interactions::default();
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
                if panels
                    .inputs
                    .hierarchy_rename
                    .as_ref()
                    .is_some_and(|(node, _, _)| *node == row.node)
                {
                    let (_, text, focused) = panels
                        .inputs
                        .hierarchy_rename
                        .as_mut()
                        .expect("the row was checked above");
                    let response = ui.put(
                        rect.shrink2(egui::vec2(gap, 1.0)),
                        egui::TextEdit::singleline(text),
                    );
                    if !*focused {
                        response.request_focus();
                        *focused = true;
                    }
                    match rename_resolution(
                        response.lost_focus(),
                        ui.input(|input| input.key_pressed(egui::Key::Enter)),
                        ui.input(|input| input.key_pressed(egui::Key::Escape)),
                    ) {
                        RenameResolution::Commit => {
                            interactions.rename = Some((row.node, text.clone()));
                            panels.inputs.hierarchy_rename = None;
                        }
                        RenameResolution::Cancel => panels.inputs.hierarchy_rename = None,
                        RenameResolution::Continue => {}
                    }
                    continue;
                }
                match draw_row(
                    panels,
                    ui,
                    row,
                    rect,
                    panels.inputs.hierarchy_drag.is_some(),
                ) {
                    Some(Hit::Select) => {
                        let modifiers = ui.input(|input| input.modifiers);
                        let intent = if modifiers.shift {
                            cy_editor_viewmodels::SelectionIntent::VisibleRange
                        } else if modifiers.command && row.selected {
                            cy_editor_viewmodels::SelectionIntent::Subtract
                        } else if modifiers.command {
                            cy_editor_viewmodels::SelectionIntent::Add
                        } else {
                            cy_editor_viewmodels::SelectionIntent::Replace
                        };
                        interactions.clicked = Some((row.node, intent));
                    }
                    Some(Hit::Disclose) => interactions.toggled = Some(row.node),
                    Some(Hit::Rename) => {
                        panels.inputs.hierarchy_rename = Some((row.node, row.label.clone(), false));
                    }
                    Some(Hit::StartDrag) => interactions.drag_started = Some(row.node),
                    Some(Hit::Drop) => interactions.dropped_on = Some(row.node),
                    None => {}
                }
            }
        });
    interactions
}

fn apply_interactions(panels: &mut Panels<'_>, ui: &egui::Ui, interactions: Interactions) {
    // Applied after the list is drawn: a selection change moves a revision, and rebuilding the rows
    // underneath the loop that is reading them is how an outliner draws a frame of the wrong tree.
    if let Some(node) = interactions.toggled {
        // The view model owns which nodes are expanded; this asks it to flip one, and the rows are
        // rebuilt on the next refresh. Nothing is written to the document — expansion is
        // presentation state, and `cy-editor-viewmodels` has a test that says so.
        let expanded = panels.hierarchy.is_expanded(node);
        panels.hierarchy.set_expanded(node, !expanded);
    }
    if let Some((node, intent)) = interactions.clicked {
        panels.hierarchy.select(panels.editor, node, intent);
    }
    if let Some(node) = interactions.drag_started {
        panels.inputs.hierarchy_drag = Some(node);
    }
    if let Some(parent) = interactions.dropped_on {
        if let Some(node) = panels.inputs.hierarchy_drag.take() {
            panels.intents.push(Intent::Invoke(
                "scene.reparent-entity".into(),
                Arguments::new()
                    .with("entity", Value::Text(node.to_string()))
                    .with("parent", Value::Text(parent.to_string())),
            ));
        }
    } else if panels.inputs.hierarchy_drag.is_some()
        && ui.input(|input| input.pointer.any_released())
    {
        panels.inputs.hierarchy_drag = None;
    }
    if let Some((node, name)) = interactions.rename {
        panels.intents.push(Intent::Invoke(
            "scene.rename-entity".into(),
            Arguments::new()
                .with("entity", Value::Text(node.to_string()))
                .with("name", Value::Text(name)),
        ));
    }
}

#[cfg(test)]
mod tests {
    use super::{RenameResolution, rename_resolution};

    #[test]
    fn inline_rename_only_commits_an_explicit_enter() {
        assert_eq!(
            rename_resolution(true, true, false),
            RenameResolution::Commit
        );
        assert_eq!(
            rename_resolution(true, false, false),
            RenameResolution::Cancel,
            "focus loss must not create an accidental transaction"
        );
        assert_eq!(
            rename_resolution(false, false, true),
            RenameResolution::Cancel
        );
        assert_eq!(
            rename_resolution(false, false, false),
            RenameResolution::Continue
        );
    }
}
