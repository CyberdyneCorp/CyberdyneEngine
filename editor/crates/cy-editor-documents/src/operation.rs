//! Typed operations addressing stable identities, recording deltas rather than snapshots. Task 3.4.
//!
//! `editor-documents-and-transactions` enumerates the minimum set — "set field, add and remove
//! component, create, delete and reparent entity, instantiate prefab, modify override, change layer
//! membership, edit a graph, and change an asset reference" — and requires that operations "record
//! **before and after values** for what they changed, not a snapshot of the document", with
//! domain-specific payloads "where a generic field delta would be inefficient".
//!
//! --- WHY EVERY OPERATION CARRIES ITS BEFORE VALUE ---------------------------------------------------
//!
//! Because [`Operation::inverse`] must be exact and must be computable without the document. Undo
//! applies the inverse of each operation in reverse order, and if an inverse had to *read* the
//! document to know what to restore, then undoing after a reload would restore whatever the document
//! says now — which is the state undo exists to leave.
//!
//! The one operation that carries something snapshot-shaped is [`Operation::DeleteNode`], and it
//! carries the deleted *node*, not the document. That is the node's own delta: everything that
//! ceased to exist. Without it, deleting is not undoable at all.
//!
//! --- THE DOMAIN PAYLOAD IS AN ESCAPE HATCH WITH A NAME ------------------------------------------------
//!
//! [`Operation::Domain`] carries opaque before and after bytes and the kind that interprets them —
//! a terrain stroke's affected tiles, a foliage paint's rule deltas. It is deliberately opaque here:
//! this crate cannot know what a terrain tile is, and a variant per domain would make every new tool
//! an edit to the operation enum. What it is *not* is a way to smuggle a snapshot past the delta
//! rule, and a payload whose bytes are the whole landscape is a defect in the tool that wrote it.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::problem::Result;
use cy_editor_core::value::Value;

use crate::content::NodeState;

/// A reference to an asset, by project-relative path.
///
/// A path rather than a content hash, because that is what an author edits and what a source-control
/// diff shows. Resolution to a cooked artefact is the asset pipeline's, at a later task.
pub type AssetRef = String;

/// One typed change to a document, addressed by stable identity.
#[derive(Clone, PartialEq, Debug)]
pub enum Operation {
    /// Create a node, optionally under a parent.
    CreateNode {
        /// The node's stable identity, allocated by the document before the operation is built.
        node: NodeId,
        /// Its parent, or `None` for a root.
        parent: Option<NodeId>,
    },
    /// Delete a node, carrying everything needed to put it back.
    DeleteNode {
        /// Which node.
        node: NodeId,
        /// What it was, so that undo restores it exactly. See the module note.
        was: Box<NodeState>,
    },
    /// Put a deleted node back exactly as it was.
    ///
    /// A separate variant from [`Operation::CreateNode`] rather than a reuse of it, because
    /// creating an empty node and restoring one with its components, its layer and its children are
    /// different operations — and the difference is precisely what "undo is exact" means. It is
    /// produced by [`Operation::inverse`] and by the journal; a tool never writes one.
    RestoreNode {
        /// Which node.
        node: NodeId,
        /// What it was.
        state: Box<NodeState>,
    },
    /// Move a node to another parent.
    Reparent {
        /// Which node.
        node: NodeId,
        /// Its parent before.
        before: Option<NodeId>,
        /// Its parent after.
        after: Option<NodeId>,
    },
    /// Add a component to a node.
    AddComponent {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// The field values it is created with.
        after: Vec<(FieldId, Value)>,
    },
    /// Remove a component from a node.
    RemoveComponent {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// The field values it had, so undo restores them.
        before: Vec<(FieldId, Value)>,
    },
    /// Set one field.
    SetField {
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
    /// Instantiate a prefab under a node, recording the provenance that makes overrides meaningful.
    InstantiatePrefab {
        /// The root node the instance is created as.
        node: NodeId,
        /// The prefab asset.
        prefab: AssetRef,
        /// The parent it is created under.
        parent: Option<NodeId>,
    },
    /// Set or clear an override of an inherited field on a prefab instance.
    SetOverride {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// Which field.
        field: FieldId,
        /// The override before, or `None` when the field was inherited.
        before: Option<Value>,
        /// The override after, or `None` to return to inheriting.
        after: Option<Value>,
    },
    /// Change which layer a node belongs to.
    SetLayer {
        /// Which node.
        node: NodeId,
        /// The layer before.
        before: String,
        /// The layer after.
        after: String,
    },
    /// Change an asset reference a field holds.
    ///
    /// Distinct from [`Operation::SetField`] with a text value, because a reference is what a
    /// dependency tracker follows and what a rename has to rewrite; a tool that cannot find asset
    /// references cannot do either.
    SetAssetReference {
        /// Which node.
        node: NodeId,
        /// Which component type.
        component: TypeId,
        /// Which field.
        field: FieldId,
        /// The reference before.
        before: AssetRef,
        /// The reference after.
        after: AssetRef,
    },
    /// A domain-specific delta: a terrain stroke, a foliage paint, a graph edit.
    Domain {
        /// The node the change belongs to, when it belongs to one.
        node: Option<NodeId>,
        /// Which domain interprets the payload. A tool registers its own.
        kind: String,
        /// The bytes to apply on undo.
        before: Vec<u8>,
        /// The bytes to apply on redo.
        after: Vec<u8>,
    },
}

impl Operation {
    /// The operation that exactly reverses this one.
    ///
    /// Total: every variant has an inverse and none of them needs the document to compute it. That
    /// is what makes "undo is exact" hold across a reload.
    #[must_use]
    #[allow(
        clippy::too_many_lines,
        reason = "one exhaustive match over twelve operation kinds, which is one logical unit. \
                  Splitting it would hide the exhaustiveness the compiler is checking — the \
                  property that makes 'every operation has an inverse' true rather than hoped."
    )]
    pub fn inverse(&self) -> Self {
        match self {
            // Creating a node and instantiating a prefab both undo to a deletion of an
            // otherwise-empty node: nothing else has been recorded on it yet, because every
            // component and field a tool then adds is its own operation in the same transaction.
            Operation::CreateNode { node, parent }
            | Operation::InstantiatePrefab { node, parent, .. } => Operation::DeleteNode {
                node: *node,
                was: Box::new(NodeState::empty(*parent)),
            },
            Operation::DeleteNode { node, was } => Operation::RestoreNode {
                node: *node,
                state: was.clone(),
            },
            Operation::RestoreNode { node, state } => Operation::DeleteNode {
                node: *node,
                was: state.clone(),
            },
            Operation::Reparent {
                node,
                before,
                after,
            } => Operation::Reparent {
                node: *node,
                before: *after,
                after: *before,
            },
            Operation::AddComponent {
                node,
                component,
                after,
            } => Operation::RemoveComponent {
                node: *node,
                component: *component,
                before: after.clone(),
            },
            Operation::RemoveComponent {
                node,
                component,
                before,
            } => Operation::AddComponent {
                node: *node,
                component: *component,
                after: before.clone(),
            },
            Operation::SetField {
                node,
                component,
                field,
                before,
                after,
            } => Operation::SetField {
                node: *node,
                component: *component,
                field: *field,
                before: after.clone(),
                after: before.clone(),
            },
            Operation::SetOverride {
                node,
                component,
                field,
                before,
                after,
            } => Operation::SetOverride {
                node: *node,
                component: *component,
                field: *field,
                before: after.clone(),
                after: before.clone(),
            },
            Operation::SetLayer {
                node,
                before,
                after,
            } => Operation::SetLayer {
                node: *node,
                before: after.clone(),
                after: before.clone(),
            },
            Operation::SetAssetReference {
                node,
                component,
                field,
                before,
                after,
            } => Operation::SetAssetReference {
                node: *node,
                component: *component,
                field: *field,
                before: after.clone(),
                after: before.clone(),
            },
            Operation::Domain {
                node,
                kind,
                before,
                after,
            } => Operation::Domain {
                node: *node,
                kind: kind.clone(),
                before: after.clone(),
                after: before.clone(),
            },
        }
    }

    /// The node this operation addresses, when it addresses one.
    #[must_use]
    pub const fn node(&self) -> Option<NodeId> {
        match self {
            Operation::CreateNode { node, .. }
            | Operation::DeleteNode { node, .. }
            | Operation::RestoreNode { node, .. }
            | Operation::Reparent { node, .. }
            | Operation::AddComponent { node, .. }
            | Operation::RemoveComponent { node, .. }
            | Operation::SetField { node, .. }
            | Operation::InstantiatePrefab { node, .. }
            | Operation::SetOverride { node, .. }
            | Operation::SetLayer { node, .. }
            | Operation::SetAssetReference { node, .. } => Some(*node),
            Operation::Domain { node, .. } => *node,
        }
    }

    /// Whether this operation changes the document's shape rather than a value.
    ///
    /// The editor's analogue of the ECS's structural change, and it is what a live-editing policy
    /// keys on: a value change can be applied to a running world at any time, a structural one has
    /// to wait for a tick boundary.
    #[must_use]
    pub const fn is_structural(&self) -> bool {
        matches!(
            self,
            Operation::CreateNode { .. }
                | Operation::DeleteNode { .. }
                | Operation::RestoreNode { .. }
                | Operation::Reparent { .. }
                | Operation::AddComponent { .. }
                | Operation::RemoveComponent { .. }
                | Operation::InstantiatePrefab { .. }
        )
    }

    /// Merge `next` into this operation when the two are compatible, for coalescing.
    ///
    /// "Coalescing SHALL merge consecutive compatible operations within a time or interaction window
    /// — typing a name, dragging a slider — into one entry." Compatible means *the same target*, and
    /// the merged operation keeps this one's before value and the next one's after value, which is
    /// what makes one history entry out of five hundred slider positions.
    ///
    /// Only value operations coalesce. A structural one never does: two `AddComponent`s of the same
    /// component are not one add, they are an add and an error, and merging them would hide it.
    #[must_use]
    pub fn coalesce(&self, next: &Self) -> Option<Self> {
        match (self, next) {
            (
                Operation::SetField {
                    node,
                    component,
                    field,
                    before,
                    ..
                },
                Operation::SetField {
                    node: next_node,
                    component: next_component,
                    field: next_field,
                    after,
                    ..
                },
            ) if node == next_node && component == next_component && field == next_field => {
                Some(Operation::SetField {
                    node: *node,
                    component: *component,
                    field: *field,
                    before: before.clone(),
                    after: after.clone(),
                })
            }
            (
                Operation::Reparent { node, before, .. },
                Operation::Reparent {
                    node: next_node,
                    after,
                    ..
                },
            ) if node == next_node => Some(Operation::Reparent {
                node: *node,
                before: *before,
                after: *after,
            }),
            _ => None,
        }
    }

    /// Roughly how many bytes this operation's payload occupies.
    ///
    /// Used by the history budget. Measured by encoding rather than estimated, because an estimate
    /// that ignored a domain payload would let a terrain tool blow a budget it appeared to respect.
    #[must_use]
    pub fn payload_bytes(&self) -> usize {
        let mut writer = Writer::new();
        self.encode(&mut writer);
        writer.len()
    }

    /// Append this operation to a byte stream, for the journal and the live bridge.
    #[allow(
        clippy::too_many_lines,
        reason = "the encoder is one exhaustive match over twelve operation kinds; see `inverse`. \
                  It is also the half of a codec whose correctness is that it mirrors `decode` \
                  line for line, which splitting would break."
    )]
    pub fn encode(&self, writer: &mut Writer) {
        match self {
            Operation::CreateNode { node, parent } => {
                writer.u8(0);
                writer.u128(node.as_u128());
                encode_option_node(writer, *parent);
            }
            Operation::DeleteNode { node, was } => {
                writer.u8(1);
                writer.u128(node.as_u128());
                was.encode(writer);
            }
            Operation::RestoreNode { node, state } => {
                writer.u8(2);
                writer.u128(node.as_u128());
                state.encode(writer);
            }
            Operation::Reparent {
                node,
                before,
                after,
            } => {
                writer.u8(3);
                writer.u128(node.as_u128());
                encode_option_node(writer, *before);
                encode_option_node(writer, *after);
            }
            Operation::AddComponent {
                node,
                component,
                after,
            } => {
                writer.u8(4);
                writer.u128(node.as_u128());
                writer.u64(component.as_u64());
                encode_fields(writer, after);
            }
            Operation::RemoveComponent {
                node,
                component,
                before,
            } => {
                writer.u8(5);
                writer.u128(node.as_u128());
                writer.u64(component.as_u64());
                encode_fields(writer, before);
            }
            Operation::SetField {
                node,
                component,
                field,
                before,
                after,
            } => {
                writer.u8(6);
                writer.u128(node.as_u128());
                writer.u64(component.as_u64());
                writer.u64(field.as_u64());
                writer.value(before);
                writer.value(after);
            }
            Operation::InstantiatePrefab {
                node,
                prefab,
                parent,
            } => {
                writer.u8(7);
                writer.u128(node.as_u128());
                writer.text(prefab);
                encode_option_node(writer, *parent);
            }
            Operation::SetOverride {
                node,
                component,
                field,
                before,
                after,
            } => {
                writer.u8(8);
                writer.u128(node.as_u128());
                writer.u64(component.as_u64());
                writer.u64(field.as_u64());
                encode_option_value(writer, before.as_ref());
                encode_option_value(writer, after.as_ref());
            }
            Operation::SetLayer {
                node,
                before,
                after,
            } => {
                writer.u8(9);
                writer.u128(node.as_u128());
                writer.text(before);
                writer.text(after);
            }
            Operation::SetAssetReference {
                node,
                component,
                field,
                before,
                after,
            } => {
                writer.u8(10);
                writer.u128(node.as_u128());
                writer.u64(component.as_u64());
                writer.u64(field.as_u64());
                writer.text(before);
                writer.text(after);
            }
            Operation::Domain {
                node,
                kind,
                before,
                after,
            } => {
                writer.u8(11);
                encode_option_node(writer, *node);
                writer.text(kind);
                writer.bytes(before);
                writer.bytes(after);
            }
        }
    }

    /// Read one operation back.
    pub fn decode(reader: &mut Reader<'_>) -> Result<Self> {
        let tag = reader.u8()?;
        let operation = match tag {
            0 => Operation::CreateNode {
                node: NodeId::from_u128(reader.u128()?),
                parent: decode_option_node(reader)?,
            },
            1 => Operation::DeleteNode {
                node: NodeId::from_u128(reader.u128()?),
                was: Box::new(NodeState::decode(reader)?),
            },
            2 => Operation::RestoreNode {
                node: NodeId::from_u128(reader.u128()?),
                state: Box::new(NodeState::decode(reader)?),
            },
            3 => Operation::Reparent {
                node: NodeId::from_u128(reader.u128()?),
                before: decode_option_node(reader)?,
                after: decode_option_node(reader)?,
            },
            4 => Operation::AddComponent {
                node: NodeId::from_u128(reader.u128()?),
                component: TypeId::from_raw(reader.u64()?),
                after: decode_fields(reader)?,
            },
            5 => Operation::RemoveComponent {
                node: NodeId::from_u128(reader.u128()?),
                component: TypeId::from_raw(reader.u64()?),
                before: decode_fields(reader)?,
            },
            6 => Operation::SetField {
                node: NodeId::from_u128(reader.u128()?),
                component: TypeId::from_raw(reader.u64()?),
                field: FieldId::from_raw(reader.u64()?),
                before: reader.value()?,
                after: reader.value()?,
            },
            7 => Operation::InstantiatePrefab {
                node: NodeId::from_u128(reader.u128()?),
                prefab: reader.text()?,
                parent: decode_option_node(reader)?,
            },
            8 => Operation::SetOverride {
                node: NodeId::from_u128(reader.u128()?),
                component: TypeId::from_raw(reader.u64()?),
                field: FieldId::from_raw(reader.u64()?),
                before: decode_option_value(reader)?,
                after: decode_option_value(reader)?,
            },
            9 => Operation::SetLayer {
                node: NodeId::from_u128(reader.u128()?),
                before: reader.text()?,
                after: reader.text()?,
            },
            10 => Operation::SetAssetReference {
                node: NodeId::from_u128(reader.u128()?),
                component: TypeId::from_raw(reader.u64()?),
                field: FieldId::from_raw(reader.u64()?),
                before: reader.text()?,
                after: reader.text()?,
            },
            11 => Operation::Domain {
                node: decode_option_node(reader)?,
                kind: reader.text()?,
                before: reader.bytes()?,
                after: reader.bytes()?,
            },
            other => {
                return Err(cy_editor_core::problem::Problem::new(
                    "decode an operation",
                    format!("operation tag {other} is not one this build knows"),
                )
                .with_remedy("the journal was written by a newer editor; open it with that one"));
            }
        };
        Ok(operation)
    }
}

fn encode_option_node(writer: &mut Writer, node: Option<NodeId>) {
    match node {
        Some(node) => {
            writer.u8(1);
            writer.u128(node.as_u128());
        }
        None => writer.u8(0),
    }
}

fn decode_option_node(reader: &mut Reader<'_>) -> Result<Option<NodeId>> {
    Ok(if reader.u8()? == 1 {
        Some(NodeId::from_u128(reader.u128()?))
    } else {
        None
    })
}

fn encode_option_value(writer: &mut Writer, value: Option<&Value>) {
    match value {
        Some(value) => {
            writer.u8(1);
            writer.value(value);
        }
        None => writer.u8(0),
    }
}

fn decode_option_value(reader: &mut Reader<'_>) -> Result<Option<Value>> {
    Ok(if reader.u8()? == 1 {
        Some(reader.value()?)
    } else {
        None
    })
}

pub(crate) fn encode_fields(writer: &mut Writer, fields: &[(FieldId, Value)]) {
    writer.u32(u32::try_from(fields.len()).unwrap_or(u32::MAX));
    for (field, value) in fields {
        writer.u64(field.as_u64());
        writer.value(value);
    }
}

pub(crate) fn decode_fields(reader: &mut Reader<'_>) -> Result<Vec<(FieldId, Value)>> {
    let count = reader.u32()? as usize;
    let mut fields = Vec::with_capacity(count.min(1024));
    for _ in 0..count {
        fields.push((FieldId::from_raw(reader.u64()?), reader.value()?));
    }
    Ok(fields)
}
