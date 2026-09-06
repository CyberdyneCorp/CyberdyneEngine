//! The editor: every service in one place, and the [`CommandContext`] a command is given.
//!
//! This type is the "explicit application state model" `editor-rust-application` requires —
//! "documents, selection, workspaces, project state, runtime sessions, commands, and notifications —
//! rather than distributing authoritative state across widgets". It is a plain struct with a field
//! per service, which is what makes "inspectable and, where useful, serialisable" cheap: a defect
//! report can name what was open, what was selected, and what the runtime was doing.
//!
//! It implements [`CommandContext`] because the command registry is below it — see that crate's
//! `context` module for why the dependency is inverted — and that is the whole of the wiring between
//! the two layers.

use cy_editor_commands::{Arguments, CommandContext, Outcome, Registry, Scope};
use cy_editor_core::Actor;
use cy_editor_core::ids::DocumentId;
use cy_editor_core::observe::Revision;
use cy_editor_core::problem::Result;
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_sdk::HostingMode;

use crate::documents::DocumentService;
use crate::notifications::{Notification, NotificationService};
use crate::operations::OperationService;
use crate::runtime::RuntimeSession;
use crate::selection::SelectionService;
use crate::workspace::Workspace;

/// The editor's authoritative state.
pub struct Editor {
    /// Open documents.
    pub documents: DocumentService,
    /// What is selected.
    pub selection: SelectionService,
    /// What is open and how it looks. Never dirties a document; see [`crate::workspace`].
    pub workspace: Workspace,
    /// What the editor has to say.
    pub notifications: NotificationService,
    /// Long operations, off the interface thread.
    pub operations: OperationService,
    /// The engine, or the considered absence of one.
    pub runtime: RuntimeSession,
    /// Who the editor believes is acting. Attribution, not authorisation.
    actor: Actor,
}

impl Default for Editor {
    fn default() -> Self {
        Self::new(Actor::human("user"))
    }
}

impl Editor {
    /// A new editor with no documents, no selection and no runtime.
    #[must_use]
    pub fn new(actor: Actor) -> Self {
        Self {
            documents: DocumentService::new(),
            selection: SelectionService::new(),
            workspace: Workspace::new(),
            notifications: NotificationService::new(),
            operations: OperationService::new(),
            runtime: RuntimeSession::none(),
            actor,
        }
    }

    /// Act as somebody else — an agent, with its session and its stated intent.
    ///
    /// Every transaction produced while this is in force carries the given actor, which is how "an
    /// agent's move is a human's move" is true of the resulting history: the same commands, the same
    /// operations, a different name on the entry.
    pub fn acting_as(&mut self, actor: Actor) {
        self.actor = actor;
    }

    /// The mode the editor is hosting its engine in.
    #[must_use]
    pub const fn hosting_mode(&self) -> HostingMode {
        self.runtime.mode()
    }

    /// Open a document, offering recovery when its journal holds anything.
    ///
    /// Offering rather than performing: the notification says how many transactions are recoverable
    /// and the user decides, because an editor that recovered without asking would overwrite work
    /// somebody had decided to abandon.
    pub fn open_document(&mut self, primary_asset: &str) -> Result<DocumentId> {
        let (id, recovery) = self.documents.open(primary_asset)?;
        self.workspace.opened(id);
        if let Some(recovery) = recovery {
            // "Has", not "recovered": nothing has been replayed. `Document::recover` is what
            // replays, and it is called only when somebody accepts the offer.
            self.notifications.post(Notification::warning(format!(
                "{} has {} recoverable transaction(s) from a previous session{}",
                primary_asset,
                recovery.count(),
                if recovery.truncated {
                    ", and its journal ends mid-record"
                } else {
                    ""
                }
            )));
        }
        Ok(id)
    }

    /// Invoke a command by identifier, under a scope.
    ///
    /// The one entry point. A menu, a shortcut, the palette, a script, a test and an agent are all
    /// callers of this, which is what makes `editor-rust-application`'s "One action, six entry
    /// points" true rather than aspirational.
    pub fn invoke(
        &mut self,
        registry: &Registry,
        id: &str,
        scope: &Scope,
        arguments: &Arguments,
    ) -> Result<Outcome> {
        registry.invoke(id, scope, self, arguments)
    }

    /// One frame of the editor's own housekeeping.
    ///
    /// Everything here is bounded and non-blocking: drain what the runtime sent, forget settled
    /// operations. It is what an interface calls once a frame, and it is deliberately the only thing
    /// that has to be called at that rate — a view model rebuilds when a revision moved, not
    /// because a frame happened.
    pub fn pump(&mut self) {
        let messages = self.runtime.pump(&mut self.notifications);
        // Reconciliation of predicted state against these echoes belongs to the viewport, at task
        // 4.1. Draining them here rather than leaving them queued is what keeps the channel from
        // growing without bound in a build that has no viewport yet.
        let _ = messages;
        self.operations.retain_running();
    }

    /// A revision that moves when anything a status bar shows has moved.
    ///
    /// The sum of the parts, which is the honest thing for a summary view to watch: it rebuilds a
    /// little more often than strictly necessary and never misses a change, and the alternative —
    /// a status bar with six watches — is six chances to forget one.
    #[must_use]
    pub fn summary_revision(&self) -> Revision {
        Revision::from_u64(
            self.documents.revision().as_u64()
                + self.selection.revision().as_u64()
                + self.workspace.revision().as_u64()
                + self.notifications.revision().as_u64()
                + self.operations.revision().as_u64(),
        )
    }
}

impl CommandContext for Editor {
    fn active_document(&self) -> Option<DocumentId> {
        self.workspace.active()
    }

    fn document(&self, id: DocumentId) -> Option<&Document> {
        self.documents.get(id)
    }

    fn document_mut(&mut self, id: DocumentId) -> Option<&mut Document> {
        self.documents.get_mut(id)
    }

    fn actor(&self) -> Actor {
        self.actor.clone()
    }

    fn selection(&self) -> &Selection {
        self.selection.get()
    }

    fn set_selection(&mut self, selection: Selection) {
        self.selection.set(selection);
    }

    fn notify(&mut self, message: &str) {
        self.notifications.post(Notification::info(message));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_editor_with_no_runtime_still_opens_and_edits_documents() {
        // The argument for `NoRuntime` being a mode: everything below happens with no engine at all.
        let mut editor = Editor::default();
        assert_eq!(editor.hosting_mode(), HostingMode::NoRuntime);

        let id = editor.open_document("worlds/city.cyworld").unwrap();
        assert_eq!(editor.workspace.active(), Some(id));

        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();
        assert!(editor.documents.any_dirty());
    }

    #[test]
    fn acting_as_an_agent_changes_who_history_names() {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        editor.acting_as(Actor::agent("claude", "s-3", "add a lamp at the corner"));

        let actor = editor.actor();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Create lamp", actor, |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        let entry = &editor.documents.get(id).unwrap().history().entries()[0];
        assert!(entry.actor.is_agent());
        assert_eq!(entry.actor.intent(), Some("add a lamp at the corner"));
    }

    #[test]
    fn pumping_an_editor_with_no_runtime_does_nothing_and_says_nothing() {
        let mut editor = Editor::default();
        let revision = editor.summary_revision();
        for _ in 0..100 {
            editor.pump();
        }
        assert_eq!(
            editor.summary_revision(),
            revision,
            "an idle editor costs nothing"
        );
    }
}
