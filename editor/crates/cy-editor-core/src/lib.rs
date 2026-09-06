//! Shared foundations for CyberEditor. Task 2.1, `editor-rust-application`.
//!
//! Layer 0 of the editor workspace: the vocabulary every other crate speaks in, and nothing else.
//! There is no engine here, no transport, no document model and no presentation — those are the
//! crates above, and each of them names this one rather than each other.
//!
//! Five things live here, and they are here rather than anywhere else because more than one crate
//! above needs each of them:
//!
//! | Module | What it is for |
//! |---|---|
//! | [`ids`] | Stable identity: the handles operations address, and the generation check that makes a stale one detectable rather than wrong |
//! | [`value`] | The value vocabulary shared by the engine's `CyVar`, a document's fields, and a command's parameters |
//! | [`problem`] | A failure that states what went wrong *and what would make it succeed* |
//! | [`observe`] | Change propagation by revision, so a view model can tell cheaply whether its inputs moved |
//! | [`progress`] | Long operations as observable state with cancellation, so nothing blocks an interface thread |
//! | [`actor`] | Who produced a change — a person, or an agent with its session and its stated intent |
//!
//! `unsafe` is forbidden, not merely absent: `editor-rust-application` confines it to "narrow,
//! audited interoperation and platform modules", and `cy-editor-sdk` is the only crate in this
//! workspace that is one.

#![forbid(unsafe_code)]

pub mod actor;
pub mod codec;
pub mod ids;
pub mod observe;
pub mod problem;
pub mod progress;
pub mod value;

pub use actor::Actor;
pub use ids::{DocumentId, FieldId, NodeId, TypeId};
pub use observe::{Revision, Versioned, Watch};
pub use problem::{Problem, Result};
pub use value::Value;
