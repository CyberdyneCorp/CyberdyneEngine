//! The agent interface: how an autonomous agent drives the editor. M5 task 5.4, M5.5 tasks 3.2–3.12.
//!
//! `editor-agent-interface` reached **Seed** at M5 and **Working** at M5.5. The design decision that
//! makes this crate small is the specification's own:
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
//! # What is here
//!
//! * [`tool`] — the registry projected as tools, with typed parameters, effect classes and
//!   exclusions that carry a reason. The exclusion is a property on the command itself from M5.5;
//!   this crate keeps no list of what it offers, and has nowhere to put one.
//! * [`resource`] — the read surface, produced from the same services the editor's own panels read,
//!   and provably free of side effects because every function takes `&Editor`.
//! * [`session`] — a connection: its identity, its stated intent, its scope, its budget, its
//!   confirmations, and its recording.
//! * [`observe`] — viewport observation, picking and spatial queries. An observation is one of the
//!   frames the human's viewport receives, through the same transport, and it states what it
//!   actually is rather than what was asked for.
//! * [`conflict`] — what happens when a person and an agent reach for the same object. The person
//!   wins, nothing is locked, and the agent is told what superseded it.
//! * [`budget`] — the limits that stop an agent starving the editor, reported rather than opaque.
//! * [`recording`] — the session as a replayable command log.
//! * [`transport`] — the engine-owned interface the wire format sits behind.
//!
//! What is NOT here is the wire format, and that is the point. `editor-agent-interface` requires the
//! Model Context Protocol and requires it to "sit behind an engine-owned interface in the manner of
//! every other integration, so that the wire format is replaceable without touching the projection
//! above it. No MCP type SHALL appear in the editor's command, document, or view-model layers."
//!
//! At M5 the seam existed ([`transport::AgentTransport`]) with nothing behind it, for the same
//! reason no Slang type appears in `cy::shader`'s interface at M2: the seam is the deliverable and
//! the library behind it is the next one. At M5.5 `cy-editor-mcp` went behind it — a crate at layer
//! 5, above this one, which is what makes the prohibition structural: nothing here can name a wire
//! type because nothing here depends on the crate that has them.
//!
//! # The three things M5.5 added that are worth knowing
//!
//! **The effect class of an invocation may be narrower than the command declares.** `design.md` §4
//! decides that writing a source file is a reversible mutation when the editor can restore what it
//! replaced and an irreversible one when it cannot, and that "the class is computed, not assumed".
//! [`AgentSession::invoke`] asks the registry for the computed class and checks the scope and the
//! confirmation against *that*, which is what keeps confirmation rare enough to be read.
//!
//! **The scope is checked before a human is asked.** An operation the connection was never granted
//! is refused by the scope rather than sent to somebody to approve. A prompt about work that could
//! not have happened anyway is exactly the habit-forming prompt the confirmation rule exists to
//! prevent.
//!
//! **Looking costs a render, and an agent that states a camera gets its own viewport.** Moving the
//! person's camera to answer an agent's question would take the editor away from the person using
//! it, which "the editor stays usable while an agent works" forbids. Opening a viewport is not a
//! capability a human lacks.

#![forbid(unsafe_code)]

pub mod budget;
pub mod conflict;
pub mod observe;
pub mod recording;
pub mod resource;
pub mod session;
pub mod tool;
pub mod transport;

pub use budget::{Budget, BudgetReport, Throttle};
pub use conflict::{Claim, Conflict};
pub use observe::{
    Observation, ObservationKind, SpatialQuery, SpatialResult, ViewportRequest, observe,
};
pub use recording::{RecordedInvocation, Recording};
pub use resource::{Resource, ResourceKind, Resources};
pub use session::{
    AgentIdentity, AgentSession, Confirmation, Confirmer, Decision, Grant, Reading,
    RefuseEverything,
};
pub use tool::{ParameterDescriptor, ToolDescriptor, arguments, coerce, project, project_one};
pub use transport::{AgentRequest, AgentResponse, AgentTransport};
