//! The generated inspector: no per-type editor code, anywhere.
//!
//! `editor-ui-ux`: "The inspector SHALL be **generated from the engine's reflection data** rather
//! than hand-written per type, so a new type is editable without editor code."
//!
//! There is one function that turns a field into a control — [`control_for`] — and it takes a
//! `cy_editor_reflection::ReflectedField` and reads nothing but its kind and its presentation. There
//! is no match on a type's *name* in this file, no table of known components, and no place to add
//! one: a component the engine registered five minutes ago is laid out by the same code as
//! `Transform`, because that code has never heard of `Transform`. The milestone's closing criterion
//! — "the generated inspector edits a reflected type with no per-type editor code" — is that
//! sentence, and `tests/the_inspector_is_generated.rs` is it against a real `CyInterface` table.
//!
//! --- THE FOUR THINGS THIS FILE IS CAREFUL ABOUT ---------------------------------------------------
//!
//! **An in-progress edit is not document state.** [`Edit`] is a separate field, cleared by
//! [`GeneratedInspector::cancel_edit`], and cancelling does nothing else because nothing else ever
//! happened. "WHEN a field is edited and focus is lost without committing THEN the document SHALL be
//! unchanged and no transaction SHALL exist."
//!
//! **A drag is one undo.** [`GeneratedInspector::begin_drag`] opens an *interactive* transaction
//! keyed by the interaction, every intermediate value is applied to the document so the viewport
//! shows the drag, and one history entry is recorded on commit. Escape calls
//! [`GeneratedInspector::cancel_drag`], which rolls the operations back and records nothing — the
//! forbidden pattern "a per-frame transaction produced by a continuous drag" is unreachable because
//! there is no per-frame commit.
//!
//! **Expansion is presentation state.** It is held here, keyed by *type name* so that an advanced
//! user configures a type once, and it never reaches a document — "Expansion state is presentation,
//! not document state, and SHALL NOT dirty a document."
//!
//! **Mixed is a value.** A multi-selection whose members disagree shows `CommonValue::Mixed`, never
//! the first member's value, and editing it writes to every selected object in **one** transaction.
//!
//! --- WHAT THIS SUPERSEDES --------------------------------------------------------------------------
//!
//! `cy_editor_viewmodels::inspector::InspectorViewModel` is the earlier, thinner inspector: it reads
//! a document's schema directly and has no disclosure, no units, no validation, no custom editors and
//! no runtime types. Everything it does, this does; retiring it is a one-line change in a crate this
//! change does not own, and it is recorded in the milestone notes rather than done here.

use std::collections::BTreeMap;

use cy_editor_commands::CommandContext;
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::observe::{Revision, Watch};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::selection::CommonValue;
use cy_editor_reflection::presentation::{Disclosure, Unit};
use cy_editor_reflection::{Catalogue, Origin, ReflectedField};
use cy_editor_services::Editor;
use cy_editor_visual::axis::Axis;
use cy_editor_visual::colour::{Rgb, Theme};

/// The control a field is edited with.
///
/// Derived from the field's kind, never from its name or its type's name. [`Control::Custom`] is the
/// registered override, and it carries a *name* because a control is a toolkit object and this crate
/// has no toolkit.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Control {
    /// A checkbox.
    Toggle,
    /// A number field, possibly with a unit and a range.
    Number,
    /// A text field.
    Text,
    /// Two, three or four numeric lanes, labelled and coloured by axis.
    Vector(usize),
    /// A rotation. Four lanes, presented as a rotation rather than as four numbers.
    Rotation,
    /// A reference to another node.
    EntityReference,
    /// Something the editor can show and not meaningfully edit inline.
    Opaque,
    /// A registered custom editor, by name.
    Custom(String),
}

/// The control for a field: its kind, or the registered override.
#[must_use]
pub fn control_for(field: &ReflectedField) -> Control {
    if let Some(editor) = &field.presentation.editor {
        return Control::Custom(editor.clone());
    }
    match field.kind {
        ValueKind::Bool => Control::Toggle,
        ValueKind::Int | ValueKind::Float | ValueKind::Double => Control::Number,
        ValueKind::Vec2 => Control::Vector(2),
        ValueKind::Vec3 => Control::Vector(3),
        ValueKind::Vec4 => Control::Vector(4),
        ValueKind::Quat => Control::Rotation,
        ValueKind::Text => Control::Text,
        ValueKind::Entity => Control::EntityReference,
        ValueKind::Bytes | ValueKind::Nil => Control::Opaque,
    }
}

/// The axis label and colour of each lane of a vector or rotation control.
///
/// `editor-visual-language`: "vector property fields in the inspector" carry the same three hues as
/// the gizmos, and they come from the same function — `cy_editor_visual::axis::colour` — so a value
/// in the inspector and a handle in the viewport cannot drift apart. A fourth lane is `W`, which is
/// not an axis and is drawn in the secondary text colour.
#[must_use]
pub fn lanes(control: &Control, theme: Theme) -> Vec<(&'static str, Option<Rgb>)> {
    let count = match control {
        Control::Vector(lanes) => *lanes,
        Control::Rotation => 4,
        _ => return Vec::new(),
    };
    (0..count)
        .map(|lane| match Axis::of_lane(lane) {
            Some(axis) => (
                axis.label(),
                Some(cy_editor_visual::axis::colour(axis, theme)),
            ),
            None => ("W", None),
        })
        .collect()
}

/// One editable row.
#[derive(Clone, PartialEq, Debug)]
pub struct InspectorRow {
    /// The component the field belongs to.
    pub component: TypeId,
    /// The field.
    pub field: FieldId,
    /// The field's name, as the schema names it now.
    pub name: String,
    /// What it means, or what it is when nothing said what it means.
    pub tooltip: String,
    /// How it is edited.
    pub control: Control,
    /// What it reads across the selection. `Mixed` is a value, not an absence.
    pub committed: CommonValue,
    /// Whether the editor may write it.
    pub writable: bool,
    /// The unit shown beside the number, when one is known.
    pub unit: Option<Unit>,
    /// Whether it opens visible or behind the advanced disclosure.
    pub disclosure: Disclosure,
    /// Whether the value differs from the declared default, so it can be marked and reset in place.
    pub modified: bool,
    /// A validation failure on the **committed** value, shown on the row that has it.
    ///
    /// "Problems SHALL be surfaced at the object that has them — in the hierarchy, the inspector
    /// row, the asset entry, the viewport — not only in a console."
    pub problem: Option<Problem>,
}

/// One component's section of the inspector.
#[derive(Clone, PartialEq, Debug)]
pub struct InspectorSection {
    /// The component type.
    pub component: TypeId,
    /// Its name, which is the section's title.
    pub title: String,
    /// Whether the document or the engine described it.
    pub origin: Origin,
    /// A registered whole-type editor that replaces this generated form.
    pub custom_editor: Option<String>,
    /// Whether the source described fewer fields than the type has members.
    pub partially_described: bool,
    /// The rows shown when the inspector opens.
    pub rows: Vec<InspectorRow>,
    /// The rows behind the advanced disclosure: collapsed, and present.
    pub advanced: Vec<InspectorRow>,
    /// Whether the section is expanded. Presentation state, persisted per type.
    pub expanded: bool,
}

/// How many objects are selected, and of what kinds.
///
/// `editor-visual-language`: "When more than one object is selected, the inspector SHALL **state the
/// count and the composition** of the selection — how many objects, and of what kinds."
///
/// A node's *kind* is the name of the first component it carries that has data. That is a
/// definition rather than a discovery — the engine has no notion of an object's class — and it is
/// the one that produces the sentence a user expects: three lights read as three lights.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct SelectionSummary {
    /// How many objects.
    pub count: usize,
    /// How many of each kind, most numerous first.
    pub composition: Vec<(String, usize)>,
}

impl SelectionSummary {
    /// The sentence a header shows: `3 selected · 2 Light, 1 Mesh`.
    ///
    /// In the engine's vocabulary, and `cy_editor_visual::vocabulary::check_label` is what keeps it
    /// that way — see this module's tests.
    #[must_use]
    pub fn line(&self) -> String {
        match self.count {
            0 => "Nothing selected".to_string(),
            1 => {
                let kind = self
                    .composition
                    .first()
                    .map_or("Node", |(name, _)| name.as_str());
                format!("1 selected · {kind}")
            }
            count => {
                let kinds: Vec<String> = self
                    .composition
                    .iter()
                    .map(|(name, number)| format!("{number} {name}"))
                    .collect();
                format!("{count} selected · {}", kinds.join(", "))
            }
        }
    }
}

/// An edit that has been typed and not committed. **Never document state.**
#[derive(Clone, PartialEq, Debug)]
pub struct Edit {
    /// The component.
    pub component: TypeId,
    /// The field.
    pub field: FieldId,
    /// What the user has typed so far.
    pub value: Value,
}

/// An interaction in progress: a slider being dragged, a value being scrubbed.
#[derive(Clone, PartialEq, Debug)]
struct Drag {
    component: TypeId,
    field: FieldId,
    targets: Vec<NodeId>,
}

/// The inspector.
#[derive(Debug, Default)]
pub struct GeneratedInspector {
    catalogue: Option<Catalogue>,
    sections: Vec<InspectorSection>,
    summary: SelectionSummary,
    expanded: BTreeMap<String, bool>,
    editing: Option<Edit>,
    drag: Option<Drag>,
    selection_watch: Watch,
    document_watch: Watch,
    rebuilds: u64,
}

impl GeneratedInspector {
    /// An inspector with nothing to describe types with, which shows nothing.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// An inspector that lays out the types in a catalogue.
    #[must_use]
    pub fn with_catalogue(catalogue: Catalogue) -> Self {
        Self {
            catalogue: Some(catalogue),
            ..Self::default()
        }
    }

    /// Replace the catalogue — when a runtime is attached, or a document's schema changed.
    ///
    /// Forces the next [`GeneratedInspector::refresh`] to rebuild, because a schema change moves no
    /// revision a document watch is looking at.
    pub fn set_catalogue(&mut self, catalogue: Catalogue) {
        self.catalogue = Some(catalogue);
        self.document_watch = Watch::new();
    }

    /// The catalogue in force.
    #[must_use]
    pub const fn catalogue(&self) -> Option<&Catalogue> {
        self.catalogue.as_ref()
    }

    /// Rebuild only if the selection or the active document moved.
    ///
    /// "The editor SHALL NOT re-query reflected properties, asset listings, or runtime state at
    /// interface frame rate when nothing has changed."
    pub fn refresh(&mut self, editor: &Editor) -> bool {
        let document_revision = editor
            .workspace
            .active()
            .and_then(|id| editor.documents.get(id))
            .map_or(Revision::INITIAL, Document::revision);
        let moved = self.selection_watch.changed(editor.selection.revision())
            || self.document_watch.changed(document_revision);
        if !moved {
            return false;
        }

        self.sections = self.build(editor);
        self.summary = self.summarise(editor);
        self.selection_watch.accept(editor.selection.revision());
        self.document_watch.accept(document_revision);
        self.rebuilds += 1;
        true
    }

    /// The sections to draw.
    #[must_use]
    pub fn sections(&self) -> &[InspectorSection] {
        &self.sections
    }

    /// Every row, default and advanced, of every section.
    #[must_use]
    pub fn rows(&self) -> Vec<&InspectorRow> {
        self.sections
            .iter()
            .flat_map(|section| section.rows.iter().chain(section.advanced.iter()))
            .collect()
    }

    /// What is selected, and of what kinds.
    #[must_use]
    pub const fn summary(&self) -> &SelectionSummary {
        &self.summary
    }

    /// How many times this inspector has rebuilt. What a cost test asserts on.
    #[must_use]
    pub const fn rebuilds(&self) -> u64 {
        self.rebuilds
    }

    /// Expand or collapse a type's section. **Presentation state**, persisted per type.
    pub fn set_expanded(&mut self, type_name: &str, expanded: bool) {
        self.expanded.insert(type_name.to_string(), expanded);
        for section in &mut self.sections {
            if section.title == type_name {
                section.expanded = expanded;
            }
        }
    }

    /// Whether a type's section is expanded. Sections open expanded; advanced rows do not.
    #[must_use]
    pub fn is_expanded(&self, type_name: &str) -> bool {
        self.expanded.get(type_name).copied().unwrap_or(true)
    }

    /// Begin editing a field. Nothing is written.
    pub fn begin_edit(&mut self, component: TypeId, field: FieldId, value: Value) {
        self.editing = Some(Edit {
            component,
            field,
            value,
        });
    }

    /// The in-progress edit, which is never document state.
    #[must_use]
    pub const fn editing(&self) -> Option<&Edit> {
        self.editing.as_ref()
    }

    /// Abandon the in-progress edit. Nothing else happens, because nothing else happened.
    pub fn cancel_edit(&mut self) {
        self.editing = None;
    }

    /// Commit the in-progress edit across the whole selection as **one** transaction.
    ///
    /// Validation runs *before* the transaction opens: an out-of-range value that was recorded and
    /// then reported would already be in the undo history.
    pub fn commit_edit(&mut self, editor: &mut Editor) -> Result<usize> {
        let Some(edit) = self.editing.take() else {
            return Ok(0);
        };
        self.validate(edit.component, edit.field, &edit.value)?;

        let Some(id) = editor.workspace.active() else {
            return Ok(0);
        };
        let actor = editor.actor();
        let selection = editor.selection.get().clone();
        let Some(document) = editor.documents.get_mut(id) else {
            return Ok(0);
        };
        selection.set_field_on_all(
            document,
            format!("Set {}", self.field_name(edit.component, edit.field)),
            actor,
            edit.component,
            edit.field,
            &edit.value,
        )
    }

    /// Begin an interactive edit — a drag, a scrub — as one transaction keyed by the interaction.
    ///
    /// The key is supplied by the caller because the widget knows where an interaction begins and
    /// ends and a clock has to guess; see `cy-editor-documents`' transaction note.
    pub fn begin_drag(
        &mut self,
        editor: &mut Editor,
        component: TypeId,
        field: FieldId,
        key: &str,
    ) -> Result<()> {
        let Some(id) = editor.workspace.active() else {
            return Err(Problem::new("begin an edit", "no document is open")
                .with_remedy("open a document first"));
        };
        let actor = editor.actor();
        let name = self.field_name(component, field);
        let selection = editor.selection.get().clone();
        let document = editor
            .documents
            .get_mut(id)
            .ok_or_else(|| Problem::not_found("the active document"))?;

        let targets: Vec<NodeId> = selection
            .nodes()
            .filter(|node| document.content().field(*node, component, field).is_some())
            .collect();
        document.begin_interaction(format!("Set {name}"), actor, key);
        self.drag = Some(Drag {
            component,
            field,
            targets,
        });
        Ok(())
    }

    /// Apply an intermediate value of a drag to every selected object.
    ///
    /// Applied rather than buffered, so the viewport shows the drag; only the committed result
    /// enters history.
    pub fn drag_to(&mut self, editor: &mut Editor, value: &Value) -> Result<usize> {
        let Some(drag) = self.drag.clone() else {
            return Err(Problem::new("continue a drag", "no drag is in progress")
                .with_remedy("call begin_drag when the interaction starts"));
        };
        self.validate(drag.component, drag.field, value)?;
        let Some(id) = editor.workspace.active() else {
            return Ok(0);
        };
        let document = editor
            .documents
            .get_mut(id)
            .ok_or_else(|| Problem::not_found("the active document"))?;
        for node in &drag.targets {
            document.set_field(*node, drag.component, drag.field, value.clone())?;
        }
        Ok(drag.targets.len())
    }

    /// Finish a drag: **one** history entry for the whole interaction.
    pub fn commit_drag(&mut self, editor: &mut Editor) -> Result<()> {
        if self.drag.take().is_none() {
            return Ok(());
        }
        let Some(id) = editor.workspace.active() else {
            return Ok(());
        };
        let document = editor
            .documents
            .get_mut(id)
            .ok_or_else(|| Problem::not_found("the active document"))?;
        document.commit().map(|_| ())
    }

    /// Abandon a drag: the values go back and **no entry is recorded**.
    ///
    /// "WHEN a user presses escape during a drag THEN the value SHALL return to its pre-drag state
    /// and no transaction SHALL be recorded."
    pub fn cancel_drag(&mut self, editor: &mut Editor) -> Result<()> {
        if self.drag.take().is_none() {
            return Ok(());
        }
        let Some(id) = editor.workspace.active() else {
            return Ok(());
        };
        let document = editor
            .documents
            .get_mut(id)
            .ok_or_else(|| Problem::not_found("the active document"))?;
        document.cancel()
    }

    /// Whether a drag is in progress.
    #[must_use]
    pub const fn is_dragging(&self) -> bool {
        self.drag.is_some()
    }

    /// Refuse a value the field's reflection says it cannot hold.
    fn validate(&self, component: TypeId, field: FieldId, value: &Value) -> Result<()> {
        let Some(described) = self
            .catalogue
            .as_ref()
            .and_then(|catalogue| catalogue.type_of(component))
            .and_then(|described| described.field(field))
        else {
            return Ok(());
        };
        described.presentation.validate(&described.name, value)
    }

    fn field_name(&self, component: TypeId, field: FieldId) -> String {
        self.catalogue
            .as_ref()
            .and_then(|catalogue| catalogue.type_of(component))
            .and_then(|described| described.field(field))
            .map_or_else(|| "property".to_string(), |field| field.name.clone())
    }

    /// Build one section per component the selection has in common.
    fn build(&self, editor: &Editor) -> Vec<InspectorSection> {
        let (Some(catalogue), Some(id)) = (self.catalogue.as_ref(), editor.workspace.active())
        else {
            return Vec::new();
        };
        let Some(document) = editor.documents.get(id) else {
            return Vec::new();
        };
        let selection = editor.selection.get();

        selection
            .common_components(document)
            .into_iter()
            .filter_map(|component| {
                let described = catalogue.type_of(component)?;
                let row = |field: &ReflectedField| {
                    let committed = selection.common_value(document, component, field.id);
                    let modified = match &committed {
                        CommonValue::Same(value) => field.presentation.is_modified(value),
                        _ => false,
                    };
                    let problem = match &committed {
                        CommonValue::Same(value) if field.presentation.writable => {
                            field.presentation.validate(&field.name, value).err()
                        }
                        _ => None,
                    };
                    InspectorRow {
                        component,
                        field: field.id,
                        name: field.name.clone(),
                        tooltip: field.tooltip(),
                        control: control_for(field),
                        committed,
                        writable: field.presentation.writable,
                        unit: field.presentation.unit,
                        disclosure: field.presentation.disclosure,
                        modified,
                        problem,
                    }
                };
                Some(InspectorSection {
                    component,
                    title: described.name.clone(),
                    origin: described.origin,
                    custom_editor: described.custom_editor.clone(),
                    partially_described: described.partially_described,
                    rows: described.default_fields().into_iter().map(row).collect(),
                    advanced: described.advanced_fields().into_iter().map(row).collect(),
                    expanded: self.is_expanded(&described.name),
                })
            })
            .collect()
    }

    /// Count the selection and describe its composition.
    fn summarise(&self, editor: &Editor) -> SelectionSummary {
        let (Some(catalogue), Some(id)) = (self.catalogue.as_ref(), editor.workspace.active())
        else {
            return SelectionSummary::default();
        };
        let Some(document) = editor.documents.get(id) else {
            return SelectionSummary::default();
        };
        let selection = editor.selection.get();

        let mut kinds: BTreeMap<String, usize> = BTreeMap::new();
        for node in selection.nodes() {
            let kind = document
                .content()
                .node(node)
                .and_then(|state| {
                    state
                        .components
                        .keys()
                        .filter_map(|component| catalogue.type_of(*component))
                        .find(|described| !described.is_tag)
                        .map(|described| described.name.clone())
                })
                .unwrap_or_else(|| "Node".to_string());
            *kinds.entry(kind).or_default() += 1;
        }

        let mut composition: Vec<(String, usize)> = kinds.into_iter().collect();
        composition.sort_by(|first, second| second.1.cmp(&first.1).then(first.0.cmp(&second.0)));
        SelectionSummary {
            count: selection.node_count(),
            composition,
        }
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_documents::selection::Selection;
    use cy_editor_reflection::presentation::Range;
    use cy_editor_reflection::{Overrides, PropertyOverride};

    use super::*;

    /// An editor with a document whose schema has a transform-like and a health-like component.
    fn editor_with(count: usize) -> (Editor, Catalogue, TypeId, FieldId, TypeId, FieldId) {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let document = editor.documents.get_mut(id).unwrap();

        let transform = document.schema_mut().declare_type("Transform", false);
        let position = document
            .schema_mut()
            .declare_field(
                transform,
                "position",
                ValueKind::Vec3,
                "where the node sits",
            )
            .unwrap();
        let health = document.schema_mut().declare_type("Health", false);
        let value = document
            .schema_mut()
            .declare_field(health, "value", ValueKind::Float, "hit points remaining")
            .unwrap();
        let derived = document
            .schema_mut()
            .declare_field(health, "regenerating", ValueKind::Bool, "whether it heals")
            .unwrap();
        document
            .schema_mut()
            .set_writable(health, derived, false)
            .unwrap();

        let nodes = document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let mut nodes = Vec::new();
                for _ in 0..count {
                    let node = document.create_node(None)?;
                    document.add_component(
                        node,
                        transform,
                        vec![(position, Value::Vec3([0.0, 0.0, 0.0]))],
                    )?;
                    document.add_component(
                        node,
                        health,
                        vec![(value, Value::Float(100.0)), (derived, Value::Bool(false))],
                    )?;
                    nodes.push(node);
                }
                Ok(nodes)
            })
            .unwrap();

        let mut overrides = Overrides::new();
        overrides.register_property(
            "Health",
            "regenerating",
            PropertyOverride::new().with_disclosure(Disclosure::Advanced),
        );
        overrides.register_property(
            "Health",
            "value",
            PropertyOverride::new()
                .with_range(Range::new(0.0, 100.0))
                .with_unit(Unit::Normalised),
        );
        let catalogue = Catalogue::of_document(editor.documents.get(id).unwrap().schema())
            .with_overrides(&overrides);

        let mut selection = Selection::new();
        selection.set_nodes(nodes);
        editor.selection.set(selection);
        (editor, catalogue, transform, position, health, value)
    }

    #[test]
    fn a_type_the_inspector_has_never_heard_of_is_laid_out_anyway() {
        // The requirement, as a property of this file: nothing here names a component type, so a
        // type declared a moment ago gets rows.
        let (editor, catalogue, transform, position, ..) = editor_with(1);
        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);

        let section = inspector
            .sections()
            .iter()
            .find(|section| section.component == transform)
            .expect("a section for a type this code has never heard of");
        assert_eq!(section.title, "Transform");
        assert_eq!(section.rows.len(), 1);
        assert_eq!(section.rows[0].field, position);
        assert_eq!(section.rows[0].control, Control::Vector(3));
    }

    #[test]
    fn a_vector_field_is_labelled_with_the_same_axis_language_as_the_viewport() {
        let (editor, catalogue, ..) = editor_with(1);
        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        let row = inspector
            .rows()
            .into_iter()
            .find(|row| matches!(row.control, Control::Vector(3)))
            .unwrap();

        let theme = Theme::default();
        let lanes = lanes(&row.control, theme);
        assert_eq!(lanes[0].0, "X");
        assert_eq!(
            lanes[0].1,
            Some(cy_editor_visual::axis::colour(Axis::X, theme))
        );
        assert_eq!(
            lanes[2].1,
            Some(cy_editor_visual::axis::colour(Axis::Z, theme))
        );
    }

    #[test]
    fn advanced_detail_opens_collapsed_and_present() {
        let (editor, catalogue, ..) = editor_with(1);
        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        let health = inspector
            .sections()
            .iter()
            .find(|section| section.title == "Health")
            .unwrap();

        assert_eq!(health.rows.len(), 1, "the ordinary value is visible");
        assert_eq!(health.advanced.len(), 1, "and the advanced one is present");
        assert!(
            !health.advanced[0].writable,
            "read-only survives into the row"
        );
    }

    #[test]
    fn an_idle_inspector_costs_nothing() {
        let (editor, catalogue, ..) = editor_with(2);
        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        assert!(inspector.refresh(&editor), "the first look rebuilds");
        for _ in 0..1_000 {
            assert!(
                !inspector.refresh(&editor),
                "nothing moved, so nothing runs"
            );
        }
        assert_eq!(inspector.rebuilds(), 1);
    }

    #[test]
    fn a_multi_selection_states_its_count_and_composition() {
        let (editor, catalogue, ..) = editor_with(3);
        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);

        assert_eq!(inspector.summary().count, 3);
        assert_eq!(inspector.summary().line(), "3 selected · 3 Transform");
        cy_editor_visual::vocabulary::check_label(&inspector.summary().line()).unwrap();
    }

    #[test]
    fn differing_values_read_as_mixed_rather_than_as_the_first_objects() {
        let (mut editor, catalogue, .., health, value) = editor_with(3);
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

        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        let row = inspector
            .rows()
            .into_iter()
            .find(|row| row.field == value)
            .unwrap();
        assert_eq!(row.committed, CommonValue::Mixed);
    }

    #[test]
    fn editing_a_mixed_value_writes_to_everything_in_one_transaction() {
        let (mut editor, catalogue, .., health, value) = editor_with(3);
        let id = editor.workspace.active().unwrap();
        let entries = editor.documents.get(id).unwrap().history().entries().len();

        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        inspector.begin_edit(health, value, Value::Float(42.0));
        assert_eq!(inspector.commit_edit(&mut editor).unwrap(), 3);

        let history = editor.documents.get(id).unwrap().history();
        assert_eq!(history.entries().len(), entries + 1, "one entry");
        assert_eq!(history.entries().last().unwrap().operations.len(), 3);
    }

    #[test]
    fn an_abandoned_edit_changes_nothing() {
        let (editor, catalogue, .., health, value) = editor_with(1);
        let id = editor.workspace.active().unwrap();
        let revision = editor.documents.get(id).unwrap().revision();

        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        inspector.begin_edit(health, value, Value::Float(7.0));
        assert!(inspector.editing().is_some());
        inspector.cancel_edit();

        assert!(inspector.editing().is_none());
        assert_eq!(editor.documents.get(id).unwrap().revision(), revision);
        assert!(editor.documents.get(id).unwrap().history().entries().len() <= 1);
    }

    #[test]
    fn a_drag_across_many_values_is_one_undo() {
        // "WHEN a user drags a slider across many values THEN one undo SHALL restore the value from
        // before the drag."
        let (mut editor, catalogue, .., health, value) = editor_with(2);
        let id = editor.workspace.active().unwrap();
        let entries = editor.documents.get(id).unwrap().history().entries().len();

        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        inspector
            .begin_drag(&mut editor, health, value, "health-slider-1")
            .unwrap();
        for step in 1_u8..=40 {
            inspector
                .drag_to(&mut editor, &Value::Float(f32::from(step)))
                .unwrap();
        }
        inspector.commit_drag(&mut editor).unwrap();

        let document = editor.documents.get(id).unwrap();
        assert_eq!(
            document.history().entries().len(),
            entries + 1,
            "one entry for forty frames"
        );

        editor.documents.get_mut(id).unwrap().undo().unwrap();
        let node = editor
            .documents
            .get(id)
            .unwrap()
            .content()
            .nodes()
            .next()
            .unwrap();
        assert_eq!(
            editor
                .documents
                .get(id)
                .unwrap()
                .content()
                .field(node, health, value),
            Some(&Value::Float(100.0)),
            "one undo restored the value from before the drag"
        );
    }

    #[test]
    fn escape_during_a_drag_restores_the_value_and_records_nothing() {
        let (mut editor, catalogue, .., health, value) = editor_with(1);
        let id = editor.workspace.active().unwrap();
        let entries = editor.documents.get(id).unwrap().history().entries().len();

        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        inspector
            .begin_drag(&mut editor, health, value, "health-slider-2")
            .unwrap();
        inspector.drag_to(&mut editor, &Value::Float(3.0)).unwrap();
        inspector.cancel_drag(&mut editor).unwrap();

        let document = editor.documents.get(id).unwrap();
        assert_eq!(document.history().entries().len(), entries, "no entry");
        let node = document.content().nodes().next().unwrap();
        assert_eq!(
            document.content().field(node, health, value),
            Some(&Value::Float(100.0))
        );
        assert!(!inspector.is_dragging());
    }

    #[test]
    fn an_out_of_range_value_is_refused_before_anything_is_written() {
        let (mut editor, catalogue, .., health, value) = editor_with(1);
        let id = editor.workspace.active().unwrap();
        let revision = editor.documents.get(id).unwrap().revision();

        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        inspector.begin_edit(health, value, Value::Float(9_000.0));
        let problem = inspector.commit_edit(&mut editor).unwrap_err();

        assert!(problem.remedy.is_some(), "{problem}");
        assert_eq!(
            editor.documents.get(id).unwrap().revision(),
            revision,
            "nothing was written"
        );
    }

    #[test]
    fn expanding_a_section_is_presentation_state_and_dirties_nothing() {
        let (mut editor, catalogue, ..) = editor_with(1);
        let id = editor.workspace.active().unwrap();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .save(|_| Ok(()))
            .unwrap();
        let revision = editor.documents.get(id).unwrap().revision();

        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        assert!(inspector.is_expanded("Health"), "sections open expanded");
        inspector.set_expanded("Health", false);

        assert!(!inspector.is_expanded("Health"));
        assert!(!editor.documents.get(id).unwrap().is_dirty());
        assert_eq!(editor.documents.get(id).unwrap().revision(), revision);
    }

    #[test]
    fn a_registered_editor_replaces_the_generated_control() {
        let (editor, catalogue, ..) = editor_with(1);
        let mut overrides = Overrides::new();
        overrides.register_property(
            "Transform",
            "position",
            PropertyOverride::new().with_editor("world-position-picker"),
        );
        let catalogue = catalogue.with_overrides(&overrides);

        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        inspector.refresh(&editor);
        let row = inspector
            .rows()
            .into_iter()
            .find(|row| row.name == "position")
            .unwrap();
        assert_eq!(row.control, Control::Custom("world-position-picker".into()));
    }

    #[test]
    fn selecting_a_typical_entity_populates_the_inspector_well_inside_the_declared_target() {
        // `editor-ui-ux`'s table: "Selection change to inspector populated: under 16 ms for typical
        // entities." Twenty components of eight fields is more than typical; the measurement is
        // printed so that a regression is visible in a log even when it stays under the bound.
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        let document = editor.documents.get_mut(id).unwrap();
        let mut components = Vec::new();
        for component in 0..20 {
            let type_id = document
                .schema_mut()
                .declare_type(format!("Component{component}"), false);
            let mut fields = Vec::new();
            for field in 0..8 {
                let field = document
                    .schema_mut()
                    .declare_field(
                        type_id,
                        format!("field{field}"),
                        ValueKind::Float,
                        "one of many fields on a busy entity",
                    )
                    .unwrap();
                fields.push((field, Value::Float(1.0)));
            }
            components.push((type_id, fields));
        }
        let node = document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let node = document.create_node(None)?;
                for (type_id, fields) in &components {
                    document.add_component(node, *type_id, fields.clone())?;
                }
                Ok(node)
            })
            .unwrap();

        let catalogue = Catalogue::of_document(editor.documents.get(id).unwrap().schema());
        let mut inspector = GeneratedInspector::with_catalogue(catalogue);
        let mut selection = Selection::new();
        selection.add_node(node);
        editor.selection.set(selection);

        let started = std::time::Instant::now();
        assert!(inspector.refresh(&editor));
        let elapsed = started.elapsed();

        assert_eq!(inspector.rows().len(), 160);
        println!("inspector populated in {elapsed:?} for 20 components of 8 fields");
        assert!(
            elapsed < std::time::Duration::from_millis(16),
            "populating the inspector took {elapsed:?}, against a declared target of 16 ms"
        );
    }
}
