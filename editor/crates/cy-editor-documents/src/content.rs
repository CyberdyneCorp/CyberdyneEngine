//! A document's content, and the token without which it cannot be changed. Task 3.3.
//!
//! `editor-documents-and-transactions`: "Every mutation of persistent project state SHALL be
//! recorded as a **transaction** against a document. Tools, panels, gizmos, importers, and plugins
//! SHALL have no other write path."
//!
//! --- HOW "NO OTHER WRITE PATH" IS A TYPE RATHER THAN A RULE -------------------------------------------
//!
//! [`DocumentContent`]'s fields are private and its only mutating method is [`DocumentContent::apply`],
//! which takes a [`WriteToken`]. `WriteToken` has a private field and no public constructor, so no
//! code outside this crate can make one — there is nothing to call. A panel, a gizmo, an importer or
//! a plugin that wanted to write around the transaction system would have to be *in this crate*.
//!
//! That is stronger than a review rule and cheaper than an audit, and it is the reason this crate
//! exposes a content model at all rather than letting each editor keep its own: the specification's
//! "Every editor SHALL use this document model rather than implementing its own save and history" is
//! only enforceable if the model is the one with the write path.
//!
//! The counters exist for the case the type system cannot reach — a bypass added *within* this
//! crate — and [`crate::audit`] is what compares them. Task 3.9 is the test that adds one.

use std::collections::BTreeMap;

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::ids::{DocumentId, FieldId, NodeId, TypeId};
use cy_editor_core::observe::Revision;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;

use crate::operation::{Operation, decode_fields, encode_fields};

/// Proof that a mutation is part of a transaction.
///
/// The private field is the whole mechanism: a struct with a private field cannot be constructed
/// outside the module that declares it, so [`DocumentContent::apply`] cannot be called from outside
/// this crate at all. Nothing about this is enforced by convention.
pub struct WriteToken {
    /// Which transaction is writing. Carried so that the audit trail names it rather than merely
    /// counting.
    transaction: u64,
}

impl WriteToken {
    /// Issue a token. Callable only inside this crate, by the transaction system.
    pub(crate) const fn issue(transaction: u64) -> Self {
        Self { transaction }
    }

    /// Which transaction this token belongs to.
    #[must_use]
    pub const fn transaction(&self) -> u64 {
        self.transaction
    }
}

/// One node's whole state: what a deletion removes and a restore puts back.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct NodeState {
    /// The node's parent, or `None` for a root.
    pub parent: Option<NodeId>,
    /// The node's children, in authored order.
    pub children: Vec<NodeId>,
    /// The layer the node belongs to.
    pub layer: String,
    /// The prefab this node is an instance of, when it is one.
    pub prefab: Option<String>,
    /// Its components and their fields.
    pub components: BTreeMap<TypeId, BTreeMap<FieldId, Value>>,
    /// Fields overridden on a prefab instance.
    pub overrides: BTreeMap<(TypeId, FieldId), Value>,
}

impl NodeState {
    /// A node with a parent and nothing else.
    #[must_use]
    pub fn empty(parent: Option<NodeId>) -> Self {
        Self {
            parent,
            ..Self::default()
        }
    }

    /// Append this state to a byte stream.
    pub fn encode(&self, writer: &mut Writer) {
        match self.parent {
            Some(parent) => {
                writer.u8(1);
                writer.u128(parent.as_u128());
            }
            None => writer.u8(0),
        }
        writer.u32(u32::try_from(self.children.len()).unwrap_or(u32::MAX));
        for child in &self.children {
            writer.u128(child.as_u128());
        }
        writer.text(&self.layer);
        match &self.prefab {
            Some(prefab) => {
                writer.u8(1);
                writer.text(prefab);
            }
            None => writer.u8(0),
        }
        writer.u32(u32::try_from(self.components.len()).unwrap_or(u32::MAX));
        for (component, fields) in &self.components {
            writer.u64(component.as_u64());
            let flat: Vec<(FieldId, Value)> = fields
                .iter()
                .map(|(field, value)| (*field, value.clone()))
                .collect();
            encode_fields(writer, &flat);
        }
        writer.u32(u32::try_from(self.overrides.len()).unwrap_or(u32::MAX));
        for ((component, field), value) in &self.overrides {
            writer.u64(component.as_u64());
            writer.u64(field.as_u64());
            writer.value(value);
        }
    }

    /// Read a node's state back.
    pub fn decode(reader: &mut Reader<'_>) -> Result<Self> {
        let parent = if reader.u8()? == 1 {
            Some(NodeId::from_u128(reader.u128()?))
        } else {
            None
        };
        let child_count = reader.u32()? as usize;
        let mut children = Vec::with_capacity(child_count.min(4096));
        for _ in 0..child_count {
            children.push(NodeId::from_u128(reader.u128()?));
        }
        let layer = reader.text()?;
        let prefab = if reader.u8()? == 1 {
            Some(reader.text()?)
        } else {
            None
        };

        let component_count = reader.u32()? as usize;
        let mut components = BTreeMap::new();
        for _ in 0..component_count {
            let component = TypeId::from_raw(reader.u64()?);
            let fields = decode_fields(reader)?;
            components.insert(component, fields.into_iter().collect());
        }

        let override_count = reader.u32()? as usize;
        let mut overrides = BTreeMap::new();
        for _ in 0..override_count {
            let component = TypeId::from_raw(reader.u64()?);
            let field = FieldId::from_raw(reader.u64()?);
            overrides.insert((component, field), reader.value()?);
        }

        Ok(Self {
            parent,
            children,
            layer,
            prefab,
            components,
            overrides,
        })
    }
}

/// A document's authoring content: nodes, their components, and their hierarchy.
///
/// **Children are kept in authored order**, unlike the ECS's, whose order is storage order and whose
/// removal swaps the last child into the gap. `cy_abi.h` states that in capitals for the runtime
/// side; the authoring side is the side where the order is a designer's decision, so this is where
/// it is kept.
#[derive(Clone, PartialEq, Debug)]
pub struct DocumentContent {
    document: DocumentId,
    nodes: BTreeMap<NodeId, NodeState>,
    roots: Vec<NodeId>,
    next_ordinal: u64,
    revision: Revision,
    /// Every mutation, however it arrived.
    writes: u64,
    /// Mutations that carried a [`WriteToken`]. Equal to `writes` in a correct build.
    transactional_writes: u64,
}

impl DocumentContent {
    /// Empty content for a document.
    #[must_use]
    pub fn new(document: DocumentId) -> Self {
        Self {
            document,
            nodes: BTreeMap::new(),
            roots: Vec::new(),
            next_ordinal: 1,
            revision: Revision::INITIAL,
            writes: 0,
            transactional_writes: 0,
        }
    }

    /// Which document this is the content of.
    #[must_use]
    pub const fn document(&self) -> DocumentId {
        self.document
    }

    /// The content's revision, which moves on every mutation.
    #[must_use]
    pub const fn revision(&self) -> Revision {
        self.revision
    }

    /// Allocate the next node identity. Ordinals are never reused; see [`NodeId::in_document`].
    ///
    /// Not a mutation of content — no node exists yet — so it needs no token. What it consumes is an
    /// ordinal, and consuming one twice for the same node would break the "undo restores the same
    /// identity" property, which is why it is here and not in the caller.
    pub fn allocate_node(&mut self) -> NodeId {
        let node = NodeId::in_document(self.document, self.next_ordinal);
        self.next_ordinal += 1;
        node
    }

    /// Every node's identity, in identity order.
    pub fn nodes(&self) -> impl Iterator<Item = NodeId> + '_ {
        self.nodes.keys().copied()
    }

    /// How many nodes the document holds.
    #[must_use]
    pub fn node_count(&self) -> usize {
        self.nodes.len()
    }

    /// The root nodes, in authored order.
    #[must_use]
    pub fn roots(&self) -> &[NodeId] {
        &self.roots
    }

    /// One node's state.
    #[must_use]
    pub fn node(&self, node: NodeId) -> Option<&NodeState> {
        self.nodes.get(&node)
    }

    /// One field's value, or `None` when the node, the component or the field is absent.
    #[must_use]
    pub fn field(&self, node: NodeId, component: TypeId, field: FieldId) -> Option<&Value> {
        self.nodes
            .get(&node)?
            .components
            .get(&component)?
            .get(&field)
    }

    /// Whether a node carries a component.
    #[must_use]
    pub fn has_component(&self, node: NodeId, component: TypeId) -> bool {
        self.nodes
            .get(&node)
            .is_some_and(|state| state.components.contains_key(&component))
    }

    /// Apply one operation. **The only way to change a document's content.**
    ///
    /// Fails without changing anything when the operation does not apply — a node that is not there,
    /// a component that is already there. That is what makes a transaction's application
    /// all-or-nothing at the operation level; the transaction system is what makes it
    /// all-or-nothing at the transaction level.
    pub fn apply(&mut self, operation: &Operation, token: &WriteToken) -> Result<()> {
        self.transactional_writes += 1;
        let _ = token.transaction();
        self.apply_inner(operation)
    }

    /// The mutation itself, with no token and no accounting for one.
    ///
    /// Private, and it is the only thing in this crate that is. [`crate::audit`] explains what it is
    /// for and task 3.9's test is what uses the one bypass that reaches it.
    fn apply_inner(&mut self, operation: &Operation) -> Result<()> {
        self.writes += 1;
        self.revision = Revision::from_u64(self.revision.as_u64() + 1);
        match operation {
            Operation::CreateNode { node, parent } => {
                self.insert_node(*node, NodeState::empty(*parent))
            }
            Operation::RestoreNode { node, state } => {
                self.insert_node(*node, state.as_ref().clone())
            }
            Operation::DeleteNode { node, .. } => self.remove_node(*node),
            Operation::Reparent { node, after, .. } => self.reparent(*node, *after),
            Operation::AddComponent {
                node,
                component,
                after,
            } => {
                let state = self.state_mut(*node)?;
                if state.components.contains_key(component) {
                    return Err(Problem::new(
                        "add a component",
                        "the node already has one of that type",
                    )
                    .with_remedy("set its fields instead, or remove it first"));
                }
                state.components.insert(
                    *component,
                    after.iter().cloned().collect::<BTreeMap<_, _>>(),
                );
                Ok(())
            }
            Operation::RemoveComponent {
                node, component, ..
            } => {
                let state = self.state_mut(*node)?;
                state
                    .components
                    .remove(component)
                    .map(|_| ())
                    .ok_or_else(|| Problem::not_found("that component on that node"))
            }
            Operation::SetField {
                node,
                component,
                field,
                after,
                ..
            } => {
                let state = self.state_mut(*node)?;
                let fields = state
                    .components
                    .get_mut(component)
                    .ok_or_else(|| Problem::not_found("that component on that node"))?;
                fields.insert(*field, after.clone());
                Ok(())
            }
            Operation::InstantiatePrefab {
                node,
                prefab,
                parent,
            } => {
                let mut state = NodeState::empty(*parent);
                state.prefab = Some(prefab.clone());
                self.insert_node(*node, state)
            }
            Operation::SetOverride {
                node,
                component,
                field,
                after,
                ..
            } => {
                let state = self.state_mut(*node)?;
                match after {
                    Some(value) => state.overrides.insert((*component, *field), value.clone()),
                    None => state.overrides.remove(&(*component, *field)),
                };
                Ok(())
            }
            Operation::SetLayer { node, after, .. } => {
                self.state_mut(*node)?.layer.clone_from(after);
                Ok(())
            }
            Operation::SetAssetReference {
                node,
                component,
                field,
                after,
                ..
            } => {
                let state = self.state_mut(*node)?;
                let fields = state
                    .components
                    .get_mut(component)
                    .ok_or_else(|| Problem::not_found("that component on that node"))?;
                fields.insert(*field, Value::Text(after.clone()));
                Ok(())
            }
            // A domain payload is opaque to this crate by construction; the tool that wrote it is
            // what applies it, through its own registered handler. Recording it here keeps it in
            // the one operation stream — which is the whole point of the escape hatch having a
            // name — without this crate pretending to understand a terrain stroke.
            Operation::Domain { .. } => Ok(()),
        }
    }

    fn insert_node(&mut self, node: NodeId, state: NodeState) -> Result<()> {
        if self.nodes.contains_key(&node) {
            return Err(
                Problem::new("create a node", "a node with that identity already exists")
                    .with_remedy("allocate a new identity; ordinals are never reused"),
            );
        }
        let parent = state.parent;
        let children = state.children.clone();
        self.nodes.insert(node, state);
        match parent {
            Some(parent) => {
                let owner = self
                    .nodes
                    .get_mut(&parent)
                    .ok_or_else(|| Problem::not_found("the parent node"))?;
                if !owner.children.contains(&node) {
                    owner.children.push(node);
                }
            }
            None => self.roots.push(node),
        }
        // A restored node's children were restored with it, and each of them still names it as a
        // parent — they were never removed. Nothing to relink; asserted by keeping the list.
        let _ = children;
        Ok(())
    }

    fn remove_node(&mut self, node: NodeId) -> Result<()> {
        let state = self
            .nodes
            .remove(&node)
            .ok_or_else(|| Problem::not_found("that node"))?;
        match state.parent {
            Some(parent) => {
                if let Some(owner) = self.nodes.get_mut(&parent) {
                    owner.children.retain(|child| *child != node);
                }
            }
            None => self.roots.retain(|root| *root != node),
        }
        for child in state.children {
            self.nodes.remove(&child);
        }
        Ok(())
    }

    fn reparent(&mut self, node: NodeId, parent: Option<NodeId>) -> Result<()> {
        if let Some(parent) = parent {
            if parent == node || self.is_descendant(parent, node) {
                return Err(
                    Problem::new("reparent", "that would put the node inside itself").with_remedy(
                        "choose a parent that is not the node or one of its descendants",
                    ),
                );
            }
            if !self.nodes.contains_key(&parent) {
                return Err(Problem::not_found("the new parent"));
            }
        }
        let previous = self.state_mut(node)?.parent;
        match previous {
            Some(previous) => {
                if let Some(owner) = self.nodes.get_mut(&previous) {
                    owner.children.retain(|child| *child != node);
                }
            }
            None => self.roots.retain(|root| *root != node),
        }
        self.state_mut(node)?.parent = parent;
        match parent {
            Some(parent) => {
                if let Some(owner) = self.nodes.get_mut(&parent) {
                    owner.children.push(node);
                }
            }
            None => self.roots.push(node),
        }
        Ok(())
    }

    fn is_descendant(&self, candidate: NodeId, ancestor: NodeId) -> bool {
        let mut walk = self.nodes.get(&candidate).and_then(|state| state.parent);
        while let Some(node) = walk {
            if node == ancestor {
                return true;
            }
            walk = self.nodes.get(&node).and_then(|state| state.parent);
        }
        false
    }

    fn state_mut(&mut self, node: NodeId) -> Result<&mut NodeState> {
        self.nodes
            .get_mut(&node)
            .ok_or_else(|| Problem::not_found("that node"))
    }

    /// How many mutations this content has seen, and how many of them were transactional.
    ///
    /// Read by [`crate::audit`]. Equal in a correct build; a divergence names a write path that got
    /// around the transaction system.
    #[must_use]
    pub const fn write_counts(&self) -> (u64, u64) {
        (self.writes, self.transactional_writes)
    }

    /// Mutate without a transaction. **Test builds only, and it exists to be caught.**
    ///
    /// This is the hole task 3.9 requires: "The test that writes around the transaction system, and
    /// fails." A test cannot prove that an audit catches a bypass unless a bypass exists, and a
    /// bypass that is compiled into a shipping editor is the defect rather than the proof — so it is
    /// `#[cfg(test)]`, which means it does not exist outside this crate's own test build, and no
    /// feature flag can turn it on.
    #[cfg(test)]
    pub(crate) fn write_around_the_transaction_system(
        &mut self,
        operation: &Operation,
    ) -> Result<()> {
        self.apply_inner(operation)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn content() -> DocumentContent {
        DocumentContent::new(DocumentId::of_asset("worlds/city.cyworld"))
    }

    fn token() -> WriteToken {
        WriteToken::issue(1)
    }

    #[test]
    fn a_node_can_be_created_reparented_and_deleted() {
        let mut content = content();
        let root = content.allocate_node();
        let child = content.allocate_node();
        content
            .apply(
                &Operation::CreateNode {
                    node: root,
                    parent: None,
                },
                &token(),
            )
            .unwrap();
        content
            .apply(
                &Operation::CreateNode {
                    node: child,
                    parent: None,
                },
                &token(),
            )
            .unwrap();
        assert_eq!(content.roots().len(), 2);

        content
            .apply(
                &Operation::Reparent {
                    node: child,
                    before: None,
                    after: Some(root),
                },
                &token(),
            )
            .unwrap();
        assert_eq!(content.roots(), &[root]);
        assert_eq!(content.node(root).unwrap().children, vec![child]);

        content
            .apply(
                &Operation::DeleteNode {
                    node: child,
                    was: Box::new(NodeState::empty(Some(root))),
                },
                &token(),
            )
            .unwrap();
        assert!(content.node(child).is_none());
        assert!(content.node(root).unwrap().children.is_empty());
    }

    #[test]
    fn a_cycle_is_refused_with_a_remedy() {
        let mut content = content();
        let parent = content.allocate_node();
        let child = content.allocate_node();
        content
            .apply(
                &Operation::CreateNode {
                    node: parent,
                    parent: None,
                },
                &token(),
            )
            .unwrap();
        content
            .apply(
                &Operation::CreateNode {
                    node: child,
                    parent: Some(parent),
                },
                &token(),
            )
            .unwrap();

        let problem = content
            .apply(
                &Operation::Reparent {
                    node: parent,
                    before: None,
                    after: Some(child),
                },
                &token(),
            )
            .unwrap_err();
        assert!(problem.remedy.is_some(), "{problem}");
    }

    #[test]
    fn the_revision_moves_on_every_mutation() {
        let mut content = content();
        let before = content.revision();
        let node = content.allocate_node();
        content
            .apply(&Operation::CreateNode { node, parent: None }, &token())
            .unwrap();
        assert!(content.revision() > before);
    }

    #[test]
    fn allocating_an_identity_is_not_a_mutation() {
        let mut content = content();
        let before = content.revision();
        let _ = content.allocate_node();
        let _ = content.allocate_node();
        assert_eq!(
            content.revision(),
            before,
            "allocating an ordinal changes no content"
        );
    }

    #[test]
    fn a_node_state_round_trips_through_the_codec() {
        let mut state = NodeState::empty(None);
        state.layer = "gameplay".into();
        state.prefab = Some("prefabs/lamp.cyprefab".into());
        state.components.insert(
            TypeId::from_raw(3),
            BTreeMap::from([(FieldId::from_raw(9), Value::Float(2.0))]),
        );
        state.overrides.insert(
            (TypeId::from_raw(3), FieldId::from_raw(9)),
            Value::Float(4.0),
        );

        let mut writer = Writer::new();
        state.encode(&mut writer);
        let bytes = writer.finish();
        let mut reader = Reader::new(&bytes);
        assert_eq!(NodeState::decode(&mut reader).unwrap(), state);
        assert!(reader.is_empty());
    }
}
