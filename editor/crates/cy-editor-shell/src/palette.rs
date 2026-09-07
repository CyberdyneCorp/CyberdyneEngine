//! The command palette, drawn. Task 1.4.
//!
//! `cy_editor_interface::palette` already answers every question that matters — what is indexed, how
//! a query is scored, how ties break, and which of the engine's own terms a borrowed word maps to.
//! This file is the input field, the list, and the two keys that move through it.
//!
//! --- WHAT THE MODEL DECIDES THAT THIS FILE MUST NOT --------------------------------------------------
//!
//! **Ranking.** `Index::search` sorts by score, then by the shorter label, then by the label — "so
//! that ranking does not reorder itself when an unrelated asset finished importing". A panel that
//! re-sorted for any reason, including moving the recently used to the top, would reintroduce
//! exactly the instability that ordering is built to avoid.
//!
//! **Vocabulary.** A result found through an alias shows `blueprint → Script Graph`: the word the
//! user typed, and then the engine's own term. That is `editor-visual-language`'s search-alias rule
//! working — type what you know, find the feature, see it labelled with the engine's word — and the
//! arrow is drawn here only because `Match::via_alias` carried it.
//!
//! --- NEVER BLOCKING -------------------------------------------------------------------------------
//!
//! "The palette SHALL remain responsive while the index is being built." `Index::is_indexing` is a
//! flag the panel reports; searching an index that is still filling returns what is in it, and the
//! palette says so rather than showing a spinner over an empty list.

use cy_editor_interface::palette::{Action, Index};
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::{Metrics, TextRole};

use crate::theme;

/// One line of text, ellipsised at `width` rather than wrapped or overrun.
fn one_line(
    painter: &egui::Painter,
    text: &str,
    size: f32,
    colour: egui::Color32,
    width: f32,
) -> std::sync::Arc<egui::Galley> {
    let mut job = egui::text::LayoutJob::single_section(
        text.to_owned(),
        egui::TextFormat::simple(egui::FontId::proportional(size), colour),
    );
    job.wrap = egui::text::TextWrapping {
        max_width: width,
        max_rows: 1,
        break_anywhere: false,
        overflow_character: Some('\u{2026}'),
    };
    painter.layout_job(job)
}

/// How many results are built. The list is short on purpose: a palette that shows a hundred results
/// is a palette that has stopped ranking.
const RESULTS: usize = 12;

/// The palette's own state, which is a query and a cursor and nothing else.
#[derive(Default)]
pub struct Palette {
    open: bool,
    query: String,
    highlighted: usize,
    /// Set for one frame after opening, so the field takes focus without stealing it every frame.
    just_opened: bool,
}

impl Palette {
    /// A closed palette.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Whether it is showing.
    #[must_use]
    pub const fn is_open(&self) -> bool {
        self.open
    }

    /// Open it, with an empty query.
    ///
    /// Empty rather than remembering the last one: `Index::search` returns the first entries for an
    /// empty query precisely so that the palette can be *browsed*, and browsing it is how a user
    /// finds out what the editor can do.
    pub fn open(&mut self) {
        self.open = true;
        self.just_opened = true;
        self.query.clear();
        self.highlighted = 0;
    }

    /// Close it.
    pub fn close(&mut self) {
        self.open = false;
    }

    /// Draw it, returning the action a user chose.
    ///
    /// Modal in the sense of taking the keyboard and nothing else: it is drawn over the interface,
    /// it does not stop a background operation, and `Escape` dismisses it. `editor-ui-ux` reserves
    /// true modals for decisions and destruction, and finding a command is neither.
    pub fn show(
        &mut self,
        ctx: &egui::Context,
        index: &Index,
        theme: cy_editor_visual::Theme,
        metrics: Metrics,
    ) -> Option<Action> {
        if !self.open {
            return None;
        }
        let results = index.search(&self.query, RESULTS);
        let count = results.len();

        // Read before the field consumes them: the arrows and Enter belong to the list even while
        // the text field has focus, which is what makes a palette usable without the mouse.
        let (up, down, enter, escape) = ctx.input(|input| {
            (
                input.key_pressed(egui::Key::ArrowUp),
                input.key_pressed(egui::Key::ArrowDown),
                input.key_pressed(egui::Key::Enter),
                input.key_pressed(egui::Key::Escape),
            )
        });
        if escape {
            self.close();
            return None;
        }
        if count > 0 {
            if down {
                self.highlighted = (self.highlighted + 1) % count;
            }
            if up {
                self.highlighted = (self.highlighted + count - 1) % count;
            }
            self.highlighted = self.highlighted.min(count - 1);
        } else {
            self.highlighted = 0;
        }

        let mut chosen = None;
        let width = (ctx.input(egui::InputState::viewport_rect).width() * 0.45).clamp(320.0, 720.0);
        egui::Area::new(egui::Id::new("command-palette"))
            .anchor(
                egui::Align2::CENTER_TOP,
                egui::vec2(0.0, metrics.row() * 4.0),
            )
            .order(egui::Order::Foreground)
            .show(ctx, |ui| {
                ui.set_width(width);
                egui::Frame::NONE
                    .fill(theme::surface(theme, Surface::Raised))
                    .stroke(egui::Stroke::new(1.0, theme::separator(theme)))
                    .corner_radius(egui::CornerRadius::same(4))
                    .inner_margin(egui::Margin::same(crate::theme::margin(metrics.padding())))
                    .show(ui, |ui| {
                        let field = ui.add(
                            egui::TextEdit::singleline(&mut self.query)
                                .hint_text("Type a command, an asset, a node or a setting")
                                .desired_width(f32::INFINITY)
                                .font(egui::FontId::proportional(metrics.text(TextRole::Title))),
                        );
                        if self.just_opened {
                            field.request_focus();
                            self.just_opened = false;
                        }
                        if index.is_indexing() {
                            ui.label(
                                egui::RichText::new(
                                    "Still indexing — results will improve as it fills.",
                                )
                                .size(metrics.text(TextRole::Secondary))
                                .color(theme::role(theme, Semantic::SecondaryText)),
                            );
                        }
                        ui.add_space(metrics.gap() * 0.5);

                        if results.is_empty() {
                            ui.label(
                                egui::RichText::new("Nothing matches.")
                                    .color(theme::role(theme, Semantic::SecondaryText)),
                            );
                        }
                        for (position, found) in results.iter().enumerate() {
                            let selected = position == self.highlighted;
                            if result_row(ui, found, selected, theme, metrics)
                                || (selected && enter)
                            {
                                chosen = Some(found.entry.action.clone());
                            }
                        }
                    });
            });

        if chosen.is_some() {
            self.close();
        }
        chosen
    }
}

/// One result: its label, what it is, and where it came from. Returns whether it was clicked.
fn result_row(
    ui: &mut egui::Ui,
    found: &cy_editor_interface::palette::Match<'_>,
    selected: bool,
    theme: cy_editor_visual::Theme,
    metrics: Metrics,
) -> bool {
    let (rect, response) = ui.allocate_exact_size(
        egui::vec2(ui.available_width(), metrics.row() * 1.4),
        egui::Sense::click(),
    );
    if selected || response.hovered() {
        ui.painter().rect_filled(
            rect,
            egui::CornerRadius::same(2),
            if selected {
                theme::selected_fill(theme)
            } else {
                theme::lifted(theme, Surface::Raised, 0.06)
            },
        );
    }
    let label = match found.via_alias {
        // The engine's own term, and the word that found it. Both, in that order, so the user
        // learns the vocabulary rather than being corrected by it.
        Some(alias) => format!("{alias} \u{2192} {}", found.entry.label),
        None => found.entry.label.clone(),
    };
    let text_left = rect.left() + metrics.gap();
    // The origin word sits at the right edge, so the label and the detail are laid out to stop
    // short of it and are ellipsised rather than drawn over it. A result whose description runs
    // under "Command" is one a user cannot read either half of.
    let origin_width = metrics.row() * 4.0;
    let available = (rect.width() - origin_width - metrics.gap() * 3.0).max(32.0);
    let primary = theme::role(theme, Semantic::PrimaryText);
    let secondary = theme::role(theme, Semantic::SecondaryText);
    let painter = ui.painter();
    painter.galley(
        egui::pos2(text_left, rect.center().y - metrics.row() * 0.62),
        one_line(
            painter,
            &label,
            metrics.text(TextRole::Body),
            primary,
            available,
        ),
        primary,
    );
    painter.galley(
        egui::pos2(text_left, rect.center().y + metrics.row() * 0.04),
        one_line(
            painter,
            &found.entry.detail,
            metrics.text(TextRole::Secondary),
            secondary,
            available,
        ),
        secondary,
    );
    painter.text(
        egui::pos2(rect.right() - metrics.gap(), rect.center().y),
        egui::Align2::RIGHT_CENTER,
        found.entry.origin.label(),
        egui::FontId::proportional(metrics.text(TextRole::Secondary)),
        secondary,
    );
    response.clicked()
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_interface::palette::{Entry, Origin};

    #[test]
    fn opening_the_palette_clears_the_query_so_it_can_be_browsed() {
        let mut palette = Palette::new();
        palette.query = "undo".into();
        palette.highlighted = 3;
        palette.open();
        assert!(palette.is_open());
        assert!(palette.query.is_empty());
        assert_eq!(palette.highlighted, 0);
    }

    #[test]
    fn an_empty_query_lists_entries_rather_than_nothing() {
        // The model's promise, restated here because the panel depends on it: a palette that is
        // blank until you type cannot be browsed.
        let mut index = Index::new();
        index.ingest([Entry::new(
            "Create Entity",
            "Creates an empty entity",
            Origin::Command,
            Action::Invoke("scene.create-entity".into()),
        )]);
        assert_eq!(index.search("", RESULTS).len(), 1);
    }
}
