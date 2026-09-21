// SPDX-License-Identifier: MIT
//! Open-document tabs: derived labels and intents over the workspace and document services.
//!
//! A tab owns no document state. Its identity, dirty marker, and active cue are rebuilt from the
//! services; activating or closing raises an intent back to [`Editor`]. This is the same separation
//! as every other view model and is what makes switching tabs incapable of dirtying content.

use cy_editor_core::ids::DocumentId;
use cy_editor_core::observe::Revision;
use cy_editor_core::problem::Result;
use cy_editor_services::{CloseDecision, CloseOutcome, Editor};

/// One visible document tab.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct DocumentTab {
    /// Stable document identity used by intents.
    pub document: DocumentId,
    /// The primary asset's filename, or the asset path when it has no filename.
    pub label: String,
    /// Whether the document differs from its last save.
    pub dirty: bool,
    /// Whether commands with no explicit target act on this document.
    pub active: bool,
}

/// The input signature needed to avoid rebuilding an idle tab strip.
#[derive(Clone, PartialEq, Eq, Debug)]
struct Input {
    document: DocumentId,
    revision: Revision,
    dirty: bool,
    active: bool,
}

/// Presentation state and intents for the open-document strip.
#[derive(Debug, Default)]
pub struct DocumentTabsViewModel {
    tabs: Vec<DocumentTab>,
    inputs: Vec<Input>,
    rebuilds: u64,
}

impl DocumentTabsViewModel {
    /// An empty tab strip.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild when the open order, active document, content revision, or dirty state changes.
    pub fn refresh(&mut self, editor: &Editor) -> bool {
        let active = editor.workspace.active();
        let inputs: Vec<Input> = editor
            .workspace
            .open_documents()
            .iter()
            .filter_map(|id| {
                editor.documents.get(*id).map(|document| Input {
                    document: *id,
                    revision: document.revision(),
                    dirty: document.is_dirty(),
                    active: active == Some(*id),
                })
            })
            .collect();
        if inputs == self.inputs {
            return false;
        }

        self.tabs = inputs
            .iter()
            .filter_map(|input| {
                let document = editor.documents.get(input.document)?;
                let asset = document.assets().first()?;
                let label = std::path::Path::new(asset)
                    .file_name()
                    .and_then(|name| name.to_str())
                    .unwrap_or(asset)
                    .to_string();
                Some(DocumentTab {
                    document: input.document,
                    label,
                    dirty: input.dirty,
                    active: input.active,
                })
            })
            .collect();
        self.inputs = inputs;
        self.rebuilds += 1;
        true
    }

    /// Tabs in workspace order.
    #[must_use]
    pub fn tabs(&self) -> &[DocumentTab] {
        &self.tabs
    }

    /// How many times derived rows changed.
    #[must_use]
    pub const fn rebuilds(&self) -> u64 {
        self.rebuilds
    }

    /// Activate a document without changing document state.
    pub fn activate(&self, editor: &mut Editor, document: DocumentId) -> Result<()> {
        if editor.documents.get(document).is_none() {
            return Err(cy_editor_core::problem::Problem::not_found("that document"));
        }
        editor.workspace.activate(document);
        Ok(())
    }

    /// Ask the authoritative editor to close a document.
    pub fn close(
        &self,
        editor: &mut Editor,
        document: DocumentId,
        decision: Option<CloseDecision>,
    ) -> Result<CloseOutcome> {
        editor.close_document(document, decision)
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;

    use super::*;

    #[test]
    fn tabs_follow_workspace_order_active_state_and_dirty_state() {
        let mut editor = Editor::default();
        let city = editor.open_document("worlds/city.cyworld").unwrap();
        let forest = editor.open_document("worlds/forest.cyworld").unwrap();
        let mut tabs = DocumentTabsViewModel::new();

        assert!(tabs.refresh(&editor));
        assert_eq!(tabs.tabs().len(), 2);
        assert_eq!(tabs.tabs()[0].label, "city.cyworld");
        assert!(!tabs.tabs()[0].active);
        assert!(tabs.tabs()[1].active);
        assert!(!tabs.refresh(&editor), "an idle strip does not rebuild");

        editor
            .documents
            .get_mut(city)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();
        assert!(tabs.refresh(&editor));
        assert!(tabs.tabs()[0].dirty);

        tabs.activate(&mut editor, city).unwrap();
        assert!(tabs.refresh(&editor));
        assert!(tabs.tabs()[0].active);
        assert!(!tabs.tabs()[1].active);
        assert_eq!(editor.workspace.active(), Some(city));
        assert_eq!(
            editor
                .documents
                .get(city)
                .unwrap()
                .history()
                .entries()
                .len(),
            1
        );
        assert!(!editor.documents.get(forest).unwrap().is_dirty());
    }

    #[test]
    fn close_intents_preserve_dirty_content_until_the_choice_is_explicit() {
        let mut editor = Editor::default();
        let city = editor.open_document("worlds/city.cyworld").unwrap();
        editor
            .documents
            .get_mut(city)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();
        let tabs = DocumentTabsViewModel::new();

        assert_eq!(
            tabs.close(&mut editor, city, None).unwrap(),
            CloseOutcome::NeedsDecision
        );
        assert!(editor.documents.get(city).unwrap().is_dirty());
        assert_eq!(
            tabs.close(&mut editor, city, Some(CloseDecision::Cancel))
                .unwrap(),
            CloseOutcome::Cancelled
        );
        assert!(editor.documents.get(city).is_some());
    }
}
