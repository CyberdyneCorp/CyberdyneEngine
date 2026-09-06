//! View models. Task 2.4, and the two rules `editor-rust-application` states as prohibitions.
//!
//! > A view model SHALL NOT hold authoritative document, project, or runtime state.
//!
//! > A view model SHALL NOT depend on, hold a reference to, or call another panel's view model.
//!
//! Both are stated as prohibitions "because [they are] violated by accident, for locally reasonable
//! reasons", and both are enforced here by shape rather than by care:
//!
//! * **Not a second source of truth.** Every view model below takes `&Editor` at rebuild time and
//!   keeps only *derived* values — formatted strings, expansion, an in-progress edit buffer. None of
//!   them owns a `Document`, a `Selection` or a `Session`, and none of them could: the services own
//!   those, and a view model is given a borrow for the length of one rebuild.
//! * **No peer dependencies.** No view model in this crate names another. The one place two panels
//!   have to agree — the selection — is a service they both read, so the hierarchy does not notify
//!   the inspector; the inspector notices that a revision moved.
//!
//! --- THE IN-PROGRESS EDIT IS A SEPARATE FIELD, AND THAT IS THE WHOLE MECHANISM --------------------------
//!
//! "An in-progress edit SHALL be explicitly distinguished from a committed value, so that an
//! uncommitted field is never mistaken for document state", and "WHEN a field is edited and focus is
//! lost without committing THEN the document SHALL be unchanged and no transaction SHALL exist."
//!
//! [`InspectorViewModel`] therefore has `committed` and `editing` as two fields rather than one
//! mutable value. Abandoning an edit clears `editing`; nothing else happens, because nothing else
//! ever happened.
//!
//! --- THE TOOLKIT IS ABSENT, NOT ABSTRACTED --------------------------------------------------------------
//!
//! There is no widget, no layout, no colour and no font in this crate, and no dependency that could
//! supply one. `editor-rust-application` requires the interface toolkit to be "an implementation
//! choice behind editor abstractions ... selected on measurement" — so the choice has not been made
//! yet, and everything above is testable before it is. Every test in this crate runs with no window
//! and no graphics device, which is what task 2.7 asks for.

#![forbid(unsafe_code)]

pub mod hierarchy;
pub mod inspector;
pub mod status;

pub use hierarchy::{HierarchyRow, HierarchyViewModel};
pub use inspector::{InspectorRow, InspectorViewModel};
pub use status::StatusViewModel;
