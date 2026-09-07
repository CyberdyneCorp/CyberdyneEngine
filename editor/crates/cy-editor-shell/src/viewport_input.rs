//! The toolkit's pointer and keys, as the viewport model's events. Tasks 2.2, 2.3, 2.4.
//!
//! Everything that *decides* anything is in `cy_editor_viewport::interaction`, which has no toolkit
//! and is tested with none. This file is the translation, and it is deliberately dull: read egui's
//! input, produce [`cy_editor_viewport::interaction::Event`]s in the order they happened, hand them
//! over. A decision that appears here has escaped from a crate a test can drive.
//!
//! --- TWO HAZARDS THIS FILE EXISTS TO CONTAIN --------------------------------------------------------
//!
//! **egui runs more than one pass over the same input.** [`crate::keys`] found it first and the note
//! there explains what it costs; here it would double every press, which turns one axis-lock keystroke
//! into a lock and an unlock. So input is consumed once per `cumulative_pass_nr`, exactly as `Keys`
//! does.
//!
//! **Pixels are not viewport pixels.** egui reports positions in the window's coordinates and the
//! viewport model reasons in the viewport's own — its top left is the panel's, not the screen's. The
//! conversion happens once, here, and the viewport's [`cy_editor_viewport::state::ViewportRect`] is
//! kept in step with the panel's size in the same place, because a ray built from a stale size points
//! somewhere the user is not looking.

use cy_editor_viewport::interaction::{Event, Key};
use cy_editor_viewport::navigation::{Button, Modifiers};

/// One viewport panel's pointer and keyboard state between frames.
#[derive(Default)]
pub struct ViewportInput {
    /// The last pass this consumed input on. See the module note.
    consumed_at: Option<u64>,
    /// Where the pointer was, in viewport pixels, so a move is a delta.
    last: Option<(f32, f32)>,
    /// Whether the pointer was inside the viewport last frame, so leaving it is an event.
    inside: bool,
}

impl ViewportInput {
    /// A panel nobody has pointed at.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// This frame's events, in the order they happened.
    ///
    /// `rect` is the panel's rectangle in screen coordinates; `focused` says whether this viewport
    /// should take keyboard input at all, which is false when a text field has it.
    pub fn events(
        &mut self,
        ui: &egui::Ui,
        rect: egui::Rect,
        response: &egui::Response,
    ) -> Vec<Event> {
        let ctx = ui.ctx();
        let pass = ctx.cumulative_pass_nr();
        if self.consumed_at == Some(pass) {
            return Vec::new();
        }
        self.consumed_at = Some(pass);

        let mut events = Vec::new();
        let modifiers = modifiers(ctx);
        let pointer = ctx
            .input(|input| input.pointer.latest_pos())
            .map(|position| (position.x - rect.left(), position.y - rect.top()));
        let inside = pointer
            .is_some_and(|(x, y)| x >= 0.0 && y >= 0.0 && x <= rect.width() && y <= rect.height());

        // A move first, so that a press is resolved at the pixel it happened at rather than at the
        // one the previous frame ended on.
        if let Some((x, y)) = pointer
            && self.last != Some((x, y))
        {
            self.last = Some((x, y));
            events.push(Event::PointerMoved { x, y });
        }

        if let Some((x, y)) = pointer {
            for (egui_button, button) in BUTTONS {
                if ctx.input(|input| input.pointer.button_pressed(egui_button)) && inside {
                    events.push(Event::PointerDown {
                        button,
                        x,
                        y,
                        modifiers,
                    });
                }
                // A release is taken wherever it happens, including outside the panel: a drag that
                // ended off the edge of the viewport is a drag that ended, and one that never
                // released would leave a transaction open.
                if ctx.input(|input| input.pointer.button_released(egui_button)) {
                    events.push(Event::PointerUp {
                        button,
                        x,
                        y,
                        modifiers,
                    });
                }
            }
        }

        if response.hovered() {
            let notches = ctx.input(|input| input.smooth_scroll_delta.y) / WHEEL_NOTCH;
            if notches != 0.0 {
                events.push(Event::Wheel { notches, modifiers });
            }
        }

        // Keys only when no text field has the keyboard, and only while the pointer is over the
        // viewport or a manipulation is running — `X` in the outliner is a letter.
        if !ctx.egui_wants_keyboard_input() && (inside || response.is_pointer_button_down_on()) {
            for (egui_key, key) in KEYS {
                if ctx.input(|input| input.key_pressed(egui_key)) {
                    events.push(Event::Key(key));
                }
            }
        }

        if self.inside && !inside {
            events.push(Event::PointerLeft);
        }
        self.inside = inside;
        events
    }
}

/// How much smooth scroll delta counts as one wheel notch.
///
/// egui reports scrolling in points rather than in notches, and a mouse wheel's detent is about fifty
/// of them on every platform this runs on. Dividing rather than clamping keeps a trackpad's fine
/// scroll fine, which is the whole reason egui smooths it.
const WHEEL_NOTCH: f32 = 50.0;

/// The three buttons, in the order a binding table names them.
const BUTTONS: [(egui::PointerButton, Button); 3] = [
    (egui::PointerButton::Primary, Button::Left),
    (egui::PointerButton::Middle, Button::Middle),
    (egui::PointerButton::Secondary, Button::Right),
];

/// The keys a viewport interprets itself. `W`/`E`/`R`/`T` are commands and are deliberately absent;
/// see `cy_editor_viewport::interaction::Key`.
const KEYS: [(egui::Key, Key); 4] = [
    (egui::Key::X, Key::LockX),
    (egui::Key::Y, Key::LockY),
    (egui::Key::Z, Key::LockZ),
    (egui::Key::Escape, Key::Cancel),
];

/// egui's modifiers as the viewport's own.
fn modifiers(ctx: &egui::Context) -> Modifiers {
    let held = ctx.input(|input| input.modifiers);
    Modifiers {
        shift: held.shift,
        // `command` rather than `ctrl`, so that a Mac user's ⌘ means what Ctrl means everywhere else
        // — the same rule `crate::keys::modifiers` follows, and for the same reason.
        control: held.command,
        alt: held.alt,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_keys_this_panel_takes_are_the_four_the_viewport_interprets_itself() {
        // W/E/R/T are commands in the registry, bound in the keymap, reachable from the palette and
        // from an agent. A viewport that took them here would be the "action reachable only through
        // a specific widget" the specification calls a defect.
        let taken: Vec<egui::Key> = KEYS.iter().map(|(key, _)| *key).collect();
        for forbidden in [egui::Key::W, egui::Key::E, egui::Key::R, egui::Key::T] {
            assert!(
                !taken.contains(&forbidden),
                "{forbidden:?} is a command, not a viewport key"
            );
        }
        assert_eq!(taken.len(), 4);
    }

    #[test]
    fn a_wheel_notch_is_a_notch_rather_than_fifty_of_them() {
        // The failure this constant prevents is a single detent zooming the camera fifty steps,
        // which reads as a viewport that teleports.
        const { assert!(WHEEL_NOTCH > 1.0) }
    }
}
