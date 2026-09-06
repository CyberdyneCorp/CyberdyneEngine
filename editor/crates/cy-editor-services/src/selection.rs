//! Selection, as a service five panels read and none of them owns.
//!
//! "WHEN an entity is selected in the hierarchy THEN the inspector, viewport, properties, and status
//! bar SHALL observe the selection service, and the hierarchy SHALL not notify them."
//!
//! The hierarchy panel *cannot* notify them: it has no reference to any of them, because
//! `editor-rust-application` forbids a view model from depending on another panel's view model and
//! this crate is what replaces that dependency. What the hierarchy does is write here; what the
//! other four do is compare a revision.

use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;

/// The editor's selection.
#[derive(Debug, Default)]
pub struct SelectionService {
    selection: Versioned<Selection>,
}

impl SelectionService {
    /// An empty selection.
    #[must_use]
    pub fn new() -> Self {
        Self {
            selection: Versioned::new(Selection::new()),
        }
    }

    /// What is selected.
    #[must_use]
    pub fn get(&self) -> &Selection {
        self.selection.get()
    }

    /// The revision, for a view model's watch.
    #[must_use]
    pub fn revision(&self) -> Revision {
        self.selection.revision()
    }

    /// Replace the selection.
    pub fn set(&mut self, selection: Selection) {
        self.selection.set(selection);
    }

    /// Change the selection in place.
    pub fn update(&mut self, mutate: impl FnOnce(&mut Selection)) {
        self.selection.update(mutate);
    }

    /// Drop selected objects a reload removed, keeping the ones that survived.
    ///
    /// "WHEN a document is reloaded THEN selection SHALL be re-resolved by identity rather than lost
    /// or pointing at the wrong objects."
    pub fn reresolve(&mut self, document: &Document) {
        self.selection
            .update(|selection| selection.reresolve(document));
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::observe::Watch;

    use super::*;

    #[test]
    fn five_panels_learn_of_a_selection_through_one_revision() {
        let mut service = SelectionService::new();
        let panels: Vec<Watch> = (0..5).map(|_| Watch::new()).collect();
        for panel in &panels {
            panel.accept(service.revision());
        }

        service.update(|selection| selection.set_assets(["materials/brick.cymat".into()]));

        for panel in &panels {
            assert!(panel.changed(service.revision()), "every observer sees it");
            panel.accept(service.revision());
            assert!(!panel.changed(service.revision()), "and only once");
        }
    }
}
