//! Commands are the single action surface. Task 2.5, `editor-rust-application` and
//! `editor-agent-interface`.
//!
//! "Every user-invokable action SHALL be registered in a **command registry** ... Menus, toolbars,
//! context menus, keyboard shortcuts, the command palette, automation, tests, and the **agent
//! interface** SHALL all invoke commands. An action reachable only through a specific widget SHALL
//! be a defect."
//!
//! And the sentence that decides this crate's shape:
//!
//! > Command metadata SHALL be rich enough for **machine invocation**, not merely for rendering a
//! > menu item: typed parameters with their meaning, a description written for a caller that cannot
//! > see the interface, and a declared **effect class** ... This therefore applies from the first
//! > command registered.
//!
//! So [`Registry::register`] **refuses** a command whose metadata would not satisfy a caller that
//! has never seen the interface. Not warns — refuses, at registration, with the missing piece named.
//! That is why the rule binds from the first command: there is no way to register a second kind.
//! Retrofitting this across an established registry is an entry-by-entry migration, and the whole
//! reason the specification says "from the first" is that nobody ever does it.
//!
//! --- THE FOUR THINGS A CALLER THAT CANNOT SEE THE INTERFACE NEEDS -------------------------------------
//!
//! | Field | Why a machine needs it |
//! |---|---|
//! | [`Metadata::description`] | What the command does, in a sentence that does not say "click" |
//! | [`Metadata::parameters`] | What it takes, typed, each with its own meaning |
//! | [`Metadata::effect`] | What class of consequence it has, *before* invoking |
//! | [`Command::availability`] | Why it is unavailable, in the words a person would be given |
//!
//! --- SCOPE IS A GRANT, ATTRIBUTION IS A CLAIM, AND THEY ARE DIFFERENT TYPES ----------------------------
//!
//! [`Scope`] is what an agent connection is *permitted* to do; [`cy_editor_core::Actor`] is who says
//! they are acting. `editor-documents-and-transactions` is explicit that attribution "is not a
//! security control and SHALL NOT be treated as one", so nothing here makes a decision on the basis
//! of an actor, and nothing writes a scope into a transaction. Conflating them is how attribution
//! becomes a security control by accident, and the two types are what stops it.

#![forbid(unsafe_code)]

pub mod assets;
pub mod context;
pub mod metadata;
pub mod registry;
pub mod scope;

pub use assets::{AssetHost, AssetImportOutcome, AssetImportRequest, ImportedSubAsset};
pub use context::{
    CommandContext, Manipulation, ManipulationKind, Outcome, ProjectHost, ViewportControls,
};
pub use metadata::{Availability, EffectClass, Metadata, ParameterSpec};
pub use registry::{Arguments, Command, CommandId, Registry};
pub use scope::Scope;
