//! A document: the unit of editing, and the only thing that can write its own content. Tasks 3.2–3.6.
//!
//! "Everything editable SHALL be presented as a **document**: world, scene, prefab, material,
//! animation, VFX graph, UI layout, behaviour graph, terrain, sequence, and any plugin-supplied
//! kind. A document SHALL expose: identity, its backing asset or assets, dirty state, save and
//! reload, and its transaction context."
//!
//! **A document is not a file.** [`Document::assets`] is a list; one of them is primary and names
//! the document's identity. A world backed by hundreds of authoring chunks is one document with one
//! dirty state and one history, which is the specification's own scenario and the reason this type
//! does not simply wrap a path.
//!
//! --- THE TRANSACTION SCOPE, AND WHY IT IS EXPLICIT RATHER THAN A GUARD ------------------------------
//!
//! An interactive transaction spans *frames*: a gizmo drag begins on press, updates for as long as
//! the mouse moves, and commits on release. A scope guard holding `&mut Document` cannot live across
//! frames without the editor's whole state being borrowed for the duration, so the scope is a stack
//! inside the document — [`Document::begin`], [`Document::record`], [`Document::commit`],
//! [`Document::cancel`] — and [`Document::with_transaction`] is the closure form for the many cases
//! that do fit in one call, where an early return rolls back automatically.
//!
//! Nesting collapses by default: an inner `begin` joins the outer scope and the inner `commit` does
//! not produce an entry, so "a tool calling another tool produces one history entry named for the
//! user's intent". [`Document::begin_isolated`] is the escape for a tool that genuinely needs its
//! own entry, and it is deliberately the longer name.

use std::path::Path;

use cy_editor_core::Actor;
use cy_editor_core::ids::{DocumentId, FieldId, NodeId, TypeId};
use cy_editor_core::observe::Revision;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;

use crate::content::{DocumentContent, WriteToken};
use crate::history::{History, HistoryStatus};
use crate::journal::{Journal, Recovery};
use crate::operation::Operation;
use crate::schema::DocumentSchema;
use crate::transaction::{Transaction, TransactionId};

/// The default per-document history budget, in bytes.
///
/// A number rather than "unbounded", because an unbounded history is a memory leak with a friendly
/// name. Sixty-four mebibytes holds tens of thousands of ordinary edits and a few hundred terrain
/// strokes; when it bites, [`HistoryStatus::truncated`] says so.
pub const DEFAULT_HISTORY_BUDGET: usize = 64 * 1024 * 1024;

/// One open transaction scope.
struct Scope {
    id: TransactionId,
    name: String,
    actor: Actor,
    coalesce_key: Option<String>,
    operations: Vec<Operation>,
    /// How many operations had been recorded when this scope opened, for a nested cancel.
    isolated: bool,
}

/// A document: identity, backing assets, content, history, journal, and dirty state.
pub struct Document {
    id: DocumentId,
    assets: Vec<String>,
    schema: DocumentSchema,
    content: DocumentContent,
    history: History,
    journal: Option<Journal>,
    dirty: bool,
    next_transaction: u64,
    scopes: Vec<Scope>,
}

impl Document {
    /// A new document backed by `primary_asset`.
    #[must_use]
    pub fn new(primary_asset: impl Into<String>) -> Self {
        let primary = primary_asset.into();
        let id = DocumentId::of_asset(&primary);
        Self {
            id,
            assets: vec![primary],
            schema: DocumentSchema::new(),
            content: DocumentContent::new(id),
            history: History::new(DEFAULT_HISTORY_BUDGET),
            journal: None,
            dirty: false,
            next_transaction: 1,
            scopes: Vec::new(),
        }
    }

    /// Attach a journal under `directory`, so that committed transactions are persisted.
    ///
    /// Separate from construction because a preview document — a material editor's isolated world —
    /// has a full transaction system and nothing to recover: journalling it would write files for
    /// state that is discarded when the panel closes.
    pub fn attach_journal(&mut self, directory: &Path) -> Result<()> {
        self.journal = Some(Journal::open(directory, self.id)?);
        Ok(())
    }

    /// Whatever the journal holds, for the recovery prompt.
    pub fn recoverable(&self) -> Result<Option<Recovery>> {
        match &self.journal {
            Some(journal) => journal.read().map(Some),
            None => Ok(None),
        }
    }

    /// Replay recovered transactions into this document, attributed to the editor.
    ///
    /// The recovered work keeps its *original* actor on each entry — a reviewer must still be able
    /// to tell a person's edit from an agent's after a crash — and the replay itself is not a new
    /// authorship claim, which is why nothing here rewrites attribution.
    pub fn recover(&mut self, recovery: &Recovery) -> Result<usize> {
        for transaction in &recovery.transactions {
            let token = WriteToken::issue(transaction.id.as_u64());
            for operation in &transaction.operations {
                self.content.apply(operation, &token)?;
            }
            self.history.push(transaction.clone());
        }
        self.dirty = !recovery.transactions.is_empty();
        Ok(recovery.transactions.len())
    }

    /// The document's identity.
    #[must_use]
    pub const fn id(&self) -> DocumentId {
        self.id
    }

    /// Every backing asset. The first is primary and is what names the document.
    #[must_use]
    pub fn assets(&self) -> &[String] {
        &self.assets
    }

    /// Add a backing asset — an authoring chunk, a layer, a scene instance.
    ///
    /// Does not dirty the document and does not enter history: which files a world happens to be
    /// stored across is the storage layer's business, not the author's.
    pub fn add_asset(&mut self, asset: impl Into<String>) {
        self.assets.push(asset.into());
    }

    /// The document's schema, which owns type and field identity.
    #[must_use]
    pub const fn schema(&self) -> &DocumentSchema {
        &self.schema
    }

    /// The schema, mutably, for declaring types before content exists.
    pub const fn schema_mut(&mut self) -> &mut DocumentSchema {
        &mut self.schema
    }

    /// The document's content, readable by anything and writable by nothing.
    #[must_use]
    pub const fn content(&self) -> &DocumentContent {
        &self.content
    }

    /// The content's revision, which a view model watches.
    #[must_use]
    pub const fn revision(&self) -> Revision {
        self.content.revision()
    }

    /// Whether there are unsaved changes.
    #[must_use]
    pub const fn is_dirty(&self) -> bool {
        self.dirty
    }

    /// The undo history.
    #[must_use]
    pub const fn history(&self) -> &History {
        &self.history
    }

    /// What to show in a status line.
    #[must_use]
    pub const fn history_status(&self) -> HistoryStatus {
        self.history.status()
    }

    /// Allocate a node identity, for a caller about to create one.
    pub fn allocate_node(&mut self) -> NodeId {
        self.content.allocate_node()
    }

    /// Whether a transaction is open.
    #[must_use]
    pub fn is_transaction_open(&self) -> bool {
        !self.scopes.is_empty()
    }

    /// Open a transaction scope. A nested call joins the one already open.
    pub fn begin(&mut self, name: impl Into<String>, actor: Actor) -> TransactionId {
        self.open(name, actor, None, false)
    }

    /// Open a transaction scope that participates in an interaction, for coalescing.
    ///
    /// The key identifies the interaction — one gizmo drag, one text field being typed into — and
    /// consecutive committed transactions carrying the same key and the same actor become one
    /// history entry. See `transaction`'s module note for why this is a key rather than a timer.
    pub fn begin_interaction(
        &mut self,
        name: impl Into<String>,
        actor: Actor,
        key: impl Into<String>,
    ) -> TransactionId {
        self.open(name, actor, Some(key.into()), false)
    }

    /// Open a transaction scope that does **not** collapse into an enclosing one.
    ///
    /// The longer name is deliberate: collapsing is the behaviour the specification asks for, so
    /// isolation should be the thing a reader notices.
    pub fn begin_isolated(&mut self, name: impl Into<String>, actor: Actor) -> TransactionId {
        self.open(name, actor, None, true)
    }

    fn open(
        &mut self,
        name: impl Into<String>,
        actor: Actor,
        coalesce_key: Option<String>,
        isolated: bool,
    ) -> TransactionId {
        let id = TransactionId::from_raw(self.next_transaction);
        self.next_transaction += 1;
        self.scopes.push(Scope {
            id,
            name: name.into(),
            actor,
            coalesce_key,
            operations: Vec::new(),
            isolated,
        });
        id
    }

    /// Record an operation into the open scope, applying it to the content immediately.
    ///
    /// Applied immediately because that is what makes an interactive transaction show a live
    /// preview: the intermediate values are in the document while the drag is happening, and only
    /// the committed result enters history.
    pub fn record(&mut self, operation: Operation) -> Result<()> {
        let Some(scope) = self.scopes.last() else {
            return Err(Problem::new(
                "record an operation",
                "no transaction is open, and there is no other write path",
            )
            .with_remedy("call begin() first, or use with_transaction()"));
        };
        let token = WriteToken::issue(scope.id.as_u64());
        self.content.apply(&operation, &token)?;
        self.scopes
            .last_mut()
            .expect("just checked")
            .operations
            .push(operation);
        self.dirty = true;
        Ok(())
    }

    /// Commit the innermost open scope.
    ///
    /// A nested, non-isolated scope hands its operations to the scope below it and produces no
    /// history entry — "a tool calling another tool produces one history entry named for the user's
    /// intent". The outermost scope, or an isolated one, produces the entry.
    pub fn commit(&mut self) -> Result<Option<TransactionId>> {
        let Some(mut scope) = self.scopes.pop() else {
            return Err(Problem::new("commit", "no transaction is open"));
        };

        if !scope.isolated && !self.scopes.is_empty() {
            let outer = self
                .scopes
                .last_mut()
                .expect("just checked it is not empty");
            outer.operations.append(&mut scope.operations);
            return Ok(None);
        }

        if scope.operations.is_empty() {
            // A transaction that changed nothing is not an entry. A history full of "Move" entries
            // that moved nothing is a history nobody reads.
            return Ok(None);
        }

        let mut transaction = Transaction {
            id: scope.id,
            document: self.id,
            name: scope.name,
            actor: scope.actor,
            operations: scope.operations,
            coalesce_key: scope.coalesce_key,
        };
        transaction.compact();

        if let Some(journal) = self.journal.as_mut() {
            journal.append(&transaction)?;
        }
        self.history.push(transaction);
        self.dirty = true;
        Ok(Some(scope.id))
    }

    /// Abandon the innermost open scope, rolling its operations back.
    ///
    /// "WHEN an interactive transaction is cancelled THEN the document SHALL return to its
    /// pre-transaction state and no entry SHALL be recorded."
    pub fn cancel(&mut self) -> Result<()> {
        let Some(scope) = self.scopes.pop() else {
            return Err(Problem::new("cancel", "no transaction is open"));
        };
        let token = WriteToken::issue(scope.id.as_u64());
        for operation in scope.operations.iter().rev() {
            self.content.apply(&operation.inverse(), &token)?;
        }
        Ok(())
    }

    /// Abandon every open scope. What closing a document does.
    pub fn cancel_all(&mut self) -> Result<()> {
        while !self.scopes.is_empty() {
            self.cancel()?;
        }
        Ok(())
    }

    /// Run `edit` inside a transaction, committing on success and rolling back on failure.
    ///
    /// The form most callers want: an early return cannot leave a half-applied transaction, because
    /// there is no path out of this function that does not either commit or cancel.
    pub fn with_transaction<R>(
        &mut self,
        name: impl Into<String>,
        actor: Actor,
        edit: impl FnOnce(&mut Self) -> Result<R>,
    ) -> Result<R> {
        self.begin(name, actor);
        match edit(self) {
            Ok(value) => {
                self.commit()?;
                Ok(value)
            }
            Err(problem) => {
                self.cancel()?;
                Err(problem)
            }
        }
    }

    /// Undo the last entry. Returns what was undone.
    pub fn undo(&mut self) -> Result<Option<Transaction>> {
        if self.is_transaction_open() {
            return Err(Problem::new("undo", "a transaction is still open")
                .with_remedy("commit or cancel it first"));
        }
        let Some(transaction) = self.history.undo() else {
            return Ok(None);
        };
        let token = WriteToken::issue(transaction.id.as_u64());
        for operation in transaction.operations.iter().rev() {
            self.content.apply(&operation.inverse(), &token)?;
        }
        self.dirty = true;
        Ok(Some(transaction))
    }

    /// Redo the next entry. Returns what was redone.
    pub fn redo(&mut self) -> Result<Option<Transaction>> {
        if self.is_transaction_open() {
            return Err(Problem::new("redo", "a transaction is still open")
                .with_remedy("commit or cancel it first"));
        }
        let Some(transaction) = self.history.redo() else {
            return Ok(None);
        };
        let token = WriteToken::issue(transaction.id.as_u64());
        for operation in &transaction.operations {
            self.content.apply(operation, &token)?;
        }
        self.dirty = true;
        Ok(Some(transaction))
    }

    /// A working copy: the same schema and content, an empty history, and no journal.
    ///
    /// What a merge needs. A three-way merge is between *states* — a base, ours and theirs — and
    /// two of those are usually revisions nobody has open; giving the copy a history would invite
    /// the mistake of merging histories, which is a different and much harder problem that the
    /// specification does not ask for.
    #[must_use]
    pub fn fork(&self) -> Self {
        Self {
            id: self.id,
            assets: self.assets.clone(),
            schema: self.schema.clone(),
            content: self.content.clone(),
            history: History::new(DEFAULT_HISTORY_BUDGET),
            journal: None,
            dirty: false,
            next_transaction: self.next_transaction,
            scopes: Vec::new(),
        }
    }

    /// Mark the document saved and discard its journal.
    ///
    /// Takes a closure that writes the assets, and resets the journal only when that closure
    /// succeeded. A journal cleared before the write is a journal that cannot recover the write that
    /// then failed — which is exactly the work the user most wants back.
    pub fn save(&mut self, write_assets: impl FnOnce(&Self) -> Result<()>) -> Result<()> {
        write_assets(self)?;
        if let Some(journal) = self.journal.as_mut() {
            journal.reset()?;
        }
        self.dirty = false;
        Ok(())
    }

    // --- convenience over `record`, for the operations every caller builds ---------------------

    /// Create a node under an optional parent, inside the open transaction.
    pub fn create_node(&mut self, parent: Option<NodeId>) -> Result<NodeId> {
        let node = self.allocate_node();
        self.record(Operation::CreateNode { node, parent })?;
        Ok(node)
    }

    /// Add a component with its initial field values, inside the open transaction.
    pub fn add_component(
        &mut self,
        node: NodeId,
        component: TypeId,
        fields: Vec<(FieldId, Value)>,
    ) -> Result<()> {
        self.record(Operation::AddComponent {
            node,
            component,
            after: fields,
        })
    }

    /// Set a field, recording its before value from the document so that undo is exact.
    ///
    /// Reading the before value here rather than asking the caller for it is what makes a
    /// mis-recorded delta impossible: a caller that passed the wrong before value would produce an
    /// undo that silently restored something else.
    pub fn set_field(
        &mut self,
        node: NodeId,
        component: TypeId,
        field: FieldId,
        value: Value,
    ) -> Result<()> {
        if let Some(definition) = self
            .schema
            .field(component, field)
            .filter(|definition| !definition.writable)
        {
            return Err(
                Problem::new(format!("set {}", definition.name), "the field is read-only")
                    .with_remedy("edit the source this value is derived from"),
            );
        }
        let before = self
            .content
            .field(node, component, field)
            .cloned()
            .ok_or_else(|| Problem::not_found("that field on that node"))?;
        self.record(Operation::SetField {
            node,
            component,
            field,
            before,
            after: value,
        })
    }

    /// Delete a node, recording what it was so that undo restores it exactly.
    pub fn delete_node(&mut self, node: NodeId) -> Result<()> {
        let was = self
            .content
            .node(node)
            .cloned()
            .ok_or_else(|| Problem::not_found("that node"))?;
        self.record(Operation::DeleteNode {
            node,
            was: Box::new(was),
        })
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::value::ValueKind;

    use super::*;

    /// A document with a `Transform { position }` type and one node carrying it.
    fn city() -> (Document, NodeId, TypeId, FieldId) {
        let mut document = Document::new("worlds/city.cyworld");
        let transform = document.schema_mut().declare_type("Transform", false);
        let position = document
            .schema_mut()
            .declare_field(transform, "position", ValueKind::Vec3, "where the node is")
            .unwrap();

        let actor = Actor::human("designer");
        let node = document
            .with_transaction("Create lamp", actor, |document| {
                let node = document.create_node(None)?;
                document.add_component(
                    node,
                    transform,
                    vec![(position, Value::Vec3([0.0, 0.0, 0.0]))],
                )?;
                Ok(node)
            })
            .unwrap();
        (document, node, transform, position)
    }

    #[test]
    fn a_composite_tool_produces_one_history_entry() {
        // "WHEN a tool duplicates an object, assigns a prefab, and sets a transform THEN history
        // SHALL show one entry named for the tool's action."
        let (document, ..) = city();
        assert_eq!(document.history().entries().len(), 1);
        assert_eq!(document.history().entries()[0].name, "Create lamp");
    }

    #[test]
    fn nested_transactions_collapse_into_the_outermost() {
        let (mut document, node, transform, position) = city();
        let actor = Actor::human("designer");

        document.begin("Align lamps", actor.clone());
        document.begin("Move one lamp", actor);
        document
            .set_field(node, transform, position, Value::Vec3([1.0, 0.0, 0.0]))
            .unwrap();
        assert_eq!(
            document.commit().unwrap(),
            None,
            "an inner commit produces no entry"
        );
        document.commit().unwrap();

        assert_eq!(document.history().entries().len(), 2);
        assert_eq!(document.history().entries()[1].name, "Align lamps");
    }

    #[test]
    fn an_isolated_nested_transaction_keeps_its_own_entry() {
        let (mut document, node, transform, position) = city();
        let actor = Actor::human("designer");

        document.begin("Outer", actor.clone());
        document.begin_isolated("Inner", actor);
        document
            .set_field(node, transform, position, Value::Vec3([1.0, 0.0, 0.0]))
            .unwrap();
        assert!(
            document.commit().unwrap().is_some(),
            "an isolated commit is its own entry"
        );
        document.commit().unwrap();

        let names: Vec<&str> = document
            .history()
            .entries()
            .iter()
            .map(|entry| entry.name.as_str())
            .collect();
        assert_eq!(names, vec!["Create lamp", "Inner"]);
    }

    #[test]
    fn a_cancelled_transaction_leaves_the_document_exactly_as_it_was() {
        let (mut document, node, transform, position) = city();
        let before = document.content().clone();
        let entries = document.history().entries().len();

        document.begin("Move", Actor::human("designer"));
        document
            .set_field(node, transform, position, Value::Vec3([9.0, 9.0, 9.0]))
            .unwrap();
        document.cancel().unwrap();

        assert_eq!(
            document.content().field(node, transform, position),
            before.field(node, transform, position)
        );
        assert_eq!(
            document.history().entries().len(),
            entries,
            "and no entry was recorded"
        );
    }

    #[test]
    fn a_gizmo_drag_is_one_entry_committed_on_release() {
        let (mut document, node, transform, position) = city();
        let actor = Actor::human("designer");
        let entries_before = document.history().entries().len();

        for step in 1_u8..=200 {
            document.begin_interaction("Move lamp", actor.clone(), "gizmo-drag-1");
            let along = f32::from(step);
            document
                .set_field(node, transform, position, Value::Vec3([along, 0.0, 0.0]))
                .unwrap();
            document.commit().unwrap();
        }

        assert_eq!(document.history().entries().len(), entries_before + 1);
        let entry = document.history().entries().last().unwrap();
        assert_eq!(
            entry.operations.len(),
            1,
            "two hundred positions, one delta"
        );
        match &entry.operations[0] {
            Operation::SetField { before, after, .. } => {
                assert_eq!(
                    *before,
                    Value::Vec3([0.0, 0.0, 0.0]),
                    "the first before value"
                );
                assert_eq!(
                    *after,
                    Value::Vec3([200.0, 0.0, 0.0]),
                    "the last after value"
                );
            }
            other => panic!("expected a SetField, got {other:?}"),
        }
    }

    #[test]
    fn undo_is_exact_and_redo_restores() {
        let (mut document, node, transform, position) = city();
        let original = document
            .content()
            .field(node, transform, position)
            .cloned()
            .unwrap();

        document
            .with_transaction("Move", Actor::human("designer"), |document| {
                document.set_field(node, transform, position, Value::Vec3([4.0, 0.0, 0.0]))
            })
            .unwrap();

        document.undo().unwrap();
        assert_eq!(
            document.content().field(node, transform, position),
            Some(&original)
        );

        document.redo().unwrap();
        assert_eq!(
            document.content().field(node, transform, position),
            Some(&Value::Vec3([4.0, 0.0, 0.0]))
        );
    }

    #[test]
    fn undo_restores_a_deleted_node_exactly() {
        let (mut document, node, transform, position) = city();
        document
            .with_transaction("Delete lamp", Actor::human("designer"), |document| {
                document.delete_node(node)
            })
            .unwrap();
        assert!(document.content().node(node).is_none());

        document.undo().unwrap();
        assert!(
            document.content().node(node).is_some(),
            "the node came back"
        );
        assert_eq!(
            document.content().field(node, transform, position),
            Some(&Value::Vec3([0.0, 0.0, 0.0])),
            "and so did its component's values"
        );
    }

    #[test]
    fn recording_without_a_transaction_is_refused_and_says_what_to_do() {
        let mut document = Document::new("worlds/city.cyworld");
        let node = document.allocate_node();
        let problem = document
            .record(Operation::CreateNode { node, parent: None })
            .unwrap_err();
        assert!(problem.because.contains("no other write path"), "{problem}");
        assert!(problem.remedy.is_some());
    }

    #[test]
    fn a_read_only_field_is_refused_with_the_reason_the_interface_would_give() {
        let (mut document, node, transform, position) = city();
        document
            .schema_mut()
            .set_writable(transform, position, false)
            .unwrap();

        document.begin("Move", Actor::human("designer"));
        let problem = document
            .set_field(node, transform, position, Value::Vec3([1.0, 0.0, 0.0]))
            .unwrap_err();
        assert_eq!(problem.because, "the field is read-only");
        assert!(problem.remedy.is_some());
        document.cancel().unwrap();
    }

    #[test]
    fn a_transaction_that_changed_nothing_is_not_an_entry() {
        let (mut document, ..) = city();
        let before = document.history().entries().len();
        document.begin("Look around", Actor::human("designer"));
        assert_eq!(document.commit().unwrap(), None);
        assert_eq!(document.history().entries().len(), before);
    }

    #[test]
    fn adding_a_backing_asset_does_not_dirty_the_document() {
        // "A document is not a file": a world gains authoring chunks as it streams, and none of
        // that is an authoring change.
        let mut document = Document::new("worlds/city.cyworld");
        document.add_asset("worlds/city/chunk_0_0.cychunk");
        document.add_asset("worlds/city/chunk_0_1.cychunk");
        assert_eq!(document.assets().len(), 3);
        assert!(
            !document.is_dirty(),
            "storage layout is not authoring content"
        );
    }
}
