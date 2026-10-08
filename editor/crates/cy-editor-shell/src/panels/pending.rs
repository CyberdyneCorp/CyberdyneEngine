//! Panels whose capability arrives in a later milestone.
//!
//! The default workspace has a Script Graph tab beside the viewport, because the composition
//! `editor-visual-language` fixes has a specialised-editor region and leaving it empty would teach
//! the wrong default. What such a panel must not do is *pretend*. (The Animation tab beside it was
//! one until the animation editor arrived, issue #29; it opens that editor now.)
//!
//! `design.md` §6, "What M5.5 deliberately does not do": **no visual scripting** — graphs are M8,
//! and an agent writes Swift, which is what M4 delivered. So the Script Graph panel says which
//! milestone it arrives in and what to use instead. A panel drawing an empty graph canvas with a
//! grid and an "Add node" button would be indistinguishable from a working one that had nothing in
//! it, and the first bug report would be "adding a node does nothing".
//!
//! This is the same rule the viewport follows for a transport that is not ready, and the same reason:
//! an editor that shows an approximation of a capability it does not have is an editor whose
//! screenshots cannot be trusted.

use super::{Panels, nothing_here};

/// Draw a panel whose capability has not arrived, naming the milestone that brings it.
pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui, kind: &str) {
    let (what, remedy) = match kind {
        "script-graph" => (
            "Visual scripting arrives at M8.",
            "Gameplay is authored in Swift today: write a script into the project and reload it. \
             The graph panel is docked here so the workspace does not change shape when it lands.",
        ),
        _ => (
            "This panel is not part of this build.",
            "It may belong to a plugin that is not loaded, or to a saved workspace made by a newer \
             editor.",
        ),
    };
    nothing_here(ui, panels.shell, what, remedy);
}
