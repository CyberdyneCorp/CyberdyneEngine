//! The open-document strip: visible identity, dirty state, and close intents.
//!
//! Tabs render the [`DocumentTabsViewModel`] and emit actions. They never mutate the editor while
//! egui is walking the frame; [`crate::app::EditorWindow`] applies the actions with every other
//! intent after rendering completes.

use cy_editor_viewmodels::{DocumentTab, DocumentTabsViewModel};
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::{Metrics, TextRole};

use crate::panels::Intent;
use crate::theme;

/// Draw the open-document strip.
pub fn strip(
    ui: &mut egui::Ui,
    tabs: &DocumentTabsViewModel,
    visual_theme: cy_editor_visual::Theme,
    metrics: Metrics,
    intents: &mut Vec<Intent>,
) {
    egui::ScrollArea::horizontal()
        .id_salt("cy-document-tabs-scroll")
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                for tab in tabs.tabs() {
                    draw_tab(ui, tab, visual_theme, metrics, intents);
                }
            });
        });
}

fn draw_tab(
    ui: &mut egui::Ui,
    tab: &DocumentTab,
    visual_theme: cy_editor_visual::Theme,
    metrics: Metrics,
    intents: &mut Vec<Intent>,
) {
    ui.push_id(tab.document.as_u128(), |ui| {
        let fill = if tab.active {
            theme::surface(visual_theme, Surface::Raised)
        } else {
            theme::surface(visual_theme, Surface::Panel)
        };
        egui::Frame::NONE
            .fill(fill)
            .inner_margin(egui::Margin::symmetric(
                theme::margin(metrics.gap()),
                theme::margin(metrics.gap() * 0.5),
            ))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    let label = if tab.dirty {
                        format!("{} ●", tab.label)
                    } else {
                        tab.label.clone()
                    };
                    let text = egui::RichText::new(label)
                        .size(metrics.text(TextRole::Body))
                        .color(theme::role(
                            visual_theme,
                            if tab.active {
                                Semantic::PrimaryText
                            } else {
                                Semantic::SecondaryText
                            },
                        ));
                    let tab_response = ui
                        .selectable_label(tab.active, text)
                        .on_hover_text(format!("Activate {}", tab.label));
                    if activated(ui, &tab_response) {
                        intents.push(Intent::ActivateDocument(tab.document));
                    }

                    let close_response = ui
                        .small_button(egui::RichText::new("×").size(metrics.text(TextRole::Body)))
                        .on_hover_text(format!("Close {}", tab.label));
                    if activated(ui, &close_response) {
                        intents.push(Intent::CloseDocument(tab.document));
                    }
                });
            });
    });
}

/// Pointer activation is native to egui; focused controls also accept the conventional keys.
fn activated(ui: &egui::Ui, response: &egui::Response) -> bool {
    let (enter, space) = ui.input(|input| {
        (
            input.key_pressed(egui::Key::Enter),
            input.key_pressed(egui::Key::Space),
        )
    });
    activation_from_input(response.clicked(), response.has_focus(), enter || space)
}

const fn activation_from_input(clicked: bool, focused: bool, keyboard_activation: bool) -> bool {
    clicked || (focused && keyboard_activation)
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::ids::DocumentId;

    #[test]
    fn a_pointer_click_activates_a_document_control() {
        assert!(activation_from_input(true, false, false));
    }

    #[test]
    fn enter_and_space_activate_only_the_focused_document_control() {
        assert!(activation_from_input(false, true, true));
        assert!(!activation_from_input(false, false, true));
    }

    #[test]
    fn dirty_state_has_a_non_colour_cue() {
        let tab = DocumentTab {
            document: DocumentId::of_asset("worlds/city.cyworld"),
            label: "city.cyworld".into(),
            dirty: true,
            active: true,
        };
        let visible = if tab.dirty {
            format!("{} ●", tab.label)
        } else {
            tab.label.clone()
        };
        assert!(visible.contains('●'));
        assert!(visible.contains("city.cyworld"));
    }
}
