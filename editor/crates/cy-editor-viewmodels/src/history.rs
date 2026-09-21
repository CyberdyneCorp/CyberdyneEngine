// SPDX-License-Identifier: MIT
//! Attributed undo history for the active document.

use cy_editor_core::Actor;
use cy_editor_core::ids::DocumentId;
use cy_editor_core::observe::Revision;
use cy_editor_services::Editor;

/// Who produced one history entry, separated so an agent's intent is not hidden in prose.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct HistoryAttribution {
    /// Human name, agent identity, or system component.
    pub actor: String,
    /// Agent connection session, when applicable.
    pub session: Option<String>,
    /// The agent's stated intent, when applicable.
    pub intent: Option<String>,
}

/// One row in the undo stack.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct HistoryRow {
    /// Stable transaction sequence within this session.
    pub transaction: u64,
    /// User-facing transaction name.
    pub name: String,
    /// Attribution shown beside the change.
    pub attribution: HistoryAttribution,
    /// Number of typed operations in the transaction.
    pub operations: usize,
    /// Applied entries are before the cursor; undone entries are redoable.
    pub applied: bool,
}

/// Derived history state for the active document.
#[derive(Debug, Default)]
pub struct HistoryViewModel {
    document: Option<DocumentId>,
    rows: Vec<HistoryRow>,
    cursor: usize,
    undoable: usize,
    redoable: usize,
    truncated: u64,
    input: Option<(DocumentId, Revision, usize, usize, u64)>,
}

impl HistoryViewModel {
    /// An empty history panel.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild only when the active document or its history moves.
    pub fn refresh(&mut self, editor: &Editor) -> bool {
        let Some(document_id) = editor.workspace.active() else {
            return self.clear();
        };
        let Some(document) = editor.documents.get(document_id) else {
            return self.clear();
        };
        let history = document.history();
        let status = history.status();
        let input = (
            document_id,
            document.revision(),
            history.entries().len(),
            history.cursor(),
            status.truncated,
        );
        if self.input == Some(input) {
            return false;
        }

        self.document = Some(document_id);
        self.cursor = history.cursor();
        self.undoable = status.undoable;
        self.redoable = status.redoable;
        self.truncated = status.truncated;
        self.rows = history
            .entries()
            .iter()
            .enumerate()
            .map(|(index, transaction)| HistoryRow {
                transaction: transaction.id.as_u64(),
                name: transaction.name.clone(),
                attribution: attribution(&transaction.actor),
                operations: transaction.operations.len(),
                applied: index < history.cursor(),
            })
            .collect();
        self.input = Some(input);
        true
    }

    fn clear(&mut self) -> bool {
        if self.input.is_none() && self.rows.is_empty() {
            return false;
        }
        *self = Self::default();
        true
    }

    /// The document whose history is shown.
    #[must_use]
    pub const fn document(&self) -> Option<DocumentId> {
        self.document
    }

    /// Entries from oldest to newest.
    #[must_use]
    pub fn rows(&self) -> &[HistoryRow] {
        &self.rows
    }

    /// The first redoable row index.
    #[must_use]
    pub const fn cursor(&self) -> usize {
        self.cursor
    }

    /// Whether Undo is currently meaningful.
    #[must_use]
    pub const fn can_undo(&self) -> bool {
        self.undoable > 0
    }

    /// Whether Redo is currently meaningful.
    #[must_use]
    pub const fn can_redo(&self) -> bool {
        self.redoable > 0
    }

    /// Entries discarded because the memory budget was exceeded.
    #[must_use]
    pub const fn truncated(&self) -> u64 {
        self.truncated
    }

    /// A non-silent truncation message.
    #[must_use]
    pub fn truncation_message(&self) -> Option<String> {
        (self.truncated > 0).then(|| {
            format!(
                "{} older history entr{} discarded to stay within the memory budget",
                self.truncated,
                if self.truncated == 1 {
                    "y was"
                } else {
                    "ies were"
                }
            )
        })
    }
}

fn attribution(actor: &Actor) -> HistoryAttribution {
    match actor {
        Actor::Human { name } => HistoryAttribution {
            actor: name.clone(),
            session: None,
            intent: None,
        },
        Actor::Agent {
            agent,
            session,
            intent,
        } => HistoryAttribution {
            actor: agent.clone(),
            session: Some(session.clone()),
            intent: Some(intent.clone()),
        },
        Actor::System { component } => HistoryAttribution {
            actor: format!("Editor: {component}"),
            session: None,
            intent: None,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn history_presents_actor_agent_intent_cursor_and_availability() {
        let mut editor = Editor::default();
        let city = editor.open_document("worlds/city.cyworld").unwrap();
        for (name, actor) in [
            ("Human edit", Actor::human("designer")),
            (
                "Agent edit",
                Actor::agent("builder", "session-7", "raise the lamps"),
            ),
        ] {
            editor
                .documents
                .get_mut(city)
                .unwrap()
                .with_transaction(name, actor, |document| {
                    document.create_node(None).map(|_| ())
                })
                .unwrap();
        }
        editor.documents.get_mut(city).unwrap().undo().unwrap();

        let mut history = HistoryViewModel::new();
        assert!(history.refresh(&editor));
        assert_eq!(history.cursor(), 1);
        assert!(history.can_undo());
        assert!(history.can_redo());
        assert!(history.rows()[0].applied);
        assert!(!history.rows()[1].applied);
        assert_eq!(history.rows()[0].attribution.actor, "designer");
        assert_eq!(history.rows()[1].attribution.actor, "builder");
        assert_eq!(
            history.rows()[1].attribution.session.as_deref(),
            Some("session-7")
        );
        assert_eq!(
            history.rows()[1].attribution.intent.as_deref(),
            Some("raise the lamps")
        );
        assert!(!history.refresh(&editor), "an idle panel does not rebuild");
    }

    #[test]
    fn truncation_is_presented_as_words_not_a_silent_flag() {
        let history = HistoryViewModel {
            truncated: 3,
            ..HistoryViewModel::default()
        };
        let message = history.truncation_message().unwrap();
        assert!(message.contains('3'));
        assert!(message.contains("memory budget"));
    }
}
