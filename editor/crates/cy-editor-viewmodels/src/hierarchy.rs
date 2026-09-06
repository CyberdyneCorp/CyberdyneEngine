//! The hierarchy: the document's nodes as rows, in authored order.
//!
//! It does not know the inspector exists. Selecting a row writes to the selection service, and the
//! inspector notices that a revision moved — which is `editor-rust-application`'s "WHEN an entity is
//! selected in the hierarchy THEN the inspector, viewport, properties, and status bar SHALL observe
//! the selection service, and the hierarchy SHALL not notify them", implemented by there being
//! nothing to notify with.

use cy_editor_core::ids::NodeId;
use cy_editor_core::observe::{Revision, Watch};
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_services::Editor;

/// One row in the hierarchy.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct HierarchyRow {
    /// The node.
    pub node: NodeId,
    /// How deep it is, for indentation.
    pub depth: usize,
    /// Whether it is selected. Derived from the selection service, never cached authoritatively.
    pub selected: bool,
    /// Whether it has children, so a disclosure triangle can be drawn.
    pub has_children: bool,
}

/// The hierarchy panel's presentation state.
#[derive(Debug, Default)]
pub struct HierarchyViewModel {
    rows: Vec<HierarchyRow>,
    /// Which nodes are expanded. **Presentation state**: it belongs to the workspace when persisted
    /// and never to the document, so expanding a node cannot dirty anything.
    expanded: Vec<NodeId>,
    filter: String,
    document_watch: Watch,
    selection_watch: Watch,
    rebuilds: u64,
}

impl HierarchyViewModel {
    /// An empty hierarchy.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild only if the document or the selection moved.
    pub fn refresh(&mut self, editor: &Editor) -> bool {
        let document_revision = editor
            .workspace
            .active()
            .and_then(|id| editor.documents.get(id))
            .map_or(Revision::INITIAL, Document::revision);

        let moved = self.document_watch.changed(document_revision)
            || self.selection_watch.changed(editor.selection.revision());
        if !moved {
            return false;
        }

        self.rows = self.build(editor);
        self.document_watch.accept(document_revision);
        self.selection_watch.accept(editor.selection.revision());
        self.rebuilds += 1;
        true
    }

    /// The rows to render.
    #[must_use]
    pub fn rows(&self) -> &[HierarchyRow] {
        &self.rows
    }

    /// How many times this panel has rebuilt.
    #[must_use]
    pub const fn rebuilds(&self) -> u64 {
        self.rebuilds
    }

    /// Expand or collapse a node. Presentation state; nothing is written to the document.
    pub fn set_expanded(&mut self, node: NodeId, expanded: bool) {
        if expanded {
            if !self.expanded.contains(&node) {
                self.expanded.push(node);
            }
        } else {
            self.expanded.retain(|held| *held != node);
        }
        // The rows change shape, so the next refresh must rebuild even though no revision moved.
        self.document_watch = Watch::new();
    }

    /// Set the name filter. Presentation state, like expansion.
    pub fn set_filter(&mut self, filter: impl Into<String>) {
        self.filter = filter.into();
        self.document_watch = Watch::new();
    }

    /// Select a node. **Writes to the selection service and notifies nobody.**
    pub fn select(&self, editor: &mut Editor, node: NodeId) {
        let mut selection = Selection::new();
        selection.add_node(node);
        editor.selection.set(selection);
    }

    fn build(&self, editor: &Editor) -> Vec<HierarchyRow> {
        let Some(id) = editor.workspace.active() else {
            return Vec::new();
        };
        let Some(document) = editor.documents.get(id) else {
            return Vec::new();
        };
        let selection = editor.selection.get();

        let mut rows = Vec::new();
        let mut stack: Vec<(NodeId, usize)> = document
            .content()
            .roots()
            .iter()
            .rev()
            .map(|node| (*node, 0))
            .collect();

        while let Some((node, depth)) = stack.pop() {
            let Some(state) = document.content().node(node) else {
                continue;
            };
            rows.push(HierarchyRow {
                node,
                depth,
                selected: selection.nodes().any(|selected| selected == node),
                has_children: !state.children.is_empty(),
            });
            if self.expanded.contains(&node) {
                for child in state.children.iter().rev() {
                    stack.push((*child, depth + 1));
                }
            }
        }
        rows
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;

    use super::*;
    use crate::inspector::InspectorViewModel;

    fn editor_with_a_tree() -> (Editor, NodeId, NodeId) {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let document = editor.documents.get_mut(id).unwrap();
        let (root, child) = document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let root = document.create_node(None)?;
                let child = document.create_node(Some(root))?;
                Ok((root, child))
            })
            .unwrap();
        (editor, root, child)
    }

    #[test]
    fn selection_reaches_another_panel_through_the_service_and_not_through_a_call() {
        let (mut editor, root, _) = editor_with_a_tree();
        let hierarchy = HierarchyViewModel::new();
        let mut inspector = InspectorViewModel::new();
        inspector.refresh(&editor);
        let before = inspector.rebuilds();

        // The hierarchy writes to the service. It holds no reference to the inspector and could
        // not call it if it wanted to — this crate's view models name each other nowhere.
        hierarchy.select(&mut editor, root);

        assert!(
            inspector.refresh(&editor),
            "the inspector noticed by watching a revision"
        );
        assert_eq!(inspector.rebuilds(), before + 1);
    }

    #[test]
    fn expanding_a_node_is_presentation_state_and_dirties_nothing() {
        let (mut editor, root, child) = editor_with_a_tree();
        let id = editor.workspace.active().unwrap();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .save(|_| Ok(()))
            .unwrap();
        let revision = editor.documents.get(id).unwrap().revision();

        let mut hierarchy = HierarchyViewModel::new();
        hierarchy.refresh(&editor);
        assert_eq!(hierarchy.rows().len(), 1, "a collapsed root shows one row");

        hierarchy.set_expanded(root, true);
        hierarchy.refresh(&editor);
        assert_eq!(hierarchy.rows().len(), 2);
        assert_eq!(hierarchy.rows()[1].node, child);
        assert_eq!(hierarchy.rows()[1].depth, 1);

        assert!(!editor.documents.get(id).unwrap().is_dirty());
        assert_eq!(editor.documents.get(id).unwrap().revision(), revision);
    }

    #[test]
    fn two_panels_showing_the_same_thing_derive_it_from_the_same_model() {
        // "WHEN the same value is shown in two panels THEN both SHALL derive it from the same
        // model, and neither SHALL cache it authoritatively."
        let (mut editor, root, _) = editor_with_a_tree();
        let mut first = HierarchyViewModel::new();
        let mut second = HierarchyViewModel::new();
        first.refresh(&editor);
        second.refresh(&editor);

        first.select(&mut editor, root);
        first.refresh(&editor);
        second.refresh(&editor);

        assert!(first.rows()[0].selected);
        assert!(
            second.rows()[0].selected,
            "neither panel has its own idea of the selection"
        );
    }
}
