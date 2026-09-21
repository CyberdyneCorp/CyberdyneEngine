// SPDX-License-Identifier: MIT
//! Authoritative state for one semantic three-way merge.
//!
//! The service owns the pending decisions. Views only read rows from it, and registered commands
//! are the only path that changes a decision or commits the resulting operations.

use cy_editor_core::ids::DocumentId;
use cy_editor_core::observe::Revision;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_documents::diff::{self, Conflict};
use cy_editor_documents::{Document, Operation};

/// A conflict decision already made by the user.
#[derive(Clone, PartialEq, Debug)]
pub enum MergeDecision {
    /// Keep the local state. No operation is required because the open document is local.
    Local,
    /// Apply the incoming semantic change.
    Incoming(Vec<Operation>),
    /// Apply a validated, user-provided value.
    Replacement(Operation),
}

/// One pending semantic merge.
#[derive(Clone, Debug)]
pub struct MergeSession {
    document: DocumentId,
    path: String,
    base_revision: String,
    incoming_revision: String,
    base: cy_editor_documents::DocumentContent,
    local: cy_editor_documents::DocumentContent,
    local_changes: Vec<cy_editor_documents::diff::Change>,
    incoming_changes: Vec<cy_editor_documents::diff::Change>,
    automatic: Vec<Operation>,
    conflicts: Vec<Conflict>,
    decisions: Vec<Option<MergeDecision>>,
}

impl MergeSession {
    /// The open document that receives the merge.
    #[must_use]
    pub const fn document(&self) -> DocumentId {
        self.document
    }

    /// Project-relative asset path.
    #[must_use]
    pub fn path(&self) -> &str {
        &self.path
    }

    /// Provider revision used as the common base.
    #[must_use]
    pub fn base_revision(&self) -> &str {
        &self.base_revision
    }

    /// Provider revision being merged into the local document.
    #[must_use]
    pub fn incoming_revision(&self) -> &str {
        &self.incoming_revision
    }

    /// Changes that did not overlap local work.
    #[must_use]
    pub fn automatic(&self) -> &[Operation] {
        &self.automatic
    }

    /// Semantic differences from the base revision to the open local document.
    #[must_use]
    pub fn local_changes(&self) -> &[cy_editor_documents::diff::Change] {
        &self.local_changes
    }

    /// Semantic differences from the base revision to the incoming revision.
    #[must_use]
    pub fn incoming_changes(&self) -> &[cy_editor_documents::diff::Change] {
        &self.incoming_changes
    }

    /// Typed conflicts that require an explicit decision.
    #[must_use]
    pub fn conflicts(&self) -> &[Conflict] {
        &self.conflicts
    }

    /// Decisions in the same stable order as [`Self::conflicts`].
    #[must_use]
    pub fn decisions(&self) -> &[Option<MergeDecision>] {
        &self.decisions
    }

    /// Whether every conflict has an explicit decision.
    #[must_use]
    pub fn ready(&self) -> bool {
        self.decisions.iter().all(Option::is_some)
    }

    fn operations(&self) -> Option<Vec<Operation>> {
        if !self.ready() {
            return None;
        }
        let mut operations = self.automatic.clone();
        for decision in self.decisions.iter().flatten() {
            match decision {
                MergeDecision::Local => {}
                MergeDecision::Incoming(incoming) => operations.extend(incoming.iter().cloned()),
                MergeDecision::Replacement(operation) => operations.push(operation.clone()),
            }
        }
        Some(operations)
    }
}

/// The editor's pending semantic merge, if any.
#[derive(Default, Debug)]
pub struct SemanticMergeService {
    revision: Revision,
    session: Option<MergeSession>,
}

impl SemanticMergeService {
    /// Observable revision for view-model refresh.
    #[must_use]
    pub const fn revision(&self) -> Revision {
        self.revision
    }

    /// Current session, if a comparison has been prepared.
    #[must_use]
    pub const fn session(&self) -> Option<&MergeSession> {
        self.session.as_ref()
    }

    /// Compare `base -> local` and `base -> incoming` without mutating the open document.
    pub fn begin(
        &mut self,
        local: &Document,
        base: &Document,
        incoming: &Document,
        base_revision: impl Into<String>,
        incoming_revision: impl Into<String>,
    ) -> Result<()> {
        if local.id() != base.id() || local.id() != incoming.id() {
            return Err(Problem::new(
                "start a semantic merge",
                "base, local, and incoming content do not identify the same document",
            ));
        }
        let local_changes = diff::diff(base.content(), local.content());
        let incoming_changes = diff::diff(base.content(), incoming.content());
        let merged = diff::merge(base.content(), local.content(), incoming.content());
        self.session = Some(MergeSession {
            document: local.id(),
            path: local.assets().first().cloned().unwrap_or_default(),
            base_revision: base_revision.into(),
            incoming_revision: incoming_revision.into(),
            base: base.content().clone(),
            local: local.content().clone(),
            local_changes,
            incoming_changes,
            automatic: merged.operations,
            decisions: vec![None; merged.conflicts.len()],
            conflicts: merged.conflicts,
        });
        self.bump_revision();
        Ok(())
    }

    /// Record one explicit decision, validating replacements against the typed conflict.
    pub fn resolve(
        &mut self,
        index: usize,
        choice: &str,
        replacement: Option<Value>,
    ) -> Result<()> {
        let session = self
            .session
            .as_mut()
            .ok_or_else(|| Problem::not_found("a pending semantic merge"))?;
        let conflict = session.conflicts.get(index).ok_or_else(|| {
            Problem::not_found(format!("merge conflict {index}"))
                .with_remedy("refresh the Merge panel and choose a listed conflict")
        })?;
        let decision = match choice {
            "local" => MergeDecision::Local,
            "incoming" => MergeDecision::Incoming(diff::incoming_resolution_operations(
                conflict,
                &session.base,
                &session.local,
            )?),
            "replacement" => MergeDecision::Replacement(diff::replacement_operation(
                conflict,
                replacement.ok_or_else(|| {
                    Problem::new(
                        "resolve a merge conflict",
                        "no replacement value was supplied",
                    )
                })?,
            )?),
            other => {
                return Err(Problem::new(
                    "resolve a merge conflict",
                    format!("{other:?} is not a merge decision"),
                )
                .with_remedy("choose local, incoming, or replacement"));
            }
        };
        session.decisions[index] = Some(decision);
        self.bump_revision();
        Ok(())
    }

    /// Resolved operations, only after every conflict has an explicit decision.
    #[must_use]
    pub fn resolved_operations(&self) -> Option<(DocumentId, Vec<Operation>)> {
        let session = self.session.as_ref()?;
        Some((session.document, session.operations()?))
    }

    /// Clear the session after its single transaction committed.
    pub fn finish(&mut self) {
        self.session = None;
        self.bump_revision();
    }

    fn bump_revision(&mut self) {
        self.revision = Revision::from_u64(self.revision.as_u64() + 1);
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::value::{Value, ValueKind};

    use super::*;

    fn base_document() -> (
        Document,
        cy_editor_core::ids::NodeId,
        cy_editor_core::ids::TypeId,
        cy_editor_core::ids::FieldId,
    ) {
        let mut document = Document::new("worlds/merge.cyworld");
        let component = document.schema_mut().declare_type("Health", false);
        let field = document
            .schema_mut()
            .declare_field(component, "value", ValueKind::Int, "Health")
            .unwrap();
        let node = document
            .with_transaction("Seed", Actor::system("test"), |document| {
                let node = document.create_node(None)?;
                document.add_component(node, component, vec![(field, Value::Int(1))])?;
                Ok(node)
            })
            .unwrap();
        (document, node, component, field)
    }

    fn edit(
        document: &mut Document,
        node: cy_editor_core::ids::NodeId,
        component: cy_editor_core::ids::TypeId,
        field: cy_editor_core::ids::FieldId,
        value: i64,
    ) {
        document
            .with_transaction("Edit", Actor::human("designer"), |document| {
                document.set_field(node, component, field, Value::Int(value))
            })
            .unwrap();
    }

    #[test]
    fn final_resolution_is_one_transaction_and_undo_restores_local_state() {
        let (base, node, component, field) = base_document();
        let mut local = base.fork();
        let mut incoming = base.fork();
        edit(&mut local, node, component, field, 2);
        edit(&mut incoming, node, component, field, 3);

        let before = local.content().node(node).cloned().unwrap();
        let history_before = local.history().entries().len();
        let mut service = SemanticMergeService::default();
        service
            .begin(&local, &base, &incoming, "base", "incoming")
            .unwrap();
        assert_eq!(service.session().unwrap().conflicts().len(), 1);
        service.resolve(0, "incoming", None).unwrap();
        let (_, operations) = service.resolved_operations().unwrap();
        local
            .with_transaction(
                "Resolve semantic merge",
                Actor::human("designer"),
                |document| {
                    for operation in operations {
                        document.record(operation)?;
                    }
                    Ok(())
                },
            )
            .unwrap();
        assert_eq!(local.history().entries().len(), history_before + 1);
        assert_eq!(
            local.content().field(node, component, field),
            Some(&Value::Int(3))
        );
        local.undo().unwrap();
        assert_eq!(local.content().node(node), Some(&before));
    }

    #[test]
    fn replacement_is_typed_and_structural_replacements_refuse() {
        let (base, node, component, field) = base_document();
        let mut local = base.fork();
        let mut incoming = base.fork();
        edit(&mut local, node, component, field, 2);
        edit(&mut incoming, node, component, field, 3);
        let mut service = SemanticMergeService::default();
        service
            .begin(&local, &base, &incoming, "base", "incoming")
            .unwrap();
        assert!(
            service
                .resolve(0, "replacement", Some(Value::Text("wrong".into())))
                .is_err()
        );
        service
            .resolve(0, "replacement", Some(Value::Int(9)))
            .unwrap();
        assert!(service.session().unwrap().ready());
    }

    #[test]
    fn accepting_incoming_after_local_structure_removal_undoes_exactly() {
        let (base, node, component, field) = base_document();
        let mut local = base.fork();
        local
            .with_transaction("Remove component", Actor::human("designer"), |document| {
                document.record(Operation::RemoveComponent {
                    node,
                    component,
                    before: vec![(field, Value::Int(1))],
                })
            })
            .unwrap();
        let mut incoming = base.fork();
        edit(&mut incoming, node, component, field, 8);
        let before = local.content().node(node).cloned().unwrap();

        let mut service = SemanticMergeService::default();
        service
            .begin(&local, &base, &incoming, "base", "incoming")
            .unwrap();
        service.resolve(0, "incoming", None).unwrap();
        let (_, operations) = service.resolved_operations().unwrap();
        local
            .with_transaction(
                "Resolve semantic merge",
                Actor::human("designer"),
                |document| {
                    for operation in operations {
                        document.record(operation)?;
                    }
                    Ok(())
                },
            )
            .unwrap();
        assert_eq!(
            local.content().field(node, component, field),
            Some(&Value::Int(8))
        );
        local.undo().unwrap();
        assert_eq!(local.content().node(node), Some(&before));
    }
}
