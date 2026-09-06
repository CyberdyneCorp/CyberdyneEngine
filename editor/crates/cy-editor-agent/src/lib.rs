//! The agent interface: how an autonomous agent drives the editor. M5 task 5.4.
//!
//! `editor-agent-interface` reaches **Seed** at M5. The design decision that makes this crate small
//! is the specification's own:
//!
//! > The design decision that makes this small is that it is a **projection, not a second API**.
//! > `editor-rust-application` already requires every user-invokable action to be a registered
//! > command, already forbids an action reachable only through one widget, and already routes
//! > state-modifying commands through the transaction system. Given that, the tools *are* the
//! > registry, the mutations *are* transactions, and the refusals *are* the existing availability
//! > predicates.
//!
//! So there is no code here that performs an edit. [`session::AgentSession::invoke`] calls
//! `Registry::invoke` — the same function a menu, a shortcut, the palette, a script and a test call
//! — with a [`cy_editor_commands::Scope`] and an [`cy_editor_core::Actor`], and everything the
//! specification asks for follows from that: an agent's edits are undoable, they appear in the
//! journal, they survive crash recovery, they carry attribution, and they cannot reach a mutation
//! path that produces no transaction, because there is not one.
//!
//! # What is here, and what a later milestone adds
//!
//! Seed is "interfaces, data model and invariants exist; dependents can be built against it". This
//! crate is that, and everything in it is exercised:
//!
//! * [`tool`] — the registry projected as tools, with typed parameters, effect classes and
//!   exclusions that carry a reason.
//! * [`resource`] — the read surface, produced from the same services the editor's own panels read,
//!   and provably free of side effects because every function takes `&Editor`.
//! * [`session`] — a connection: its identity, its stated intent, its scope, its budget, its
//!   confirmations, and its recording.
//! * [`observe`] — viewport observation, picking and spatial queries, as requests the runtime
//!   answers through the same transport the human's viewport uses.
//! * [`budget`] — the limits that stop an agent starving the editor, reported rather than opaque.
//! * [`recording`] — the session as a replayable command log.
//! * [`transport`] — the engine-owned interface the wire format sits behind.
//!
//! What is NOT here is the wire format. `editor-agent-interface` requires the Model Context Protocol
//! and requires it to "sit behind an engine-owned interface in the manner of every other
//! integration, so that the wire format is replaceable without touching the projection above it. No
//! MCP type SHALL appear in the editor's command, document, or view-model layers." At M5 the
//! engine-owned interface exists ([`transport::AgentTransport`]) and no MCP implementation is behind
//! it, for the same reason no Slang type appears in `cy::shader`'s interface at M2: the seam is the
//! deliverable, and the library behind it is the next one. A transport lives in its own crate when
//! it lands, and nothing in this one changes.

#![forbid(unsafe_code)]

pub mod budget;
pub mod observe;
pub mod recording;
pub mod resource;
pub mod session;
pub mod tool;
pub mod transport;

pub use budget::{Budget, BudgetReport, Throttle};
pub use observe::{Observation, ObservationKind, SpatialQuery, SpatialResult, ViewportRequest};
pub use recording::{RecordedInvocation, Recording};
pub use resource::{Resource, ResourceKind, Resources};
pub use session::{AgentIdentity, AgentSession, Confirmation, Confirmer, Decision, Grant};
pub use tool::{Exclusion, Exclusions, ParameterDescriptor, ToolDescriptor, project};
pub use transport::{AgentRequest, AgentResponse, AgentTransport};
