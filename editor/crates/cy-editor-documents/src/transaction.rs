//! Transactions: what a change *is*, who made it, and what it can be merged with. Tasks 3.3, 3.5, 3.8.
//!
//! A transaction "SHALL carry a user-facing name, an ordered list of typed operations, and the
//! document it applies to", and "SHALL record the **actor** that produced it — the human user, or
//! the agent, session and stated intent". All four are fields below, and none of them is optional:
//! a transaction with no actor is exactly the history entry a reviewer cannot act on, and the type
//! has no way to spell one.
//!
//! --- WHY COALESCING IS KEYED RATHER THAN TIMED ------------------------------------------------------
//!
//! The specification says coalescing merges "consecutive compatible operations within a time or
//! interaction window — typing a name, dragging a slider". This implementation takes the interaction
//! window, as an explicit [`Transaction::coalesce_key`] the caller supplies, and not the time one.
//!
//! Two reasons. A time window makes history depend on how fast the machine was, so the same drag
//! produces one entry on a developer's desktop and three on a loaded continuous-integration runner —
//! and a test asserting "a gizmo drag is one entry" would be flaky rather than wrong. And the caller
//! genuinely knows: a slider drag is one interaction from the first press to the release, which is
//! information the widget has and a clock has to guess at.

use cy_editor_core::Actor;
use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::ids::DocumentId;
use cy_editor_core::problem::Result;

use crate::operation::Operation;

/// A transaction's identity within a session.
///
/// Session-scoped rather than persistent: a journal records the transaction's *content*, and a
/// recovered journal replays operations rather than resuming identities. Making it persistent would
/// be a second identity space to keep unique across restarts for no reader that needs it.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct TransactionId(u64);

impl TransactionId {
    /// A transaction identity with a given number.
    #[must_use]
    pub const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    /// The number.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }
}

/// One committed change to one document.
#[derive(Clone, PartialEq, Debug)]
pub struct Transaction {
    /// Its identity within the session.
    pub id: TransactionId,
    /// The document it applies to. Histories are per document, so this is also its home.
    pub document: DocumentId,
    /// What a person reads in the undo menu: "Move 3 objects", not "SetField × 9".
    pub name: String,
    /// Who produced it. Required — see the module note.
    pub actor: Actor,
    /// The operations, in the order they were applied.
    pub operations: Vec<Operation>,
    /// The interaction this transaction belongs to, when it belongs to one.
    ///
    /// Two consecutive transactions with the same key and the same actor coalesce into one history
    /// entry. `None` never coalesces, which is the right default: an ordinary edit is its own entry.
    pub coalesce_key: Option<String>,
}

impl Transaction {
    /// How many bytes this transaction's payloads occupy, for the history budget.
    #[must_use]
    pub fn payload_bytes(&self) -> usize {
        self.operations
            .iter()
            .map(Operation::payload_bytes)
            .sum::<usize>()
            + self.name.len()
    }

    /// The transaction that exactly reverses this one: inverted operations, in reverse order.
    #[must_use]
    pub fn inverse(&self) -> Self {
        Self {
            id: self.id,
            document: self.document,
            name: self.name.clone(),
            actor: self.actor.clone(),
            operations: self
                .operations
                .iter()
                .rev()
                .map(Operation::inverse)
                .collect(),
            coalesce_key: None,
        }
    }

    /// Whether `next` may be merged into this entry.
    ///
    /// Both must name the same interaction and the same actor. The actor test is not ceremony: a
    /// human nudge and an agent's edit that happened to carry the same key are two changes with two
    /// authors, and merging them would produce one history entry attributed to one of them.
    #[must_use]
    pub fn can_coalesce_with(&self, next: &Self) -> bool {
        match (&self.coalesce_key, &next.coalesce_key) {
            (Some(ours), Some(theirs)) => {
                ours == theirs && self.actor == next.actor && self.document == next.document
            }
            _ => false,
        }
    }

    /// Merge `next` into this transaction, collapsing operations that address the same target.
    pub fn coalesce(&mut self, next: Transaction) {
        for operation in next.operations {
            match self
                .operations
                .last()
                .and_then(|last| last.coalesce(&operation))
            {
                Some(merged) => {
                    let last = self.operations.last_mut().expect("just matched on it");
                    *last = merged;
                }
                None => self.operations.push(operation),
            }
        }
        self.name = next.name;
    }

    /// Collapse consecutive operations within this transaction that address the same target.
    ///
    /// A gizmo drag records hundreds of `SetField`s on one field; this turns them into one, before
    /// the entry ever reaches history or the journal. Undo is unaffected — the first before value
    /// and the last after value are what an exact undo needs, and they are what survive.
    pub fn compact(&mut self) {
        let mut compacted: Vec<Operation> = Vec::with_capacity(self.operations.len());
        for operation in self.operations.drain(..) {
            match compacted.last().and_then(|last| last.coalesce(&operation)) {
                Some(merged) => *compacted.last_mut().expect("just matched on it") = merged,
                None => compacted.push(operation),
            }
        }
        self.operations = compacted;
    }

    /// Append this transaction to a byte stream: the journal's record and the bridge's message.
    pub fn encode(&self, writer: &mut Writer) {
        writer.u64(self.id.as_u64());
        writer.u128(self.document.as_u128());
        writer.text(&self.name);
        encode_actor(writer, &self.actor);
        match &self.coalesce_key {
            Some(key) => {
                writer.u8(1);
                writer.text(key);
            }
            None => writer.u8(0),
        }
        writer.u32(u32::try_from(self.operations.len()).unwrap_or(u32::MAX));
        for operation in &self.operations {
            operation.encode(writer);
        }
    }

    /// Read a transaction back.
    pub fn decode(reader: &mut Reader<'_>) -> Result<Self> {
        let id = TransactionId::from_raw(reader.u64()?);
        let document = DocumentId::from_u128(reader.u128()?);
        let name = reader.text()?;
        let actor = decode_actor(reader)?;
        let coalesce_key = if reader.u8()? == 1 {
            Some(reader.text()?)
        } else {
            None
        };
        let count = reader.u32()? as usize;
        let mut operations = Vec::with_capacity(count.min(4096));
        for _ in 0..count {
            operations.push(Operation::decode(reader)?);
        }
        Ok(Self {
            id,
            document,
            name,
            actor,
            operations,
            coalesce_key,
        })
    }
}

fn encode_actor(writer: &mut Writer, actor: &Actor) {
    match actor {
        Actor::Human { name } => {
            writer.u8(0);
            writer.text(name);
        }
        Actor::Agent {
            agent,
            session,
            intent,
        } => {
            writer.u8(1);
            writer.text(agent);
            writer.text(session);
            writer.text(intent);
        }
        Actor::System { component } => {
            writer.u8(2);
            writer.text(component);
        }
    }
}

fn decode_actor(reader: &mut Reader<'_>) -> Result<Actor> {
    let tag = reader.u8()?;
    let actor = match tag {
        0 => Actor::Human {
            name: reader.text()?,
        },
        1 => Actor::Agent {
            agent: reader.text()?,
            session: reader.text()?,
            intent: reader.text()?,
        },
        2 => Actor::System {
            component: reader.text()?,
        },
        other => {
            return Err(cy_editor_core::problem::Problem::new(
                "decode an actor",
                format!("actor tag {other} is not one this build knows"),
            )
            .with_remedy("the journal was written by a newer editor; open it with that one"));
        }
    };
    Ok(actor)
}

#[cfg(test)]
mod tests {
    use cy_editor_core::ids::{FieldId, NodeId, TypeId};
    use cy_editor_core::value::Value;

    use super::*;

    fn document() -> DocumentId {
        DocumentId::of_asset("worlds/city.cyworld")
    }

    fn set(node: NodeId, before: f32, after: f32) -> Operation {
        Operation::SetField {
            node,
            component: TypeId::from_raw(1),
            field: FieldId::from_raw(1),
            before: Value::Float(before),
            after: Value::Float(after),
        }
    }

    fn transaction(name: &str, operations: Vec<Operation>, key: Option<&str>) -> Transaction {
        Transaction {
            id: TransactionId::from_raw(1),
            document: document(),
            name: name.into(),
            actor: Actor::human("designer"),
            operations,
            coalesce_key: key.map(Into::into),
        }
    }

    #[test]
    fn a_drag_compacts_to_one_operation_keeping_the_first_before_and_the_last_after() {
        let node = NodeId::in_document(document(), 1);
        let mut drag = transaction(
            "Move",
            vec![
                set(node, 0.0, 1.0),
                set(node, 1.0, 2.0),
                set(node, 2.0, 3.0),
            ],
            Some("gizmo-drag-1"),
        );
        drag.compact();
        assert_eq!(drag.operations, vec![set(node, 0.0, 3.0)]);
    }

    #[test]
    fn two_actors_do_not_coalesce_even_with_the_same_key() {
        let mut human = transaction("Move", vec![], Some("drag"));
        let mut agent = transaction("Move", vec![], Some("drag"));
        agent.actor = Actor::agent("claude", "s-1", "align the lamps");
        assert!(!human.can_coalesce_with(&agent));

        human.actor = agent.actor.clone();
        assert!(human.can_coalesce_with(&agent));
    }

    #[test]
    fn an_unkeyed_transaction_never_coalesces() {
        let first = transaction("Rename", vec![], None);
        let second = transaction("Rename", vec![], None);
        assert!(!first.can_coalesce_with(&second));
    }

    #[test]
    fn a_transaction_round_trips_through_the_codec_with_its_attribution() {
        let node = NodeId::in_document(document(), 1);
        let mut original = transaction("Move", vec![set(node, 0.0, 1.0)], Some("drag"));
        original.actor = Actor::agent("claude", "s-9", "raise the lamps to 4 m");

        let mut writer = Writer::new();
        original.encode(&mut writer);
        let bytes = writer.finish();
        let mut reader = Reader::new(&bytes);
        let restored = Transaction::decode(&mut reader).unwrap();

        assert_eq!(restored, original);
        assert_eq!(restored.actor.intent(), Some("raise the lamps to 4 m"));
    }

    #[test]
    fn an_inverse_reverses_both_the_operations_and_their_order() {
        let first = NodeId::in_document(document(), 1);
        let second = NodeId::in_document(document(), 2);
        let forward = transaction(
            "Two edits",
            vec![set(first, 0.0, 1.0), set(second, 5.0, 6.0)],
            None,
        );
        let back = forward.inverse();
        assert_eq!(
            back.operations,
            vec![set(second, 6.0, 5.0), set(first, 1.0, 0.0)]
        );
    }
}
