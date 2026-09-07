//! Notifications that do not interrupt, and the one thing that does. Task 1.5.
//!
//! --- THE RULE, AND WHY IT IS ABOUT FOCUS -----------------------------------------------------------
//!
//! `editor-ui-ux`: *"WHEN an import fails while a user is editing a value THEN a notification SHALL
//! appear without taking focus, and SHALL remain reviewable."* The failing half of that is almost
//! always focus. A toast drawn in a corner looks harmless and then swallows the next keystroke,
//! and the user discovers it by finding half a number in a field.
//!
//! So the toasts are drawn in an `egui::Area` above the interface and **nothing in this file ever
//! calls `request_focus`**. They carry two clickable controls — an offer and a dismissal — and a
//! button that is clicked takes no keyboard focus in egui, so a toast appearing mid-edit cannot
//! swallow the next keystroke. The palette is the one thing here that does take the keyboard, once,
//! on the frame it opens, because a palette that did not would not be a palette.
//!
//! --- DISMISSED IS NOT DELETED -----------------------------------------------------------------------
//!
//! `NotificationCentre::showing` returns what is on screen and `history` keeps everything;
//! dismissing moves a toast from the first to the second. The console panel draws the history, so a
//! message that faded is still readable — which is what makes a non-interrupting notification an
//! acceptable way to report a failure at all.
//!
//! --- MODALS ARE FOR DECISIONS AND DESTRUCTION -------------------------------------------------------
//!
//! `Modal::decision` refuses fewer than two choices and refuses an empty consequence, and
//! `Modal::destructive` refuses a confirmation that does not say what will be lost. Those refusals
//! are the mechanism; this file only draws what survived them, and there is no path here that
//! constructs a modal out of a message.

use cy_editor_interface::notifications::{Modal, NotificationCentre, Toast};
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::{Metrics, TextRole};

use crate::panels::Intent;
use crate::theme;

/// What a modal's choice, once made, asked for.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Decision {
    /// Which choice, by index into the modal's own list.
    pub choice: usize,
    /// Whether that choice was the destructive one.
    pub destructive: bool,
}

/// Draw the toasts.
///
/// Returns the offer a user accepted, as a command to invoke. An offer is a *command identifier* —
/// `Offer` carries one — so accepting a notification's offer goes through the registry like every
/// other action, and an offer cannot do something no other caller could.
pub fn toasts(
    ctx: &egui::Context,
    centre: &mut NotificationCentre,
    theme: cy_editor_visual::Theme,
    metrics: Metrics,
    intents: &mut Vec<Intent>,
) {
    // Retired first, so a run of successful commands does not build a wall of toasts over the
    // panel underneath. The history keeps every one of them; see `NotificationCentre::retire_transient`.
    centre.retire_transient(cy_editor_interface::notifications::TRANSIENT);
    let showing: Vec<(usize, Toast)> = centre
        .showing_indexed()
        .into_iter()
        .map(|(index, toast)| (index, toast.clone()))
        .collect();
    if showing.is_empty() {
        return;
    }
    let mut dismissed = None;

    egui::Area::new(egui::Id::new("notifications"))
        .anchor(
            egui::Align2::RIGHT_BOTTOM,
            egui::vec2(-metrics.padding() * 2.0, -metrics.row() * 2.5),
        )
        .order(egui::Order::Tooltip)
        .show(ctx, |ui| {
            ui.set_max_width(metrics.row() * 18.0);
            for (index, toast) in showing.iter().map(|(index, toast)| (*index, toast)) {
                let role = toast.role();
                egui::Frame::NONE
                    .fill(theme::surface(theme, Surface::Raised))
                    .stroke(egui::Stroke::new(1.0, theme::role(theme, role)))
                    .corner_radius(egui::CornerRadius::same(3))
                    .inner_margin(egui::Margin::same(theme::margin(metrics.padding())))
                    .show(ui, |ui| {
                        ui.horizontal_wrapped(|ui| {
                            // Glyph, word, colour: three encodings of the same severity.
                            ui.label(
                                egui::RichText::new(role.glyph().to_string())
                                    .color(theme::role(theme, role)),
                            );
                            ui.label(
                                egui::RichText::new(toast.line())
                                    .size(metrics.text(TextRole::Body)),
                            );
                        });
                        ui.horizontal(|ui| {
                            if let Some(offer) = &toast.offer
                                && ui.small_button(&offer.label).clicked()
                            {
                                intents.push(Intent::Invoke(
                                    offer.command.clone(),
                                    offer.arguments.clone(),
                                ));
                                dismissed = Some(index);
                            }
                            if ui.small_button("Dismiss").clicked() {
                                dismissed = Some(index);
                            }
                        });
                    });
                ui.add_space(metrics.gap() * 0.5);
            }
        });

    if let Some(index) = dismissed {
        centre.dismiss(index);
    }
}

/// Draw a modal, returning the decision once one is made.
///
/// The destructive choice is drawn in the error colour *and* is the one that is not the default
/// focus, which is the pair `editor-ui-ux` asks for: a destructive action is identifiable and is not
/// what a stray `Enter` chooses.
pub fn modal(
    ctx: &egui::Context,
    modal: &Modal,
    theme: cy_editor_visual::Theme,
    metrics: Metrics,
) -> Option<Decision> {
    let mut decision = None;
    egui::Modal::new(egui::Id::new("cy-modal")).show(ctx, |ui| {
        ui.set_max_width(metrics.row() * 20.0);
        ui.label(
            egui::RichText::new(&modal.question)
                .size(metrics.text(TextRole::Title))
                .color(theme::role(theme, Semantic::PrimaryText)),
        );
        ui.add_space(metrics.gap());
        // What will be lost, in words, always. `Modal::destructive` refused an empty one; this is
        // where it is shown, and it is shown before the buttons rather than beside them.
        ui.label(
            egui::RichText::new(&modal.consequence)
                .size(metrics.text(TextRole::Body))
                .color(theme::role(theme, Semantic::SecondaryText)),
        );
        ui.add_space(metrics.gap() * 1.5);
        ui.horizontal(|ui| {
            for (index, choice) in modal.choices.iter().enumerate() {
                let text = if choice.destructive {
                    egui::RichText::new(&choice.label).color(theme::role(theme, Semantic::Error))
                } else {
                    egui::RichText::new(&choice.label)
                };
                if ui.button(text).clicked() {
                    decision = Some(Decision {
                        choice: index,
                        destructive: choice.destructive,
                    });
                }
            }
        });
    });
    decision
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_interface::notifications::Choice;

    #[test]
    fn a_confirmation_that_does_not_say_what_will_be_lost_cannot_be_constructed() {
        // The forbidden pattern, refused at the constructor rather than caught in review. This file
        // draws what survived that refusal and has no other way to make a modal.
        assert!(Modal::destructive("Discard changes?", "   ", "Discard").is_err());
        let modal = Modal::destructive(
            "Discard changes?",
            "12 unsaved changes to worlds/city.cyworld",
            "Discard",
        )
        .unwrap();
        assert!(modal.consequence.contains("12 unsaved changes"));
        assert!(modal.choices.iter().any(|choice| choice.destructive));
        assert!(modal.choices.iter().any(|choice| !choice.destructive));
    }

    #[test]
    fn information_is_not_a_modal() {
        assert!(
            Modal::decision("Imported", "Nothing to decide", vec![Choice::new("OK")]).is_err(),
            "a one-choice modal is information wearing a decision's clothes"
        );
    }
}
