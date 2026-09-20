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
    /// What a person reads on the row.
    ///
    /// The node's **author-given name** where it has one, and its **kind** where it has not — the
    /// name of the first component it carries that has data, falling back to `Node`.
    /// `editor-documents-and-transactions` requires exactly that fallback: "the outliner SHALL
    /// label a row with it, falling back to the node's kind only where a node has no name".
    ///
    /// The kind half is the same definition
    /// [`SelectionSummary`](cy_editor_interface::SelectionSummary) uses, so an unnamed node reads
    /// the same word in the outliner and in the inspector's header.
    pub label: String,
    /// Whether [`HierarchyRow::label`] is the node's own name rather than its kind.
    ///
    /// Two rows may legitimately read the same *kind*; two rows reading the same *name* is the
    /// defect the requirement's first scenario names, and a check cannot tell them apart from the
    /// label alone.
    pub named: bool,
}

/// How a hierarchy selection intent combines with the current selection.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SelectionIntent {
    /// Replace with one node.
    Replace,
    /// Add one node.
    Add,
    /// Remove one node.
    Subtract,
    /// Select the visible range from the anchor to one node.
    VisibleRange,
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
    selection_anchor: Option<NodeId>,
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

    /// Whether a node is expanded.
    ///
    /// A panel needs it to draw the disclosure and to know what a click on one means, and deriving
    /// it from the rows — "the next row is deeper" — is wrong the moment a filter flattens them.
    #[must_use]
    pub fn is_expanded(&self, node: NodeId) -> bool {
        self.expanded.contains(&node)
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

    /// The filter in force.
    #[must_use]
    pub fn filter(&self) -> &str {
        &self.filter
    }

    /// Set the name filter. Presentation state, like expansion.
    pub fn set_filter(&mut self, filter: impl Into<String>) {
        self.filter = filter.into();
        self.document_watch = Watch::new();
    }

    /// Select a node. **Writes to the selection service and notifies nobody.**
    pub fn select(&mut self, editor: &mut Editor, node: NodeId, intent: SelectionIntent) {
        let mut selection = editor.selection.get().clone();
        match intent {
            SelectionIntent::Replace => {
                selection = Selection::new();
                selection.add_node(node);
                self.selection_anchor = Some(node);
            }
            SelectionIntent::Add => {
                selection.add_node(node);
                self.selection_anchor = Some(node);
            }
            SelectionIntent::Subtract => {
                selection.remove_node(node);
                self.selection_anchor = Some(node);
            }
            SelectionIntent::VisibleRange => {
                let anchor = self.selection_anchor.unwrap_or(node);
                let first = self.rows.iter().position(|row| row.node == anchor);
                let last = self.rows.iter().position(|row| row.node == node);
                if let (Some(first), Some(last)) = (first, last) {
                    let range = first.min(last)..=first.max(last);
                    selection.set_nodes(self.rows[range].iter().map(|row| row.node));
                } else {
                    selection.set_nodes([node]);
                }
            }
        }
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

        let filter = self.filter.trim().to_lowercase();
        while let Some((node, depth)) = stack.pop() {
            let Some(state) = document.content().node(node) else {
                continue;
            };
            let label = label_of(document, state);
            let named = !state.name.is_empty();
            // A filtered outliner shows the matches, flattened. Keeping the depth would draw
            // indentation against ancestors that are not on screen, which reads as a broken tree
            // rather than as a filtered one; and expansion is left untouched so clearing the filter
            // restores exactly the tree the user had.
            let matched = filter.is_empty() || label.to_lowercase().contains(&filter);
            if matched {
                rows.push(HierarchyRow {
                    node,
                    depth: if filter.is_empty() { depth } else { 0 },
                    selected: selection.nodes().any(|selected| selected == node),
                    has_children: !state.children.is_empty(),
                    label,
                    named,
                });
            }
            // A filter searches the whole tree, not the part that happens to be expanded — an
            // outliner that only finds what is already visible finds nothing worth finding.
            if !filter.is_empty() || self.expanded.contains(&node) {
                for child in state.children.iter().rev() {
                    stack.push((*child, depth + 1));
                }
            }
        }
        rows
    }
}

/// The node's name, or — where it has none — its kind: the first component it carries that has
/// fields, or `Node`.
fn label_of(document: &Document, state: &cy_editor_documents::content::NodeState) -> String {
    if !state.name.is_empty() {
        return state.name.clone();
    }
    state
        .components
        .keys()
        .filter_map(|component| document.schema().type_of(*component))
        .find(|definition| !definition.fields.is_empty())
        .map_or_else(|| "Node".to_string(), |definition| definition.name.clone())
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
    fn two_nodes_of_one_kind_are_distinguishable_in_the_outliner() {
        // "WHEN an author creates two nodes carrying the same first component and names them
        // differently THEN the hierarchy SHALL show two rows bearing the two names, and a check
        // SHALL fail if two distinct nodes are indistinguishable in the outliner."
        //
        // THE DEFECT THIS CATCHES SHIPPED. M8.a's own screenshot of the editor showed two authored
        // objects as two outliner rows BOTH READING `Transform`, because the label was the node's
        // kind and the document model had nowhere to put a name.
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let document = editor.documents.get_mut(id).unwrap();
        let transform = document.schema_mut().declare_type("Transform", false);
        let translation = document
            .schema_mut()
            .declare_field(
                transform,
                "translation",
                cy_editor_core::value::ValueKind::Vec3,
                "where it is",
            )
            .unwrap();
        document
            .with_transaction("Build", Actor::human("designer"), |document| {
                for name in ["Pillar", "Crate"] {
                    let node = document.create_node(None)?;
                    document.add_component(
                        node,
                        transform,
                        vec![(translation, cy_editor_core::value::Value::Vec3([0.0; 3]))],
                    )?;
                    document.set_name(node, name)?;
                }
                Ok(())
            })
            .unwrap();

        let mut hierarchy = HierarchyViewModel::new();
        hierarchy.refresh(&editor);
        let labels: Vec<&str> = hierarchy
            .rows()
            .iter()
            .map(|row| row.label.as_str())
            .collect();
        assert_eq!(labels, ["Pillar", "Crate"]);
        assert!(hierarchy.rows().iter().all(|row| row.named));

        // The check the scenario asks for, stated as one assertion: no two rows read the same word.
        let distinct: std::collections::BTreeSet<&str> = labels.iter().copied().collect();
        assert_eq!(
            distinct.len(),
            labels.len(),
            "two distinct nodes are indistinguishable in the outliner"
        );
    }

    #[test]
    fn an_unnamed_node_falls_back_to_its_kind() {
        // "falling back to the node's kind only where a node has no name" — the fallback is the
        // requirement's, not a convenience, because an unnamed node reading as an identifier is
        // worse than one reading as what it is.
        let (editor, ..) = editor_with_a_tree();
        let mut hierarchy = HierarchyViewModel::new();
        hierarchy.refresh(&editor);
        assert_eq!(hierarchy.rows()[0].label, "Node");
        assert!(!hierarchy.rows()[0].named);
    }

    #[test]
    fn the_search_filter_actually_filters_and_searches_the_whole_tree() {
        // A regression test, and the defect it catches was real: `set_filter` stored a string that
        // `build` never read, so the outliner's permanent search field would have been a control
        // that does nothing — the worst kind, because it looks like it works.
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let document = editor.documents.get_mut(id).unwrap();
        let light = document.schema_mut().declare_type("Light", false);
        let intensity = document
            .schema_mut()
            .declare_field(
                light,
                "intensity",
                cy_editor_core::value::ValueKind::Float,
                "how bright it is",
            )
            .unwrap();
        let (root, child) = document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let root = document.create_node(None)?;
                let child = document.create_node(Some(root))?;
                document.add_component(
                    child,
                    light,
                    vec![(intensity, cy_editor_core::value::Value::Float(1.0))],
                )?;
                Ok((root, child))
            })
            .unwrap();

        let mut hierarchy = HierarchyViewModel::new();
        hierarchy.refresh(&editor);
        // Collapsed: only the root is on screen, and it is not a Light.
        assert_eq!(hierarchy.rows().len(), 1);
        assert_eq!(hierarchy.rows()[0].node, root);
        assert_eq!(hierarchy.rows()[0].label, "Node");

        // Filtering finds the child even though its parent is collapsed, which is the whole point
        // of a search field in an outliner of thousands of entities.
        hierarchy.set_filter("light");
        hierarchy.refresh(&editor);
        assert_eq!(hierarchy.rows().len(), 1);
        assert_eq!(hierarchy.rows()[0].node, child);
        assert_eq!(hierarchy.rows()[0].label, "Light");
        assert_eq!(hierarchy.rows()[0].depth, 0, "a filtered list is flat");

        // Clearing it restores exactly the tree that was there before: expansion was never touched.
        hierarchy.set_filter("");
        hierarchy.refresh(&editor);
        assert_eq!(hierarchy.rows().len(), 1);
        assert_eq!(hierarchy.rows()[0].node, root);
    }

    #[test]
    fn selection_reaches_another_panel_through_the_service_and_not_through_a_call() {
        let (mut editor, root, _) = editor_with_a_tree();
        let mut hierarchy = HierarchyViewModel::new();
        let mut inspector = InspectorViewModel::new();
        inspector.refresh(&editor);
        let before = inspector.rebuilds();

        // The hierarchy writes to the service. It holds no reference to the inspector and could
        // not call it if it wanted to — this crate's view models name each other nowhere.
        hierarchy.select(&mut editor, root, SelectionIntent::Replace);

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

        first.select(&mut editor, root, SelectionIntent::Replace);
        first.refresh(&editor);
        second.refresh(&editor);

        assert!(first.rows()[0].selected);
        assert!(
            second.rows()[0].selected,
            "neither panel has its own idea of the selection"
        );
    }

    #[test]
    fn replace_add_subtract_and_visible_range_use_node_identities() {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let nodes = editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Build", Actor::human("designer"), |document| {
                (0..4)
                    .map(|index| {
                        let node = document.create_node(None)?;
                        document.set_name(node, format!("Item {index}"))?;
                        Ok(node)
                    })
                    .collect::<cy_editor_core::problem::Result<Vec<_>>>()
            })
            .unwrap();
        let mut hierarchy = HierarchyViewModel::new();
        hierarchy.set_filter("Item");
        hierarchy.refresh(&editor);

        hierarchy.select(&mut editor, nodes[0], SelectionIntent::Replace);
        hierarchy.select(&mut editor, nodes[2], SelectionIntent::Add);
        assert_eq!(editor.selection.get().node_count(), 2);
        hierarchy.select(&mut editor, nodes[0], SelectionIntent::Subtract);
        assert_eq!(
            editor.selection.get().nodes().collect::<Vec<_>>(),
            vec![nodes[2]]
        );
        hierarchy.select(&mut editor, nodes[2], SelectionIntent::Replace);
        hierarchy.select(&mut editor, nodes[3], SelectionIntent::VisibleRange);
        assert_eq!(
            editor.selection.get().nodes().collect::<Vec<_>>(),
            vec![nodes[2], nodes[3]],
            "the visible range converted rows back to stable identities"
        );
    }
}
