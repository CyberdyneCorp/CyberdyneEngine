//! Source control, as a panel reads it. `editor-documents-and-transactions`, M11.b task 4.2.
//!
//! --- WHY THE PROVIDER INTERFACE NEEDS THIS TO BE FINISHED ------------------------------------------
//!
//! The requirement's second sentence is the one with a consequence: *"features a provider does not
//! support SHALL be reported as unavailable rather than emulated incorrectly"*, and its scenario is
//! about **locking**. A refusal returned by
//! [`cy_editor_services::source_control::GitSourceControl::lock`] is only half of that — the other
//! half is somebody being told, before they press it, that the button will not work. So this view
//! model turns the provider's declared [`Capability`] set into per-action availability, and a panel
//! that greys out what is unavailable is telling the truth rather than discovering it.
//!
//! --- IT HOLDS NOTHING AUTHORITATIVE --------------------------------------------------------------------
//!
//! The crate's rule, stated in `lib.rs` as a prohibition: a view model never holds document,
//! project or runtime state. This one holds rows — a path, a state and a word — derived at refresh
//! from the service it is handed a borrow of, and it could not hold a provider if it wanted to,
//! because it is never given one to keep.

use cy_editor_services::Editor;
use cy_editor_services::source_control::{
    Capability, FileState, SourceControlProvider, SourceControlService,
};

/// Whether an action can be taken, and — where it cannot — why not.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Availability {
    /// The provider declares the capability.
    Available,
    /// It does not. The string is what a person is shown, and it names the provider, so "Git has no
    /// exclusive locking" reads as a fact about Git rather than as a defect in the editor.
    Unavailable(String),
}

impl Availability {
    /// Whether a control should be enabled.
    #[must_use]
    pub const fn is_available(&self) -> bool {
        matches!(self, Availability::Available)
    }
}

/// One open document, as the source-control panel shows it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct SourceControlRow {
    /// The document's path, as the editor opened it.
    pub path: String,
    /// Where it stands with the provider.
    pub state: FileState,
    /// What a person reads in the state column.
    pub state_label: String,
    /// Who holds an exclusive lock on it, where the provider has locking and one is held.
    pub locked_by: Option<String>,
}

/// One provider revision for the selected file.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct SourceControlHistoryRow {
    /// Provider-native revision or changelist identifier.
    pub revision: String,
    /// Author reported by the provider.
    pub author: String,
    /// Submitted description.
    pub description: String,
}

/// The source-control panel's presentation state.
#[derive(Debug, Default)]
pub struct SourceControlViewModel {
    provider: String,
    rows: Vec<SourceControlRow>,
    check_out: Availability,
    lock: Availability,
    submit: Availability,
    history: Availability,
    revert: Availability,
    problem: Option<String>,
    pending: bool,
    history_path: Option<String>,
    history_rows: Vec<SourceControlHistoryRow>,
    input: Option<(
        cy_editor_core::observe::Revision,
        cy_editor_core::observe::Revision,
    )>,
}

impl Default for Availability {
    fn default() -> Self {
        Availability::Unavailable("no source control".to_string())
    }
}

impl SourceControlViewModel {
    /// An empty panel.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild from the service and the open documents.
    ///
    /// Provider work is requested once per path set and results arrive through the service revision.
    pub fn refresh(&mut self, source_control: &SourceControlService, editor: &Editor) -> bool {
        let paths: Vec<std::path::PathBuf> = editor
            .documents
            .ids()
            .filter_map(|id| editor.documents.get(id))
            .filter_map(|document| document.assets().first().cloned())
            .map(std::path::PathBuf::from)
            .collect();
        let _ = source_control.request_status(paths);
        let input = (source_control.revision(), editor.documents.revision());
        if self.input == Some(input) {
            return false;
        }
        let provider = source_control.provider();
        self.provider = provider.name().to_string();
        self.check_out = availability(provider, Capability::CheckOut);
        self.lock = availability(provider, Capability::ExclusiveLock);
        self.submit = availability(provider, Capability::Submit);
        self.history = availability(provider, Capability::History);
        self.revert = availability(provider, Capability::Revert);

        // The document's PRIMARY ASSET, not the absolute path `DocumentService::path_of` builds:
        // a provider spells a path relative to its own working root, and an absolute one would be
        // a path no provider recognises on any other machine.
        self.rows.clear();
        self.problem = None;
        self.pending = source_control.status_pending();
        match source_control.status_snapshot() {
            Some(Ok(status)) => {
                for entry in status {
                    self.rows.push(SourceControlRow {
                        path: entry.path.to_string_lossy().into_owned(),
                        state: entry.state,
                        state_label: state_label(entry.state).to_string(),
                        locked_by: entry.locked_by,
                    });
                }
            }
            // A provider that could not answer is REPORTED rather than shown as a clean tree. A
            // panel that drew "unchanged" because the client failed to start would be the silent
            // fallback the whole provider interface exists to avoid.
            Some(Err(problem)) => self.problem = Some(problem.to_string()),
            None => {}
        }
        self.history_path = None;
        self.history_rows.clear();
        if let Some((path, result)) = source_control.history_snapshot() {
            self.history_path = Some(path.to_string_lossy().into_owned());
            match result {
                Ok(revisions) => {
                    self.history_rows = revisions
                        .into_iter()
                        .map(|revision| SourceControlHistoryRow {
                            revision: revision.id,
                            author: revision.author,
                            description: revision.description,
                        })
                        .collect();
                }
                Err(problem) => self.problem = Some(problem.to_string()),
            }
        }
        self.input = Some(input);
        true
    }

    /// The provider's name, for the panel's header.
    #[must_use]
    pub fn provider(&self) -> &str {
        &self.provider
    }

    /// The rows to render.
    #[must_use]
    pub fn rows(&self) -> &[SourceControlRow] {
        &self.rows
    }

    /// Whether the check-out control should be enabled, and what to say if not.
    #[must_use]
    pub const fn check_out(&self) -> &Availability {
        &self.check_out
    }

    /// The same for an exclusive lock, which is the requirement's own scenario.
    #[must_use]
    pub const fn lock(&self) -> &Availability {
        &self.lock
    }

    /// The same for submit.
    #[must_use]
    pub const fn submit(&self) -> &Availability {
        &self.submit
    }

    /// The same for history.
    #[must_use]
    pub const fn history(&self) -> &Availability {
        &self.history
    }

    /// The same for discarding local changes.
    #[must_use]
    pub const fn revert(&self) -> &Availability {
        &self.revert
    }

    /// What went wrong asking the provider, when something did.
    #[must_use]
    pub fn problem(&self) -> Option<&str> {
        self.problem.as_deref()
    }

    /// Whether a background provider request is in flight.
    #[must_use]
    pub const fn pending(&self) -> bool {
        self.pending
    }

    /// Path whose provider history is shown.
    #[must_use]
    pub fn history_path(&self) -> Option<&str> {
        self.history_path.as_deref()
    }

    /// Revisions returned by the latest history command.
    #[must_use]
    pub fn history_rows(&self) -> &[SourceControlHistoryRow] {
        &self.history_rows
    }
}

fn availability(provider: &dyn SourceControlProvider, capability: Capability) -> Availability {
    if provider.capabilities().has(capability) {
        Availability::Available
    } else {
        Availability::Unavailable(format!(
            "{} has no {}",
            provider.name(),
            capability.spelling()
        ))
    }
}

const fn state_label(state: FileState) -> &'static str {
    match state {
        FileState::Clean => "unchanged",
        FileState::Modified => "modified",
        FileState::Added => "added",
        FileState::Deleted => "deleted",
        FileState::Renamed => "renamed",
        FileState::Unversioned => "not in source control",
        FileState::Conflicted => "conflicted",
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_services::source_control::{GitSourceControl, NullSourceControl};

    use super::*;

    #[test]
    fn the_panel_greys_out_locking_against_a_provider_that_has_none() {
        // The scenario, at the surface a person actually meets: "WHEN a provider has no exclusive
        // locking THEN locking SHALL be reported as unavailable rather than silently doing
        // nothing." A refusal returned from `lock()` is the other half; this is the half that stops
        // somebody pressing it.
        let editor = Editor::default();
        let service = SourceControlService::new(Box::new(GitSourceControl::new(".")));
        let mut panel = SourceControlViewModel::new();
        panel.refresh(&service, &editor);

        assert_eq!(panel.provider(), "Git");
        assert!(!panel.lock().is_available());
        assert_eq!(
            panel.lock(),
            &Availability::Unavailable("Git has no exclusive locking".to_string()),
            "and it names the provider, so it reads as a fact about Git"
        );
        assert!(!panel.check_out().is_available());
        assert!(panel.submit().is_available());
        assert!(panel.history().is_available());
    }

    #[test]
    fn the_panel_works_against_no_source_control_at_all() {
        // The null provider is what makes the other two optional: a project in no repository is a
        // project the panel still draws, with every action greyed out and every file unversioned.
        let mut editor = Editor::default();
        editor.open_document("worlds/city.cyworld").unwrap();
        let service = SourceControlService::default();
        service.refresh_now(vec![std::path::PathBuf::from("worlds/city.cyworld")]);
        let mut panel = SourceControlViewModel::new();
        panel.refresh(&service, &editor);

        assert_eq!(panel.provider(), "no source control");
        assert_eq!(panel.rows().len(), 1);
        assert_eq!(panel.rows()[0].state, FileState::Unversioned);
        assert_eq!(panel.rows()[0].state_label, "not in source control");
        assert!(!panel.submit().is_available());
        assert!(!panel.history().is_available());
        assert!(panel.problem().is_none());
    }

    #[test]
    fn switching_the_provider_changes_the_panel_and_nothing_else() {
        // "WHEN a project switches source control systems THEN editor integration SHALL continue to
        // work through the provider interface." The panel names no provider; it reads capabilities.
        let editor = Editor::default();
        let mut service = SourceControlService::default();
        let mut panel = SourceControlViewModel::new();
        panel.refresh(&service, &editor);
        assert!(!panel.submit().is_available());

        service.adopt(Box::new(GitSourceControl::new(".")));
        panel.refresh(&service, &editor);
        assert!(panel.submit().is_available());
        assert_eq!(panel.provider(), "Git");

        service.adopt(Box::new(NullSourceControl));
        panel.refresh(&service, &editor);
        assert!(!panel.submit().is_available());
    }
}
