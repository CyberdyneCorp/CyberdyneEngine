//! Privacy-labelled diagnostics for agent activity.
//!
//! Records deliberately contain no command arguments, resource contents, source text, or image
//! bytes. The diagnostics surface is useful for attribution without becoming a second copy of the
//! project data an agent was allowed to see.

/// The kind of agent activity recorded for diagnostics.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum AgentAuditKind {
    /// A session connected or disconnected.
    Connection,
    /// A command invocation began or completed.
    Invocation,
    /// Policy or command execution refused work.
    Refusal,
    /// A document transaction was attributed to the session.
    Transaction,
    /// A viewport render was requested.
    RenderRequest,
    /// Budget was charged or reported.
    Cost,
}

impl AgentAuditKind {
    /// Stable text used by the diagnostics panel and exported logs.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Connection => "connection",
            Self::Invocation => "invocation",
            Self::Refusal => "refusal",
            Self::Transaction => "transaction",
            Self::RenderRequest => "render-request",
            Self::Cost => "cost",
        }
    }
}

/// The most sensitive information carried by a record.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PrivacyClass {
    /// Product and protocol state only.
    Public,
    /// Project identity or command names, but no project contents.
    ProjectMetadata,
}

impl PrivacyClass {
    /// Stable, human-readable classification.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Public => "public",
            Self::ProjectMetadata => "project-metadata",
        }
    }
}

/// One append-only diagnostic record, attributed to exactly one session.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct AgentAuditRecord {
    /// Session-local sequence number.
    pub sequence: u64,
    /// Agent identity.
    pub agent: String,
    /// Session identity. Never inferred from the agent name.
    pub session: String,
    /// Event kind.
    pub kind: AgentAuditKind,
    /// Privacy classification of the summary.
    pub privacy: PrivacyClass,
    /// Redacted summary suitable for Editor diagnostics.
    pub summary: String,
}
