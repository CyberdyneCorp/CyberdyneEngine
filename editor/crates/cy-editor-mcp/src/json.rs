//! Compatibility projection of the shared JSON wire foundation.
//!
//! Existing MCP callers use `cy_editor_mcp::json`; retaining this module keeps that public API
//! stable while SourceKit and future transports share the parser without depending on MCP.

pub use cy_editor_json::{Json, base64, parse};
