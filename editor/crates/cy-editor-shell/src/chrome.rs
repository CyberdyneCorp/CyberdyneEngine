//! The application shell: the header, the toolbar and the footer. Tasks 1.1, 1.5, 1.5b, 1.7.
//!
//! --- THE HEADER, AND THE ONE RULE ABOUT THE MARK -----------------------------------------------------
//!
//! The horizontal lockup at the far left, then the menus, then the project and world, then the
//! runtime state and the settings. One row, and the mark identifies rather than dominates. The
//! chrome around it is `Surface::Window` — flat charcoal — because *"a header that picks up the
//! logo's gradients has misread it"*. See [`crate::identity`], where the rule is checked rather than
//! merely stated.
//!
//! --- THE MENUS ARE THE REGISTRY, NOT A LIST ---------------------------------------------------------
//!
//! Every menu entry below `View` is generated from `Registry::all()`, grouped by each command's
//! declared category. That is what makes "One action, six entry points" true rather than
//! aspirational: a command added to the registry is in the menu, in the palette, on its declared
//! key, and available to an agent, with no second list to update — and a menu item that has drifted
//! out of the registry cannot exist because there is nowhere for it to live.
//!
//! Availability comes from the registry too, so a greyed entry explains *why* on hover, which is
//! `editor-ui-ux`'s "a disabled action SHALL explain why it is unavailable" rather than a
//! disabled control with nothing to say.
//!
//! --- WHY THERE IS A TOOLBAR AND YET "NO SECOND TOOLBAR" -----------------------------------------------
//!
//! `editor-visual-language` forbids a *second, full-width* toolbar carrying viewport controls, and
//! requires projection, render mode and show flags to float in the viewport. So there is exactly one
//! application toolbar — selection mode, play, build, the transform modes, snapping — and the
//! viewport's own controls are overlays drawn by `panels::viewport`. The distinction is the point:
//! chrome that belongs to the *application* is a row; chrome that belongs to *this viewport* floats
//! in it, and stops existing when the viewport is not on screen.

use cy_editor_commands::Registry;
use cy_editor_interface::shell::Shell;
use cy_editor_services::Editor;
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::TextRole;
use cy_editor_visual::gizmo::GizmoMode;

use crate::identity::{self, Identity};
use crate::panels::Intent;
use crate::theme;
use crate::view::{self, ViewAction};

/// The order the known menus appear in.
///
/// The menu bar is **derived** from the categories the registry's commands declare, not written out
/// here: a category that appeared in a command and not in this list would be a set of actions with
/// no menu, reachable only by somebody who already knew they existed. This list only fixes the order
/// of the ones we know about; anything else is appended alphabetically, so a new category is
/// immediately reachable and merely unranked.
pub(crate) const MENU_ORDER: [&str; 7] = [
    "Project", "File", "Edit", "Scene", "Assets", "Build", "Debug",
];

/// The registry's categories, in menu order.
fn menus(registry: &Registry) -> Vec<String> {
    let mut categories: Vec<String> = registry
        .all()
        .map(|metadata| metadata.category.clone())
        .collect();
    categories.sort_unstable();
    categories.dedup();
    categories.sort_by_key(|category| {
        let rank = MENU_ORDER
            .iter()
            .position(|known| known == category)
            .unwrap_or(MENU_ORDER.len());
        (rank, category.clone())
    });
    categories
}

/// Draw the header row.
pub fn header(
    ui: &mut egui::Ui,
    shell: &Shell,
    editor: &Editor,
    registry: &Registry,
    id: &mut Identity,
    intents: &mut Vec<Intent>,
) {
    let metrics = shell.metrics();
    ui.horizontal(|ui| {
        id.lockup(ui, shell.theme, metrics.row() * 1.35);
        ui.add_space(metrics.gap());

        for category in menus(registry) {
            menu(ui, shell, editor, registry, &category, intents);
        }
        view_menu(ui, shell, intents);
        help_menu(ui, shell);

        // The project and world, centred by being pushed from both sides. `Shell::header` writes the
        // sentence; this row places it.
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            let live = editor.runtime.is_connected();
            let role = if live {
                Semantic::Live
            } else {
                Semantic::SecondaryText
            };
            ui.label(
                egui::RichText::new(format!("{} {}", role.glyph(), editor.hosting_mode().name()))
                    .color(theme::role(shell.theme, role))
                    .size(metrics.text(TextRole::Secondary)),
            );
            ui.separator();
            ui.label(
                egui::RichText::new(shell.header(editor))
                    .size(metrics.text(TextRole::Body))
                    .color(theme::role(shell.theme, Semantic::PrimaryText)),
            );
        });
    });
}

/// One generated menu.
fn menu(
    ui: &mut egui::Ui,
    shell: &Shell,
    editor: &Editor,
    registry: &Registry,
    category: &str,
    intents: &mut Vec<Intent>,
) {
    let mut commands: Vec<&cy_editor_commands::Metadata> = registry
        .all()
        .filter(|metadata| metadata.category == category)
        .collect();
    if commands.is_empty() {
        return;
    }
    commands.sort_by(|first, second| first.label.cmp(&second.label));

    ui.menu_button(category, |ui| {
        for metadata in commands {
            let availability = registry.availability(&metadata.id, editor);
            let binding = shell
                .keymap
                .bindings_of(&metadata.id)
                .first()
                .map(|(_, chord)| chord.to_string())
                .unwrap_or_default();
            let entry = egui::Button::new(&metadata.label).shortcut_text(binding);
            let response = ui.add_enabled(availability.is_available(), entry);
            let response = match availability.reason() {
                // A disabled entry says why, and what would make it available. That is the whole
                // difference between a greyed control and a useful one.
                Some(problem) => response.on_disabled_hover_text(format!(
                    "{}\n{}",
                    problem.because,
                    problem.remedy.as_deref().unwrap_or_default()
                )),
                None => response.on_hover_text(&metadata.description),
            };
            if response.clicked() {
                intents.push(Intent::Invoke(
                    metadata.id.clone(),
                    cy_editor_commands::Arguments::new(),
                ));
                ui.close();
            }
        }
    });
}

/// The View menu: the shell's own actions, which change no document.
fn view_menu(ui: &mut egui::Ui, shell: &Shell, intents: &mut Vec<Intent>) {
    ui.menu_button("View", |ui| {
        for action in ViewAction::ALL {
            let binding = shell
                .keymap
                .bindings_of(action.id())
                .first()
                .map(|(_, chord)| chord.to_string())
                .unwrap_or_default();
            if ui
                .add(egui::Button::new(action.label()).shortcut_text(binding))
                .on_hover_text(action.description())
                .clicked()
            {
                intents.push(Intent::Invoke(
                    action.id().to_string(),
                    cy_editor_commands::Arguments::new(),
                ));
                ui.close();
            }
        }
    });
}

/// The Help menu, which is where the publisher is named.
fn help_menu(ui: &mut egui::Ui, shell: &Shell) {
    ui.menu_button("Help", |ui| {
        ui.label(format!(
            "{} {}",
            identity::PRODUCT,
            env!("CARGO_PKG_VERSION")
        ));
        ui.label(crate::panels::secondary(
            shell,
            format!("Published by {}", identity::PUBLISHER),
        ));
    });
}

/// Draw the application toolbar.
pub fn toolbar(
    ui: &mut egui::Ui,
    shell: &Shell,
    editor: &Editor,
    registry: &Registry,
    intents: &mut Vec<Intent>,
) {
    let metrics = shell.metrics();
    ui.horizontal(|ui| {
        // The transform modes, in the order the gizmo reference fixes: Move W, Rotate E, Scale R,
        // Universal T. They are drawn only when the registry actually has them — the viewport's
        // interactive path is task 2.4 — because four buttons that report "no such command" teach a
        // user that the toolbar is unreliable, and one sentence saying what is missing does not.
        let modes: Vec<(GizmoMode, &str)> = GizmoMode::ALL
            .into_iter()
            .map(|mode| (mode, view::transform_command(mode)))
            .filter(|(_, id)| registry.metadata(id).is_some())
            .collect();
        if modes.is_empty() {
            ui.add_enabled(false, egui::Button::new("Move  Rotate  Scale  Universal"))
                .on_disabled_hover_text(
                    "The transform tools arrive with the viewport's interactive path. Their \
                     commands are not registered in this build.",
                );
        }
        for (mode, id) in modes {
            let available = registry.availability(id, editor).is_available();
            let response = ui.add_enabled(
                available,
                egui::Button::new(
                    egui::RichText::new(mode.label()).size(metrics.text(TextRole::Body)),
                ),
            );
            if response
                .on_hover_text(format!(
                    "{} — {}",
                    mode.label(),
                    shell
                        .keymap
                        .bindings_of(id)
                        .first()
                        .map_or_else(|| "unbound".to_string(), |(_, chord)| chord.to_string())
                ))
                .clicked()
            {
                intents.push(Intent::Invoke(
                    id.to_string(),
                    cy_editor_commands::Arguments::new(),
                ));
            }
        }
        ui.separator();
        for id in ["edit.undo", "edit.redo", "file.save"] {
            let Some(metadata) = registry.metadata(id) else {
                continue;
            };
            let available = registry.availability(id, editor).is_available();
            if ui
                .add_enabled(available, egui::Button::new(&metadata.label))
                .on_hover_text(&metadata.description)
                .clicked()
            {
                intents.push(Intent::Invoke(
                    id.to_string(),
                    cy_editor_commands::Arguments::new(),
                ));
            }
        }
    });
}

/// Draw the footer.
///
/// "WHEN every document is saved THEN the footer SHALL state it in words, not only by an icon
/// colour." `Shell::footer` writes the words and `Shell::save_role` picks the hue, so the two cannot
/// disagree — a green dot beside "3 unsaved" is exactly the defect that pairing prevents.
pub fn footer(ui: &mut egui::Ui, shell: &Shell, editor: &Editor, pending_chord: &str) {
    let metrics = shell.metrics();
    ui.horizontal(|ui| {
        let role = shell.save_role(editor);
        ui.label(
            egui::RichText::new(format!("{} {}", role.glyph(), shell.footer(editor)))
                .size(metrics.text(TextRole::Secondary))
                .color(theme::role(shell.theme, role)),
        );
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            ui.label(crate::panels::secondary(
                shell,
                format!(
                    "{} · {}",
                    shell.workspaces.current_name(),
                    shell.density.label()
                ),
            ));
            if !pending_chord.is_empty() {
                // A chord in progress is shown, because a chord that gives no feedback is
                // indistinguishable from a key that did nothing.
                ui.separator();
                ui.label(
                    egui::RichText::new(format!("{pending_chord} …"))
                        .family(egui::FontFamily::Monospace)
                        .color(theme::role(shell.theme, Semantic::Active)),
                );
            }
        });
    });
}

/// The frame the header, toolbar and footer are drawn in: flat, charcoal, one hairline.
pub fn bar(shell: &Shell) -> egui::Frame {
    let metrics = shell.metrics();
    egui::Frame::NONE
        .fill(theme::surface(shell.theme, Surface::Window))
        .inner_margin(egui::Margin::symmetric(
            theme::margin(metrics.padding()),
            theme::margin(metrics.padding() * 0.4),
        ))
}
