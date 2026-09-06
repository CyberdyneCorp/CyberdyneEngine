//! Documents and transactions. Tasks 3.1 through 3.9.
//!
//! This crate holds the milestone's invariant: **transactions are the only path for persistent
//! mutation**. `editor-documents-and-transactions` states why that is worth a whole crate's
//! discipline — "one operation stream is five products at once: undo, autosave as a journal, crash
//! recovery, semantic diff and merge, and the delta a running game consumes" — and states the
//! consequence of leaving one hole: "A single direct write path does not merely bypass undo; it
//! makes the other five silently incomplete for that property, and nothing will point at it."
//!
//! --- HOW THE INVARIANT IS ENFORCED, IN THREE LAYERS -------------------------------------------------
//!
//! 1. **The type system, outside this crate.** [`content::DocumentContent`] has no public mutating
//!    method that does not take a [`content::WriteToken`], and a `WriteToken` has a private field,
//!    so no code outside this crate can construct one. There is no direct write path to write.
//! 2. **The audit hook, inside it.** Every mutation increments two counters, and only a
//!    token-carrying one increments both. [`audit::Audit`] compares them, so a bypass added *within*
//!    this crate — the only place one could be — is detected and reported naming the document.
//!    That is `editor-documents-and-transactions`' "development builds SHALL detect and report it".
//! 3. **The test that writes around it.** `audit::tests::the_test_that_writes_around_the_system`
//!    is task 3.9: it uses the deliberately-provided bypass and asserts that the audit catches it.
//!    A test that could not fail would prove nothing, so the bypass exists in test builds only and
//!    exists solely to be caught.
//!
//! --- THE OTHER TWO RULES THAT DO A LOT OF WORK ------------------------------------------------------
//!
//! **A document is not a file.** [`document::Document`] carries a list of backing assets, one of
//! which is primary; a world backed by hundreds of authoring chunks is one document with one dirty
//! state and one history.
//!
//! **View state is not content.** Nothing in this crate stores a camera position, an expansion
//! state or a scroll offset, and there is no operation that could carry one. Those belong to the
//! workspace, in `cy-editor-services`, and the separation is structural rather than a convention.

#![forbid(unsafe_code)]

pub mod audit;
pub mod content;
pub mod diff;
pub mod document;
pub mod history;
pub mod journal;
pub mod operation;
pub mod schema;
pub mod selection;
pub mod transaction;
pub mod worlds;

pub use content::DocumentContent;
pub use document::Document;
pub use operation::Operation;
pub use schema::{DocumentSchema, FieldDefinition, TypeDefinition};
pub use selection::Selection;
pub use transaction::{Transaction, TransactionId};
