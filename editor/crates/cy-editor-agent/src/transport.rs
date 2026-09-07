//! The seam the wire format sits behind.
//!
//! `editor-agent-interface`, "The protocol is MCP, behind an engine-owned interface":
//!
//! > The agent interface SHALL speak the **Model Context Protocol** ... The protocol SHALL sit behind
//! > an engine-owned interface in the manner of every other integration, so that the wire format is
//! > replaceable without touching the projection above it. **No MCP type SHALL appear in the
//! > editor's command, document, or view-model layers.**
//!
//! This module is that interface. It was empty at M5 — the shape this project uses everywhere a third
//! party will eventually appear: `cy::physics::PhysicsServer` before Jolt,
//! `cy::shader::ShaderCompiler` before Slang, `cy::text::TextServer` before HarfBuzz. The seam is the
//! deliverable; the library is the next one.
//!
//! At M5.5 `cy-editor-mcp` went behind it, and nothing in this file changed to let it — which is the
//! evidence the shape was right. That crate is at layer 5, above this one, so **nothing in the
//! editor's command, document or view-model layers can name an MCP type even by accident**: the
//! dependency direction forbids it, and `cy-editor-app/tests/gating.rs` checks that no crate but the
//! binary names the transport at all.
//!
//! # What the interface has to be shaped like, and why
//!
//! Three requirements between them decide it:
//!
//! * **"long-running work through the protocol's progress and cancellation facilities"** — so a
//!   response can be progress rather than a result, and a request carries an identifier the caller
//!   can cancel by.
//! * **"the editor stays usable while an agent works"** — so nothing here blocks. A transport takes a
//!   request and hands back nothing; answers arrive through `poll`. That is the same shape
//!   `cy_editor_protocol::Session` takes for the live bridge, for the same reason and after the same
//!   measurement.
//! * **"tool and resource descriptions SHALL be generated from command metadata"** — so a transport
//!   is handed descriptors rather than asked to describe anything. There is no place in this trait
//!   for a hand-written description, which is the point.

use cy_editor_core::problem::Result;

use crate::observe::{Observation, ViewportRequest};
use crate::resource::Resource;
use crate::tool::ToolDescriptor;

/// What an agent asked for.
///
/// Deliberately small: four things, because there are four things an agent does — find out what it
/// can do, read, invoke, and look. A fifth would mean a capability the projection does not have.
#[derive(Clone, PartialEq, Debug)]
pub enum AgentRequest {
    /// List the tools.
    ListTools,
    /// List the readable resources.
    ListResources,
    /// Read one resource, by its address.
    ReadResource {
        /// The address, as `ListResources` gave it.
        uri: String,
    },
    /// Invoke a command.
    Invoke {
        /// The command's identifier.
        command: String,
        /// Its arguments, as name and rendered value pairs.
        ///
        /// Rendered rather than typed because this is the WIRE side of the seam: a transport that
        /// carried `cy_editor_core::Value` would make the editor's value type part of the protocol,
        /// which is the coupling the seam exists to prevent. The projection above converts, and the
        /// registry's own validation is what refuses a value of the wrong type — the same validation
        /// a person's invocation goes through.
        arguments: Vec<(String, String)>,
    },
    /// Render a viewport.
    Observe(Box<ViewportRequest>),
}

/// What came back.
#[derive(Clone, PartialEq, Debug)]
pub enum AgentResponse {
    /// The tools, with their effect classes and their exclusions.
    Tools(Vec<ToolDescriptor>),
    /// The resources that can be read.
    Resources(Vec<(String, String)>),
    /// One resource's content.
    Content(Box<Resource>),
    /// What an invocation produced.
    Outcome {
        /// One line a person reads.
        summary: String,
        /// The structured results, as name and rendered value pairs.
        values: Vec<(String, String)>,
    },
    /// An image.
    Image(Box<Observation>),
    /// Work in progress. `editor-agent-interface` requires long-running work to report through the
    /// protocol's progress facility rather than by blocking.
    Progress {
        /// Which request.
        request: u64,
        /// What it is doing.
        step: String,
        /// How far, when it can say.
        fraction: Option<f32>,
    },
    /// It could not proceed, and this is why and what would make it succeed.
    ///
    /// `editor-agent-interface`: "An error SHALL state what went wrong and what would make it
    /// succeed. An agent that receives 'invalid argument' learns nothing; one that receives 'the
    /// property is read-only because the object is a prefab instance; override it first or edit the
    /// prefab' can act." So a refusal carries the whole `Problem`, rendered — never a code.
    Refused {
        /// What was being attempted.
        what: String,
        /// Why it could not be.
        because: String,
        /// What would make it succeed, when there is such a thing.
        remedy: Option<String>,
    },
}

/// A wire format.
///
/// Implemented by whatever speaks to an agent. Nothing above this trait knows which, and nothing
/// below it knows what a command is.
pub trait AgentTransport {
    /// Accept a request, returning the identifier a progress report and a cancellation will name.
    ///
    /// Does not block and does not answer. An implementation that did either would put an agent's
    /// round trip on whatever thread called it, which is how "the editor stays usable while an agent
    /// works" stops being true.
    fn submit(&mut self, request: AgentRequest) -> Result<u64>;

    /// Answer one, when there is one to answer.
    fn respond(&mut self, request: u64, response: AgentResponse) -> Result<()>;

    /// Abandon a request. Cancellation is cooperative: the work stops where stopping leaves a
    /// consistent result, which is the same contract `cy_editor_core::progress::Cancellation` holds.
    fn cancel(&mut self, request: u64) -> Result<()>;

    /// Whether an agent is connected, for the indicator `editor-agent-interface` requires the human
    /// to be able to see.
    fn is_connected(&self) -> bool;
}
