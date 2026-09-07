//! Who wins when a person and an agent reach for the same object. Task 3.11.
//!
//! `editor-agent-interface`, "The editor stays usable while an agent works":
//!
//! > An agent connection SHALL NOT take exclusive control of the editor. The human SHALL be able to
//! > navigate, select, edit and undo throughout.
//! >
//! > Where a human action and an agent action conflict, **the human action SHALL win**, and the
//! > agent SHALL be told its operation was superseded and why.
//!
//! # Why a claim, rather than a lock
//!
//! A lock would satisfy the second sentence and violate the first: the human would be locked out of
//! whatever the agent held, which is exactly the exclusive control the requirement forbids. So
//! nothing here blocks anybody. The agent states what it is about to work on; the editor keeps
//! working; and when the agent's next invocation arrives, the claim is checked against what has
//! happened to the document since. If a **person** changed one of the claimed objects, the agent's
//! operation is refused as superseded, with what happened and what to do about it.
//!
//! That ordering is the whole of "the human action wins": the human's edit is already in the
//! history, unconditionally, and the agent's is the one that does not happen.
//!
//! # Why the history rather than a revision counter
//!
//! A revision moves when anything in the document moves, so a counter would supersede an agent
//! because a person renamed something on the other side of the level. The history says **who** and
//! **what**, so a claim can be narrow: only a human's change, and only to an object the agent named.
//! A conflict nobody would have noticed is not a conflict, and an interface that reported one would
//! teach an agent to re-read the world after every keystroke.
//!
//! An agent that claims nothing is never superseded. That is the honest default: an agent working on
//! the whole document cannot say what it conflicts with, and inventing a conflict for it would be a
//! guess.

use std::collections::BTreeSet;

use cy_editor_core::Actor;
use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::operation::Operation;
use cy_editor_services::editor::Editor;

/// What an agent said it was about to work on, and when it said so.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Claim {
    /// The document the objects are in.
    pub document: DocumentId,
    /// The objects.
    pub nodes: BTreeSet<NodeId>,
    /// How many history entries the document had when the claim was staked.
    ///
    /// An index rather than a transaction identity, because the question is "what has happened
    /// since", and the answer is the tail of the history from here.
    pub after: usize,
}

impl Claim {
    /// Stake a claim on some objects of the active document.
    pub fn stake(editor: &Editor, nodes: &[NodeId]) -> Result<Self> {
        let document = editor.workspace.active().ok_or_else(|| {
            Problem::new("claim these objects", "no document is active")
                .with_remedy("open a document first")
        })?;
        let after = editor
            .documents
            .get(document)
            .ok_or_else(|| Problem::not_found("the active document"))?
            .history()
            .entries()
            .len();
        Ok(Self {
            document,
            nodes: nodes.iter().copied().collect(),
            after,
        })
    }

    /// Whether a person has changed any of the claimed objects since the claim was staked.
    #[must_use]
    pub fn check(&self, editor: &Editor) -> Conflict {
        let Some(document) = editor.documents.get(self.document) else {
            // The document was closed under the agent, which is a supersession of the broadest
            // kind: nothing it planned still applies.
            return Conflict::Superseded {
                by: Actor::human("someone at the interface"),
                what: "closing the document".to_string(),
                nodes: self.nodes.iter().copied().collect(),
            };
        };
        let entries = document.history().entries();
        for entry in entries.iter().skip(self.after) {
            if entry.actor.is_agent() {
                continue;
            }
            let touched: Vec<NodeId> = entry
                .operations
                .iter()
                .flat_map(nodes_of)
                .filter(|node| self.nodes.contains(node))
                .collect();
            if !touched.is_empty() {
                return Conflict::Superseded {
                    by: entry.actor.clone(),
                    what: entry.name.clone(),
                    nodes: touched,
                };
            }
        }
        Conflict::None
    }
}

/// What the check found.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Conflict {
    /// Nothing a person did affects what the agent claimed.
    None,
    /// A person changed something the agent claimed, and the person's change stands.
    Superseded {
        /// Who did it.
        by: Actor,
        /// What their change was called, in the words their history entry uses.
        what: String,
        /// Which of the claimed objects they touched.
        nodes: Vec<NodeId>,
    },
}

impl Conflict {
    /// Whether the agent's operation may go ahead.
    #[must_use]
    pub const fn is_clear(&self) -> bool {
        matches!(self, Conflict::None)
    }
}

/// Which objects an operation touched.
///
/// Exhaustive rather than a wildcard: a new operation kind that fell through a `_` arm would make a
/// conflict invisible, and an invisible conflict is an agent overwriting a person's work.
fn nodes_of(operation: &Operation) -> Vec<NodeId> {
    match operation {
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
        | Operation::SetAssetReference { node, .. } => vec![*node],
        Operation::Domain { node, .. } => node.iter().copied().collect(),
    }
}
