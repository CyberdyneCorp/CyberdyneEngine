//! Keyboard-first operation: chords, contexts, conflicts reported rather than silently overridden.
//!
//! `editor-ui-ux`: "Every frequent workflow SHALL be completable **without the mouse** where the
//! operation is not inherently spatial. Keyboard bindings SHALL be user-configurable per command,
//! SHALL support chords and contexts, and SHALL **report conflicts rather than silently
//! overriding**."
//!
//! And, from the familiarity requirement: "The editor SHALL ship a **Unity-compatible keymap** as a
//! selectable preset, and SHALL support user-defined keymaps."
//!
//! --- WHY A CONFLICT IS AN ERROR AND NOT A LAST-WINS ------------------------------------------------
//!
//! Because the failure is silent and the diagnosis is expensive. A user rebinds something, an
//! unrelated command stops working, and nothing anywhere says the two are related. [`Keymap::bind`]
//! therefore refuses, and the refusal names **both** commands — which is the scenario the
//! specification writes: "WHEN a user assigns a binding already in use in the same context THEN the
//! conflict SHALL be shown with both commands named."
//!
//! A **prefix** conflict is refused for the same reason and is easier to miss: if `Ctrl+K` invokes
//! something, then `Ctrl+K Ctrl+S` can never fire, because the first stroke has already resolved.
//! Nothing about that is visible in a list of bindings, which is exactly why it is checked here.
//!
//! --- WHY CONTEXTS ARE STRINGS ---------------------------------------------------------------------
//!
//! A plugin's panel is a context, and an enum would make that false. `global` is the context every
//! other one falls back to, and a binding in a panel's context shadows the global one *for that
//! panel* rather than replacing it — which is how `Delete` can mean "delete the node" in the
//! hierarchy and "delete the keyframe" in the timeline without either of them being a conflict.

use std::collections::BTreeMap;
use std::fmt;

use cy_editor_commands::Registry;
use cy_editor_core::problem::{Problem, Result};

/// The modifiers held with a key.
///
/// Four booleans rather than a state machine: they are independent, they are what every platform's
/// key event carries, and a two-variant enum per modifier would make `Ctrl+Shift+N` read as
/// `Control::Held, Shift::Held, Alt::Free, Meta::Free` at every construction site.
#[expect(
    clippy::struct_excessive_bools,
    reason = "a keyboard has four independent modifiers; see the note above"
)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Default)]
pub struct Modifiers {
    /// Control.
    pub control: bool,
    /// Shift.
    pub shift: bool,
    /// Alt or option.
    pub alt: bool,
    /// The platform's command or super key.
    pub meta: bool,
}

/// One key with its modifiers: `Ctrl+Shift+N`.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct Stroke {
    /// What was held.
    pub modifiers: Modifiers,
    /// The key, normalised to its canonical spelling.
    pub key: String,
}

impl Stroke {
    /// Read a stroke such as `Ctrl+Shift+N`.
    ///
    /// The names are the ones a user types into a settings field and the ones a specification
    /// writes, so parsing them here rather than in the toolkit means a keymap file is portable
    /// across whatever toolkit is eventually chosen.
    pub fn parse(text: &str) -> Result<Self> {
        let mut modifiers = Modifiers::default();
        let mut key = None;
        for part in text
            .split('+')
            .map(str::trim)
            .filter(|part| !part.is_empty())
        {
            match part.to_ascii_lowercase().as_str() {
                "ctrl" | "control" => modifiers.control = true,
                "shift" => modifiers.shift = true,
                "alt" | "option" => modifiers.alt = true,
                "meta" | "cmd" | "command" | "super" => modifiers.meta = true,
                _ => {
                    if key.is_some() {
                        return Err(Problem::new(
                            format!("read the binding {text:?}"),
                            "it names two keys, and a stroke is modifiers plus one key",
                        )
                        .with_remedy("write a chord as separate strokes: \"Ctrl+K Ctrl+S\""));
                    }
                    key = Some(canonical(part));
                }
            }
        }
        let key = key.ok_or_else(|| {
            Problem::new(
                format!("read the binding {text:?}"),
                "it names modifiers and no key",
            )
            .with_remedy("a binding is modifiers plus a key, such as Ctrl+S")
        })?;
        Ok(Self { modifiers, key })
    }
}

/// The canonical spelling of a key name, so that `ctrl+s` and `Ctrl+S` are one binding.
fn canonical(key: &str) -> String {
    let mut characters = key.chars();
    match characters.next() {
        Some(first) if key.chars().count() == 1 => first.to_ascii_uppercase().to_string(),
        Some(first) => {
            format!(
                "{}{}",
                first.to_uppercase(),
                characters.as_str().to_lowercase()
            )
        }
        None => String::new(),
    }
}

impl fmt::Display for Stroke {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.modifiers.control {
            formatter.write_str("Ctrl+")?;
        }
        if self.modifiers.alt {
            formatter.write_str("Alt+")?;
        }
        if self.modifiers.shift {
            formatter.write_str("Shift+")?;
        }
        if self.modifiers.meta {
            formatter.write_str("Meta+")?;
        }
        formatter.write_str(&self.key)
    }
}

/// A sequence of strokes: one, or a chord such as `Ctrl+K Ctrl+S`.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct Chord(Vec<Stroke>);

impl Chord {
    /// Read a chord: strokes separated by spaces.
    pub fn parse(text: &str) -> Result<Self> {
        let strokes: Vec<Stroke> = text
            .split_whitespace()
            .map(Stroke::parse)
            .collect::<Result<_>>()?;
        if strokes.is_empty() {
            return Err(
                Problem::new(format!("read the binding {text:?}"), "it is empty")
                    .with_remedy("a binding is one or more strokes, such as \"Ctrl+K Ctrl+S\""),
            );
        }
        Ok(Self(strokes))
    }

    /// The strokes, in order.
    #[must_use]
    pub fn strokes(&self) -> &[Stroke] {
        &self.0
    }

    /// Whether this chord begins with `other` and is longer — the shadowing relation.
    #[must_use]
    pub fn extends(&self, other: &Chord) -> bool {
        self.0.len() > other.0.len() && self.0.starts_with(&other.0)
    }
}

impl fmt::Display for Chord {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let strokes: Vec<String> = self.0.iter().map(ToString::to_string).collect();
        formatter.write_str(&strokes.join(" "))
    }
}

/// Where a binding applies. `global` is the fallback every other context has.
pub const GLOBAL: &str = "global";

/// What a sequence of strokes so far means.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Resolution {
    /// It invokes this command.
    Command(String),
    /// It is the start of a longer chord; the editor waits for the next stroke.
    Pending,
    /// It means nothing here.
    Unbound,
}

/// A named set of bindings.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Keymap {
    name: String,
    bindings: BTreeMap<(String, Chord), String>,
}

impl Keymap {
    /// An empty keymap.
    #[must_use]
    pub fn new(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            bindings: BTreeMap::new(),
        }
    }

    /// The keymap's name, which a refusal quotes.
    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    /// How many bindings it holds.
    #[must_use]
    pub fn len(&self) -> usize {
        self.bindings.len()
    }

    /// Whether it holds none.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.bindings.is_empty()
    }

    /// Bind a chord in a context, **refusing** a conflict and naming both commands.
    pub fn bind(&mut self, context: &str, chord: &str, command: impl Into<String>) -> Result<()> {
        let chord = Chord::parse(chord)?;
        let command = command.into();
        if let Some(problem) = self.conflict(context, &chord, &command) {
            return Err(problem);
        }
        self.bindings.insert((context.to_string(), chord), command);
        Ok(())
    }

    /// Replace an existing binding deliberately.
    ///
    /// The escape hatch a settings panel needs *after* it has shown the conflict and the user has
    /// said yes. It is a different method from [`Keymap::bind`] so that the refusal cannot be
    /// bypassed by accident, which is the whole value of the refusal.
    pub fn rebind(&mut self, context: &str, chord: &str, command: impl Into<String>) -> Result<()> {
        let chord = Chord::parse(chord)?;
        self.bindings
            .retain(|(held_context, held), _| held_context != context || held != &chord);
        self.bindings
            .insert((context.to_string(), chord), command.into());
        Ok(())
    }

    /// Unbind a chord in a context.
    pub fn unbind(&mut self, context: &str, chord: &Chord) {
        self.bindings.remove(&(context.to_string(), chord.clone()));
    }

    /// The command a chord invokes in a context, falling back to the global context.
    #[must_use]
    pub fn command_for(&self, context: &str, chord: &Chord) -> Option<&str> {
        self.bindings
            .get(&(context.to_string(), chord.clone()))
            .or_else(|| self.bindings.get(&(GLOBAL.to_string(), chord.clone())))
            .map(String::as_str)
    }

    /// The bindings for a command, in context order.
    #[must_use]
    pub fn bindings_of(&self, command: &str) -> Vec<(String, Chord)> {
        self.bindings
            .iter()
            .filter(|(_, bound)| bound.as_str() == command)
            .map(|((context, chord), _)| (context.clone(), chord.clone()))
            .collect()
    }

    /// What a partial sequence of strokes means so far.
    ///
    /// The three answers a keyboard handler needs, and the reason `Pending` exists: a chord's first
    /// stroke must not be swallowed as "unbound" or the second stroke never arrives.
    #[must_use]
    pub fn resolve(&self, context: &str, strokes: &[Stroke]) -> Resolution {
        let sequence = Chord(strokes.to_vec());
        if let Some(command) = self.command_for(context, &sequence) {
            return Resolution::Command(command.to_string());
        }
        let pending = self.bindings.iter().any(|((held_context, chord), _)| {
            (held_context == context || held_context == GLOBAL) && chord.extends(&sequence)
        });
        if pending {
            Resolution::Pending
        } else {
            Resolution::Unbound
        }
    }

    /// The conflict a binding would create, if it would create one.
    fn conflict(&self, context: &str, chord: &Chord, command: &str) -> Option<Problem> {
        for ((held_context, held), held_command) in &self.bindings {
            if held_context != context {
                continue;
            }
            if held == chord && held_command != command {
                return Some(
                    Problem::new(
                        format!("bind {chord} to {command} in the {context} context"),
                        format!("{chord} already invokes {held_command} there"),
                    )
                    .with_remedy(format!(
                        "rebind {held_command} first, or choose another binding for {command}"
                    )),
                );
            }
            if chord.extends(held) {
                return Some(
                    Problem::new(
                        format!("bind {chord} to {command} in the {context} context"),
                        format!(
                            "{held} already invokes {held_command} there, so the first stroke \
                             resolves and the rest of the chord never arrives"
                        ),
                    )
                    .with_remedy(format!(
                        "rebind {held_command} to something that is not a prefix of {chord}"
                    )),
                );
            }
            if held.extends(chord) {
                return Some(
                    Problem::new(
                        format!("bind {chord} to {command} in the {context} context"),
                        format!(
                            "{held} invokes {held_command} there, and binding its prefix would \
                             stop that chord ever completing"
                        ),
                    )
                    .with_remedy(format!("rebind {held_command}, or use a longer chord")),
                );
            }
        }
        None
    }

    /// The editor's own keymap, taken from what each command declares.
    ///
    /// Built from the registry rather than written out again, so a command that declares a default
    /// binding gets one and a command that does not is reachable through the palette — and the two
    /// cannot disagree about which key does what.
    pub fn cyberdyne(registry: &Registry) -> Result<Self> {
        let mut keymap = Self::new("Cyberdyne");
        for metadata in registry.all() {
            if let Some(binding) = &metadata.default_binding {
                keymap.bind(GLOBAL, binding, metadata.id.clone())?;
            }
        }
        Ok(keymap)
    }

    /// The Unity-compatible preset.
    ///
    /// The transform tools on `W`, `E`, `R` and framing on `F` are the four an expert's hands do
    /// without thinking, and they are what "an expert user's first hour SHALL not be spent
    /// relearning where things are" is mostly about. It starts from the editor's own map so that
    /// everything not mentioned still works.
    pub fn unity(registry: &Registry) -> Result<Self> {
        let mut keymap = Self::cyberdyne(registry)?;
        keymap.name = "Unity".into();
        for (chord, command) in [
            ("W", "viewport.transform-mode-move"),
            ("E", "viewport.transform-mode-rotate"),
            ("R", "viewport.transform-mode-scale"),
            ("F", "viewport.frame-selection"),
            ("Ctrl+Y", "edit.redo"),
            ("Ctrl+D", "scene.duplicate-entity"),
        ] {
            keymap.rebind(GLOBAL, chord, command)?;
        }
        Ok(keymap)
    }

    /// Every command in `registry` that this keymap does not bind.
    ///
    /// Not a failure: most commands should not have a binding, and a keymap that bound everything
    /// would have no keys left. It is the input to the check that matters — that an unbound command
    /// is still reachable by typing — which `palette` asserts from the other side.
    #[must_use]
    pub fn unbound(&self, registry: &Registry) -> Vec<String> {
        registry
            .all()
            .filter(|metadata| self.bindings_of(&metadata.id).is_empty())
            .map(|metadata| metadata.id.clone())
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_services::builtin;

    use super::*;

    fn registry() -> Registry {
        let mut registry = Registry::new();
        builtin::register(&mut registry).unwrap();
        registry
    }

    #[test]
    fn a_conflict_is_reported_with_both_commands_named() {
        // "WHEN a user assigns a binding already in use in the same context THEN the conflict SHALL
        // be shown with both commands named."
        let mut keymap = Keymap::new("User");
        keymap.bind(GLOBAL, "Ctrl+S", "file.save").unwrap();
        let problem = keymap.bind(GLOBAL, "Ctrl+S", "file.save-all").unwrap_err();

        assert!(problem.what.contains("file.save-all"), "{problem}");
        assert!(problem.because.contains("file.save"), "{problem}");
        assert!(problem.remedy.is_some(), "{problem}");
        assert_eq!(
            keymap.command_for(GLOBAL, &Chord::parse("Ctrl+S").unwrap()),
            Some("file.save")
        );
    }

    #[test]
    fn the_same_binding_in_another_context_is_not_a_conflict() {
        let mut keymap = Keymap::new("User");
        keymap
            .bind("hierarchy", "Delete", "scene.delete-entity")
            .unwrap();
        keymap
            .bind("timeline", "Delete", "sequence.delete-key")
            .unwrap();

        let delete = Chord::parse("Delete").unwrap();
        assert_eq!(
            keymap.command_for("hierarchy", &delete),
            Some("scene.delete-entity")
        );
        assert_eq!(
            keymap.command_for("timeline", &delete),
            Some("sequence.delete-key")
        );
    }

    #[test]
    fn a_chord_whose_prefix_is_already_bound_is_refused_with_the_reason() {
        // The conflict nobody sees in a list of bindings: the first stroke resolves, so the chord
        // can never complete.
        let mut keymap = Keymap::new("User");
        keymap.bind(GLOBAL, "Ctrl+K", "window.close").unwrap();
        let problem = keymap
            .bind(GLOBAL, "Ctrl+K Ctrl+S", "file.save-all")
            .unwrap_err();
        assert!(problem.because.contains("never arrives"), "{problem}");
    }

    #[test]
    fn binding_the_prefix_of_an_existing_chord_is_refused_too() {
        let mut keymap = Keymap::new("User");
        keymap
            .bind(GLOBAL, "Ctrl+K Ctrl+S", "file.save-all")
            .unwrap();
        let problem = keymap.bind(GLOBAL, "Ctrl+K", "window.close").unwrap_err();
        assert!(problem.because.contains("completing"), "{problem}");
    }

    #[test]
    fn a_chord_resolves_one_stroke_at_a_time() {
        let mut keymap = Keymap::new("User");
        keymap
            .bind(GLOBAL, "Ctrl+K Ctrl+S", "file.save-all")
            .unwrap();

        let first = Stroke::parse("Ctrl+K").unwrap();
        let second = Stroke::parse("Ctrl+S").unwrap();
        assert_eq!(
            keymap.resolve(GLOBAL, std::slice::from_ref(&first)),
            Resolution::Pending
        );
        assert_eq!(
            keymap.resolve(GLOBAL, &[first, second]),
            Resolution::Command("file.save-all".into())
        );
        assert_eq!(
            keymap.resolve(GLOBAL, &[Stroke::parse("Ctrl+J").unwrap()]),
            Resolution::Unbound
        );
    }

    #[test]
    fn the_editors_keymap_comes_from_what_the_commands_declare() {
        let registry = registry();
        let keymap = Keymap::cyberdyne(&registry).unwrap();
        assert_eq!(
            keymap.command_for(GLOBAL, &Chord::parse("Ctrl+Z").unwrap()),
            Some("edit.undo")
        );
        for metadata in registry.all() {
            if let Some(binding) = &metadata.default_binding {
                let chord = Chord::parse(binding).unwrap();
                assert_eq!(
                    keymap.command_for(GLOBAL, &chord),
                    Some(metadata.id.as_str())
                );
            }
        }
    }

    #[test]
    fn a_unity_user_finds_the_transform_tools_where_they_expect_them() {
        // "WHEN a user selects the Unity keymap THEN navigation, transform tools, and common
        // commands SHALL match their expectations."
        let keymap = Keymap::unity(&registry()).unwrap();
        for (key, command) in [
            ("W", "viewport.transform-mode-move"),
            ("E", "viewport.transform-mode-rotate"),
            ("R", "viewport.transform-mode-scale"),
            ("F", "viewport.frame-selection"),
        ] {
            assert_eq!(
                keymap.command_for(GLOBAL, &Chord::parse(key).unwrap()),
                Some(command)
            );
        }
        assert_eq!(
            keymap.command_for(GLOBAL, &Chord::parse("Ctrl+S").unwrap()),
            Some("file.save"),
            "a preset starts from the editor's own map rather than replacing it"
        );
    }

    #[test]
    fn a_binding_is_read_the_way_a_user_writes_it() {
        assert_eq!(
            Stroke::parse("ctrl+shift+n").unwrap(),
            Stroke::parse("Ctrl+Shift+N").unwrap()
        );
        assert_eq!(Stroke::parse("Ctrl+S").unwrap().to_string(), "Ctrl+S");
        assert_eq!(
            Chord::parse("Ctrl+K Ctrl+S").unwrap().to_string(),
            "Ctrl+K Ctrl+S"
        );
        assert_eq!(Stroke::parse("escape").unwrap().key, "Escape");

        assert!(Stroke::parse("Ctrl+").is_err(), "modifiers with no key");
        assert!(Stroke::parse("Ctrl+S+N").is_err(), "two keys in one stroke");
        assert!(Chord::parse("   ").is_err());
    }

    #[test]
    fn rebinding_is_a_different_call_from_binding() {
        let mut keymap = Keymap::new("User");
        keymap.bind(GLOBAL, "Ctrl+S", "file.save").unwrap();
        assert!(keymap.bind(GLOBAL, "Ctrl+S", "file.save-all").is_err());
        keymap.rebind(GLOBAL, "Ctrl+S", "file.save-all").unwrap();
        assert_eq!(
            keymap.command_for(GLOBAL, &Chord::parse("Ctrl+S").unwrap()),
            Some("file.save-all")
        );
    }
}
