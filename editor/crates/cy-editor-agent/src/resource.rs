//! The read surface: what an agent can see, produced from the services the panels read.
//!
//! `editor-agent-interface`, "Resources are the read surface":
//!
//! > The agent interface SHALL expose as resources, at minimum: the scene hierarchy; an entity's
//! > components and properties as the inspector presents them; the current selection; the asset
//! > browser's contents and an asset's metadata; open documents and their dirty state; diagnostics —
//! > errors, warnings and validation results; and the play and build state.
//! >
//! > Resource content SHALL come from the same services the editor's own panels read, so that an
//! > agent and a panel cannot disagree about what is true.
//! >
//! > Reads SHALL NOT mutate. A read SHALL NOT dirty a document, alter selection, or produce a
//! > transaction.
//!
//! # How "reads do not mutate" is enforced rather than promised
//!
//! Every function in this module takes `&Editor`. Not `&mut`, not interior mutability, no cell: a
//! read cannot dirty a document, alter a selection or open a transaction because it does not hold
//! anything it could do those to, and the compiler is what says so. That is the whole enforcement,
//! and it is the reason this module has no state of its own — a cache here would need `&mut` and the
//! guarantee would become a convention.
//!
//! # Why the content is text
//!
//! A resource's content is a rendered string rather than a typed tree. Two reasons: the Model
//! Context Protocol's resources are text or blobs, so a typed tree would be converted at the
//! transport anyway; and the rendering is the one place the editor decides what an agent sees, which
//! is easier to review as a function than as a serialisation. Where a caller needs structure, it
//! invokes a command — which returns `Outcome::values`, typed.

use std::fmt::Write as _;

use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::observe::Cursor;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::Document;
use cy_editor_services::editor::Editor;

/// What kind of thing a resource describes.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum ResourceKind {
    /// The nodes of one document and how they nest.
    Hierarchy,
    /// One node's components and fields, as the inspector presents them.
    Node,
    /// What is selected right now.
    Selection,
    /// Which documents are open, and which of them have unsaved changes.
    Documents,
    /// The assets a document is backed by.
    Assets,
    /// Errors, warnings and validation results.
    Diagnostics,
    /// Whether a runtime is connected and what it is doing.
    PlayState,
    /// Long-running work and its progress.
    Operations,
}

impl ResourceKind {
    /// The scheme a resource of this kind is addressed under.
    #[must_use]
    pub const fn scheme(self) -> &'static str {
        match self {
            ResourceKind::Hierarchy => "hierarchy",
            ResourceKind::Node => "node",
            ResourceKind::Selection => "selection",
            ResourceKind::Documents => "documents",
            ResourceKind::Assets => "assets",
            ResourceKind::Diagnostics => "diagnostics",
            ResourceKind::PlayState => "play",
            ResourceKind::Operations => "operations",
        }
    }

    /// Every kind, for a listing. Exhaustive by construction: adding a kind without adding it here
    /// makes `listing` incomplete, and the test that counts them fails.
    pub const ALL: [ResourceKind; 8] = [
        ResourceKind::Hierarchy,
        ResourceKind::Node,
        ResourceKind::Selection,
        ResourceKind::Documents,
        ResourceKind::Assets,
        ResourceKind::Diagnostics,
        ResourceKind::PlayState,
        ResourceKind::Operations,
    ];
}

/// One readable thing.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Resource {
    /// How it is addressed: `selection:`, `hierarchy:<document>`, `node:<document>/<node>`.
    pub uri: String,
    /// What it is.
    pub kind: ResourceKind,
    /// A sentence saying what a reader will find, for a caller that cannot see the interface.
    pub description: String,
    /// The rendering. See the module note for why it is text.
    pub content: String,
}

/// The read surface.
///
/// A unit struct rather than a value with state, because it has none and must not: see the module
/// note on how "reads do not mutate" is enforced.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Resources;

impl Resources {
    /// Everything readable right now, without reading any of it.
    ///
    /// A listing is cheap and a read is not — a hierarchy of ten thousand nodes is a string — so
    /// they are separate calls, which is also the shape the protocol's own resource listing takes.
    #[must_use]
    pub fn list(editor: &Editor) -> Vec<(String, ResourceKind, String)> {
        let mut listing = vec![
            (
                "selection:".to_string(),
                ResourceKind::Selection,
                "The nodes and assets selected right now, in the order the selection holds them."
                    .to_string(),
            ),
            (
                "documents:".to_string(),
                ResourceKind::Documents,
                "Every open document, which of them is active, and which have unsaved changes."
                    .to_string(),
            ),
            (
                "diagnostics:".to_string(),
                ResourceKind::Diagnostics,
                "Errors, warnings and notices the editor has produced, newest last.".to_string(),
            ),
            (
                "play:".to_string(),
                ResourceKind::PlayState,
                "Whether a runtime is connected, how it is hosted, and how it failed if it did."
                    .to_string(),
            ),
            (
                "operations:".to_string(),
                ResourceKind::Operations,
                "Long-running work, with what each is doing and how far it has got.".to_string(),
            ),
        ];
        for document in editor.documents.ids() {
            listing.push((
                format!("hierarchy:{document}"),
                ResourceKind::Hierarchy,
                "The document's nodes and how they nest, in authored order.".to_string(),
            ));
            listing.push((
                format!("assets:{document}"),
                ResourceKind::Assets,
                "The files this document is stored across; the first is what names it.".to_string(),
            ));
        }
        listing
    }

    /// Read one, by its address.
    ///
    /// Fails with a `Problem` naming what would have worked, rather than an empty result: an agent
    /// that asked for `hierarchy:` with no document and got nothing back cannot tell that from a
    /// document with no nodes.
    pub fn read(editor: &Editor, uri: &str) -> Result<Resource> {
        let (scheme, rest) = uri.split_once(':').ok_or_else(|| {
            Problem::new(
                format!("read {uri:?}"),
                "a resource address is <scheme>:<path>",
            )
            .with_remedy("list the resources to see the addresses that exist")
        })?;

        match scheme {
            "selection" => Ok(Self::selection(editor)),
            "documents" => Ok(Self::documents(editor)),
            "diagnostics" => Ok(Self::diagnostics(editor)),
            "play" => Ok(Self::play_state(editor)),
            "operations" => Ok(Self::operations(editor)),
            "hierarchy" => Self::hierarchy(editor, Self::document_of(editor, rest)?),
            "assets" => Self::assets(editor, Self::document_of(editor, rest)?),
            "node" => {
                let (document, node) = rest.split_once('/').ok_or_else(|| {
                    Problem::new(
                        format!("read {uri:?}"),
                        "a node address is node:<document>/<node>",
                    )
                    .with_remedy("read the document's hierarchy to find a node's identifier")
                })?;
                Self::node(editor, Self::document_of(editor, document)?, node)
            }
            other => Err(Problem::new(
                format!("read {uri:?}"),
                format!("no resource scheme named {other:?}"),
            )
            .with_remedy(format!(
                "the schemes are: {}",
                ResourceKind::ALL
                    .iter()
                    .map(|kind| kind.scheme())
                    .collect::<Vec<_>>()
                    .join(", ")
            ))),
        }
    }

    fn document_of(editor: &Editor, text: &str) -> Result<DocumentId> {
        // An empty path means the active document, which is what an agent working in one document
        // means every time and is the difference between one call and two.
        if text.is_empty() {
            return editor.workspace.active().ok_or_else(|| {
                Problem::new("read the active document", "no document is active")
                    .with_remedy("open one, or name a document in the address")
            });
        }
        editor
            .documents
            .ids()
            .find(|id| id.to_string() == text)
            .ok_or_else(|| {
                Problem::new(
                    format!("read document {text:?}"),
                    "no such document is open",
                )
                .with_remedy("read documents: to see what is open")
            })
    }

    fn selection(editor: &Editor) -> Resource {
        let selection = editor.selection.get();
        let mut content = String::new();
        let _ = writeln!(content, "nodes: {}", selection.node_count());
        for node in selection.nodes() {
            let _ = writeln!(content, "  {node}");
        }
        let assets: Vec<&str> = selection.assets().collect();
        let _ = writeln!(content, "assets: {}", assets.len());
        for asset in assets {
            let _ = writeln!(content, "  {asset}");
        }
        Resource {
            uri: "selection:".to_string(),
            kind: ResourceKind::Selection,
            description: "The nodes and assets selected right now.".to_string(),
            content,
        }
    }

    fn documents(editor: &Editor) -> Resource {
        let active = editor.workspace.active();
        let mut content = String::new();
        for id in editor.documents.ids() {
            let Some(document) = editor.documents.get(id) else {
                continue;
            };
            let _ = writeln!(
                content,
                "{id} {}{} {} node(s), {} history entr(ies)",
                document
                    .assets()
                    .first()
                    .map_or("<unnamed>", String::as_str),
                if Some(id) == active { " [active]" } else { "" },
                document.content().node_count(),
                document.history().entries().len(),
            );
            let _ = writeln!(
                content,
                "  dirty: {}",
                if document.is_dirty() { "yes" } else { "no" }
            );
        }
        if content.is_empty() {
            content.push_str("no documents are open\n");
        }
        Resource {
            uri: "documents:".to_string(),
            kind: ResourceKind::Documents,
            description: "Every open document and whether it has unsaved changes.".to_string(),
            content,
        }
    }

    fn diagnostics(editor: &Editor) -> Resource {
        let mut content = String::new();
        // A cursor at the default position is the beginning of the log, so this reads everything
        // still held. It takes `&self`, which is what lets a read surface that must not mutate ask
        // for it at all.
        let mut cursor = Cursor::default();
        for notification in editor.notifications.drain_from(&mut cursor) {
            let _ = writeln!(
                content,
                "{:?}: {}",
                notification.severity, notification.message
            );
            if let Some(problem) = &notification.problem {
                let _ = writeln!(content, "  because {}", problem.because);
                if let Some(remedy) = &problem.remedy {
                    let _ = writeln!(content, "  remedy {remedy}");
                }
            }
        }
        if content.is_empty() {
            content.push_str("nothing to report\n");
        }
        let dropped = editor.notifications.dropped();
        if dropped != 0 {
            // Said rather than silent: a reader that missed a diagnostic and cannot tell is worse
            // off than one that knows it did. The log itself makes the same argument.
            let _ = writeln!(content, "({dropped} older notification(s) were dropped)");
        }
        Resource {
            uri: "diagnostics:".to_string(),
            kind: ResourceKind::Diagnostics,
            description: "Errors, warnings and notices, newest last.".to_string(),
            content,
        }
    }

    fn play_state(editor: &Editor) -> Resource {
        let mut content = String::new();
        let _ = writeln!(content, "hosting: {:?}", editor.hosting_mode());
        let _ = writeln!(
            content,
            "connected: {}",
            if editor.runtime.is_connected() {
                "yes"
            } else {
                "no"
            }
        );
        if let Some(endpoint) = editor.runtime.endpoint() {
            let _ = writeln!(content, "endpoint: {endpoint}");
        }
        if let Some(failure) = editor.runtime.failure() {
            // How it failed, not merely that it did: an agent that is told "the runtime is gone" and
            // not why will try the same thing again.
            let _ = writeln!(content, "failed: {}", failure.because);
        }
        Resource {
            uri: "play:".to_string(),
            kind: ResourceKind::PlayState,
            description: "Whether a runtime is connected and how it is hosted.".to_string(),
            content,
        }
    }

    fn operations(editor: &Editor) -> Resource {
        let mut content = String::new();
        for operation in editor.operations.all() {
            let _ = writeln!(content, "{}: {:?}", operation.label(), operation.state());
        }
        if content.is_empty() {
            content.push_str("nothing is running\n");
        }
        Resource {
            uri: "operations:".to_string(),
            kind: ResourceKind::Operations,
            description: "Long-running work and how far it has got.".to_string(),
            content,
        }
    }

    fn hierarchy(editor: &Editor, id: DocumentId) -> Result<Resource> {
        let document = Self::open(editor, id)?;
        let mut content = String::new();
        for root in document.content().roots() {
            Self::write_node(document, *root, 0, &mut content);
        }
        if content.is_empty() {
            content.push_str("the document has no nodes\n");
        }
        Ok(Resource {
            uri: format!("hierarchy:{id}"),
            kind: ResourceKind::Hierarchy,
            description: "The document's nodes and how they nest, in authored order.".to_string(),
            content,
        })
    }

    fn write_node(document: &Document, node: NodeId, depth: usize, out: &mut String) {
        let Some(state) = document.content().node(node) else {
            return;
        };
        for _ in 0..depth {
            out.push_str("  ");
        }
        let _ = writeln!(
            out,
            "{node} layer={:?} components={}{}",
            state.layer,
            state.components.len(),
            state
                .prefab
                .as_ref()
                .map_or(String::new(), |prefab| format!(" prefab={prefab}"))
        );
        for child in &state.children {
            Self::write_node(document, *child, depth + 1, out);
        }
    }

    fn node(editor: &Editor, id: DocumentId, node: &str) -> Result<Resource> {
        let document = Self::open(editor, id)?;
        let node_id = document
            .content()
            .nodes()
            .find(|candidate| candidate.to_string() == node)
            .ok_or_else(|| {
                Problem::new(
                    format!("read node {node:?}"),
                    "the document has no such node",
                )
                .with_remedy("read the document's hierarchy to see what it has")
            })?;
        let state = document
            .content()
            .node(node_id)
            .ok_or_else(|| Problem::new(format!("read node {node:?}"), "the node has no state"))?;

        // As the inspector presents them: by type name and field name, not by identity. A resource
        // that printed numeric identities would be true and useless — an agent cannot act on
        // `type 7 field 3`, and the names are what the schema is for.
        let mut content = String::new();
        let _ = writeln!(content, "node: {node_id}");
        if let Some(parent) = state.parent {
            let _ = writeln!(content, "parent: {parent}");
        }
        let _ = writeln!(content, "children: {}", state.children.len());
        for (type_id, fields) in &state.components {
            let definition = document.schema().type_of(*type_id);
            let name = definition.map_or("<unknown type>", |definition| definition.name.as_str());
            let _ = writeln!(content, "component {name}");
            for (field_id, value) in fields {
                let field = definition.and_then(|definition| definition.field(*field_id));
                let field_name = field.map_or("<unknown field>", |field| field.name.as_str());
                let overridden = state.overrides.contains_key(&(*type_id, *field_id));
                let _ = writeln!(
                    content,
                    "  {field_name} = {value}{}{}",
                    if overridden { " [overridden]" } else { "" },
                    field.map_or("", |field| if field.writable { "" } else { " [read-only]" })
                );
            }
        }
        Ok(Resource {
            uri: format!("node:{id}/{node_id}"),
            kind: ResourceKind::Node,
            description: "One node's components and fields, as the inspector presents them."
                .to_string(),
            content,
        })
    }

    fn assets(editor: &Editor, id: DocumentId) -> Result<Resource> {
        let document = Self::open(editor, id)?;
        let mut content = String::new();
        for (index, asset) in document.assets().iter().enumerate() {
            let _ = writeln!(
                content,
                "{asset}{}",
                if index == 0 { " [primary]" } else { "" }
            );
        }
        Ok(Resource {
            uri: format!("assets:{id}"),
            kind: ResourceKind::Assets,
            description: "The files this document is stored across.".to_string(),
            content,
        })
    }

    fn open(editor: &Editor, id: DocumentId) -> Result<&Document> {
        editor.documents.get(id).ok_or_else(|| {
            Problem::new(format!("read document {id}"), "no such document is open")
                .with_remedy("read documents: to see what is open")
        })
    }
}
