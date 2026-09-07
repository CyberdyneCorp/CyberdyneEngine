//! Keyboard-first operation, over the keymap M5 built. Task 1.4.
//!
//! `editor-ui-ux` requires that "every action SHALL be reachable from the keyboard" and that chords
//! work — `Ctrl+K Ctrl+S` is two strokes, and the first must not resolve to "unbound" or the second
//! never arrives. `cy_editor_interface::keymap` already answers all three questions a keyboard
//! handler asks (`Command`, `Pending`, `Unbound`); this module is the thin part that turns the
//! toolkit's events into the strokes it resolves, and holds the one piece of state a chord needs.
//!
//! --- WHY THE PENDING STROKE LIVES HERE AND NOT IN THE KEYMAP ---------------------------------------
//!
//! A `Keymap` is a *table*. Which strokes have been pressed so far is session state belonging to the
//! window that received them, and putting it in the table would make two windows share one chord in
//! progress — which is exactly the bug that makes chords feel unreliable in editors that have it.
//!
//! --- THE ORDER OF PRECEDENCE, WHICH IS A REQUIREMENT AND NOT A DETAIL --------------------------------
//!
//! A text field being edited takes the keystroke first. `editor-ui-ux` is explicit that an
//! in-progress edit is a distinct state, and a `Ctrl+Z` typed inside a renamed node's field must
//! undo the typing rather than the last transaction. [`Keys::pump`] therefore refuses to consume
//! anything while egui reports a focused widget — **unless a chord is already in progress**.
//!
//! That exception is not a convenience. Driving the real window found the failure it prevents: with
//! the guard applied unconditionally, `Ctrl+K` was dropped on a frame where something held focus and
//! `Ctrl+S` then resolved *by itself*, so the workspace-save chord ran `file.save`. A chord that
//! silently becomes its own second stroke invokes a **different command**, which is a worse outcome
//! than invoking none, and it is invisible until somebody watches what happened. Once a chord is
//! open the window has already taken the keyboard; `Escape` and any unbound stroke abandon it.

use cy_editor_interface::keymap::{Keymap, Modifiers, Resolution, Stroke};

/// What a frame's keyboard input resolved to.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Pressed {
    /// A command was invoked. The identifier may be a registry command or one of the shell's own.
    Command(String),
    /// A chord is part-way through. The interface says so, because a chord that gives no feedback
    /// is indistinguishable from a key that did nothing.
    Pending(String),
    /// Nothing was bound.
    Nothing,
}

/// The keyboard state of one window: the strokes of a chord so far.
#[derive(Default)]
pub struct Keys {
    strokes: Vec<Stroke>,
    /// The last pass this consumed input on. See [`Keys::pump`].
    consumed_at: Option<u64>,
}

impl Keys {
    /// A window with no chord in progress.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The chord so far, for the status line. Empty when none is in progress.
    #[must_use]
    pub fn pending(&self) -> String {
        self.strokes
            .iter()
            .map(ToString::to_string)
            .collect::<Vec<_>>()
            .join(" ")
    }

    /// Abandon a chord in progress.
    pub fn clear(&mut self) {
        self.strokes.clear();
    }

    /// Resolve this frame's key presses against a keymap.
    ///
    /// Returns the first thing that resolved. One per frame is not a limitation: two commands from
    /// one frame's input would be two undo entries a user could not have intended, and the editor
    /// runs at interface frame rate rather than at typing rate.
    pub fn pump(&mut self, ctx: &egui::Context, keymap: &Keymap, context: &str) -> Pressed {
        // egui runs **more than one pass over the same input** when a widget asks the frame to be
        // discarded and rebuilt, and `input.events` is the same list on every one of them. Consuming
        // keys without noticing that turns one press into two, and a doubled stroke does not fail
        // cleanly: `Ctrl+K` pushed twice resolves to Unbound and abandons the chord, so the next
        // stroke resolves on its own and a *different* command runs.
        //
        // So input is consumed once per pass number. `cumulative_pass_nr` counts passes and never
        // goes backwards, which makes this a comparison rather than a heuristic. It is a separate
        // measure from [`is_modifier`], which is what the doubled-stroke failure actually turned out
        // to be when the window was driven — but the multi-pass hazard is real and costs one field.
        let pass = ctx.cumulative_pass_nr();
        if self.consumed_at == Some(pass) {
            return self.holding();
        }
        self.consumed_at = Some(pass);

        // A chord already in progress owns the keyboard until it resolves or is abandoned. The
        // guard is skipped for it because egui reports "wants keyboard" for any focused widget, and
        // a chord that gave up its second stroke because something had focus would not fail cleanly:
        // the second stroke then resolves *on its own*, and a different command runs. See
        // [`is_modifier`] for the same shape of failure arriving by a different route.
        if self.strokes.is_empty() && ctx.egui_wants_keyboard_input() {
            // A field has focus and no chord is open. The keystroke is text, not a command.
            return Pressed::Nothing;
        }

        let strokes: Vec<Stroke> = ctx.input(|input| {
            input
                .events
                .iter()
                .filter_map(|event| match event {
                    egui::Event::Key {
                        key,
                        pressed: true,
                        repeat: false,
                        modifiers,
                        ..
                    } if !is_modifier(*key) => stroke(*key, *modifiers),
                    _ => None,
                })
                .collect()
        });

        for stroke in strokes {
            self.strokes.push(stroke);
            match keymap.resolve(context, &self.strokes) {
                Resolution::Command(command) => {
                    self.clear();
                    return Pressed::Command(command);
                }
                Resolution::Pending => return Pressed::Pending(self.pending()),
                Resolution::Unbound => self.clear(),
            }
        }
        self.holding()
    }

    /// What to report on a pass that consumed nothing: the chord still open, or nothing.
    fn holding(&self) -> Pressed {
        if self.strokes.is_empty() {
            Pressed::Nothing
        } else {
            Pressed::Pending(self.pending())
        }
    }
}

/// Whether a key event is a modifier being pressed by itself.
///
/// egui 0.36 emits a physical `Event::Key` for `ControlLeft`, `ShiftRight` and the rest **in
/// addition to** setting them in `modifiers`, so pressing `Ctrl+K` delivers two key events: the
/// control key, and then `K` with control held. Feeding the first to the keymap is not a harmless
/// extra — `Controlleft` parses as a perfectly good stroke, resolves to `Unbound`, and **abandons
/// any chord in progress**. Driving the real window found the consequence: `Ctrl+K Ctrl+S` ran
/// `file.save`, because the chord was cleared by the control key of its own second stroke and the
/// `Ctrl+S` then resolved alone.
///
/// A stroke is modifiers plus one key, which `Stroke::parse` already says; this is the same rule
/// applied to what the toolkit reports.
fn is_modifier(key: egui::Key) -> bool {
    matches!(
        key,
        egui::Key::ShiftLeft
            | egui::Key::ShiftRight
            | egui::Key::ControlLeft
            | egui::Key::ControlRight
            | egui::Key::AltLeft
            | egui::Key::AltRight
            | egui::Key::SuperLeft
            | egui::Key::SuperRight
    )
}

/// One toolkit key event as a stroke the keymap understands.
///
/// Built through `Stroke::parse` rather than by constructing the struct, so that the spelling of a
/// key is normalised by exactly the same code that normalises a binding written in a settings file.
/// A key whose name the keymap cannot parse is dropped rather than guessed at — the modifier keys
/// themselves are the whole of that set, and a stroke of "Ctrl" alone is not a binding.
fn stroke(key: egui::Key, modifiers: egui::Modifiers) -> Option<Stroke> {
    let mut text = String::new();
    if modifiers.ctrl {
        text.push_str("Ctrl+");
    }
    if modifiers.alt {
        text.push_str("Alt+");
    }
    if modifiers.shift {
        text.push_str("Shift+");
    }
    if modifiers.command && !modifiers.ctrl {
        text.push_str("Meta+");
    }
    text.push_str(key.name());
    Stroke::parse(&text).ok()
}

/// The modifiers of a pointer event, in the interface's own vocabulary.
///
/// The viewport's manipulation modifiers — `Ctrl` for temporary snap, `Shift` for precision, `Alt`
/// for duplicate-and-transform — are read from the same struct the keymap uses, so a panel and the
/// viewport cannot disagree about what "Shift is held" means.
#[must_use]
pub fn modifiers(from: egui::Modifiers) -> Modifiers {
    Modifiers {
        control: from.ctrl,
        shift: from.shift,
        alt: from.alt,
        meta: from.mac_cmd,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_interface::keymap::GLOBAL;

    fn keymap() -> Keymap {
        let mut keymap = Keymap::new("test");
        keymap.bind(GLOBAL, "Ctrl+S", "file.save").unwrap();
        keymap
            .bind(GLOBAL, "Ctrl+K Ctrl+W", "window.close-all")
            .unwrap();
        keymap
    }

    #[test]
    fn a_toolkit_key_event_becomes_the_stroke_a_binding_parses_to() {
        let held = egui::Modifiers {
            ctrl: true,
            ..Default::default()
        };
        assert_eq!(
            stroke(egui::Key::S, held),
            Some(Stroke::parse("Ctrl+S").unwrap())
        );
        // The spelling the toolkit uses and the spelling a settings file uses are normalised by one
        // function, which is the property that stops "PageUp" and "Pageup" being two bindings.
        assert_eq!(
            stroke(egui::Key::PageUp, egui::Modifiers::default()),
            Some(Stroke::parse("PageUp").unwrap())
        );
    }

    #[test]
    fn a_modifier_pressed_by_itself_is_not_a_stroke() {
        // A regression test, and the defect was real and expensive to see: egui emits a physical
        // key event for the control key as well as for the key it modifies, so `Ctrl+K Ctrl+S`
        // arrived as four events. The two modifier events resolved to Unbound and abandoned the
        // chord, and the workspace-save chord silently ran `file.save`.
        for key in [
            egui::Key::ShiftLeft,
            egui::Key::ShiftRight,
            egui::Key::ControlLeft,
            egui::Key::ControlRight,
            egui::Key::AltLeft,
            egui::Key::AltRight,
            egui::Key::SuperLeft,
            egui::Key::SuperRight,
        ] {
            assert!(is_modifier(key), "{} is a modifier", key.name());
        }
        assert!(!is_modifier(egui::Key::K));
        assert!(!is_modifier(egui::Key::F11));
    }

    #[test]
    fn a_chord_holds_across_frames_and_resolves_on_its_last_stroke() {
        // The behaviour the `Pending` answer exists for. Driven through the state machine directly,
        // because the toolkit's event queue is not what is under test here.
        let keymap = keymap();
        let mut keys = Keys::new();
        keys.strokes.push(Stroke::parse("Ctrl+K").unwrap());
        assert_eq!(keymap.resolve(GLOBAL, &keys.strokes), Resolution::Pending);
        assert_eq!(keys.pending(), "Ctrl+K");
        keys.strokes.push(Stroke::parse("Ctrl+W").unwrap());
        assert_eq!(
            keymap.resolve(GLOBAL, &keys.strokes),
            Resolution::Command("window.close-all".into())
        );
    }

    #[test]
    fn an_unbound_stroke_abandons_the_chord_rather_than_accumulating_forever() {
        let keymap = keymap();
        let mut keys = Keys::new();
        keys.strokes.push(Stroke::parse("Ctrl+K").unwrap());
        keys.strokes.push(Stroke::parse("Q").unwrap());
        assert_eq!(keymap.resolve(GLOBAL, &keys.strokes), Resolution::Unbound);
        keys.clear();
        assert_eq!(keys.pending(), "");
    }

    #[test]
    fn the_viewport_modifiers_are_the_same_struct_the_keymap_uses() {
        let held = egui::Modifiers {
            shift: true,
            alt: true,
            ..Default::default()
        };
        let held = modifiers(held);
        assert!(held.shift && held.alt && !held.control);
    }
}
