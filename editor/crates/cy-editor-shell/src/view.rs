//! The window's own actions: the ones that change how the editor looks, not what it holds.
//!
//! --- WHY THESE ARE NOT REGISTRY COMMANDS -----------------------------------------------------------
//!
//! `cy-editor-commands` is *"the single action surface"* for **mutation**. Every entry in it carries a
//! declared effect class, is refusable by scope, and is projectable to an agent — because every entry
//! in it can change the project. Opening the command palette, switching to the comfortable density
//! and resetting the workspace change none of that. They change this window.
//!
//! Putting them in the registry would give an agent tools that move a human's panels around, which
//! `editor-agent-interface`'s scope model has no vocabulary for and no reason to want. Leaving them
//! with no identifier at all would make them unbindable, unsearchable, and invisible to the
//! conflict check — the three things `editor-ui-ux` requires of every action.
//!
//! So they are a small closed set with stable identifiers, bound in the **same** `Keymap` as the
//! registry's commands so that a conflict between `Ctrl+P` and a future `project.publish` is caught
//! by `Keymap::bind` and reported with both commands named, and indexed in the **same** palette so
//! that they are found by typing.
//!
//! --- AND WHY THE SET IS CLOSED ----------------------------------------------------------------------
//!
//! [`ViewAction::ALL`] is the whole of it, so an addition is a visible edit here rather than a string
//! appearing in a match arm somewhere. The moment one of these needs an effect class or a scope, it
//! is not a view action and belongs in the registry.

use cy_editor_interface::palette::{Action, Entry, Origin};
use cy_editor_visual::gizmo::GizmoMode;

/// Something the window can do to itself.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ViewAction {
    /// Open the command palette.
    CommandPalette,
    /// Switch between compact and comfortable density.
    ToggleDensity,
    /// Switch between the dark and light themes.
    ToggleTheme,
    /// Switch between the standard and red-green-safe palettes.
    ToggleVision,
    /// Make the interface larger.
    ScaleUp,
    /// Make the interface smaller.
    ScaleDown,
    /// Save the current arrangement under its name.
    SaveWorkspace,
    /// Return to the shipped arrangement.
    ResetWorkspace,
    /// Show or hide the viewport's floating chrome.
    ToggleViewportChrome,
}

impl ViewAction {
    /// Every view action. The whole set.
    pub const ALL: [ViewAction; 9] = [
        ViewAction::CommandPalette,
        ViewAction::ToggleDensity,
        ViewAction::ToggleTheme,
        ViewAction::ToggleVision,
        ViewAction::ScaleUp,
        ViewAction::ScaleDown,
        ViewAction::SaveWorkspace,
        ViewAction::ResetWorkspace,
        ViewAction::ToggleViewportChrome,
    ];

    /// The stable identifier a binding, a menu and the palette all name.
    #[must_use]
    pub const fn id(self) -> &'static str {
        match self {
            ViewAction::CommandPalette => "view.command-palette",
            ViewAction::ToggleDensity => "view.toggle-density",
            ViewAction::ToggleTheme => "view.toggle-theme",
            ViewAction::ToggleVision => "view.toggle-vision",
            ViewAction::ScaleUp => "view.scale-up",
            ViewAction::ScaleDown => "view.scale-down",
            ViewAction::SaveWorkspace => "view.save-workspace",
            ViewAction::ResetWorkspace => "view.reset-workspace",
            ViewAction::ToggleViewportChrome => "view.toggle-viewport-chrome",
        }
    }

    /// What a menu shows.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            ViewAction::CommandPalette => "Command Palette",
            ViewAction::ToggleDensity => "Density",
            ViewAction::ToggleTheme => "Theme",
            ViewAction::ToggleVision => "Colour Palette",
            ViewAction::ScaleUp => "Larger Interface",
            ViewAction::ScaleDown => "Smaller Interface",
            ViewAction::SaveWorkspace => "Save Workspace",
            ViewAction::ResetWorkspace => "Reset Workspace",
            ViewAction::ToggleViewportChrome => "Viewport Chrome",
        }
    }

    /// What it does, in one line.
    #[must_use]
    pub const fn description(self) -> &'static str {
        match self {
            ViewAction::CommandPalette => "Find and run any command by typing part of its name.",
            ViewAction::ToggleDensity => {
                "Switch between compact and comfortable row heights and spacing."
            }
            ViewAction::ToggleTheme => "Switch between the dark and light themes.",
            ViewAction::ToggleVision => {
                "Switch between the standard palette and the one whose status hues stay apart \
                 without red-green discrimination."
            }
            ViewAction::ScaleUp => "Increase the interface scale.",
            ViewAction::ScaleDown => "Decrease the interface scale.",
            ViewAction::SaveWorkspace => {
                "Save the current panel arrangement under the workspace in force."
            }
            ViewAction::ResetWorkspace => {
                "Return every panel to the shipped arrangement. Nothing in the project changes."
            }
            ViewAction::ToggleViewportChrome => {
                "Show or hide the controls that float over the viewport."
            }
        }
    }

    /// The binding it ships with.
    ///
    /// Two of them are chords, deliberately: `editor-ui-ux` requires chords to work, and a chord
    /// that nothing uses is a chord nobody would notice was broken.
    #[must_use]
    pub const fn binding(self) -> &'static str {
        match self {
            ViewAction::CommandPalette => "Ctrl+P",
            ViewAction::ToggleDensity => "Ctrl+Shift+D",
            ViewAction::ToggleTheme => "Ctrl+Shift+L",
            ViewAction::ToggleVision => "Ctrl+Shift+C",
            ViewAction::ScaleUp => "Ctrl+Equals",
            ViewAction::ScaleDown => "Ctrl+Minus",
            ViewAction::SaveWorkspace => "Ctrl+K Ctrl+S",
            ViewAction::ResetWorkspace => "Ctrl+K Ctrl+R",
            ViewAction::ToggleViewportChrome => "F11",
        }
    }

    /// The action by identifier, or `None` if the identifier is a registry command's.
    #[must_use]
    pub fn of_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|action| action.id() == id)
    }

    /// The palette entry for it, so that a view action is found by typing like anything else.
    #[must_use]
    pub fn entry(self) -> Entry {
        Entry::new(
            self.label(),
            self.description(),
            Origin::Setting,
            Action::OpenSetting(self.id().to_string()),
        )
    }
}

/// The registry command a transform mode invokes.
///
/// The identifiers are the ones `cy_editor_interface::keymap::Keymap::unity` already names, so the
/// Unity preset's `W`/`E`/`R` and this table cannot disagree. They are not registered in this
/// milestone — the viewport's interactive path is task 2.4 — and the toolbar says so rather than
/// offering four buttons that fail.
#[must_use]
pub const fn transform_command(mode: GizmoMode) -> &'static str {
    match mode {
        GizmoMode::Translate => "viewport.transform-mode-move",
        GizmoMode::Rotate => "viewport.transform-mode-rotate",
        GizmoMode::Scale => "viewport.transform-mode-scale",
        GizmoMode::Universal => "viewport.transform-mode-universal",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_interface::keymap::{GLOBAL, Keymap};

    #[test]
    fn every_view_action_has_a_distinct_identifier_label_and_binding() {
        let mut ids: Vec<&str> = ViewAction::ALL.iter().map(|action| action.id()).collect();
        ids.sort_unstable();
        let count = ids.len();
        ids.dedup();
        assert_eq!(ids.len(), count, "two view actions share an identifier");

        for action in ViewAction::ALL {
            assert_eq!(ViewAction::of_id(action.id()), Some(action));
            assert!(!action.label().is_empty());
            assert!(
                action.description().ends_with('.'),
                "{} has no descriptive sentence",
                action.id()
            );
        }
    }

    #[test]
    fn the_shipped_bindings_do_not_conflict_with_each_other() {
        // `Keymap::bind` refuses a conflict, including a chord whose prefix is already bound — which
        // is the failure that makes a chord silently unreachable. Binding all nine here is the check.
        let mut keymap = Keymap::new("view");
        for action in ViewAction::ALL {
            keymap
                .bind(GLOBAL, action.binding(), action.id())
                .unwrap_or_else(|problem| panic!("{}: {problem}", action.id()));
        }
        assert_eq!(keymap.len(), ViewAction::ALL.len());
    }

    #[test]
    fn every_view_action_is_reachable_by_typing() {
        // "Every action SHALL be reachable from the keyboard" has two halves, and this is the one a
        // binding cannot satisfy: a user who does not know the key finds it in the palette.
        let mut index = cy_editor_interface::palette::Index::new();
        index.ingest(ViewAction::ALL.map(ViewAction::entry));
        for action in ViewAction::ALL {
            let found = index.search(action.label(), 8);
            assert!(
                found.iter().any(|hit| hit.entry.label == action.label()),
                "{} is not findable by its own label",
                action.id()
            );
        }
    }
}
