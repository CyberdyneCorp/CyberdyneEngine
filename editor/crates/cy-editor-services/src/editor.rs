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
use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_sdk::HostingMode;
use cy_editor_viewport::play::PlayState;

use crate::documents::DocumentService;
use crate::manipulate;
use crate::notifications::{Notification, NotificationService};
use crate::operations::OperationService;
use crate::project::ProjectService;
use crate::runtime::RuntimeSession;
use crate::selection::SelectionService;
use crate::viewports::ViewportService;
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
    /// What the editor is showing: the viewports, their cameras and their tools.
    ///
    /// A service like the others, and for the same reason: a viewport's transform mode is state a
    /// command changes, a panel reads and an agent asks about, so it cannot live in the window.
    pub viewports: ViewportService,
    /// The project around the documents: its source tree, its build, and what has been built.
    pub project: ProjectService,
    /// Who the editor believes is acting. Attribution, not authorisation.
    actor: Actor,
    /// The directories the invocation in progress may touch, and the scope that says so.
    ///
    /// Set by [`Editor::invoke`] for the duration and restored afterwards, exactly as the actor is,
    /// and for the same reason: the value belongs to the invocation rather than to the editor. A
    /// command that touches a path reads it through
    /// [`cy_editor_commands::CommandContext::permitted_paths`], because only the command knows which
    /// of its arguments is a path.
    permitted: (String, Vec<String>),
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
        let project = ProjectService::default();
        let mut documents = DocumentService::new();
        // Rooted from the start, not only when a project is named — otherwise an editor started the
        // way the artefacts start it would open every world empty. But rooted only when the working
        // directory SAYS it is a project: `ProjectService::default()` roots at wherever the process
        // was launched, and treating that as a project means an editor started in a source tree
        // saves worlds into it. That is not hypothetical; see `ProjectService::is_declared`.
        if project.is_declared() {
            documents.rooted_at(project.root());
        }
        Self {
            documents,
            selection: SelectionService::new(),
            workspace: Workspace::new(),
            notifications: NotificationService::new(),
            operations: OperationService::new(),
            runtime: RuntimeSession::none(),
            viewports: ViewportService::new(),
            project,
            actor,
            permitted: unrestricted(),
        }
    }

    /// Point the editor at a project on disk.
    ///
    /// The documents learn the root at the same moment, because a document service that did not
    /// know where the project was is precisely the state M5.5 shipped: `open` produced a name and
    /// an empty schema, and there was nowhere for a world to come from.
    ///
    /// Under the same rule [`Editor::new`] applies, and deliberately not a looser one: a project is
    /// a directory that says it is one. Two rules — a strict one for the implicit root and a lax one
    /// for a named root — would mean the editor wrote worlds in one configuration and not the other,
    /// which is exactly the kind of difference nobody finds until it matters.
    #[must_use]
    pub fn with_project(mut self, project: ProjectService) -> Self {
        if project.is_declared() {
            self.documents.rooted_at(project.root());
        }
        self.project = project;
        self
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
        let (id, recovery, report) = self.documents.open_reporting(primary_asset)?;
        self.workspace.opened(id);
        if report.nodes > 0 {
            self.notifications.post(Notification::info(format!(
                "Opened {primary_asset}: {} node(s), {} component type(s)",
                report.nodes, report.types
            )));
        }
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
        // The directory half of the scope travels with the invocation. The registry checks the
        // effect class and the documents; it cannot check a path, because it does not know which
        // argument is one — so the command does, and this is how it is told.
        let previous = std::mem::replace(
            &mut self.permitted,
            (scope.name.clone(), scope.directories.clone()),
        );
        let outcome = registry.invoke(id, scope, self, arguments);
        self.permitted = previous;
        outcome
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

    fn viewport(&mut self) -> Option<&mut dyn cy_editor_commands::ViewportControls> {
        Some(&mut self.viewports)
    }

    fn project(&mut self) -> Option<&mut dyn cy_editor_commands::ProjectHost> {
        Some(self)
    }

    fn open_document(&mut self, asset: &str) -> Result<DocumentId> {
        Editor::open_document(self, asset)
    }

    fn play_state(&self) -> Option<String> {
        Some(cy_editor_commands::ProjectHost::play_state(self))
    }

    fn permitted_paths(&self) -> Option<(String, Vec<String>)> {
        Some(self.permitted.clone())
    }

    fn manipulate(&mut self, request: &cy_editor_commands::Manipulation) -> Result<String> {
        manipulate::apply(self, request)
    }
}

/// What a human at the interface may touch: everything.
///
/// Not a claim that the human is trusted more than an agent — `cy_editor_commands::scope` makes that
/// argument in full — but the observation that an editor which refused its own menu items would be
/// refusing itself.
fn unrestricted() -> (String, Vec<String>) {
    ("interactive".to_string(), vec![String::new()])
}

/// The project's source tree, its build, and the runtime that runs it.
///
/// Implemented on [`Editor`] rather than on [`ProjectService`] because two of the three reach past
/// it: a reload goes to [`Editor::runtime`] and a play switch to [`Editor::viewports`]. They are one
/// trait because they are one loop — write, build, reload, play — and splitting them into three
/// would put three hooks on [`CommandContext`] for one capability.
impl cy_editor_commands::ProjectHost for Editor {
    fn project_root(&self) -> String {
        self.project.root().display().to_string()
    }

    fn source_paths(&self) -> Vec<String> {
        self.project.source_paths()
    }

    fn source_exists(&self, path: &str) -> bool {
        self.project.source_exists(path)
    }

    fn read_source(&self, path: &str) -> Result<String> {
        self.project.read_source(path)
    }

    fn source_is_restorable(&self, path: &str) -> bool {
        self.project.source_is_restorable(path)
    }

    fn put_source(&mut self, path: &str, contents: Option<&str>) -> Result<()> {
        self.project.put_source(path, contents)
    }

    fn build(&mut self) -> Result<String> {
        let (label, work) = self.project.next_build()?;
        // Off the interface thread, where every other long operation goes. "The editor stays usable
        // while an agent works" is not satisfiable by a build that blocks the caller, and an agent
        // reads the operations resource for progress exactly as a person reads the same panel.
        let operation = self.operations.start(label.clone(), move |_| {
            work();
            Ok(())
        });
        Ok(format!("{} — {}", label, operation.label()))
    }

    fn build_state(&self) -> String {
        self.project.describe_build()
    }

    fn reload(&mut self, module: &str) -> Result<String> {
        let (library, generation) = self.project.built()?;
        let module = if module.trim().is_empty() {
            self.project.module().to_string()
        } else {
            module.to_string()
        };
        let request = self
            .runtime
            .reload(&module, &library.display().to_string(), generation)?;
        Ok(format!(
            "Asked the runtime to load {module} generation {generation} (request {})",
            request.as_u64()
        ))
    }

    fn set_play(&mut self, state: &str) -> Result<String> {
        let wanted = match state {
            "playing" => PlayState::Playing,
            "paused" => PlayState::Paused,
            "editing" => PlayState::Editing,
            other => {
                return Err(Problem::new(
                    format!("set the play state to {other:?}"),
                    "there is no such play state",
                )
                .with_remedy("the states are: editing, playing, paused"));
            }
        };
        // Every viewport, because play is a property of the runtime rather than of a panel: two
        // viewports showing different play states would be two runtimes.
        for viewport in self.viewports.all_mut().iter_mut() {
            viewport.play = wanted;
        }
        Ok(format!(
            "{} — {}",
            wanted.badge(),
            wanted
                .persistence(cy_editor_viewport::play::Persistence::default())
                .statement()
        ))
    }

    fn play_state(&self) -> String {
        match self.viewports.focused().play {
            PlayState::Editing => "editing",
            PlayState::Playing => "playing",
            PlayState::Paused => "paused",
        }
        .to_string()
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
