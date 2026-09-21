// SPDX-License-Identifier: MIT
//! Editor-owned language-service values.

use std::collections::BTreeMap;
use std::fmt;
use std::time::Duration;

/// Zero-based source position.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Position {
    /// Zero-based line.
    pub line: u32,
    /// Zero-based UTF-16 code-unit offset.
    pub character: u32,
}

/// Source range.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Range {
    /// Inclusive start.
    pub start: Position,
    /// Exclusive end.
    pub end: Position,
}

/// A location in a source document.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Location {
    /// Document URI.
    pub uri: String,
    /// Location range.
    pub range: Range,
}

/// A textual source edit.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TextEdit {
    /// Range replaced by this edit.
    pub range: Range,
    /// Replacement text.
    pub new_text: String,
}

/// Edits grouped by document URI.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct WorkspaceEdit {
    /// Text edits for each changed document.
    pub changes: BTreeMap<String, Vec<TextEdit>>,
}

/// Diagnostic severity from the language server.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DiagnosticSeverity {
    /// Compilation error.
    Error,
    /// Warning.
    Warning,
    /// Informational notice.
    Information,
    /// Hint.
    Hint,
    /// A server-specific numeric severity.
    Other(u32),
}

/// A source diagnostic.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Diagnostic {
    /// Affected source range.
    pub range: Range,
    /// Human-readable message.
    pub message: String,
    /// Optional producer or rule identifier.
    pub source: Option<String>,
    /// Optional severity.
    pub severity: Option<DiagnosticSeverity>,
}

/// A diagnostics publication accepted for the current buffer revision.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Diagnostics {
    /// Document URI.
    pub uri: String,
    /// Document version associated with this publication.
    pub version: i32,
    /// Published diagnostics.
    pub items: Vec<Diagnostic>,
}

/// One completion proposal.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CompletionItem {
    /// Display label.
    pub label: String,
    /// Optional explanatory detail.
    pub detail: Option<String>,
    /// Text inserted when accepted, when different from the label.
    pub insert_text: Option<String>,
}

/// Hover information rendered as plain or Markdown source text.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Hover {
    /// Server-provided hover contents.
    pub contents: String,
    /// Optional range to which the hover applies.
    pub range: Option<Range>,
}

/// A symbol returned from workspace search.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WorkspaceSymbol {
    /// Symbol name.
    pub name: String,
    /// LSP symbol kind number.
    pub kind: u32,
    /// Source location when supplied by the server.
    pub location: Option<Location>,
}

/// An optional language-server feature that may be advertised at initialization.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LanguageFeature {
    /// Source completion.
    Completion,
    /// Hover information.
    Hover,
    /// Workspace-wide symbol search.
    WorkspaceSymbols,
    /// Symbol rename.
    Rename,
    /// Definition navigation.
    Definition,
    /// Reference navigation.
    References,
}

impl LanguageFeature {
    pub(crate) const fn label(self) -> &'static str {
        match self {
            Self::Completion => "completion",
            Self::Hover => "hover",
            Self::WorkspaceSymbols => "workspace symbols",
            Self::Rename => "rename",
            Self::Definition => "definition",
            Self::References => "references",
        }
    }
}

/// Language features advertised during initialization.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[allow(
    clippy::struct_excessive_bools,
    reason = "these are independent capabilities advertised by the LSP server, not state"
)]
pub struct ServerCapabilities {
    /// Completion support.
    pub completion: bool,
    /// Hover support.
    pub hover: bool,
    /// Workspace-symbol support.
    pub workspace_symbols: bool,
    /// Rename support.
    pub rename: bool,
    /// Go-to-definition support.
    pub definition: bool,
    /// Find-references support.
    pub references: bool,
}

impl ServerCapabilities {
    /// Whether the server advertised one optional feature.
    #[must_use]
    pub const fn supports(self, feature: LanguageFeature) -> bool {
        match feature {
            LanguageFeature::Completion => self.completion,
            LanguageFeature::Hover => self.hover,
            LanguageFeature::WorkspaceSymbols => self.workspace_symbols,
            LanguageFeature::Rename => self.rename,
            LanguageFeature::Definition => self.definition,
            LanguageFeature::References => self.references,
        }
    }
}

/// Configuration used to initialize a SourceKit-LSP session.
#[derive(Clone, Debug)]
pub struct ClientOptions {
    /// URI of the Swift package root.
    pub root_uri: String,
    /// Maximum wait for a request response.
    pub request_timeout: Duration,
}

impl ClientOptions {
    /// Create options with a five-second request deadline.
    #[must_use]
    pub fn new(root_uri: impl Into<String>) -> Self {
        Self {
            root_uri: root_uri.into(),
            request_timeout: Duration::from_secs(5),
        }
    }
}

/// Why a language-service operation could not complete.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ClientError {
    /// The SourceKit-LSP executable could not be started.
    Unavailable {
        /// Why process startup failed.
        reason: String,
        /// Action the developer can take to restore the feature.
        remedy: String,
    },
    /// The server stopped before completing the operation.
    Crashed {
        /// Operation interrupted by the process exit.
        operation: String,
    },
    /// The server exceeded the configured deadline.
    Timeout {
        /// Operation whose deadline expired.
        operation: String,
    },
    /// The server does not advertise the requested feature.
    Unsupported {
        /// Feature requested by the caller.
        feature: &'static str,
        /// Capability explanation suitable for an unavailable action.
        reason: String,
    },
    /// The server or transport produced invalid data.
    Protocol {
        /// Invalid framing or response detail.
        reason: String,
    },
    /// A document version did not advance monotonically.
    StaleDocumentVersion {
        /// Latest synchronized buffer version.
        current: i32,
        /// Version rejected by the client.
        received: i32,
    },
    /// The client was used after shutdown.
    ShutDown,
}

impl fmt::Display for ClientError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Unavailable { reason, remedy } => {
                write!(
                    formatter,
                    "SourceKit-LSP is unavailable: {reason}; {remedy}"
                )
            }
            Self::Crashed { operation } => {
                write!(
                    formatter,
                    "SourceKit-LSP stopped while trying to {operation}"
                )
            }
            Self::Timeout { operation } => {
                write!(
                    formatter,
                    "SourceKit-LSP timed out while trying to {operation}"
                )
            }
            Self::Unsupported { feature, reason } => {
                write!(formatter, "{feature} is unavailable: {reason}")
            }
            Self::Protocol { reason } => write!(formatter, "invalid SourceKit-LSP data: {reason}"),
            Self::StaleDocumentVersion { current, received } => write!(
                formatter,
                "document version {received} does not advance current version {current}"
            ),
            Self::ShutDown => formatter.write_str("the SourceKit-LSP client is shut down"),
        }
    }
}

impl std::error::Error for ClientError {}
