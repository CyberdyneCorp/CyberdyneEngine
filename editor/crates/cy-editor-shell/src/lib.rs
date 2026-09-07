//! CyberEditor's window. M5.5, tasks 1.1 through 1.8.
//!
//! **The one crate in this workspace that may see an interface toolkit.** Everything below it is
//! specified to be testable with no window, no graphics device and no toolkit, and
//! `cy-editor-app/tests/containment.rs` derives which crate this is rather than naming it — so a
//! second render crate is a failing test rather than a review comment.
//!
//! | Module | What it draws |
//! |---|---|
//! | [`app`] | The window, the frame order, and the device the viewport shares |
//! | [`chrome`] | The header with the identity lockup, the toolbar, the footer |
//! | [`dock`] | `Layout` in, `egui_dock` out, and the user's arrangement back again |
//! | [`identity`] | The CyberEngine lockup and the application icon |
//! | [`keys`] | Toolkit key events as strokes, and a chord in progress |
//! | [`notify`] | Toasts that never take focus; modals for decisions and destruction |
//! | [`palette`] | The command palette over the registry's own index |
//! | [`panels`] | Hierarchy, generated inspector, content browser, viewport, diagnostics |
//! | [`theme`] | The visual language as an `egui::Style`. The only colour conversion there is |
//! | [`view`] | The window's own actions, which change no document |
//! | [`viewport_input`] | The toolkit's pointer and keys, as the viewport model's events |
//! | [`viewport_link`] | The engine's rendered image, or the sentence saying why there is none |
//!
//! --- THE SHIPPED DEFAULT WORKSPACE, DECIDED HERE (task 2.4.9) -------------------------------------
//!
//! `docs/design/` holds **three** reference layouts and they differ. `editor-visual-language` says
//! the composition requirement fixes *regions* rather than pixel positions, and all three satisfy
//! it — "but if one is meant to be the default, it should be said rather than left to whichever a
//! contributor opens first." So it is said:
//!
//! > **The shipped default is the arrangement of `editor-rts-desertfrontier.png`:** the outliner
//! > upper-left with the content browser beneath it, the viewport centre with its chrome overlaid,
//! > the specialised editor below the viewport, the inspector down the right, and the diagnostics
//! > tabs beneath the inspector.
//!
//! Three reasons, in the order they decided it.
//!
//! 1. It is the arrangement `cy_editor_visual::chrome::Composition::default()` already encodes and
//!    the one the "Default workspace" diagram in `docs/design/editor-visual-language.md` draws. The
//!    other two would have made the document disagree with the product, and a reference that no
//!    longer reflects the product has to be replaced rather than left to decay.
//! 2. `editor-adventure-ancientfrontier.png` is the same arrangement plus a vertical tool rail and a
//!    graph docked below the viewport. The rail is a second way to reach mode-level tools and it
//!    costs permanent width before there are enough modes to need it; the graph is M8. Shipping it
//!    would mean shipping two regions that are empty on the day the editor opens.
//! 3. `editor-scene-view.png` puts the hierarchy bottom-left and the project browser in the
//!    centre-lower region. That region is where a specialised editor goes — the script graph, the
//!    animation editor, the sequencer — so a browser living there means every specialised editor
//!    displaces the browser when it opens, which is a panel moving because of context. That is the
//!    one thing "content adapts; position does not" exists to prevent.
//!
//! `Layout::scene_editing` is that arrangement, and it derives its proportions from `Composition`
//! rather than restating them, so the default and the specification cannot drift.
//!
//! --- THE FOUR THINGS IN THE REFERENCES THAT ARE NOT COPIED (task 1.8) -----------------------------
//!
//! | The reference does | This editor does |
//! |---|---|
//! | Unreal's vocabulary — `actors`, `Static Mesh`, `Blueprint Log`, `Content Drawer` | Node and entity, mesh, script graph, **Content Browser**. Checked by [`tests::every_label_this_crate_can_draw_uses_the_engines_vocabulary`], not merely intended |
//! | A profiler in a panel labelled "World Partition" | A panel called **Profiler**, and no panel called World Partition. The ambient overlay answers *is this frame affordable*; the panel answers *why* |
//! | Two different headers across the two images | One identity, from `docs/design/images/cyberengine-logo.png`, drawn by [`identity`] |
//! | A universal gizmo at the edge of readability | Not drawn here at all; `editor-viewport-and-gizmos` owns it, and its readability rule is the gizmo model's |
//!
//! --- WHAT THIS CRATE DELIBERATELY DOES NOT DECIDE ---------------------------------------------------
//!
//! Which panels exist, where they start, what the palette finds and in what order, which key does
//! what, which rows the inspector generates, where a problem is shown, and what a notification may
//! interrupt. All of that is `cy-editor-interface`, finished and tested at M5 with no window. When a
//! question here cannot be answered by asking a model below, that is the signal that a decision has
//! leaked upward — and the fix is to move it back down, not to answer it here.

#![forbid(unsafe_code)]

pub mod app;
pub mod chrome;
pub mod dock;
pub mod identity;
pub mod keys;
pub mod notify;
pub mod palette;
pub mod panels;
pub mod theme;
pub mod view;
pub mod viewport_input;
pub mod viewport_link;

pub use app::{EditorWindow, run};
pub use identity::{PRODUCT, PUBLISHER};
pub use view::ViewAction;

#[cfg(test)]
mod tests {
    use cy_editor_interface::shell::{Shell, panel_title};
    use cy_editor_visual::vocabulary;

    use crate::view::ViewAction;

    /// Every static string this crate can put in front of a person.
    ///
    /// Collected by hand and asserted against the vocabulary table, which is the only honest way to
    /// do it without a compiler plugin — and it is worth doing because vocabulary is the thing the
    /// reference images get most wrong, and terminology spreads into every panel, document and
    /// tutorial before anybody notices.
    fn labels() -> Vec<String> {
        let mut labels: Vec<String> = Vec::new();
        labels.extend(crate::chrome::MENU_ORDER.iter().map(ToString::to_string));
        labels.push("View".to_string());
        labels.push("Help".to_string());
        for action in ViewAction::ALL {
            labels.push(action.label().to_string());
            labels.push(action.description().to_string());
        }
        for kind in [
            "hierarchy",
            "content-browser",
            "viewport",
            "inspector",
            "script-graph",
            "animation",
            "console",
            "profiler",
            "problems",
        ] {
            let panel = cy_editor_interface::PanelId::new(kind).expect("a built-in panel");
            labels.push(panel_title(&panel).to_string());
        }
        labels.extend(
            [
                "Search the scene",
                "Search content",
                "No entities in the active world.",
                "Create Entity (Ctrl+Shift+N), or open a world.",
                "No component types are described, so there is nothing to generate a form from.",
                "Nothing selected.",
                "The selection carries no described components.",
                "Select an entity in the Hierarchy.",
                "The open worlds name no assets.",
                "Open a world; its assets appear here.",
                "Nothing is wrong.",
                "The editor has said nothing yet.",
                "Interface frame",
                "Background work",
                "Runtime",
                "Visual scripting arrives at M8.",
                "Type a command, an asset, a node or a setting",
                "The transport is not ready: no runtime has offered a rendered frame.",
            ]
            .map(ToString::to_string),
        );
        labels
    }

    #[test]
    fn every_label_this_crate_can_draw_uses_the_engines_vocabulary() {
        for label in labels() {
            vocabulary::check_label(&label)
                .unwrap_or_else(|problem| panic!("{label:?}: {problem}"));
        }
    }

    #[test]
    fn no_panel_is_called_world_partition() {
        // The second thing the reference images get wrong, as a check rather than a comment. The
        // panel in the adventure mockup labelled "World Partition" contains a profiler, and world
        // partition is a streaming capability with entirely different concerns.
        for label in labels() {
            assert!(
                !label.to_lowercase().contains("world partition"),
                "{label:?} labels a panel with a streaming capability's name"
            );
        }
        assert!(
            labels().iter().any(|label| label == "Profiler"),
            "the panel that shows frame cost is called Profiler"
        );
    }

    #[test]
    fn the_shell_the_window_builds_agrees_with_the_specifications_composition() {
        // The default this crate's header commits to, checked against the table it claims to come
        // from. If `Composition::default()` ever stops making the viewport the largest region, the
        // shipped default has stopped being viewport-first and this says so.
        let mut registry = cy_editor_commands::Registry::new();
        cy_editor_services::builtin::register(&mut registry).unwrap();
        let shell = Shell::new(&registry).unwrap();
        assert_eq!(
            shell.composition().largest(),
            cy_editor_visual::chrome::Region::Centre,
            "the shipped default is not viewport-first"
        );
        let panels = shell.workspaces.current().panels();
        for expected in ["viewport", "hierarchy", "inspector", "content-browser"] {
            assert!(
                panels.iter().any(|panel| panel.as_str() == expected),
                "the shipped default workspace has no {expected}"
            );
        }
    }
}
