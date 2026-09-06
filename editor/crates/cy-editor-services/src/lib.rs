//! The editor's services: authoritative state, and the one place it lives. Tasks 2.4 and 2.6.
//!
//! `editor-rust-application` splits the editor into three layers and gives each exactly one thing to
//! own:
//!
//! | Layer | Owns |
//! |---|---|
//! | **Models and services** | Authoritative state: documents, project, workspace, runtime sessions, selection, transactions, assets, builds |
//! | View models | Presentation state and user intents |
//! | Views | Rendering a view model and emitting intents |
//!
//! This crate is the first row, and the rule that makes the other two work is stated as a
//! prohibition because it is violated by accident: **a view model is never a second source of
//! truth**. Here that is a dependency edge — `cy-editor-viewmodels` depends on this crate, and this
//! crate has never heard of it — plus a mechanism: every service exposes a
//! [`cy_editor_core::observe::Revision`], and a view model holds a
//! [`cy_editor_core::observe::Watch`] rather than a copy.
//!
//! --- WHAT IS DELIBERATELY NOT HERE ---------------------------------------------------------------------
//!
//! No panels, no toolkit, no window, no graphics device. Everything in this crate is exercised by
//! `cargo test` with none of those present, which is `editor-rust-application`'s "Services, models,
//! view models, and commands SHALL be testable **headlessly**" as a property of what the crate can
//! name rather than as an aspiration.

#![forbid(unsafe_code)]

pub mod builtin;
pub mod documents;
pub mod editor;
pub mod notifications;
pub mod operations;
pub mod runtime;
pub mod selection;
pub mod workspace;

// Re-exported so that a view model can say which mode the engine is in without naming the SDK. A
// view model that depended on `cy-editor-sdk` would be a presentation crate holding the one crate
// in the workspace that may say `unsafe`, which is exactly the reach-through
// `editor-rust-application` forbids.
pub use cy_editor_sdk::HostingMode;
pub use documents::DocumentService;
pub use editor::Editor;
pub use notifications::{Notification, NotificationService, Severity};
pub use operations::OperationService;
pub use runtime::RuntimeSession;
pub use selection::SelectionService;
pub use workspace::{ViewState, Workspace};
