//! The Model Context Protocol transport for CyberEditor's agent interface. M5.5 task 3.1.
//!
//! `editor-agent-interface`, "The protocol is MCP, behind an engine-owned interface":
//!
//! > The agent interface SHALL speak the **Model Context Protocol**: editor actions are exposed as
//! > MCP **tools**, project state as MCP **resources**, and long-running work through the protocol's
//! > progress and cancellation facilities.
//! >
//! > The protocol SHALL sit behind an engine-owned interface in the manner of every other
//! > integration, so that the wire format is replaceable without touching the projection above it.
//! > **No MCP type SHALL appear in the editor's command, document, or view-model layers.**
//!
//! At M5 the interface existed and nothing was behind it. This crate is what went behind it, and the
//! second paragraph is enforced by the dependency direction rather than by care: this crate depends
//! on `cy-editor-agent`, nothing depends on this crate but the binary, and so no type declared here
//! can appear anywhere below. `cy-editor-app/tests/layering.rs` fails the day that stops being true.
//!
//! # What is here
//!
//! | | |
//! |---|---|
//! | [`json`] | JSON, in the subset JSON-RPC carries, with no dependency. The manifest argues why. |
//! | [`rpc`] | JSON-RPC 2.0 over newline-delimited stdio, which is the transport MCP specifies |
//! | [`server`] | The methods, and [`server::Connection`] — the one implementation of `AgentTransport` |
//!
//! # The three things worth knowing before reading it
//!
//! **The tools are the registry and there is no list.** `tools/list` is one `map` over
//! [`cy_editor_agent::project`]. A hand-written entry would have to be spliced into that iterator,
//! which is an edit a reviewer notices — and it is the first of the twelve patterns
//! `editor-agent-interface` calls forbidden.
//!
//! **Observation is a resource, not a tool.** `viewport:` is what an agent reads to see the engine's
//! own rendered image; `viewport:overlays` is the editor's frame with its own drawing on it; and
//! `viewport:<view mode>` is a debug buffer. Making it a tool would have been the hand-maintained
//! entry the rule above forbids, arrived at by the back door. The protocol's resource contents
//! already carry a base64 blob with a media type, so nothing had to be invented for it.
//!
//! **The interface is optional at build time.** "The interface SHALL be optional at build time and
//! absent from a shipped runtime." This crate is an *optional* dependency of `cy-editor-app` behind
//! its `agent-interface` feature. The feature is on by default, because a delivered capability that
//! has to be asked for is a capability nothing exercises; building without it links none of this and
//! `cy-editor-app/tests/gating.rs` is what says so.
//!
//! # Using it
//!
//! ```no_run
//! # fn main() -> cy_editor_core::problem::Result<()> {
//! use cy_editor_agent::budget::Budget;
//! use cy_editor_agent::session::{AgentIdentity, AgentSession, RefuseEverything};
//! use cy_editor_commands::scope::Scope;
//! use cy_editor_services::editor::Editor;
//!
//! let mut editor = Editor::default();
//! let mut registry = cy_editor_commands::registry::Registry::new();
//! cy_editor_services::builtin::register(&mut registry)?;
//!
//! let session = AgentSession::new(
//!     AgentIdentity { agent: "claude".into(), session: "s-1".into() },
//!     "compose the opening scene",
//!     Scope::default(),
//!     Budget::default(),
//!     "r-1",
//!     0,
//! );
//! let mut server = cy_editor_mcp::McpServer::new(std::io::stdout().lock(), session);
//! cy_editor_mcp::serve(
//!     std::io::stdin().lock(),
//!     &mut server,
//!     &mut editor,
//!     &registry,
//!     &mut RefuseEverything,
//! )
//! # }
//! ```

#![forbid(unsafe_code)]

pub mod json;
pub mod rpc;
pub mod server;

pub use json::Json;
pub use rpc::{Incoming, PROTOCOL_VERSION};
pub use server::{Connection, McpServer, SERVER_NAME, serve};
