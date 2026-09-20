//! The open documents, addressed by identity and by generation-checked handle.
//!
//! Two ways in, on purpose. [`DocumentService::get`] takes a [`DocumentId`], which is what a
//! transaction, a journal and an agent's tool call carry — stable across a restart. The
//! [`cy_editor_core::ids::HandleMap`] underneath is what makes a closed document's handle stop
//! resolving rather than resolving to whatever was opened next.

use std::collections::BTreeMap;
use std::path::PathBuf;

use cy_editor_core::Actor;
use cy_editor_core::ids::DocumentId;
use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::Document;
use cy_editor_documents::journal::Recovery;

use crate::worldfile::{self, LoadReport};

/// What the user chose after asking to close a dirty document.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CloseDecision {
    /// Persist the document, then close it only if the write succeeds.
    Save,
    /// Close without persisting the document's dirty state.
    Discard,
    /// Keep the document open exactly as it is.
    Cancel,
}

/// The observable result of a guarded close request.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CloseOutcome {
    /// The document was removed from the service.
    Closed,
    /// The document is dirty and the caller has not supplied a decision yet.
    NeedsDecision,
    /// The caller cancelled; no state changed.
    Cancelled,
}

/// Every open document.
#[derive(Default)]
pub struct DocumentService {
    documents: BTreeMap<DocumentId, Document>,
    /// Moves whenever a document is opened or closed — not when one is edited, which is what a
    /// document's own revision is for. A tab strip watches this; an inspector watches the document.
    revision: Versioned<()>,
    journal_directory: Option<PathBuf>,
    /// The project the documents belong to, when the editor was opened on one.
    ///
    /// This is what turns [`DocumentService::open`] from "a name and an empty schema" into a
    /// document that has something in it — see [`crate::worldfile`], and M6 task 2.4.
    project_root: Option<PathBuf>,
}

impl DocumentService {
    /// A service with no documents open and no journal directory.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Read worlds and the engine's type manifest from `root`.
    ///
    /// Absent in a test that does not care, which is what keeps every document test working with no
    /// filesystem at all — a document with no project is still a document.
    pub fn rooted_at(&mut self, root: impl Into<PathBuf>) {
        self.project_root = Some(root.into());
    }

    /// The project the documents are read from and written to.
    #[must_use]
    pub fn project_root(&self) -> Option<&std::path::Path> {
        self.project_root.as_deref()
    }

    /// Where a document's primary asset lives on disk, when there is a project.
    #[must_use]
    pub fn path_of(&self, document: &Document) -> Option<PathBuf> {
        let root = self.project_root.as_ref()?;
        let asset = document.assets().first()?;
        Some(root.join(asset))
    }

    /// Journal every document opened from now on into `directory`.
    ///
    /// Set by the application from the project's settings. Absent in a test that does not care,
    /// which is what keeps the transaction system testable with no filesystem at all.
    pub fn journal_into(&mut self, directory: impl Into<PathBuf>) {
        self.journal_directory = Some(directory.into());
    }

    /// Open a document backed by `primary_asset`, journalling it when a directory is configured.
    ///
    /// Returns whatever the journal already held, so the caller can offer recovery. Opening does not
    /// replay it: `editor-documents-and-transactions` says the editor "SHALL **offer** recovery",
    /// and an editor that recovered without asking would overwrite a file the user had decided to
    /// abandon.
    pub fn open(&mut self, primary_asset: &str) -> Result<(DocumentId, Option<Recovery>)> {
        self.open_reporting(primary_asset)
            .map(|(id, recovery, _)| (id, recovery))
    }

    /// [`DocumentService::open`], and what the world loader found.
    ///
    /// A second entry point rather than a changed signature, because a caller that only wants a
    /// document should not have to name a report it will discard — and the report is what lets the
    /// editor say "12 nodes, 3 component types" instead of opening in silence.
    pub fn open_reporting(
        &mut self,
        primary_asset: &str,
    ) -> Result<(DocumentId, Option<Recovery>, LoadReport)> {
        let mut document = Document::new(primary_asset);
        let id = document.id();
        if self.documents.contains_key(&id) {
            return Ok((id, None, LoadReport::default()));
        }
        // THE WORLD BEFORE THE JOURNAL, AND THE FILE BEFORE THE MANIFEST. Loading first means the
        // "Open world" transaction is never journalled — a load is not work anybody would want
        // recovered — and reading the world's own schema INSTEAD of the manifest when there is a
        // world means a type is never declared twice, which `DocumentSchema::declare_type` would
        // happily do and which would leave two components both called Transform.
        let report = self.populate(&mut document)?;
        let mut recovery = None;
        if let Some(directory) = &self.journal_directory {
            document.attach_journal(directory)?;
            recovery = document
                .recoverable()?
                .filter(|recovery| recovery.count() > 0);
        }
        self.documents.insert(id, document);
        self.revision.update(|()| {});
        Ok((id, recovery, report))
    }

    /// Give a new document its schema, and its content when a world exists to load.
    fn populate(&self, document: &mut Document) -> Result<LoadReport> {
        let Some(path) = self.path_of(document) else {
            return Ok(LoadReport::default());
        };
        if let Ok(text) = std::fs::read_to_string(&path) {
            return worldfile::load(&text, document, Actor::system("world loader"));
        }
        // No world yet: the file is created by the first save. The schema still arrives, so an
        // empty world is authorable — the gizmo binds, the inspector describes, and a created
        // entity has a Transform to move.
        let root = self
            .project_root
            .as_deref()
            .unwrap_or(std::path::Path::new("."));
        let types = worldfile::declare_project_types(root, document.schema_mut())?;
        Ok(LoadReport {
            types,
            ..LoadReport::default()
        })
    }

    /// Write a document's world back to the project, which is what `file.save` does.
    pub fn write(&self, document: &Document) -> Result<()> {
        let Some(path) = self.path_of(document) else {
            return Ok(());
        };
        worldfile::write_to(document, &path)
    }

    /// Adopt an already-constructed document. Used by tests and by preview worlds.
    pub fn insert(&mut self, document: Document) -> DocumentId {
        let id = document.id();
        self.documents.insert(id, document);
        self.revision.update(|()| {});
        id
    }

    /// Close a clean document, abandoning any open transaction.
    ///
    /// "An uncommitted transaction SHALL roll back automatically when its scope ends", and closing
    /// a document is the end of every scope in it. A dirty document is refused until the caller
    /// supplies an explicit choice through [`DocumentService::request_close`].
    pub fn close(&mut self, id: DocumentId) -> Result<()> {
        match self.request_close(id, None)? {
            CloseOutcome::Closed => Ok(()),
            CloseOutcome::NeedsDecision => {
                Err(Problem::new("close the document", "it has unsaved changes")
                    .with_remedy("save it, discard its changes explicitly, or cancel closing"))
            }
            CloseOutcome::Cancelled => unreachable!("no decision cannot cancel"),
        }
    }

    /// Ask to close a document, applying an explicit dirty-document decision when supplied.
    ///
    /// The document remains in the service until save and transaction cancellation have both
    /// succeeded. That ordering is what makes a failed save a failed *close*, rather than a closed
    /// tab whose recoverable work the interface can no longer reach.
    pub fn request_close(
        &mut self,
        id: DocumentId,
        decision: Option<CloseDecision>,
    ) -> Result<CloseOutcome> {
        let document = self
            .documents
            .get(&id)
            .ok_or_else(|| Problem::not_found("that document"))?;
        if document.is_dirty() {
            match decision {
                None => return Ok(CloseOutcome::NeedsDecision),
                Some(CloseDecision::Cancel) => return Ok(CloseOutcome::Cancelled),
                Some(CloseDecision::Save | CloseDecision::Discard) => {}
            }
        }

        if document.is_dirty() && decision == Some(CloseDecision::Save) {
            let target = self.path_of(document);
            self.documents
                .get_mut(&id)
                .expect("the document was checked above")
                .save(|document| match &target {
                    Some(path) => worldfile::write_to(document, path),
                    None => Ok(()),
                })?;
        }

        let mut document = self
            .documents
            .remove(&id)
            .expect("the document was checked above");
        document.cancel_all()?;
        self.revision.update(|()| {});
        Ok(CloseOutcome::Closed)
    }

    /// A document, for reading.
    #[must_use]
    pub fn get(&self, id: DocumentId) -> Option<&Document> {
        self.documents.get(&id)
    }

    /// A document, for writing — through its transaction system, which is its only write path.
    pub fn get_mut(&mut self, id: DocumentId) -> Option<&mut Document> {
        self.documents.get_mut(&id)
    }

    /// Every open document's identity.
    pub fn ids(&self) -> impl Iterator<Item = DocumentId> + '_ {
        self.documents.keys().copied()
    }

    /// Roll back every still-open transaction attributed to one revoked agent session.
    pub fn cancel_agent_session(&mut self, session: &str) -> Result<usize> {
        let mut cancelled = 0;
        for document in self.documents.values_mut() {
            cancelled += document.cancel_agent_session(session)?;
        }
        Ok(cancelled)
    }

    /// How many documents are open.
    #[must_use]
    pub fn len(&self) -> usize {
        self.documents.len()
    }

    /// Whether nothing is open.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.documents.is_empty()
    }

    /// Whether any open document has unsaved changes.
    #[must_use]
    pub fn any_dirty(&self) -> bool {
        self.documents.values().any(Document::is_dirty)
    }

    /// The revision of the *open set*, not of any document's content.
    #[must_use]
    pub fn revision(&self) -> Revision {
        self.revision.revision()
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;

    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let directory =
            std::env::temp_dir().join(format!("cy-editor-documents-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&directory);
        directory
    }

    #[test]
    fn a_journalled_document_offers_what_it_holds_when_reopened() {
        let directory = scratch("recovery");
        let mut service = DocumentService::new();
        service.journal_into(&directory);

        let (id, recovery) = service.open("worlds/city.cyworld").unwrap();
        assert!(recovery.is_none(), "a fresh journal has nothing to offer");

        service
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        // The editor stops abruptly: the service is dropped without saving.
        drop(service);

        let mut restarted = DocumentService::new();
        restarted.journal_into(&directory);
        let (_, recovery) = restarted.open("worlds/city.cyworld").unwrap();
        let recovery = recovery.expect("the journal holds the uncommitted work");
        assert_eq!(
            recovery.count(),
            1,
            "and says how many transactions are recoverable"
        );

        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn closing_a_document_rolls_back_its_open_transaction() {
        let mut service = DocumentService::new();
        let (id, _) = service.open("worlds/city.cyworld").unwrap();
        let document = service.get_mut(id).unwrap();
        document.begin("Half an edit", Actor::human("designer"));
        document.create_node(None).unwrap();
        assert!(document.is_transaction_open());

        assert_eq!(
            service.request_close(id, None).unwrap(),
            CloseOutcome::NeedsDecision
        );
        assert!(service.get(id).unwrap().is_transaction_open());
        assert_eq!(
            service
                .request_close(id, Some(CloseDecision::Discard))
                .unwrap(),
            CloseOutcome::Closed
        );
        assert!(service.is_empty());
    }

    #[test]
    fn a_dirty_close_requires_save_discard_or_cancel() {
        let mut service = DocumentService::new();
        let (id, _) = service.open("worlds/city.cyworld").unwrap();
        service
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        assert_eq!(
            service.request_close(id, None).unwrap(),
            CloseOutcome::NeedsDecision
        );
        assert!(service.close(id).unwrap_err().because.contains("unsaved"));
        assert_eq!(
            service
                .request_close(id, Some(CloseDecision::Cancel))
                .unwrap(),
            CloseOutcome::Cancelled
        );
        let document = service.get(id).expect("cancel keeps it open");
        assert!(document.is_dirty());
        assert_eq!(document.history().entries().len(), 1);
    }

    #[test]
    fn saving_a_dirty_document_closes_only_after_the_write_succeeds() {
        let directory = scratch("close-save");
        let mut service = DocumentService::new();
        service.rooted_at(&directory);
        let (id, _) = service.open("worlds/city.cyworld").unwrap();
        service
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        assert_eq!(
            service
                .request_close(id, Some(CloseDecision::Save))
                .unwrap(),
            CloseOutcome::Closed
        );
        assert!(directory.join("worlds/city.cyworld").is_file());
        std::fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn a_failed_save_leaves_the_dirty_document_open() {
        let directory = scratch("close-save-failure");
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(directory.join("worlds"), "not a directory").unwrap();
        let mut service = DocumentService::new();
        service.rooted_at(&directory);
        let (id, _) = service.open("worlds/city.cyworld").unwrap();
        service
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        let failure = service
            .request_close(id, Some(CloseDecision::Save))
            .expect_err("the target's parent is a file");
        assert!(
            failure.what.contains("write") || failure.what.contains("create"),
            "{failure:?}"
        );
        assert!(
            service
                .get(id)
                .expect("save failure keeps it open")
                .is_dirty()
        );
        std::fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn the_open_set_and_a_documents_content_have_separate_revisions() {
        let mut service = DocumentService::new();
        let (id, _) = service.open("worlds/city.cyworld").unwrap();
        let open_set = service.revision();
        let content = service.get(id).unwrap().revision();

        service
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        assert_eq!(
            service.revision(),
            open_set,
            "a tab strip does not rebuild for an edit"
        );
        assert!(
            service.get(id).unwrap().revision() > content,
            "and an inspector does"
        );
    }
}
