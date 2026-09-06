//! Selection as typed sets addressed by stable identity, and the multi-edit that is one transaction.
//!
//! `editor-documents-and-transactions`: "Selection SHALL be a **service**, not per-panel state,
//! holding typed selection sets — entities, assets, components, graph nodes, and plugin-defined
//! domains — addressed by stable identity."
//!
//! The *service* is in `cy-editor-services`, because a service is a thing panels observe and this
//! crate has no panels. What lives here is the model it holds, and the two rules that are properties
//! of the model rather than of the service: selection is re-resolved by identity after a reload, and
//! editing a value shared by five hundred selected objects produces **one** transaction.

use std::collections::{BTreeMap, BTreeSet};

use cy_editor_core::Actor;
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::problem::Result;
use cy_editor_core::value::Value;

use crate::document::Document;

/// What is selected, by domain.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Selection {
    nodes: BTreeSet<NodeId>,
    assets: BTreeSet<String>,
    /// Plugin-defined domains, keyed by the domain's name. A domain the editor has never heard of
    /// is still selectable, which is what "plugin-defined domains" requires.
    other: BTreeMap<String, BTreeSet<String>>,
}

impl Selection {
    /// Nothing selected.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The selected nodes, in identity order.
    pub fn nodes(&self) -> impl Iterator<Item = NodeId> + '_ {
        self.nodes.iter().copied()
    }

    /// How many nodes are selected.
    #[must_use]
    pub fn node_count(&self) -> usize {
        self.nodes.len()
    }

    /// Whether nothing at all is selected.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.nodes.is_empty() && self.assets.is_empty() && self.other.is_empty()
    }

    /// Replace the node selection.
    pub fn set_nodes(&mut self, nodes: impl IntoIterator<Item = NodeId>) {
        self.nodes = nodes.into_iter().collect();
    }

    /// Add a node to the selection.
    pub fn add_node(&mut self, node: NodeId) {
        self.nodes.insert(node);
    }

    /// Remove a node from the selection.
    pub fn remove_node(&mut self, node: NodeId) {
        self.nodes.remove(&node);
    }

    /// The selected assets, by project-relative path.
    pub fn assets(&self) -> impl Iterator<Item = &str> {
        self.assets.iter().map(String::as_str)
    }

    /// Replace the asset selection.
    pub fn set_assets(&mut self, assets: impl IntoIterator<Item = String>) {
        self.assets = assets.into_iter().collect();
    }

    /// Select in a plugin-defined domain.
    pub fn set_domain(
        &mut self,
        domain: impl Into<String>,
        items: impl IntoIterator<Item = String>,
    ) {
        self.other
            .insert(domain.into(), items.into_iter().collect());
    }

    /// What is selected in a plugin-defined domain.
    pub fn domain(&self, domain: &str) -> impl Iterator<Item = &str> {
        self.other
            .get(domain)
            .into_iter()
            .flatten()
            .map(String::as_str)
    }

    /// Clear everything.
    pub fn clear(&mut self) {
        *self = Self::default();
    }

    /// Drop selected nodes that no longer exist, after a reload.
    ///
    /// "WHEN a document is reloaded THEN selection SHALL be re-resolved by identity rather than lost
    /// or pointing at the wrong objects." Re-resolving by identity is what this is: nothing is
    /// remapped, because a [`NodeId`] means the same node it always did — what changes is only
    /// whether it is still there.
    pub fn reresolve(&mut self, document: &Document) {
        self.nodes
            .retain(|node| document.content().node(*node).is_some());
    }

    /// What the inspector should show for one field across the selection.
    ///
    /// `Mixed` is a value in its own right rather than an absence, because an inspector that showed
    /// a blank for differing values would be indistinguishable from one showing an empty string.
    #[must_use]
    pub fn common_value(
        &self,
        document: &Document,
        component: TypeId,
        field: FieldId,
    ) -> CommonValue {
        let mut values = self
            .nodes
            .iter()
            .filter_map(|node| document.content().field(*node, component, field));
        let Some(first) = values.next() else {
            return CommonValue::None;
        };
        if values.all(|value| value == first) {
            CommonValue::Same(first.clone())
        } else {
            CommonValue::Mixed
        }
    }

    /// The component types every selected node carries.
    ///
    /// What a multi-selection inspector shows: "common components are shown, differing values are
    /// presented as mixed".
    #[must_use]
    pub fn common_components(&self, document: &Document) -> Vec<TypeId> {
        let mut nodes = self.nodes.iter();
        let Some(first) = nodes.next().and_then(|node| document.content().node(*node)) else {
            return Vec::new();
        };
        let mut common: Vec<TypeId> = first.components.keys().copied().collect();
        for node in nodes {
            common.retain(|component| document.content().has_component(*node, *component));
        }
        common
    }

    /// Set one field on every selected node, as **one** transaction.
    ///
    /// "WHEN five hundred entities are selected and a shared field is changed THEN one transaction
    /// SHALL be recorded affecting all of them." One transaction, five hundred operations, each
    /// carrying its own before value — so undoing restores five hundred *different* previous values
    /// rather than one shared one.
    pub fn set_field_on_all(
        &self,
        document: &mut Document,
        name: impl Into<String>,
        actor: Actor,
        component: TypeId,
        field: FieldId,
        value: &Value,
    ) -> Result<usize> {
        let targets: Vec<NodeId> = self
            .nodes
            .iter()
            .copied()
            .filter(|node| document.content().field(*node, component, field).is_some())
            .collect();
        document.with_transaction(name, actor, |document| {
            for node in &targets {
                document.set_field(*node, component, field, value.clone())?;
            }
            Ok(targets.len())
        })
    }
}

/// What one field looks like across a selection.
#[derive(Clone, PartialEq, Debug)]
pub enum CommonValue {
    /// Nothing selected has that field.
    None,
    /// Everything selected agrees.
    Same(Value),
    /// The selected objects disagree. Shown as "mixed", and editing it writes to all of them.
    Mixed,
}

#[cfg(test)]
mod tests {
    use cy_editor_core::value::ValueKind;

    use super::*;

    fn document_with(count: usize) -> (Document, Vec<NodeId>, TypeId, FieldId) {
        let mut document = Document::new("worlds/city.cyworld");
        let health = document.schema_mut().declare_type("Health", false);
        let value = document
            .schema_mut()
            .declare_field(health, "value", ValueKind::Float, "hit points remaining")
            .unwrap();
        let nodes = document
            .with_transaction("Populate", Actor::human("designer"), |document| {
                let mut nodes = Vec::new();
                for index in 0..count {
                    let node = document.create_node(None)?;
                    let initial = f32::from(u16::try_from(index).expect("a test's node count"));
                    document.add_component(node, health, vec![(value, Value::Float(initial))])?;
                    nodes.push(node);
                }
                Ok(nodes)
            })
            .unwrap();
        (document, nodes, health, value)
    }

    #[test]
    fn a_bulk_edit_of_five_hundred_objects_is_one_entry() {
        let (mut document, nodes, health, value) = document_with(500);
        let mut selection = Selection::new();
        selection.set_nodes(nodes.iter().copied());
        let entries = document.history().entries().len();

        let changed = selection
            .set_field_on_all(
                &mut document,
                "Set health",
                Actor::human("designer"),
                health,
                value,
                &Value::Float(100.0),
            )
            .unwrap();

        assert_eq!(changed, 500);
        assert_eq!(document.history().entries().len(), entries + 1, "one entry");
        assert_eq!(
            document
                .history()
                .entries()
                .last()
                .unwrap()
                .operations
                .len(),
            500
        );
    }

    #[test]
    fn undoing_a_bulk_edit_restores_each_objects_own_previous_value() {
        let (mut document, nodes, health, value) = document_with(4);
        let mut selection = Selection::new();
        selection.set_nodes(nodes.iter().copied());
        selection
            .set_field_on_all(
                &mut document,
                "Set health",
                Actor::human("designer"),
                health,
                value,
                &Value::Float(100.0),
            )
            .unwrap();

        document.undo().unwrap();
        for (index, node) in nodes.iter().enumerate() {
            assert_eq!(
                document.content().field(*node, health, value),
                Some(&Value::Float(f32::from(u16::try_from(index).unwrap()))),
                "each object kept its own before value"
            );
        }
    }

    #[test]
    fn differing_values_are_mixed_rather_than_blank() {
        let (document, nodes, health, value) = document_with(3);
        let mut selection = Selection::new();
        selection.set_nodes(nodes.iter().copied());
        assert_eq!(
            selection.common_value(&document, health, value),
            CommonValue::Mixed
        );

        selection.set_nodes([nodes[1]]);
        assert_eq!(
            selection.common_value(&document, health, value),
            CommonValue::Same(Value::Float(1.0))
        );
    }

    #[test]
    fn selection_survives_a_reload_by_identity() {
        let (mut document, nodes, ..) = document_with(3);
        let mut selection = Selection::new();
        selection.set_nodes(nodes.iter().copied());

        document
            .with_transaction("Delete one", Actor::human("designer"), |document| {
                document.delete_node(nodes[1])
            })
            .unwrap();

        selection.reresolve(&document);
        let remaining: Vec<NodeId> = selection.nodes().collect();
        assert_eq!(remaining.len(), 2);
        assert!(
            !remaining.contains(&nodes[1]),
            "the deleted node is dropped"
        );
        assert!(
            remaining.contains(&nodes[0]),
            "and the survivors keep pointing at themselves"
        );
    }

    #[test]
    fn common_components_are_the_intersection() {
        let (mut document, nodes, health, _) = document_with(2);
        let armour = document.schema_mut().declare_type("Armour", false);
        document
            .with_transaction("Armour one", Actor::human("designer"), |document| {
                document.add_component(nodes[0], armour, Vec::new())
            })
            .unwrap();

        let mut selection = Selection::new();
        selection.set_nodes(nodes.iter().copied());
        assert_eq!(selection.common_components(&document), vec![health]);
    }
}
