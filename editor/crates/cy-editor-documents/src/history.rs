//! Undo, redo, and the budget that is enforced visibly. Task 3.5.
//!
//! "Histories SHALL be **per document**, with a separate history for project-level changes, so
//! undoing in one document does not affect another." So a [`History`] belongs to a
//! [`crate::document::Document`] and knows nothing about any other.
//!
//! "History SHALL hold a configurable **memory budget**. When exceeded, the oldest entries SHALL be
//! discarded, and the discard SHALL be reported rather than silent." [`History::truncated`] is that
//! report, and it is a count rather than a flag so that a status line can say *how much* was lost.
//!
//! --- WHY REDO IS A TRUNCATED TAIL RATHER THAN A SECOND STACK ------------------------------------------
//!
//! One vector and a cursor. Everything before the cursor has been applied; everything from it on has
//! been undone and can be redone. A new transaction truncates the tail, which is the behaviour every
//! editor has and the only one that keeps history a single ordered sequence — which matters because
//! the journal, the diff and the live bridge all read that sequence, and a two-stack model would
//! have to linearise it again at each of them.

use crate::transaction::Transaction;

/// A document's undo history.
#[derive(Clone, Debug)]
pub struct History {
    entries: Vec<Transaction>,
    /// How many entries have been applied. `entries[..cursor]` are done, `entries[cursor..]` undone.
    cursor: usize,
    budget_bytes: usize,
    used_bytes: usize,
    truncated: u64,
}

/// What a history reports about itself, for a status line or a test.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct HistoryStatus {
    /// How many entries can still be undone.
    pub undoable: usize,
    /// How many entries can be redone.
    pub redoable: usize,
    /// How many entries have been dropped for the budget, over the document's whole session.
    pub truncated: u64,
    /// How many payload bytes the held entries occupy.
    pub used_bytes: usize,
    /// The budget those bytes are measured against.
    pub budget_bytes: usize,
}

impl History {
    /// A history with a payload budget in bytes.
    ///
    /// The default the editor uses is 64 MiB per document, which is a decision for the settings
    /// layer rather than for this type — this type's job is to enforce whatever it is told and to
    /// say when it bit.
    #[must_use]
    pub const fn new(budget_bytes: usize) -> Self {
        Self {
            entries: Vec::new(),
            cursor: 0,
            budget_bytes,
            used_bytes: 0,
            truncated: 0,
        }
    }

    /// Record a committed transaction, coalescing it into the previous entry when both agree.
    ///
    /// Returns whether it merged rather than being appended, which is what lets a caller tell a
    /// drag's hundredth update from its first.
    pub fn push(&mut self, transaction: Transaction) -> bool {
        self.discard_redoable();

        let at_end = self.cursor == self.entries.len();
        let mergeable = at_end
            && self
                .entries
                .last()
                .is_some_and(|last| last.can_coalesce_with(&transaction));
        if mergeable {
            let last = self
                .entries
                .last_mut()
                .expect("mergeable implies there is a last entry");
            self.used_bytes = self.used_bytes.saturating_sub(last.payload_bytes());
            last.coalesce(transaction);
            let grown = last.payload_bytes();
            self.used_bytes += grown;
            self.enforce_budget();
            return true;
        }

        self.used_bytes += transaction.payload_bytes();
        self.entries.push(transaction);
        self.cursor = self.entries.len();
        self.enforce_budget();
        false
    }

    /// The transaction to undo, without undoing it.
    #[must_use]
    pub fn peek_undo(&self) -> Option<&Transaction> {
        self.entries.get(self.cursor.checked_sub(1)?)
    }

    /// The transaction to redo, without redoing it.
    #[must_use]
    pub fn peek_redo(&self) -> Option<&Transaction> {
        self.entries.get(self.cursor)
    }

    /// Move the cursor back one entry, returning what was undone.
    pub fn undo(&mut self) -> Option<Transaction> {
        let index = self.cursor.checked_sub(1)?;
        let transaction = self.entries.get(index)?.clone();
        self.cursor = index;
        Some(transaction)
    }

    /// Move the cursor forward one entry, returning what was redone.
    pub fn redo(&mut self) -> Option<Transaction> {
        let transaction = self.entries.get(self.cursor)?.clone();
        self.cursor += 1;
        Some(transaction)
    }

    /// Every entry, oldest first. Attribution is visible here because history is where it belongs.
    #[must_use]
    pub fn entries(&self) -> &[Transaction] {
        &self.entries
    }

    /// How many entries have been applied.
    #[must_use]
    pub const fn cursor(&self) -> usize {
        self.cursor
    }

    /// How many entries were dropped for the budget over this session.
    #[must_use]
    pub const fn truncated(&self) -> u64 {
        self.truncated
    }

    /// What to show in a status line, and what a test asserts on.
    #[must_use]
    pub const fn status(&self) -> HistoryStatus {
        HistoryStatus {
            undoable: self.cursor,
            redoable: self.entries.len() - self.cursor,
            truncated: self.truncated,
            used_bytes: self.used_bytes,
            budget_bytes: self.budget_bytes,
        }
    }

    /// Forget everything. Used when a document is closed, not when it is saved: saving does not
    /// invalidate history, which is why a save is not a barrier here.
    pub fn clear(&mut self) {
        self.entries.clear();
        self.cursor = 0;
        self.used_bytes = 0;
    }

    /// Drop the redoable tail, which a new transaction invalidates.
    fn discard_redoable(&mut self) {
        while self.entries.len() > self.cursor {
            if let Some(dropped) = self.entries.pop() {
                self.used_bytes = self.used_bytes.saturating_sub(dropped.payload_bytes());
            }
        }
    }

    /// Drop the oldest entries until the budget is met, counting each one.
    ///
    /// Never drops the newest entry, however large: a history that discarded the change the user
    /// just made would make undo unavailable exactly when it is wanted, and a payload bigger than
    /// the whole budget is a tool to fix rather than an entry to hide.
    fn enforce_budget(&mut self) {
        while self.used_bytes > self.budget_bytes && self.entries.len() > 1 {
            let dropped = self.entries.remove(0);
            self.used_bytes = self.used_bytes.saturating_sub(dropped.payload_bytes());
            self.cursor = self.cursor.saturating_sub(1);
            self.truncated += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::ids::{DocumentId, FieldId, NodeId, TypeId};
    use cy_editor_core::value::Value;

    use super::*;
    use crate::operation::Operation;
    use crate::transaction::TransactionId;

    fn document() -> DocumentId {
        DocumentId::of_asset("worlds/city.cyworld")
    }

    fn entry(sequence: u16, key: Option<&str>) -> Transaction {
        Transaction {
            id: TransactionId::from_raw(u64::from(sequence)),
            document: document(),
            name: format!("Edit {sequence}"),
            actor: Actor::human("designer"),
            operations: vec![Operation::SetField {
                node: NodeId::in_document(document(), 1),
                component: TypeId::from_raw(1),
                field: FieldId::from_raw(1),
                before: Value::Float(f32::from(sequence)),
                after: Value::Float(f32::from(sequence) + 1.0),
            }],
            coalesce_key: key.map(Into::into),
        }
    }

    #[test]
    fn undo_and_redo_walk_one_ordered_sequence() {
        let mut history = History::new(1 << 20);
        history.push(entry(1, None));
        history.push(entry(2, None));
        assert_eq!(history.status().undoable, 2);

        assert_eq!(history.undo().unwrap().name, "Edit 2");
        assert_eq!(
            history.status(),
            HistoryStatus {
                undoable: 1,
                redoable: 1,
                ..history.status()
            }
        );
        assert_eq!(history.redo().unwrap().name, "Edit 2");
        assert_eq!(history.status().redoable, 0);
    }

    #[test]
    fn a_new_transaction_discards_the_redoable_tail() {
        let mut history = History::new(1 << 20);
        history.push(entry(1, None));
        history.push(entry(2, None));
        history.undo();
        history.push(entry(3, None));

        assert_eq!(history.status().redoable, 0);
        assert_eq!(history.entries().len(), 2);
        assert_eq!(history.entries()[1].name, "Edit 3");
    }

    #[test]
    fn a_drag_is_one_entry() {
        // The specification's scenario: "WHEN a user drags a transform gizmo through hundreds of
        // intermediate positions THEN history SHALL contain one entry, committed on release."
        let mut history = History::new(1 << 20);
        for sequence in 0..500 {
            history.push(entry(sequence, Some("gizmo-drag-1")));
        }
        assert_eq!(history.entries().len(), 1);
        assert_eq!(
            history.entries()[0].operations.len(),
            1,
            "and one operation inside it"
        );
    }

    #[test]
    fn the_budget_is_enforced_and_reported() {
        // Small enough that a handful of entries exceeds it.
        let mut history = History::new(200);
        for sequence in 0..20 {
            history.push(entry(sequence, None));
        }
        let status = history.status();
        assert!(
            status.truncated > 0,
            "the budget must actually bite in this test"
        );
        assert!(status.used_bytes <= status.budget_bytes);
        assert!(
            !history.entries().is_empty(),
            "the newest entry is never dropped"
        );
    }

    #[test]
    fn the_newest_entry_survives_a_budget_it_alone_exceeds() {
        let mut history = History::new(1);
        history.push(entry(1, None));
        assert_eq!(history.entries().len(), 1);
        assert!(
            history.peek_undo().is_some(),
            "undo must be available for what was just done"
        );
    }
}
