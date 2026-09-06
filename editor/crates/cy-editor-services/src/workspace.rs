//! The workspace: what is open, how it is laid out, and where a camera position is *not* stored.
//!
//! `editor-documents-and-transactions`: "View state — camera position, expanded tree nodes, active
//! tab, panel layout, filter settings — SHALL be stored with the workspace or the user's settings,
//! and SHALL NOT dirty an asset or enter its history."
//!
//! And `editor-rust-application`, from the other direction: "**Presentation state SHALL be separated
//! from authoritative state** in persistence as well as in memory: layout, expansion, filters, and
//! scroll positions belong to workspace or user settings and SHALL NOT dirty a document."
//!
//! The separation here is structural rather than careful. A [`ViewState`] lives in this type, keyed
//! by document identity; a `Document` has no field that could hold one; and there is no operation in
//! `cy-editor-documents` that could carry one into a transaction. Moving the camera cannot dirty a
//! document because there is nowhere for the camera to go that a document would see.

use std::collections::BTreeMap;

use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::observe::{Revision, Versioned};

/// One document's presentation state.
///
/// Everything here is a user's view of a document and none of it is the document. It is persisted
/// with the workspace so that "WHEN the editor restarts THEN open documents, layout, and view state
/// SHALL be restored" holds, and persisted *separately* so that restoring it cannot dirty anything.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct ViewState {
    /// Where the viewport camera is, as position and a look direction.
    pub camera: Option<([f32; 3], [f32; 3])>,
    /// Which tree nodes are expanded.
    pub expanded: Vec<NodeId>,
    /// The active tab within the document's editor.
    pub active_tab: Option<String>,
    /// The hierarchy or asset filter in force.
    pub filter: String,
    /// Vertical scroll position, in rows.
    pub scroll: u32,
}

/// What is open and how it looks.
#[derive(Debug, Default)]
pub struct Workspace {
    open: Versioned<Vec<DocumentId>>,
    view_state: BTreeMap<DocumentId, ViewState>,
    layout: Versioned<String>,
    active: Option<DocumentId>,
}

impl Workspace {
    /// An empty workspace.
    #[must_use]
    pub fn new() -> Self {
        Self {
            open: Versioned::new(Vec::new()),
            view_state: BTreeMap::new(),
            layout: Versioned::new(String::new()),
            active: None,
        }
    }

    /// The documents that are open, in the order they were opened.
    #[must_use]
    pub fn open_documents(&self) -> &[DocumentId] {
        self.open.get()
    }

    /// The revision of the open set, for a tab strip's watch.
    #[must_use]
    pub fn revision(&self) -> Revision {
        self.open.revision()
    }

    /// The document commands with no explicit target act on.
    #[must_use]
    pub const fn active(&self) -> Option<DocumentId> {
        self.active
    }

    /// Make a document active.
    pub fn activate(&mut self, document: DocumentId) {
        self.active = Some(document);
    }

    /// Record that a document is open.
    pub fn opened(&mut self, document: DocumentId) {
        self.open.update(|open| {
            if !open.contains(&document) {
                open.push(document);
            }
        });
        self.view_state.entry(document).or_default();
        self.active = Some(document);
    }

    /// Record that a document is closed, keeping its view state for when it is reopened.
    ///
    /// Kept rather than dropped, because reopening a world and finding the camera at the origin is
    /// the behaviour that makes people stop closing tabs.
    pub fn closed(&mut self, document: DocumentId) {
        self.open
            .update(|open| open.retain(|open| *open != document));
        if self.active == Some(document) {
            self.active = self.open.get().last().copied();
        }
    }

    /// A document's view state.
    #[must_use]
    pub fn view_state(&self, document: DocumentId) -> Option<&ViewState> {
        self.view_state.get(&document)
    }

    /// Change a document's view state. **Cannot dirty the document**; see the module note.
    pub fn update_view_state(&mut self, document: DocumentId, mutate: impl FnOnce(&mut ViewState)) {
        mutate(self.view_state.entry(document).or_default());
    }

    /// The panel layout, as the interface layer chooses to describe it.
    ///
    /// Opaque here on purpose: `editor-rust-application` requires that the interface toolkit "SHALL
    /// NOT appear in editor extension interfaces", and a typed layout tree would be the toolkit's
    /// vocabulary arriving in the service layer by the back door.
    #[must_use]
    pub fn layout(&self) -> &str {
        self.layout.get()
    }

    /// Replace the layout.
    pub fn set_layout(&mut self, layout: impl Into<String>) {
        self.layout.set(layout.into());
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::value::ValueKind;
    use cy_editor_documents::Document;

    use super::*;

    #[test]
    fn looking_around_dirties_nothing() {
        // The specification's scenario, tested the only way it can be: by doing everything a user
        // does when they are looking rather than editing, and asserting the document did not move.
        let mut document = Document::new("worlds/city.cyworld");
        let transform = document.schema_mut().declare_type("Transform", false);
        document
            .schema_mut()
            .declare_field(transform, "position", ValueKind::Vec3, "where it is")
            .unwrap();
        let node = document
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None)
            })
            .unwrap();
        document.save(|_| Ok(())).unwrap();
        assert!(!document.is_dirty());
        let revision = document.revision();
        let entries = document.history().entries().len();

        let mut workspace = Workspace::new();
        workspace.opened(document.id());
        workspace.update_view_state(document.id(), |view| {
            view.camera = Some(([10.0, 5.0, -3.0], [0.0, 0.0, 1.0]));
            view.expanded.push(node);
            view.filter = "lamp".into();
            view.scroll = 42;
            view.active_tab = Some("Scene".into());
        });
        workspace.set_layout("{\"docked\":[\"hierarchy\",\"inspector\"]}");

        assert!(
            !document.is_dirty(),
            "moving a camera is not an authoring change"
        );
        assert_eq!(
            document.revision(),
            revision,
            "and it does not move the content revision"
        );
        assert_eq!(
            document.history().entries().len(),
            entries,
            "and creates no history entry"
        );
    }

    #[test]
    fn view_state_survives_closing_and_reopening() {
        let document = DocumentId::of_asset("worlds/city.cyworld");
        let mut workspace = Workspace::new();
        workspace.opened(document);
        workspace.update_view_state(document, |view| view.scroll = 17);

        workspace.closed(document);
        assert!(workspace.open_documents().is_empty());

        workspace.opened(document);
        assert_eq!(workspace.view_state(document).unwrap().scroll, 17);
    }

    #[test]
    fn closing_the_active_document_activates_another() {
        let city = DocumentId::of_asset("worlds/city.cyworld");
        let forest = DocumentId::of_asset("worlds/forest.cyworld");
        let mut workspace = Workspace::new();
        workspace.opened(city);
        workspace.opened(forest);
        assert_eq!(workspace.active(), Some(forest));
        workspace.closed(forest);
        assert_eq!(workspace.active(), Some(city));
    }
}
