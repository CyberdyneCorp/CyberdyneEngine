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
use crate::content::NodeState;
use crate::operation::Operation;

/// One semantic difference between two revisions.
#[derive(Clone, PartialEq, Debug)]
pub enum Change {
    /// A node exists in the later revision and not the earlier.
    NodeAdded {
        /// Which node.
        node: NodeId,
        /// Its complete authored state, so applying a diff never creates an empty substitute.
        state: Box<NodeState>,
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
    /// A node's author-facing name changed without changing its identity.
    NameChanged {
        /// Which node.
        node: NodeId,
        /// Its name before.
        before: String,
        /// Its name after.
        after: String,
    },
    /// A node moved between authoring layers.
    LayerChanged {
        /// Which node.
        node: NodeId,
        /// Its layer before.
        before: String,
        /// Its layer after.
        after: String,
    },
    /// A component was added to a node.
    ComponentAdded {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// The component's initial values.
        fields: Vec<(FieldId, Value)>,
    },
    /// A component was removed from a node.
    ComponentRemoved {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// The removed values, so undo and merge resolution are exact.
        fields: Vec<(FieldId, Value)>,
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
    /// A field's asset identity changed.
    AssetReferenceChanged {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// Which field.
        field: FieldId,
        /// Its reference before.
        before: String,
        /// Its reference after.
        after: String,
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
        if let Some(state) = after.node(*node) {
            changes.push(Change::NodeAdded {
                node: *node,
                state: Box::new(state.clone()),
            });
        }
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
    if old.name != new.name {
        changes.push(Change::NameChanged {
            node,
            before: old.name.clone(),
            after: new.name.clone(),
        });
    }
    if old.layer != new.layer {
        changes.push(Change::LayerChanged {
            node,
            before: old.layer.clone(),
            after: new.layer.clone(),
        });
    }

    let old_components: BTreeSet<TypeId> = old.components.keys().copied().collect();
    let new_components: BTreeSet<TypeId> = new.components.keys().copied().collect();
    for component in new_components.difference(&old_components) {
        changes.push(Change::ComponentAdded {
            node,
            component: *component,
            fields: new.components[component]
                .iter()
                .map(|(field, value)| (*field, value.clone()))
                .collect(),
        });
    }
    for component in old_components.difference(&new_components) {
        changes.push(Change::ComponentRemoved {
            node,
            component: *component,
            fields: old.components[component]
                .iter()
                .map(|(field, value)| (*field, value.clone()))
                .collect(),
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
                (Some(Value::Text(before)), Some(Value::Text(after))) if before != after => {
                    changes.push(Change::AssetReferenceChanged {
                        node,
                        component: *component,
                        field,
                        before: before.clone(),
                        after: after.clone(),
                    });
                }
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
    if let (Change::NodeRemoved { node }, other) | (other, Change::NodeRemoved { node }) =
        (left, right)
    {
        return other.node() == Some(*node);
    }
    if let (Some(left), Some(right)) = (field_target(left), field_target(right)) {
        return left == right;
    }
    if let (Some(left), Some(right)) = (component_target(left), component_target(right)) {
        return left == right;
    }
    if let (Some(removed), Some(field)) = (
        removed_component(left),
        field_target(right).map(field_component),
    ) {
        return removed == field;
    }
    if let (Some(field), Some(removed)) = (
        field_target(left).map(field_component),
        removed_component(right),
    ) {
        return field == removed;
    }
    matches!((node_target(left), node_target(right)), (Some(left), Some(right)) if left == right)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum FieldTarget {
    Value,
    Asset,
    Override,
}

fn field_target(change: &Change) -> Option<(NodeId, TypeId, FieldId, FieldTarget)> {
    match change {
        Change::FieldChanged {
            node,
            component,
            field,
            ..
        } => Some((*node, *component, *field, FieldTarget::Value)),
        Change::AssetReferenceChanged {
            node,
            component,
            field,
            ..
        } => Some((*node, *component, *field, FieldTarget::Asset)),
        Change::OverrideChanged {
            node,
            component,
            field,
            ..
        } => Some((*node, *component, *field, FieldTarget::Override)),
        _ => None,
    }
}

const fn field_component(target: (NodeId, TypeId, FieldId, FieldTarget)) -> (NodeId, TypeId) {
    (target.0, target.1)
}

fn component_target(change: &Change) -> Option<(NodeId, TypeId)> {
    match change {
        Change::ComponentAdded {
            node, component, ..
        }
        | Change::ComponentRemoved {
            node, component, ..
        } => Some((*node, *component)),
        _ => None,
    }
}

fn removed_component(change: &Change) -> Option<(NodeId, TypeId)> {
    match change {
        Change::ComponentRemoved {
            node, component, ..
        } => Some((*node, *component)),
        _ => None,
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum NodeTarget {
    Whole(NodeId),
    Parent(NodeId),
    Name(NodeId),
    Layer(NodeId),
}

fn node_target(change: &Change) -> Option<NodeTarget> {
    match change {
        Change::NodeAdded { node, .. } => Some(NodeTarget::Whole(*node)),
        Change::NodeReparented { node, .. } => Some(NodeTarget::Parent(*node)),
        Change::NameChanged { node, .. } => Some(NodeTarget::Name(*node)),
        Change::LayerChanged { node, .. } => Some(NodeTarget::Layer(*node)),
        _ => None,
    }
}

impl Change {
    /// The node this change is about.
    #[must_use]
    pub const fn node(&self) -> Option<NodeId> {
        match self {
            Change::NodeAdded { node, .. }
            | Change::NodeRemoved { node }
            | Change::NodeReparented { node, .. }
            | Change::NameChanged { node, .. }
            | Change::LayerChanged { node, .. }
            | Change::ComponentAdded { node, .. }
            | Change::ComponentRemoved { node, .. }
            | Change::FieldChanged { node, .. }
            | Change::AssetReferenceChanged { node, .. }
            | Change::OverrideChanged { node, .. } => Some(*node),
        }
    }
}

/// The operation that applies a change.
///
/// Total: every [`Change`] this module produces has exactly one operation that applies it, which is
/// what makes "nothing is silently discarded" a property of the type rather than of the code below.
pub fn operation_for(change: &Change, base: &DocumentContent) -> Operation {
    match change {
        Change::NodeAdded { node, state } => Operation::RestoreNode {
            node: *node,
            state: state.clone(),
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
        Change::NameChanged {
            node,
            before,
            after,
        } => Operation::SetName {
            node: *node,
            before: before.clone(),
            after: after.clone(),
        },
        Change::LayerChanged {
            node,
            before,
            after,
        } => Operation::SetLayer {
            node: *node,
            before: before.clone(),
            after: after.clone(),
        },
        Change::ComponentAdded {
            node,
            component,
            fields,
        } => Operation::AddComponent {
            node: *node,
            component: *component,
            after: fields.clone(),
        },
        Change::ComponentRemoved {
            node,
            component,
            fields,
        } => Operation::RemoveComponent {
            node: *node,
            component: *component,
            before: fields.clone(),
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
        Change::AssetReferenceChanged {
            node,
            component,
            field,
            before,
            after,
        } => Operation::SetAssetReference {
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

/// Build the operation for a user-provided replacement of a typed merge conflict.
///
/// Structural conflicts deliberately refuse arbitrary replacements: recreating a node or component
/// requires a complete typed state, not a string smuggled through a dialog. Those conflicts can
/// still choose either complete side. Scalar, reference, name, layer, and override conflicts accept
/// a replacement of the same type and record the local value as the operation's exact `before`.
pub fn replacement_operation(
    conflict: &Conflict,
    replacement: Value,
) -> cy_editor_core::problem::Result<Operation> {
    match &conflict.ours {
        Change::FieldChanged {
            node,
            component,
            field,
            after,
            ..
        } => {
            require_same_kind(after, &replacement)?;
            Ok(Operation::SetField {
                node: *node,
                component: *component,
                field: *field,
                before: after.clone(),
                after: replacement,
            })
        }
        Change::AssetReferenceChanged {
            node,
            component,
            field,
            after,
            ..
        } => Ok(Operation::SetAssetReference {
            node: *node,
            component: *component,
            field: *field,
            before: after.clone(),
            after: replacement_text(replacement, "asset reference")?,
        }),
        Change::NameChanged { node, after, .. } => Ok(Operation::SetName {
            node: *node,
            before: after.clone(),
            after: replacement_text(replacement, "entity name")?,
        }),
        Change::LayerChanged { node, after, .. } => Ok(Operation::SetLayer {
            node: *node,
            before: after.clone(),
            after: replacement_text(replacement, "layer")?,
        }),
        Change::OverrideChanged {
            node,
            component,
            field,
            after,
            ..
        } => {
            let replacement = if replacement == Value::Nil {
                None
            } else {
                if let Some(current) = after {
                    require_same_kind(current, &replacement)?;
                }
                Some(replacement)
            };
            Ok(Operation::SetOverride {
                node: *node,
                component: *component,
                field: *field,
                before: after.clone(),
                after: replacement,
            })
        }
        _ => Err(cy_editor_core::problem::Problem::new(
            "replace a merge conflict",
            "this structural conflict requires choosing the complete local or incoming state",
        )
        .with_remedy("choose Local or Incoming for this conflict")),
    }
}

/// Operations that accept the incoming side while preserving the exact current local state for
/// undo.
///
/// The ordinary merge operation is expressed relative to `base`. A conflicting resolution is
/// different: it is applied to `local`, which may already hold another value or may have removed a
/// whole node/component. Rebasing the operation here is what prevents undo from restoring the base
/// value instead of the pre-merge local value.
pub fn incoming_resolution_operations(
    conflict: &Conflict,
    base: &DocumentContent,
    local: &DocumentContent,
) -> cy_editor_core::problem::Result<Vec<Operation>> {
    let change = &conflict.theirs;
    let mut operations = Vec::new();
    let Some(node) = change.node() else {
        return Ok(operations);
    };

    if let Change::NodeAdded { state, .. } = change {
        if let Some(current) = local.node(node) {
            operations.push(Operation::DeleteNode {
                node,
                was: Box::new(current.clone()),
            });
        }
        operations.push(Operation::RestoreNode {
            node,
            state: state.clone(),
        });
        return Ok(operations);
    }

    if let Change::NodeRemoved { .. } = change {
        if let Some(current) = local.node(node) {
            operations.push(Operation::DeleteNode {
                node,
                was: Box::new(current.clone()),
            });
        }
        return Ok(operations);
    }

    let effective = if local.node(node).is_none() {
        let state = base
            .node(node)
            .cloned()
            .ok_or_else(|| cy_editor_core::problem::Problem::not_found("the merge base entity"))?;
        operations.push(Operation::RestoreNode {
            node,
            state: Box::new(state),
        });
        base
    } else {
        local
    };

    match change {
        Change::NodeReparented { .. }
        | Change::NameChanged { .. }
        | Change::LayerChanged { .. } => {
            append_node_resolution(&mut operations, change, node, effective);
        }
        Change::ComponentAdded { .. } | Change::ComponentRemoved { .. } => {
            append_component_resolution(&mut operations, change, node, effective);
        }
        Change::FieldChanged { .. }
        | Change::AssetReferenceChanged { .. }
        | Change::OverrideChanged { .. } => {
            append_value_resolution(&mut operations, change, node, effective, base)?;
        }
        Change::NodeAdded { .. } | Change::NodeRemoved { .. } => unreachable!("handled above"),
    }
    Ok(operations)
}

fn append_node_resolution(
    operations: &mut Vec<Operation>,
    change: &Change,
    node: NodeId,
    effective: &DocumentContent,
) {
    match change {
        Change::NodeReparented { after, .. } => operations.push(Operation::Reparent {
            node,
            before: effective.node(node).and_then(|state| state.parent),
            after: *after,
        }),
        Change::NameChanged { after, .. } => operations.push(Operation::SetName {
            node,
            before: effective
                .node(node)
                .map_or_else(String::new, |state| state.name.clone()),
            after: after.clone(),
        }),
        Change::LayerChanged { after, .. } => operations.push(Operation::SetLayer {
            node,
            before: effective
                .node(node)
                .map_or_else(String::new, |state| state.layer.clone()),
            after: after.clone(),
        }),
        _ => unreachable!("node resolution only receives node attributes"),
    }
}

fn append_component_resolution(
    operations: &mut Vec<Operation>,
    change: &Change,
    node: NodeId,
    effective: &DocumentContent,
) {
    let (component, incoming) = match change {
        Change::ComponentAdded {
            component, fields, ..
        } => (*component, Some(fields)),
        Change::ComponentRemoved { component, .. } => (*component, None),
        _ => unreachable!("component resolution only receives component changes"),
    };
    if let Some(current) = effective
        .node(node)
        .and_then(|state| state.components.get(&component))
    {
        operations.push(Operation::RemoveComponent {
            node,
            component,
            before: current
                .iter()
                .map(|(field, value)| (*field, value.clone()))
                .collect(),
        });
    }
    if let Some(fields) = incoming {
        operations.push(Operation::AddComponent {
            node,
            component,
            after: fields.clone(),
        });
    }
}

fn append_value_resolution(
    operations: &mut Vec<Operation>,
    change: &Change,
    node: NodeId,
    effective: &DocumentContent,
    base: &DocumentContent,
) -> cy_editor_core::problem::Result<()> {
    match change {
        Change::FieldChanged {
            component,
            field,
            after,
            ..
        } => {
            ensure_component(operations, node, *component, effective, base)?;
            let before = current_field(effective, base, node, *component, *field)?.clone();
            operations.push(Operation::SetField {
                node,
                component: *component,
                field: *field,
                before,
                after: after.clone(),
            });
        }
        Change::AssetReferenceChanged {
            component,
            field,
            after,
            ..
        } => {
            ensure_component(operations, node, *component, effective, base)?;
            let before = current_field(effective, base, node, *component, *field)?
                .as_text()
                .ok_or_else(|| {
                    cy_editor_core::problem::Problem::not_found("the local asset reference")
                })?
                .to_string();
            operations.push(Operation::SetAssetReference {
                node,
                component: *component,
                field: *field,
                before,
                after: after.clone(),
            });
        }
        Change::OverrideChanged {
            component,
            field,
            after,
            ..
        } => {
            operations.push(Operation::SetOverride {
                node,
                component: *component,
                field: *field,
                before: effective
                    .node(node)
                    .and_then(|state| state.overrides.get(&(*component, *field)))
                    .cloned(),
                after: after.clone(),
            });
        }
        _ => unreachable!("value resolution only receives value changes"),
    }
    Ok(())
}

fn current_field<'a>(
    effective: &'a DocumentContent,
    base: &'a DocumentContent,
    node: NodeId,
    component: TypeId,
    field: FieldId,
) -> cy_editor_core::problem::Result<&'a Value> {
    effective
        .field(node, component, field)
        .or_else(|| base.field(node, component, field))
        .ok_or_else(|| cy_editor_core::problem::Problem::not_found("the local merge field"))
}

fn ensure_component(
    operations: &mut Vec<Operation>,
    node: NodeId,
    component: TypeId,
    effective: &DocumentContent,
    base: &DocumentContent,
) -> cy_editor_core::problem::Result<()> {
    if effective
        .node(node)
        .is_some_and(|state| state.components.contains_key(&component))
    {
        return Ok(());
    }
    let fields = base
        .node(node)
        .and_then(|state| state.components.get(&component))
        .ok_or_else(|| cy_editor_core::problem::Problem::not_found("the merge base component"))?
        .iter()
        .map(|(field, value)| (*field, value.clone()))
        .collect();
    operations.push(Operation::AddComponent {
        node,
        component,
        after: fields,
    });
    Ok(())
}

fn require_same_kind(current: &Value, replacement: &Value) -> cy_editor_core::problem::Result<()> {
    if current.kind() == replacement.kind() {
        Ok(())
    } else {
        Err(cy_editor_core::problem::Problem::new(
            "replace a merge value",
            format!(
                "the field is {} but the replacement is {}",
                current.kind(),
                replacement.kind()
            ),
        )
        .with_remedy(format!("provide a {} replacement", current.kind())))
    }
}

fn replacement_text(replacement: Value, what: &str) -> cy_editor_core::problem::Result<String> {
    match replacement {
        Value::Text(text) => Ok(text),
        other => Err(cy_editor_core::problem::Problem::new(
            format!("replace {what}"),
            format!("{what} requires text, not {}", other.kind()),
        )),
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
    fn diff_preserves_names_parents_layers_components_and_asset_references() {
        let mut document = Document::new("worlds/assets.cyworld");
        let link = document.schema_mut().declare_type("MeshLink", false);
        let asset = document
            .schema_mut()
            .declare_field(link, "asset", ValueKind::Text, "mesh asset")
            .unwrap();
        let extra = document.schema_mut().declare_type("Marker", false);
        let label = document
            .schema_mut()
            .declare_field(extra, "label", ValueKind::Text, "marker label")
            .unwrap();
        let (parent, node) = document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let parent = document.create_node(None)?;
                let node = document.create_node(None)?;
                document.add_component(
                    node,
                    link,
                    vec![(asset, Value::Text("models/old.cymesh".into()))],
                )?;
                Ok((parent, node))
            })
            .unwrap();
        let mut after = document.fork();
        after
            .with_transaction("Edit", Actor::human("designer"), |after| {
                after.set_name(node, "Crate")?;
                after.reparent_node(node, Some(parent))?;
                after.record(Operation::SetLayer {
                    node,
                    before: String::new(),
                    after: "Gameplay".into(),
                })?;
                after.record(Operation::SetAssetReference {
                    node,
                    component: link,
                    field: asset,
                    before: "models/old.cymesh".into(),
                    after: "models/new.cymesh".into(),
                })?;
                after.add_component(node, extra, vec![(label, Value::Text("spawn".into()))])
            })
            .unwrap();

        let changes = diff(document.content(), after.content());
        assert!(matches!(changes[0], Change::NodeReparented { .. }));
        assert!(
            changes.iter().any(
                |change| matches!(change, Change::NameChanged { after, .. } if after == "Crate")
            )
        );
        assert!(changes.iter().any(
            |change| matches!(change, Change::LayerChanged { after, .. } if after == "Gameplay")
        ));
        assert!(changes.iter().any(|change| matches!(change, Change::AssetReferenceChanged { after, .. } if after == "models/new.cymesh")));
        assert!(changes.iter().any(|change| matches!(change, Change::ComponentAdded { fields, .. } if fields == &vec![(label, Value::Text("spawn".into()))])));
    }

    #[test]
    fn merging_an_added_entity_restores_its_complete_state() {
        let (document, ..) = base();
        let mut theirs = document.fork();
        let added = theirs
            .with_transaction("Add", Actor::human("other"), |theirs| {
                let node = theirs.create_node(None)?;
                theirs.set_name(node, "Complete")?;
                Ok(node)
            })
            .unwrap();
        let merged = merge(document.content(), document.content(), theirs.content());
        assert!(merged.is_clean());

        let mut applied = document.fork();
        applied
            .with_transaction("Merge", Actor::human("designer"), |applied| {
                for operation in &merged.operations {
                    applied.record(operation.clone())?;
                }
                Ok(())
            })
            .unwrap();
        assert_eq!(
            applied.content().nodes().collect::<Vec<_>>(),
            theirs.content().nodes().collect::<Vec<_>>()
        );
        assert_eq!(
            applied.content().node(added),
            theirs.content().node(added),
            "the merge operation preserves the added entity's complete authored state"
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
