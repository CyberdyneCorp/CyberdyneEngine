// SPDX-License-Identifier: MIT
//! SourceKit-LSP process lifecycle and typed editor-facing language operations.
//!
//! JSON and Language Server Protocol details stop at this crate. Callers work with the source
//! positions, diagnostics, edits, and symbols declared here, so neither services nor view models
//! need to understand the wire protocol. The process boundary is injectable for deterministic
//! lifecycle tests and uses only the standard library in production.

#![forbid(unsafe_code)]

mod process;
mod types;

pub use process::{CommandProcessFactory, LanguageServerProcess, ProcessFactory, ProcessFailure};
pub use types::{
    ClientError, ClientOptions, CompletionItem, Diagnostic, DiagnosticSeverity, Diagnostics, Hover,
    LanguageFeature, Location, Position, Range, ServerCapabilities, TextEdit, WorkspaceEdit,
    WorkspaceSymbol,
};

use std::collections::BTreeMap;
use std::time::{Duration, Instant};

use cy_editor_json::{Json, parse};

/// An initialized SourceKit-LSP session.
pub struct SourceKitClient {
    process: Box<dyn LanguageServerProcess>,
    timeout: Duration,
    next_request_id: u32,
    capabilities: ServerCapabilities,
    document_versions: BTreeMap<String, i32>,
    diagnostics: BTreeMap<String, Diagnostics>,
    stopped: bool,
}

impl SourceKitClient {
    /// Start and initialize a session using the installed `sourcekit-lsp`.
    pub fn launch(options: ClientOptions) -> Result<Self, ClientError> {
        Self::start(&CommandProcessFactory::sourcekit_lsp(), options)
    }

    /// Start and initialize a session from an injectable process factory.
    pub fn start(
        factory: &dyn ProcessFactory,
        options: ClientOptions,
    ) -> Result<Self, ClientError> {
        let process = factory.start().map_err(|reason| ClientError::Unavailable {
            reason,
            remedy: "install a supported Swift toolchain or select its sourcekit-lsp executable"
                .to_string(),
        })?;
        let mut client = Self {
            process,
            timeout: options.request_timeout,
            next_request_id: 1,
            capabilities: ServerCapabilities::default(),
            document_versions: BTreeMap::new(),
            diagnostics: BTreeMap::new(),
            stopped: false,
        };
        let result = client.request(
            "initialize",
            object([
                ("processId", Json::Null),
                ("rootUri", Json::text(options.root_uri)),
                ("capabilities", object([])),
            ]),
        )?;
        client.capabilities = parse_capabilities(result.get("capabilities"));
        client.notify("initialized", object([]))?;
        Ok(client)
    }

    /// Features advertised by the initialized server.
    #[must_use]
    pub const fn capabilities(&self) -> ServerCapabilities {
        self.capabilities
    }

    /// Check whether an optional action should be enabled, returning its user-facing explanation
    /// when the server did not advertise it.
    pub fn availability(&self, feature: LanguageFeature) -> Result<(), ClientError> {
        Self::require(self.capabilities.supports(feature), feature.label())
    }

    /// Notify the server that a source document was opened.
    pub fn open_document(
        &mut self,
        uri: impl Into<String>,
        text: impl Into<String>,
        version: i32,
    ) -> Result<(), ClientError> {
        let uri = uri.into();
        self.advance_version(&uri, version)?;
        self.notify(
            "textDocument/didOpen",
            object([(
                "textDocument",
                object([
                    ("uri", Json::text(uri)),
                    ("languageId", Json::text("swift")),
                    ("version", number(version)),
                    ("text", Json::text(text)),
                ]),
            )]),
        )
    }

    /// Synchronize a complete new buffer revision.
    pub fn change_document(
        &mut self,
        uri: &str,
        text: impl Into<String>,
        version: i32,
    ) -> Result<(), ClientError> {
        self.advance_version(uri, version)?;
        self.notify(
            "textDocument/didChange",
            object([
                (
                    "textDocument",
                    object([("uri", Json::text(uri)), ("version", number(version))]),
                ),
                (
                    "contentChanges",
                    Json::Array(vec![object([("text", Json::text(text))])]),
                ),
            ]),
        )
    }

    /// Notify the server that a document was closed.
    pub fn close_document(&mut self, uri: &str) -> Result<(), ClientError> {
        self.document_versions.remove(uri);
        self.diagnostics.remove(uri);
        self.notify(
            "textDocument/didClose",
            object([("textDocument", object([("uri", Json::text(uri))]))]),
        )
    }

    /// Most recently accepted diagnostics for a document.
    #[must_use]
    pub fn diagnostics(&self, uri: &str) -> Option<&Diagnostics> {
        self.diagnostics.get(uri)
    }

    /// Consume one server notification, if one arrives before `timeout`.
    ///
    /// Returns `true` when a notification was processed and `false` on a quiet deadline.
    pub fn poll(&mut self, timeout: Duration) -> Result<bool, ClientError> {
        self.ensure_running()?;
        match self.process.receive(timeout) {
            Ok(message) => {
                let message = parse_message(&message)?;
                self.handle_notification(&message);
                Ok(true)
            }
            Err(ProcessFailure::Timeout) => Ok(false),
            Err(ProcessFailure::Stopped) => Err(ClientError::Crashed {
                operation: "receive a server notification".to_string(),
            }),
            Err(ProcessFailure::Invalid(reason)) => Err(ClientError::Protocol { reason }),
        }
    }

    /// Request completion proposals.
    pub fn completion(
        &mut self,
        uri: &str,
        position: Position,
    ) -> Result<Vec<CompletionItem>, ClientError> {
        self.availability(LanguageFeature::Completion)?;
        let result = self.request("textDocument/completion", position_params(uri, position))?;
        let items = match &result {
            Json::Array(items) => items.as_slice(),
            Json::Object(_) => result.get("items").as_array().unwrap_or(&[]),
            Json::Null => &[],
            _ => return Err(protocol("completion result is not a list")),
        };
        Ok(items
            .iter()
            .filter_map(|item| {
                Some(CompletionItem {
                    label: item.get("label").as_text()?.to_string(),
                    detail: text_member(item, "detail"),
                    insert_text: text_member(item, "insertText"),
                })
            })
            .collect())
    }

    /// Request hover information.
    pub fn hover(&mut self, uri: &str, position: Position) -> Result<Option<Hover>, ClientError> {
        self.availability(LanguageFeature::Hover)?;
        let result = self.request("textDocument/hover", position_params(uri, position))?;
        if result.is_null() {
            return Ok(None);
        }
        Ok(Some(Hover {
            contents: hover_text(result.get("contents")),
            range: optional_range(result.get("range"))?,
        }))
    }

    /// Search symbols in the active workspace.
    pub fn workspace_symbols(&mut self, query: &str) -> Result<Vec<WorkspaceSymbol>, ClientError> {
        self.availability(LanguageFeature::WorkspaceSymbols)?;
        let result = self.request("workspace/symbol", object([("query", Json::text(query))]))?;
        result
            .as_array()
            .ok_or_else(|| protocol("workspace symbol result is not a list"))?
            .iter()
            .map(parse_symbol)
            .collect()
    }

    /// Request a workspace edit that renames the symbol at a position.
    pub fn rename(
        &mut self,
        uri: &str,
        position: Position,
        new_name: &str,
    ) -> Result<WorkspaceEdit, ClientError> {
        self.availability(LanguageFeature::Rename)?;
        let mut params = position_params(uri, position)
            .as_object()
            .cloned()
            .unwrap_or_default();
        params.insert("newName".to_string(), Json::text(new_name));
        parse_workspace_edit(&self.request("textDocument/rename", Json::Object(params))?)
    }

    /// Request definitions for the symbol at a position.
    pub fn definition(
        &mut self,
        uri: &str,
        position: Position,
    ) -> Result<Vec<Location>, ClientError> {
        self.availability(LanguageFeature::Definition)?;
        let result = self.request("textDocument/definition", position_params(uri, position))?;
        parse_locations(&result)
    }

    /// Request references for the symbol at a position.
    pub fn references(
        &mut self,
        uri: &str,
        position: Position,
        include_declaration: bool,
    ) -> Result<Vec<Location>, ClientError> {
        self.availability(LanguageFeature::References)?;
        let mut params = position_params(uri, position)
            .as_object()
            .cloned()
            .unwrap_or_default();
        params.insert(
            "context".to_string(),
            object([("includeDeclaration", Json::Bool(include_declaration))]),
        );
        let result = self.request("textDocument/references", Json::Object(params))?;
        parse_locations(&result)
    }

    /// Complete the LSP shutdown handshake and stop the child process.
    pub fn shutdown(&mut self) -> Result<(), ClientError> {
        self.ensure_running()?;
        self.request("shutdown", Json::Null)?;
        self.notify("exit", Json::Null)?;
        self.process.stop();
        self.stopped = true;
        Ok(())
    }

    fn advance_version(&mut self, uri: &str, version: i32) -> Result<(), ClientError> {
        if let Some(current) = self.document_versions.get(uri)
            && version <= *current
        {
            return Err(ClientError::StaleDocumentVersion {
                current: *current,
                received: version,
            });
        }
        self.document_versions.insert(uri.to_string(), version);
        Ok(())
    }

    fn require(available: bool, feature: &'static str) -> Result<(), ClientError> {
        if available {
            Ok(())
        } else {
            Err(ClientError::Unsupported {
                feature,
                reason: "the active SourceKit-LSP server did not advertise this capability"
                    .to_string(),
            })
        }
    }

    fn ensure_running(&self) -> Result<(), ClientError> {
        if self.stopped {
            Err(ClientError::ShutDown)
        } else {
            Ok(())
        }
    }

    fn notify(&mut self, method: &str, params: Json) -> Result<(), ClientError> {
        self.ensure_running()?;
        let message = object([
            ("jsonrpc", Json::text("2.0")),
            ("method", Json::text(method)),
            ("params", params),
        ]);
        self.process
            .send(&message.render())
            .map_err(|failure| process_error(failure, method))
    }

    fn request(&mut self, method: &str, params: Json) -> Result<Json, ClientError> {
        self.ensure_running()?;
        let id = self.next_request_id;
        self.next_request_id += 1;
        let message = object([
            ("jsonrpc", Json::text("2.0")),
            ("id", Json::Number(f64::from(id))),
            ("method", Json::text(method)),
            ("params", params),
        ]);
        self.process
            .send(&message.render())
            .map_err(|failure| process_error(failure, method))?;

        let deadline = Instant::now() + self.timeout;
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(ClientError::Timeout {
                    operation: method.to_string(),
                });
            }
            let raw = self
                .process
                .receive(remaining)
                .map_err(|failure| process_error(failure, method))?;
            let response = parse_message(&raw)?;
            if response.get("method").as_text().is_some() {
                self.handle_notification(&response);
                continue;
            }
            if response.get("id").as_number() != Some(f64::from(id)) {
                continue;
            }
            if !response.get("error").is_null() {
                return Err(protocol(format!(
                    "{method} failed: {}",
                    response.get("error").render()
                )));
            }
            return Ok(response.get("result").clone());
        }
    }

    fn handle_notification(&mut self, message: &Json) {
        if message.get("method").as_text() != Some("textDocument/publishDiagnostics") {
            return;
        }
        let params = message.get("params");
        let Some(uri) = params.get("uri").as_text() else {
            return;
        };
        let Some(current) = self.document_versions.get(uri).copied() else {
            return;
        };
        let version = integer(params.get("version")).unwrap_or(current);
        if version != current {
            return;
        }
        let items = params
            .get("diagnostics")
            .as_array()
            .unwrap_or(&[])
            .iter()
            .filter_map(parse_diagnostic)
            .collect();
        self.diagnostics.insert(
            uri.to_string(),
            Diagnostics {
                uri: uri.to_string(),
                version,
                items,
            },
        );
    }
}

impl Drop for SourceKitClient {
    fn drop(&mut self) {
        if !self.stopped {
            self.process.stop();
        }
    }
}

trait JsonArray {
    fn as_array(&self) -> Option<&[Json]>;
}

impl JsonArray for Json {
    fn as_array(&self) -> Option<&[Json]> {
        match self {
            Json::Array(items) => Some(items),
            _ => None,
        }
    }
}

fn object<const N: usize>(entries: [(&str, Json); N]) -> Json {
    Json::Object(
        entries
            .into_iter()
            .map(|(key, value)| (key.to_string(), value))
            .collect(),
    )
}

fn number(value: i32) -> Json {
    Json::Number(f64::from(value))
}

fn integer(value: &Json) -> Option<i32> {
    let value = value.as_number()?;
    if value.fract() == 0.0 && value >= f64::from(i32::MIN) && value <= f64::from(i32::MAX) {
        #[allow(clippy::cast_possible_truncation)]
        Some(value as i32)
    } else {
        None
    }
}

fn parse_message(message: &str) -> Result<Json, ClientError> {
    parse(message).map_err(|error| protocol(error.to_string()))
}

fn protocol(reason: impl Into<String>) -> ClientError {
    ClientError::Protocol {
        reason: reason.into(),
    }
}

fn process_error(failure: ProcessFailure, operation: &str) -> ClientError {
    match failure {
        ProcessFailure::Stopped => ClientError::Crashed {
            operation: operation.to_string(),
        },
        ProcessFailure::Timeout => ClientError::Timeout {
            operation: operation.to_string(),
        },
        ProcessFailure::Invalid(reason) => ClientError::Protocol { reason },
    }
}

fn parse_capabilities(value: &Json) -> ServerCapabilities {
    ServerCapabilities {
        completion: advertised(value.get("completionProvider")),
        hover: advertised(value.get("hoverProvider")),
        workspace_symbols: advertised(value.get("workspaceSymbolProvider")),
        rename: advertised(value.get("renameProvider")),
        definition: advertised(value.get("definitionProvider")),
        references: advertised(value.get("referencesProvider")),
    }
}

fn advertised(value: &Json) -> bool {
    !matches!(value, Json::Null | Json::Bool(false))
}

fn position_params(uri: &str, position: Position) -> Json {
    object([
        ("textDocument", object([("uri", Json::text(uri))])),
        (
            "position",
            object([
                ("line", Json::Number(f64::from(position.line))),
                ("character", Json::Number(f64::from(position.character))),
            ]),
        ),
    ])
}

fn parse_position(value: &Json) -> Result<Position, ClientError> {
    let line = integer(value.get("line")).ok_or_else(|| protocol("position has no line"))?;
    let character =
        integer(value.get("character")).ok_or_else(|| protocol("position has no character"))?;
    Ok(Position {
        line: u32::try_from(line).map_err(|_| protocol("position line is outside u32"))?,
        character: u32::try_from(character)
            .map_err(|_| protocol("position character is outside u32"))?,
    })
}

fn parse_range(value: &Json) -> Result<Range, ClientError> {
    Ok(Range {
        start: parse_position(value.get("start"))?,
        end: parse_position(value.get("end"))?,
    })
}

fn optional_range(value: &Json) -> Result<Option<Range>, ClientError> {
    if value.is_null() {
        Ok(None)
    } else {
        parse_range(value).map(Some)
    }
}

fn parse_location(value: &Json) -> Result<Location, ClientError> {
    Ok(Location {
        uri: value
            .get("uri")
            .as_text()
            .ok_or_else(|| protocol("location has no URI"))?
            .to_string(),
        range: parse_range(value.get("range"))?,
    })
}

fn parse_locations(value: &Json) -> Result<Vec<Location>, ClientError> {
    match value {
        Json::Null => Ok(Vec::new()),
        Json::Array(values) => values.iter().map(parse_location).collect(),
        Json::Object(_) => Ok(vec![parse_location(value)?]),
        _ => Err(protocol("locations result has an invalid shape")),
    }
}

fn text_member(value: &Json, key: &str) -> Option<String> {
    value.get(key).as_text().map(ToOwned::to_owned)
}

fn hover_text(value: &Json) -> String {
    match value {
        Json::Text(text) => text.clone(),
        Json::Object(_) => value.get("value").as_text().unwrap_or_default().to_string(),
        Json::Array(values) => values
            .iter()
            .map(hover_text)
            .collect::<Vec<_>>()
            .join("\n\n"),
        _ => String::new(),
    }
}

fn parse_symbol(value: &Json) -> Result<WorkspaceSymbol, ClientError> {
    let kind = integer(value.get("kind")).ok_or_else(|| protocol("symbol has no kind"))?;
    Ok(WorkspaceSymbol {
        name: value
            .get("name")
            .as_text()
            .ok_or_else(|| protocol("symbol has no name"))?
            .to_string(),
        kind: u32::try_from(kind).map_err(|_| protocol("symbol kind is outside u32"))?,
        location: if value.get("location").is_null() {
            None
        } else {
            Some(parse_location(value.get("location"))?)
        },
    })
}

fn parse_workspace_edit(value: &Json) -> Result<WorkspaceEdit, ClientError> {
    let mut changes = BTreeMap::new();
    let Some(documents) = value.get("changes").as_object() else {
        return Ok(WorkspaceEdit { changes });
    };
    for (uri, edits) in documents {
        let edits = edits
            .as_array()
            .ok_or_else(|| protocol("workspace edits are not a list"))?
            .iter()
            .map(|edit| {
                Ok(TextEdit {
                    range: parse_range(edit.get("range"))?,
                    new_text: edit
                        .get("newText")
                        .as_text()
                        .ok_or_else(|| protocol("text edit has no replacement"))?
                        .to_string(),
                })
            })
            .collect::<Result<Vec<_>, ClientError>>()?;
        changes.insert(uri.clone(), edits);
    }
    Ok(WorkspaceEdit { changes })
}

fn parse_diagnostic(value: &Json) -> Option<Diagnostic> {
    let severity = integer(value.get("severity")).and_then(|number| {
        u32::try_from(number).ok().map(|number| match number {
            1 => DiagnosticSeverity::Error,
            2 => DiagnosticSeverity::Warning,
            3 => DiagnosticSeverity::Information,
            4 => DiagnosticSeverity::Hint,
            other => DiagnosticSeverity::Other(other),
        })
    });
    Some(Diagnostic {
        range: parse_range(value.get("range")).ok()?,
        message: value.get("message").as_text()?.to_string(),
        source: text_member(value, "source"),
        severity,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};

    #[derive(Clone)]
    struct RecordingFactory {
        state: Arc<Mutex<RecordingState>>,
        unavailable: Option<String>,
    }

    #[derive(Default)]
    struct RecordingState {
        sent: Vec<Json>,
        incoming: VecDeque<Result<String, ProcessFailure>>,
        stopped: bool,
    }

    struct RecordingProcess(Arc<Mutex<RecordingState>>);

    impl ProcessFactory for RecordingFactory {
        fn start(&self) -> Result<Box<dyn LanguageServerProcess>, String> {
            if let Some(reason) = &self.unavailable {
                return Err(reason.clone());
            }
            Ok(Box::new(RecordingProcess(Arc::clone(&self.state))))
        }
    }

    impl LanguageServerProcess for RecordingProcess {
        fn send(&mut self, message: &str) -> Result<(), ProcessFailure> {
            self.0.lock().unwrap().sent.push(parse(message).unwrap());
            Ok(())
        }

        fn receive(&mut self, _timeout: Duration) -> Result<String, ProcessFailure> {
            self.0
                .lock()
                .unwrap()
                .incoming
                .pop_front()
                .unwrap_or(Err(ProcessFailure::Timeout))
        }

        fn stop(&mut self) {
            self.0.lock().unwrap().stopped = true;
        }
    }

    fn response(id: u32, result: Json) -> String {
        object([
            ("jsonrpc", Json::text("2.0")),
            ("id", Json::Number(f64::from(id))),
            ("result", result),
        ])
        .render()
    }

    fn factory(incoming: Vec<String>) -> RecordingFactory {
        RecordingFactory {
            state: Arc::new(Mutex::new(RecordingState {
                incoming: incoming.into_iter().map(Ok).collect(),
                ..RecordingState::default()
            })),
            unavailable: None,
        }
    }

    fn all_capabilities() -> Json {
        object([(
            "capabilities",
            object([
                ("completionProvider", object([])),
                ("hoverProvider", Json::Bool(true)),
                ("workspaceSymbolProvider", Json::Bool(true)),
                ("renameProvider", Json::Bool(true)),
                ("definitionProvider", Json::Bool(true)),
                ("referencesProvider", Json::Bool(true)),
            ]),
        )])
    }

    fn initialized(incoming: Vec<String>) -> (SourceKitClient, RecordingFactory) {
        let mut messages = vec![response(1, all_capabilities())];
        messages.extend(incoming);
        let factory = factory(messages);
        let client = SourceKitClient::start(&factory, ClientOptions::new("file:///project"))
            .expect("initialize succeeds");
        (client, factory)
    }

    #[test]
    fn initializes_and_shuts_down_a_recording_process() {
        let factory = factory(vec![
            response(1, all_capabilities()),
            response(2, Json::Null),
        ]);
        let mut client =
            SourceKitClient::start(&factory, ClientOptions::new("file:///project")).unwrap();
        assert_eq!(
            client.capabilities(),
            ServerCapabilities {
                completion: true,
                hover: true,
                workspace_symbols: true,
                rename: true,
                definition: true,
                references: true,
            }
        );
        client.shutdown().unwrap();
        let state = factory.state.lock().unwrap();
        let methods: Vec<_> = state
            .sent
            .iter()
            .filter_map(|message| message.get("method").as_text())
            .collect();
        assert_eq!(methods, ["initialize", "initialized", "shutdown", "exit"]);
        assert!(state.stopped);
    }

    #[test]
    fn reports_unavailable_crashed_and_timed_out_processes() {
        let unavailable = RecordingFactory {
            state: Arc::default(),
            unavailable: Some("not found".to_string()),
        };
        assert!(matches!(
            SourceKitClient::start(&unavailable, ClientOptions::new("file:///project")),
            Err(ClientError::Unavailable { .. })
        ));

        for (failure, expected_timeout) in [
            (ProcessFailure::Stopped, false),
            (ProcessFailure::Timeout, true),
        ] {
            let factory = RecordingFactory {
                state: Arc::new(Mutex::new(RecordingState {
                    incoming: VecDeque::from([Err(failure)]),
                    ..RecordingState::default()
                })),
                unavailable: None,
            };
            let result = SourceKitClient::start(&factory, ClientOptions::new("file:///project"));
            assert_eq!(
                matches!(&result, Err(ClientError::Timeout { .. })),
                expected_timeout
            );
            assert_eq!(
                matches!(&result, Err(ClientError::Crashed { .. })),
                !expected_timeout
            );
        }
    }

    #[test]
    fn rejects_stale_diagnostics_after_newer_buffer_revision() {
        let stale = object([
            ("jsonrpc", Json::text("2.0")),
            ("method", Json::text("textDocument/publishDiagnostics")),
            (
                "params",
                object([
                    ("uri", Json::text("file:///project/Main.swift")),
                    ("version", Json::Number(1.0)),
                    (
                        "diagnostics",
                        Json::Array(vec![object([
                            ("range", range_json(0, 0, 0, 3)),
                            ("message", Json::text("old")),
                        ])]),
                    ),
                ]),
            ),
        ])
        .render();
        let fresh = object([
            ("jsonrpc", Json::text("2.0")),
            ("method", Json::text("textDocument/publishDiagnostics")),
            (
                "params",
                object([
                    ("uri", Json::text("file:///project/Main.swift")),
                    ("version", Json::Number(2.0)),
                    (
                        "diagnostics",
                        Json::Array(vec![object([
                            ("range", range_json(1, 0, 1, 3)),
                            ("message", Json::text("fresh")),
                        ])]),
                    ),
                ]),
            ),
        ])
        .render();
        let (mut client, _) = initialized(vec![stale, fresh]);
        client
            .open_document("file:///project/Main.swift", "let a = 1", 1)
            .unwrap();
        client
            .change_document("file:///project/Main.swift", "let a = 2", 2)
            .unwrap();
        assert!(client.poll(Duration::ZERO).unwrap());
        assert!(client.diagnostics("file:///project/Main.swift").is_none());
        assert!(client.poll(Duration::ZERO).unwrap());
        let diagnostics = client.diagnostics("file:///project/Main.swift").unwrap();
        assert_eq!(diagnostics.version, 2);
        assert_eq!(diagnostics.items[0].message, "fresh");
        assert!(matches!(
            client.change_document("file:///project/Main.swift", "older", 1),
            Err(ClientError::StaleDocumentVersion { .. })
        ));
    }

    #[test]
    fn gates_unadvertised_features_with_an_explanation() {
        let factory = factory(vec![response(1, object([("capabilities", object([]))]))]);
        let client =
            SourceKitClient::start(&factory, ClientOptions::new("file:///project")).unwrap();
        for feature in [
            LanguageFeature::Completion,
            LanguageFeature::Hover,
            LanguageFeature::WorkspaceSymbols,
            LanguageFeature::Rename,
            LanguageFeature::Definition,
            LanguageFeature::References,
        ] {
            let error = client.availability(feature).unwrap_err();
            assert!(matches!(error, ClientError::Unsupported { .. }));
            assert!(error.to_string().contains("did not advertise"));
        }
    }

    #[test]
    fn decodes_all_advertised_language_operations() {
        let completion = Json::Array(vec![object([
            ("label", Json::text("print")),
            ("detail", Json::text("Swift.print")),
            ("insertText", Json::text("print($0)")),
        ])]);
        let hover = object([
            (
                "contents",
                object([("value", Json::text("`let value: Int`"))]),
            ),
            ("range", range_json(0, 4, 0, 9)),
        ]);
        let symbol = Json::Array(vec![object([
            ("name", Json::text("Player")),
            ("kind", Json::Number(5.0)),
            ("location", location_json("file:///project/Player.swift")),
        ])]);
        let rename = object([(
            "changes",
            Json::Object(BTreeMap::from([(
                "file:///project/Main.swift".to_string(),
                Json::Array(vec![object([
                    ("range", range_json(0, 4, 0, 9)),
                    ("newText", Json::text("score")),
                ])]),
            )])),
        )]);
        let location = location_json("file:///project/Main.swift");
        let (mut client, _) = initialized(vec![
            response(2, completion),
            response(3, hover),
            response(4, symbol),
            response(5, rename),
            response(6, location.clone()),
            response(7, Json::Array(vec![location])),
        ]);
        let uri = "file:///project/Main.swift";
        let position = Position {
            line: 0,
            character: 4,
        };
        assert_eq!(client.completion(uri, position).unwrap()[0].label, "print");
        assert_eq!(
            client.hover(uri, position).unwrap().unwrap().contents,
            "`let value: Int`"
        );
        assert_eq!(
            client.workspace_symbols("Player").unwrap()[0].name,
            "Player"
        );
        assert_eq!(
            client.rename(uri, position, "score").unwrap().changes.len(),
            1
        );
        assert_eq!(client.definition(uri, position).unwrap().len(), 1);
        assert_eq!(client.references(uri, position, true).unwrap().len(), 1);
    }

    fn range_json(
        start_line: u32,
        start_character: u32,
        end_line: u32,
        end_character: u32,
    ) -> Json {
        object([
            (
                "start",
                object([
                    ("line", Json::Number(f64::from(start_line))),
                    ("character", Json::Number(f64::from(start_character))),
                ]),
            ),
            (
                "end",
                object([
                    ("line", Json::Number(f64::from(end_line))),
                    ("character", Json::Number(f64::from(end_character))),
                ]),
            ),
        ])
    }

    fn location_json(uri: &str) -> Json {
        object([("uri", Json::text(uri)), ("range", range_json(0, 0, 0, 1))])
    }
}
