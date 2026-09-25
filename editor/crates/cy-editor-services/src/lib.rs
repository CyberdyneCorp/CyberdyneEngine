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

pub mod asset_catalogue;
pub mod assets;
pub mod authoring;
pub mod backend;
pub mod bodies;
pub mod builtin;
pub mod documents;
pub mod editor;
pub mod gizmo;
pub mod manipulate;
pub mod material_commands;
pub mod material_graph;
pub mod material_parameters;
pub mod merge_commands;
pub mod mirror;
pub mod notifications;
pub mod operations;
pub mod picking;
pub mod primitives;
pub mod project;
pub mod runtime;
pub mod scene_actors;
pub mod selection;
pub mod semantic_merge;
pub mod settings;
pub mod source_control;
pub mod source_language;
pub mod source_workspace;
pub mod terrain;
pub mod vfx_commands;
pub mod vfx_document;
pub mod viewports;
pub mod workspace;
pub mod workspace_store;
pub mod worldfile;

// Re-exported so that a view model can say which mode the engine is in without naming the SDK. A
// view model that depended on `cy-editor-sdk` would be a presentation crate holding the one crate
// in the workspace that may say `unsafe`, which is exactly the reach-through
// `editor-rust-application` forbids.
pub use asset_catalogue::{AssetCatalogueService, AssetEntry, AssetMove};
pub use assets::{AssetImportService, ExternalImportCompletion};
pub use backend::{
    BackendServices, MaterialCatalogueState, MaterialDiagnostic, MaterialDiagnosticLocation,
    MaterialDiagnosticSeverity, MaterialOperation, MaterialParameterValue, MaterialPreviewState,
    MaterialPreviewTarget, MaterialRequestState,
};
pub use cy_editor_sdk::HostingMode;
pub use documents::{CloseDecision, CloseOutcome, DocumentService};
pub use editor::{Editor, ReloadReport};
pub use mirror::{RuntimeMirror, engine_identity};
pub use notifications::{Notification, NotificationService, Severity};
pub use operations::OperationService;
pub use project::{BuildState, ModuleBuilder, ProjectService};
pub use runtime::RuntimeSession;
pub use selection::SelectionService;
pub use semantic_merge::{MergeDecision, MergeSession, SemanticMergeService};
pub use settings::{Catalogue, SettingValue, SettingsService, Template};
pub use source_control::SourceControlService;
pub use source_language::{LanguageDiagnostic, LanguageServiceState, SourceLanguageService};
pub use source_workspace::{
    SourceFile, SourceFingerprint, SourceSave, SourceSnapshot, SourceWorkspaceService,
};
pub use viewports::ViewportService;
pub use workspace::{ViewState, Workspace};
pub use workspace_store::{RestoreReport, WorkspaceStore};
pub use worldfile::{LoadReport, write_world};
