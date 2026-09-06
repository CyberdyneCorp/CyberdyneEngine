//! What a command handler is given, expressed as a trait so that this crate stays below services.
//!
//! A command needs documents, selection, the acting actor and a way to notify — all of which are
//! *services*, and services are layer 3. Depending on them from layer 2 would be the upward
//! dependency `editor-rust-application` requires to be a build error, so the dependency is inverted:
//! this crate declares the interface a handler needs, and `cy-editor-services` implements it.
//!
//! The second payoff is testability. A test double implementing [`CommandContext`] is thirty lines
//! and needs no engine, no window and no service graph, which is how this crate's own tests invoke
//! commands — and how `editor-rust-application`'s "Services, models, view models, and commands SHALL
//! be testable **headlessly**" is satisfied by construction rather than by effort.

use std::collections::BTreeMap;

use cy_editor_core::Actor;
use cy_editor_core::ids::DocumentId;
use cy_editor_core::value::Value;
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;

/// What a command invocation produced.
///
/// `summary` is for a person and a log; `values` is for a machine. Both, because the same invocation
/// serves a menu item and an agent, and an agent that has to parse a sentence to learn which entity
/// was created is an agent that will get it wrong.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Outcome {
    /// One line a person reads: "Created 3 entities".
    pub summary: String,
    /// Structured results, keyed by name.
    pub values: BTreeMap<String, Value>,
}

impl Outcome {
    /// An outcome with a summary and no values.
    pub fn new(summary: impl Into<String>) -> Self {
        Self {
            summary: summary.into(),
            values: BTreeMap::new(),
        }
    }

    /// Attach a structured result.
    #[must_use]
    pub fn with(mut self, name: impl Into<String>, value: Value) -> Self {
        self.values.insert(name.into(), value);
        self
    }
}

/// What a command handler may reach.
///
/// Deliberately small. Everything a command does to persistent state goes through a document, and a
/// document's only write path is a transaction — so a handler cannot mutate project state any other
/// way however much it would like to, and this trait is where that stays true.
pub trait CommandContext {
    /// The document a command with no explicit target acts on.
    fn active_document(&self) -> Option<DocumentId>;

    /// A document, for reading.
    fn document(&self, id: DocumentId) -> Option<&Document>;

    /// A document, for writing — which still means through its transaction system.
    fn document_mut(&mut self, id: DocumentId) -> Option<&mut Document>;

    /// Who is acting. Attribution, not authorisation; see the crate note.
    fn actor(&self) -> Actor;

    /// What is selected.
    fn selection(&self) -> &Selection;

    /// Replace the selection.
    fn set_selection(&mut self, selection: Selection);

    /// Report something to the user. A notification, not a log line.
    fn notify(&mut self, message: &str);
}
