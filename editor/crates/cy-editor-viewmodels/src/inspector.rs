//! The inspector: what the selection's properties are, and what the user is in the middle of typing.

use cy_editor_commands::CommandContext;
use cy_editor_core::ids::{FieldId, TypeId};
use cy_editor_core::observe::{Revision, Watch};
use cy_editor_core::problem::Result;
use cy_editor_core::value::Value;
use cy_editor_documents::selection::CommonValue;
use cy_editor_services::Editor;

/// One row the inspector shows.
#[derive(Clone, PartialEq, Debug)]
pub struct InspectorRow {
    /// The component the field belongs to.
    pub component: TypeId,
    /// The field.
    pub field: FieldId,
    /// The field's name as the schema names it *now* — a rename shows here and moves no identity.
    pub name: String,
    /// What it means, for a tooltip and for an agent.
    pub description: String,
    /// The committed value across the selection.
    pub committed: CommonValue,
    /// Whether the schema allows writing it.
    pub writable: bool,
}

/// The inspector's presentation state.
#[derive(Debug, Default)]
pub struct InspectorViewModel {
    rows: Vec<InspectorRow>,
    /// What the user is typing, before it is committed. **Not document state**; see the crate note.
    editing: Option<(TypeId, FieldId, Value)>,
    selection_watch: Watch,
    document_watch: Watch,
    rebuilds: u64,
}

impl InspectorViewModel {
    /// An inspector showing nothing.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild only if the selection or the active document moved.
    ///
    /// Returns whether it rebuilt, which is what the test for "an idle inspector costs nothing"
    /// counts. `editor-rust-application`: "WHEN the selection and its values are unchanged THEN the
    /// inspector SHALL perform no engine queries."
    pub fn refresh(&mut self, editor: &Editor) -> bool {
        let document_revision = editor
            .workspace
            .active()
            .and_then(|id| editor.documents.get(id))
            .map_or(Revision::INITIAL, cy_editor_documents::Document::revision);

        let moved = self.selection_watch.changed(editor.selection.revision())
            || self.document_watch.changed(document_revision);
        if !moved {
            return false;
        }

        self.rows = build_rows(editor);
        self.selection_watch.accept(editor.selection.revision());
        self.document_watch.accept(document_revision);
        self.rebuilds += 1;
        true
    }

    /// The rows to render.
    #[must_use]
    pub fn rows(&self) -> &[InspectorRow] {
        &self.rows
    }

    /// How many times this inspector has rebuilt. What a cost test asserts on.
    #[must_use]
    pub const fn rebuilds(&self) -> u64 {
        self.rebuilds
    }

    /// Begin editing a field. Nothing is written; the buffer is presentation state.
    pub fn begin_edit(&mut self, component: TypeId, field: FieldId, value: Value) {
        self.editing = Some((component, field, value));
    }

    /// The in-progress edit, which is never document state.
    #[must_use]
    pub fn editing(&self) -> Option<(TypeId, FieldId, &Value)> {
        self.editing
            .as_ref()
            .map(|(component, field, value)| (*component, *field, value))
    }

    /// Abandon the in-progress edit.
    ///
    /// "WHEN a field is edited and focus is lost without committing THEN the document SHALL be
    /// unchanged and no transaction SHALL exist." The implementation is one line because there was
    /// never anything to undo.
    pub fn abandon_edit(&mut self) {
        self.editing = None;
    }

    /// Commit the in-progress edit as **one transaction** across the whole selection.
    ///
    /// The view model raises the intent and the services perform it; nothing here writes to a
    /// document directly, which is `editor-rust-application`'s "WHEN a property field is edited THEN
    /// the view SHALL raise an intent, the view model SHALL invoke a command, and the change SHALL
    /// become a transaction — with no path from the widget to engine state".
    pub fn commit_edit(&mut self, editor: &mut Editor) -> Result<usize> {
        let Some((component, field, value)) = self.editing.take() else {
            return Ok(0);
        };
        let Some(id) = editor.workspace.active() else {
            return Ok(0);
        };
        let actor = editor.actor();
        let selection = editor.selection.get().clone();
        let Some(document) = editor.documents.get_mut(id) else {
            return Ok(0);
        };
        let changed = selection.set_field_on_all(
            document,
            "Set property",
            actor,
            component,
            field,
            &value,
        )?;
        Ok(changed)
    }
}

/// The rows for the current selection: common components, and each of their fields.
fn build_rows(editor: &Editor) -> Vec<InspectorRow> {
    let Some(id) = editor.workspace.active() else {
        return Vec::new();
    };
    let Some(document) = editor.documents.get(id) else {
        return Vec::new();
    };
    let selection = editor.selection.get();

    let mut rows = Vec::new();
    for component in selection.common_components(document) {
        let Some(definition) = document.schema().type_of(component) else {
            continue;
        };
        for field in &definition.fields {
            rows.push(InspectorRow {
                component,
                field: field.id,
                name: field.name.clone(),
                description: field.description.clone(),
                committed: selection.common_value(document, component, field.id),
                writable: field.writable,
            });
        }
    }
    rows
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::value::ValueKind;
    use cy_editor_documents::selection::Selection;

    use super::*;

    fn editor_with_two_lamps() -> (Editor, TypeId, FieldId) {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let document = editor.documents.get_mut(id).unwrap();
        let health = document.schema_mut().declare_type("Health", false);
        let value = document
            .schema_mut()
            .declare_field(health, "value", ValueKind::Float, "hit points remaining")
            .unwrap();

        let nodes = document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let mut nodes = Vec::new();
                for _ in 0..2 {
                    let node = document.create_node(None)?;
                    document.add_component(node, health, vec![(value, Value::Float(100.0))])?;
                    nodes.push(node);
                }
                Ok(nodes)
            })
            .unwrap();

        let mut selection = Selection::new();
        selection.set_nodes(nodes);
        editor.selection.set(selection);
        (editor, health, value)
    }

    #[test]
    fn an_idle_inspector_costs_nothing() {
        let (editor, ..) = editor_with_two_lamps();
        let mut inspector = InspectorViewModel::new();
        assert!(inspector.refresh(&editor), "the first look rebuilds");

        for _ in 0..1000 {
            assert!(
                !inspector.refresh(&editor),
                "nothing moved, so nothing runs"
            );
        }
        assert_eq!(inspector.rebuilds(), 1);
    }

    #[test]
    fn an_abandoned_edit_changes_nothing() {
        let (editor, health, value) = editor_with_two_lamps();
        let id = editor.workspace.active().unwrap();
        let revision = editor.documents.get(id).unwrap().revision();
        let entries = editor.documents.get(id).unwrap().history().entries().len();

        let mut inspector = InspectorViewModel::new();
        inspector.refresh(&editor);
        inspector.begin_edit(health, value, Value::Float(7.0));
        assert!(inspector.editing().is_some());

        inspector.abandon_edit();

        assert!(inspector.editing().is_none());
        assert_eq!(editor.documents.get(id).unwrap().revision(), revision);
        assert_eq!(
            editor.documents.get(id).unwrap().history().entries().len(),
            entries
        );
    }

    #[test]
    fn committing_an_edit_across_a_selection_is_one_transaction() {
        let (mut editor, health, value) = editor_with_two_lamps();
        let id = editor.workspace.active().unwrap();
        let entries = editor.documents.get(id).unwrap().history().entries().len();

        let mut inspector = InspectorViewModel::new();
        inspector.refresh(&editor);
        inspector.begin_edit(health, value, Value::Float(42.0));
        assert_eq!(inspector.commit_edit(&mut editor).unwrap(), 2);

        assert_eq!(
            editor.documents.get(id).unwrap().history().entries().len(),
            entries + 1
        );
        assert_eq!(
            editor
                .documents
                .get(id)
                .unwrap()
                .history()
                .entries()
                .last()
                .unwrap()
                .operations
                .len(),
            2
        );
    }

    #[test]
    fn differing_values_are_shown_as_mixed() {
        let (mut editor, health, value) = editor_with_two_lamps();
        let id = editor.workspace.active().unwrap();
        let first = editor
            .documents
            .get(id)
            .unwrap()
            .content()
            .nodes()
            .next()
            .unwrap();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Wound one", Actor::human("designer"), |document| {
                document.set_field(first, health, value, Value::Float(10.0))
            })
            .unwrap();

        let mut inspector = InspectorViewModel::new();
        inspector.refresh(&editor);
        assert_eq!(inspector.rows()[0].committed, CommonValue::Mixed);
    }

    #[test]
    fn a_rename_shows_the_new_name_against_the_same_identity() {
        let (mut editor, health, value) = editor_with_two_lamps();
        let id = editor.workspace.active().unwrap();
        let mut inspector = InspectorViewModel::new();
        inspector.refresh(&editor);
        assert_eq!(inspector.rows()[0].name, "value");
        let identity = inspector.rows()[0].field;

        editor
            .documents
            .get_mut(id)
            .unwrap()
            .schema_mut()
            .rename_field(health, value, "hitPoints")
            .unwrap();
        // A schema change is not a content change, so the inspector is asked to rebuild explicitly —
        // which is what a schema-editing tool does after it renames something.
        inspector.rows = build_rows(&editor);

        assert_eq!(inspector.rows()[0].name, "hitPoints");
        assert_eq!(
            inspector.rows()[0].field,
            identity,
            "and the identity did not move"
        );
    }
}
