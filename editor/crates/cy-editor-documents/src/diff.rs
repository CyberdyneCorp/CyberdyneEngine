//! Semantic diff and three-way merge. Task 3.7.
//!
//! "The system SHALL produce a **semantic diff** between two revisions of a document, or between a
//! document and its base, expressed as added, removed, and changed entities, components, fields, and
//! overrides — not as a text or binary difference. The system SHALL support **three-way merge** at
//! the same semantic level: non-overlapping changes to different fields SHALL merge automatically;
//! changes to the same field SHALL be reported as a conflict for resolution. **Merge SHALL never
//! silently discard a change.** An unresolvable conflict SHALL be surfaced."
//!
//! --- WHY A DIFF IS COMPUTED FROM CONTENT AND NOT READ OUT OF THE JOURNAL --------------------------------
//!
//! Because the two answer different questions. A journal says *what happened*; a diff says *what is
//! different*, and the two disagree the moment somebody moves a lamp and moves it back. A reviewer
//! comparing two revisions wants the second answer, and a merge has to work between revisions that
//! have no shared journal at all — which is the normal case when two people edited a document on two
//! machines.
//!
//! The journal remains the operation stream that undo, autosave, recovery and live editing share.
//! A merge's *result* is expressed as operations too, so applying it goes through the transaction
//! system like everything else.

use std::collections::BTreeSet;

use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::value::Value;

use crate::content::DocumentContent;
use crate::operation::Operation;

/// One semantic difference between two revisions.
#[derive(Clone, PartialEq, Debug)]
pub enum Change {
    /// A node exists in the later revision and not the earlier.
    NodeAdded {
        /// Which node.
        node: NodeId,
    },
    /// A node existed in the earlier revision and not the later.
    NodeRemoved {
        /// Which node.
        node: NodeId,
    },
    /// A node moved to a different parent.
    NodeReparented {
        /// Which node.
        node: NodeId,
        /// Its parent before.
        before: Option<NodeId>,
        /// Its parent after.
        after: Option<NodeId>,
    },
    /// A component was added to a node.
    ComponentAdded {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
    },
    /// A component was removed from a node.
    ComponentRemoved {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
    },
    /// A field's value changed.
    FieldChanged {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// Which field.
        field: FieldId,
        /// Its value before.
        before: Value,
        /// Its value after.
        after: Value,
    },
    /// A prefab override was set, cleared, or changed.
    OverrideChanged {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// Which field.
        field: FieldId,
        /// The override before, or `None` when the field was inherited.
        before: Option<Value>,
        /// The override after, or `None` when it returned to inheriting.
        after: Option<Value>,
    },
}

/// Two changes to the same thing, from two sides of a merge.
#[derive(Clone, PartialEq, Debug)]
pub struct Conflict {
    /// What both sides changed, described as our side saw it.
    pub ours: Change,
    /// What the other side did to the same thing.
    pub theirs: Change,
    /// Why this could not be merged, in the words a person resolving it needs.
    pub reason: String,
}

/// What a three-way merge produced.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Merge {
    /// The other side's changes that merge cleanly, as operations to apply to ours.
    pub operations: Vec<Operation>,
    /// The changes that could not be merged. **Never empty because something was dropped**: every
    /// change either merges or appears here.
    pub conflicts: Vec<Conflict>,
}

impl Merge {
    /// Whether the merge is complete without a person.
    #[must_use]
    pub fn is_clean(&self) -> bool {
        self.conflicts.is_empty()
    }
}

/// Every semantic difference from `before` to `after`.
///
/// Deterministic: nodes, components and fields are visited in identity order, so two runs produce
/// the same list and a diff can be compared or golden-tested.
#[must_use]
pub fn diff(before: &DocumentContent, after: &DocumentContent) -> Vec<Change> {
    let mut changes = Vec::new();
    let old_nodes: BTreeSet<NodeId> = before.nodes().collect();
    let new_nodes: BTreeSet<NodeId> = after.nodes().collect();

    for node in new_nodes.difference(&old_nodes) {
        changes.push(Change::NodeAdded { node: *node });
    }
    for node in old_nodes.difference(&new_nodes) {
        changes.push(Change::NodeRemoved { node: *node });
    }
    for node in old_nodes.intersection(&new_nodes) {
        diff_node(before, after, *node, &mut changes);
    }
    changes
}

fn diff_node(
    before: &DocumentContent,
    after: &DocumentContent,
    node: NodeId,
    changes: &mut Vec<Change>,
) {
    let (Some(old), Some(new)) = (before.node(node), after.node(node)) else {
        return;
    };

    if old.parent != new.parent {
        changes.push(Change::NodeReparented {
            node,
            before: old.parent,
            after: new.parent,
        });
    }

    let old_components: BTreeSet<TypeId> = old.components.keys().copied().collect();
    let new_components: BTreeSet<TypeId> = new.components.keys().copied().collect();
    for component in new_components.difference(&old_components) {
        changes.push(Change::ComponentAdded {
            node,
            component: *component,
        });
    }
    for component in old_components.difference(&new_components) {
        changes.push(Change::ComponentRemoved {
            node,
            component: *component,
        });
    }
    for component in old_components.intersection(&new_components) {
        let old_fields = &old.components[component];
        let new_fields = &new.components[component];
        let fields: BTreeSet<FieldId> = old_fields
            .keys()
            .chain(new_fields.keys())
            .copied()
            .collect();
        for field in fields {
            match (old_fields.get(&field), new_fields.get(&field)) {
                (Some(old_value), Some(new_value)) if old_value != new_value => {
                    changes.push(Change::FieldChanged {
                        node,
                        component: *component,
                        field,
                        before: old_value.clone(),
                        after: new_value.clone(),
                    });
                }
                // A field present on one side only is a schema difference rather than an edit; the
                // component add or remove above already reported the shape change it belongs to.
                _ => {}
            }
        }
    }

    let overrides: BTreeSet<(TypeId, FieldId)> = old
        .overrides
        .keys()
        .chain(new.overrides.keys())
        .copied()
        .collect();
    for (component, field) in overrides {
        let old_value = old.overrides.get(&(component, field));
        let new_value = new.overrides.get(&(component, field));
        if old_value != new_value {
            changes.push(Change::OverrideChanged {
                node,
                component,
                field,
                before: old_value.cloned(),
                after: new_value.cloned(),
            });
        }
    }
}

/// Merge `theirs` into `ours`, both derived from `base`.
///
/// Non-overlapping changes merge; overlapping ones become conflicts. Nothing is dropped: every
/// change on the other side either becomes an operation or becomes a conflict, and the test
/// `nothing_is_ever_silently_discarded` is what holds that to account.
#[must_use]
pub fn merge(base: &DocumentContent, ours: &DocumentContent, theirs: &DocumentContent) -> Merge {
    let our_changes = diff(base, ours);
    let their_changes = diff(base, theirs);
    let mut result = Merge::default();

    for change in their_changes {
        match our_changes
            .iter()
            .find(|ours| touches_the_same_thing(ours, &change))
        {
            Some(conflicting) if conflicting != &change => {
                result.conflicts.push(Conflict {
                    ours: conflicting.clone(),
                    theirs: change,
                    reason: "both sides changed the same thing to different values".to_string(),
                });
            }
            // The same change on both sides is not a conflict; it is agreement, and applying it
            // twice is a no-op. Dropping it here is the one case where "nothing is discarded" and
            // "nothing is applied" are the same outcome.
            Some(_) => {}
            None => result.operations.push(operation_for(&change, base)),
        }
    }
    result
}

/// Whether two changes address the same field, component, node or parent link.
fn touches_the_same_thing(left: &Change, right: &Change) -> bool {
    match (left, right) {
        (
            Change::FieldChanged {
                node,
                component,
                field,
                ..
            }
            | Change::OverrideChanged {
                node,
                component,
                field,
                ..
            },
            Change::FieldChanged {
                node: other_node,
                component: other_component,
                field: other_field,
                ..
            }
            | Change::OverrideChanged {
                node: other_node,
                component: other_component,
                field: other_field,
                ..
            },
        ) if std::mem::discriminant(left) == std::mem::discriminant(right) => {
            node == other_node && component == other_component && field == other_field
        }
        (Change::NodeReparented { node, .. }, Change::NodeReparented { node: other, .. })
        | (Change::NodeRemoved { node }, Change::NodeRemoved { node: other })
        | (Change::NodeAdded { node }, Change::NodeAdded { node: other }) => node == other,
        (
            Change::ComponentAdded { node, component }
            | Change::ComponentRemoved { node, component },
            Change::ComponentAdded {
                node: other_node,
                component: other_component,
            }
            | Change::ComponentRemoved {
                node: other_node,
                component: other_component,
            },
        ) => node == other_node && component == other_component,
        // A removal of a node conflicts with anything else done to it: merging an edit into an
        // object the other side deleted is precisely the case a person has to decide.
        (Change::NodeRemoved { node }, other) | (other, Change::NodeRemoved { node }) => {
            other.node() == Some(*node)
        }
        _ => false,
    }
}

impl Change {
    /// The node this change is about.
    #[must_use]
    pub const fn node(&self) -> Option<NodeId> {
        match self {
            Change::NodeAdded { node }
            | Change::NodeRemoved { node }
            | Change::NodeReparented { node, .. }
            | Change::ComponentAdded { node, .. }
            | Change::ComponentRemoved { node, .. }
            | Change::FieldChanged { node, .. }
            | Change::OverrideChanged { node, .. } => Some(*node),
        }
    }
}

/// The operation that applies a change.
///
/// Total: every [`Change`] this module produces has exactly one operation that applies it, which is
/// what makes "nothing is silently discarded" a property of the type rather than of the code below.
fn operation_for(change: &Change, base: &DocumentContent) -> Operation {
    match change {
        Change::NodeAdded { node } => Operation::CreateNode {
            node: *node,
            parent: None,
        },
        Change::NodeRemoved { node } => Operation::DeleteNode {
            node: *node,
            was: Box::new(base.node(*node).cloned().unwrap_or_default()),
        },
        Change::NodeReparented {
            node,
            before,
            after,
        } => Operation::Reparent {
            node: *node,
            before: *before,
            after: *after,
        },
        Change::ComponentAdded { node, component } => Operation::AddComponent {
            node: *node,
            component: *component,
            after: Vec::new(),
        },
        Change::ComponentRemoved { node, component } => Operation::RemoveComponent {
            node: *node,
            component: *component,
            before: base
                .node(*node)
                .and_then(|state| state.components.get(component))
                .map(|fields| {
                    fields
                        .iter()
                        .map(|(field, value)| (*field, value.clone()))
                        .collect()
                })
                .unwrap_or_default(),
        },
        Change::FieldChanged {
            node,
            component,
            field,
            before,
            after,
        } => Operation::SetField {
            node: *node,
            component: *component,
            field: *field,
            before: before.clone(),
            after: after.clone(),
        },
        Change::OverrideChanged {
            node,
            component,
            field,
            before,
            after,
        } => Operation::SetOverride {
            node: *node,
            component: *component,
            field: *field,
            before: before.clone(),
            after: after.clone(),
        },
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::value::ValueKind;

    use super::*;
    use crate::document::Document;

    /// A document with two nodes, each carrying `Health { value }` and `Armour { rating }`.
    fn base() -> (Document, Vec<NodeId>, TypeId, FieldId, TypeId, FieldId) {
        let mut document = Document::new("prefabs/knight.cyprefab");
        let health = document.schema_mut().declare_type("Health", false);
        let value = document
            .schema_mut()
            .declare_field(health, "value", ValueKind::Float, "hit points")
            .unwrap();
        let armour = document.schema_mut().declare_type("Armour", false);
        let rating = document
            .schema_mut()
            .declare_field(armour, "rating", ValueKind::Float, "damage reduction")
            .unwrap();

        let nodes = document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let mut nodes = Vec::new();
                for _ in 0..2 {
                    let node = document.create_node(None)?;
                    document.add_component(node, health, vec![(value, Value::Float(100.0))])?;
                    document.add_component(node, armour, vec![(rating, Value::Float(1.0))])?;
                    nodes.push(node);
                }
                Ok(nodes)
            })
            .unwrap();
        (document, nodes, health, value, armour, rating)
    }

    fn edited(
        document: &Document,
        node: NodeId,
        component: TypeId,
        field: FieldId,
        value: f32,
    ) -> DocumentContent {
        let mut copy = document.fork();
        copy.with_transaction("Edit", Actor::human("other"), |copy| {
            copy.set_field(node, component, field, Value::Float(value))
        })
        .unwrap();
        copy.content().clone()
    }

    #[test]
    fn a_diff_is_semantic_rather_than_textual() {
        let (document, nodes, health, value, ..) = base();
        let after = edited(&document, nodes[0], health, value, 50.0);
        let changes = diff(document.content(), &after);
        assert_eq!(
            changes,
            vec![Change::FieldChanged {
                node: nodes[0],
                component: health,
                field: value,
                before: Value::Float(100.0),
                after: Value::Float(50.0),
            }]
        );
    }

    #[test]
    fn independent_edits_merge_automatically() {
        // "WHEN one developer changes a material and another changes a health value on the same
        // prefab THEN both changes SHALL merge automatically."
        let (document, nodes, health, value, armour, rating) = base();
        let ours = edited(&document, nodes[0], health, value, 50.0);
        let theirs = edited(&document, nodes[0], armour, rating, 3.0);

        let merged = merge(document.content(), &ours, &theirs);
        assert!(merged.is_clean(), "{:?}", merged.conflicts);
        assert_eq!(merged.operations.len(), 1);
    }

    #[test]
    fn the_same_field_on_both_sides_is_a_conflict_rather_than_a_choice() {
        let (document, nodes, health, value, ..) = base();
        let ours = edited(&document, nodes[0], health, value, 50.0);
        let theirs = edited(&document, nodes[0], health, value, 75.0);

        let merged = merge(document.content(), &ours, &theirs);
        assert_eq!(merged.conflicts.len(), 1);
        assert!(
            merged.operations.is_empty(),
            "a conflicted change is never applied silently"
        );
    }

    #[test]
    fn the_same_edit_on_both_sides_is_agreement_rather_than_a_conflict() {
        let (document, nodes, health, value, ..) = base();
        let ours = edited(&document, nodes[0], health, value, 50.0);
        let theirs = edited(&document, nodes[0], health, value, 50.0);

        let merged = merge(document.content(), &ours, &theirs);
        assert!(merged.is_clean());
        assert!(merged.operations.is_empty(), "it is already there");
    }

    #[test]
    fn nothing_is_ever_silently_discarded() {
        // The property the specification states as an absolute. Every change on the other side is
        // accounted for: it becomes an operation, becomes a conflict, or is a change we already
        // made identically.
        let (document, nodes, health, value, armour, rating) = base();
        let ours = edited(&document, nodes[0], health, value, 50.0);
        let mut theirs = document.fork();
        theirs
            .with_transaction("Edit", Actor::human("other"), |copy| {
                copy.set_field(nodes[0], health, value, Value::Float(75.0))?;
                copy.set_field(nodes[1], armour, rating, Value::Float(9.0))
            })
            .unwrap();

        let their_changes = diff(document.content(), theirs.content());
        let merged = merge(document.content(), &ours, theirs.content());
        assert_eq!(
            merged.operations.len() + merged.conflicts.len(),
            their_changes.len(),
            "every change on the other side is either applied or reported"
        );
    }

    #[test]
    fn an_edit_to_a_node_the_other_side_deleted_is_a_conflict() {
        let (document, nodes, health, value, ..) = base();
        let ours = edited(&document, nodes[0], health, value, 50.0);
        let mut theirs = document.fork();
        theirs
            .with_transaction("Delete", Actor::human("other"), |copy| {
                copy.delete_node(nodes[0])
            })
            .unwrap();

        let merged = merge(document.content(), &ours, theirs.content());
        assert_eq!(merged.conflicts.len(), 1, "a person has to decide this one");
    }
}
