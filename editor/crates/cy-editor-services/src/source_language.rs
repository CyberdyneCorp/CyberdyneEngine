//! Optional SourceKit-LSP lifecycle and diagnostics behind service-owned values.
//!
//! A missing or crashed language server changes only this service's availability. Source buffers,
//! conflict-safe saves, and `ProjectService` builds have no dependency on it and remain usable.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::time::Duration;

use cy_editor_core::observe::Revision;
use cy_editor_sourcekit::{
    ClientError, ClientOptions, DiagnosticSeverity, ProcessFactory, SourceKitClient,
};

/// One revision-matched diagnostic in editor-owned coordinates.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct LanguageDiagnostic {
    /// Project-relative source path.
    pub path: String,
    /// Buffer revision described by this diagnostic.
    pub revision: Revision,
    /// Zero-based line.
    pub line: u32,
    /// Zero-based UTF-16 column, matching SourceKit-LSP.
    pub column: u32,
    /// Human-readable diagnostic.
    pub message: String,
    /// `error`, `warning`, `information`, `hint`, or `unknown`.
    pub severity: &'static str,
}

/// Observable language-service lifecycle.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum LanguageServiceState {
    /// No source has requested language features yet.
    NotStarted,
    /// SourceKit-LSP is initializing off the interface thread.
    Starting,
    /// SourceKit-LSP is initialized and accepting synchronization.
    Available,
    /// Language features are disabled, with an actionable explanation.
    Unavailable {
        /// Why the language service cannot be used.
        reason: String,
    },
}

/// Editor-owned SourceKit-LSP session for one project.
pub struct SourceLanguageService {
    root: PathBuf,
    client: Option<SourceKitClient>,
    starting: Option<Receiver<Result<SourceKitClient, ClientError>>>,
    state: LanguageServiceState,
    synchronized: BTreeMap<String, Revision>,
    diagnostics: BTreeMap<String, Vec<LanguageDiagnostic>>,
}

impl std::fmt::Debug for SourceLanguageService {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("SourceLanguageService")
            .field("root", &self.root)
            .field("state", &self.state)
            .field("synchronized", &self.synchronized)
            .field("diagnostics", &self.diagnostics)
            .finish_non_exhaustive()
    }
}

impl SourceLanguageService {
    /// Create a lazy language service rooted at a Swift package.
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self {
            root: root.into(),
            client: None,
            starting: None,
            state: LanguageServiceState::NotStarted,
            synchronized: BTreeMap::new(),
            diagnostics: BTreeMap::new(),
        }
    }

    /// Current availability for status and disabled-action explanations.
    #[must_use]
    pub const fn state(&self) -> &LanguageServiceState {
        &self.state
    }

    /// Whether SourceKit-LSP is currently initialized.
    #[must_use]
    pub const fn is_available(&self) -> bool {
        matches!(self.state, LanguageServiceState::Available)
    }

    /// Start through an injectable process factory.
    ///
    /// This is public so the lifecycle can be tested without relying on an installed toolchain.
    pub fn connect_with(
        &mut self,
        factory: &dyn ProcessFactory,
        timeout: Duration,
    ) -> Result<(), ClientError> {
        let mut options = ClientOptions::new(file_uri(&self.root));
        options.request_timeout = timeout;
        match SourceKitClient::start(factory, options) {
            Ok(client) => {
                self.client = Some(client);
                self.starting = None;
                self.state = LanguageServiceState::Available;
                Ok(())
            }
            Err(error) => {
                self.disable(&error);
                Err(error)
            }
        }
    }

    /// Synchronize an editor buffer if its revision advanced.
    ///
    /// Startup is lazy so creating a headless Editor never launches an external process.
    pub fn synchronize(&mut self, path: &str, text: &str, revision: Revision) {
        if matches!(self.state, LanguageServiceState::NotStarted) {
            let mut options = ClientOptions::new(file_uri(&self.root));
            options.request_timeout = Duration::from_secs(10);
            let (sender, receiver) = mpsc::channel();
            std::thread::spawn(move || {
                let _ = sender.send(SourceKitClient::launch(options));
            });
            self.starting = Some(receiver);
            self.state = LanguageServiceState::Starting;
        }
        self.finish_startup();
        if !self.is_available() || self.synchronized.get(path) == Some(&revision) {
            return;
        }
        let Some(version) = revision_to_version(revision) else {
            self.state = LanguageServiceState::Unavailable {
                reason:
                    "the source revision exceeded SourceKit-LSP's version range; reopen the editor"
                        .to_string(),
            };
            self.client = None;
            return;
        };
        let uri = self.uri_for(path);
        let result = if self.synchronized.contains_key(path) {
            self.client
                .as_mut()
                .expect("available state owns a client")
                .change_document(&uri, text, version)
        } else {
            self.client
                .as_mut()
                .expect("available state owns a client")
                .open_document(&uri, text, version)
        };
        match result {
            Ok(()) => {
                self.synchronized.insert(path.to_string(), revision);
            }
            Err(error) => self.disable(&error),
        }
    }

    /// Drain a bounded number of server publications without blocking a frame.
    pub fn pump(&mut self) {
        self.finish_startup();
        if !self.is_available() {
            return;
        }
        for _ in 0..16 {
            let result = self
                .client
                .as_mut()
                .expect("available state owns a client")
                .poll(Duration::ZERO);
            match result {
                Ok(true) => {}
                Ok(false) => break,
                Err(error) => {
                    self.disable(&error);
                    return;
                }
            }
        }
        self.project_diagnostics();
    }

    /// Diagnostics for the currently synchronized revision of one path.
    #[must_use]
    pub fn diagnostics(&self, path: &str) -> &[LanguageDiagnostic] {
        self.diagnostics.get(path).map_or(&[], Vec::as_slice)
    }

    fn project_diagnostics(&mut self) {
        let Some(client) = self.client.as_ref() else {
            return;
        };
        for (path, revision) in &self.synchronized {
            let uri = self.uri_for(path);
            let Some(published) = client.diagnostics(&uri) else {
                continue;
            };
            if revision_to_version(*revision) != Some(published.version) {
                continue;
            }
            self.diagnostics.insert(
                path.clone(),
                published
                    .items
                    .iter()
                    .map(|diagnostic| LanguageDiagnostic {
                        path: path.clone(),
                        revision: *revision,
                        line: diagnostic.range.start.line,
                        column: diagnostic.range.start.character,
                        message: diagnostic.message.clone(),
                        severity: severity_name(diagnostic.severity),
                    })
                    .collect(),
            );
        }
    }

    fn uri_for(&self, path: &str) -> String {
        file_uri(&self.root.join(path))
    }

    fn disable(&mut self, error: &ClientError) {
        self.client = None;
        self.starting = None;
        self.synchronized.clear();
        self.diagnostics.clear();
        self.state = LanguageServiceState::Unavailable {
            reason: format!(
                "{error}. Source editing, saving, Swift builds, and external editors remain available."
            ),
        };
    }

    fn finish_startup(&mut self) {
        let Some(starting) = self.starting.as_ref() else {
            return;
        };
        match starting.try_recv() {
            Ok(Ok(client)) => {
                self.client = Some(client);
                self.starting = None;
                self.state = LanguageServiceState::Available;
            }
            Ok(Err(error)) => self.disable(&error),
            Err(TryRecvError::Empty) => {}
            Err(TryRecvError::Disconnected) => self.disable(&ClientError::Unavailable {
                reason: "the SourceKit-LSP startup worker stopped".to_string(),
                remedy: "install or select a supported Swift toolchain".to_string(),
            }),
        }
    }
}

fn revision_to_version(revision: Revision) -> Option<i32> {
    i32::try_from(revision.as_u64()).ok()
}

fn severity_name(severity: Option<DiagnosticSeverity>) -> &'static str {
    match severity {
        Some(DiagnosticSeverity::Error) => "error",
        Some(DiagnosticSeverity::Warning) => "warning",
        Some(DiagnosticSeverity::Information) => "information",
        Some(DiagnosticSeverity::Hint) => "hint",
        Some(DiagnosticSeverity::Other(_)) | None => "unknown",
    }
}

fn file_uri(path: &Path) -> String {
    let absolute = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
    let text = absolute.to_string_lossy();
    let mut uri = String::from("file://");
    for byte in text.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'/' | b'-' | b'_' | b'.' | b'~') {
            uri.push(char::from(byte));
        } else {
            use std::fmt::Write as _;
            let _ = write!(uri, "%{byte:02X}");
        }
    }
    uri
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::project::{BuildRequest, ModuleBuilder, ProjectService};
    use crate::source_workspace::{SourceSave, SourceWorkspaceService};
    use cy_editor_core::problem::Result as CyResult;
    use cy_editor_sourcekit::{LanguageServerProcess, ProcessFailure};
    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};

    struct Factory(Arc<Mutex<VecDeque<Result<String, ProcessFailure>>>>);

    struct Process(Arc<Mutex<VecDeque<Result<String, ProcessFailure>>>>);

    struct Builder;

    impl ModuleBuilder for Builder {
        fn describe(&self) -> String {
            "recording Swift builder".to_string()
        }

        fn build(&self, request: &BuildRequest) -> CyResult<PathBuf> {
            Ok(request.work.join("libGame.dylib"))
        }
    }

    impl ProcessFactory for Factory {
        fn start(&self) -> Result<Box<dyn LanguageServerProcess>, String> {
            Ok(Box::new(Process(Arc::clone(&self.0))))
        }
    }

    impl LanguageServerProcess for Process {
        fn send(&mut self, message: &str) -> Result<(), ProcessFailure> {
            assert!(message.starts_with('{'), "client sends JSON objects");
            Ok(())
        }

        fn receive(&mut self, _timeout: Duration) -> Result<String, ProcessFailure> {
            self.0
                .lock()
                .unwrap()
                .pop_front()
                .unwrap_or(Err(ProcessFailure::Timeout))
        }

        fn stop(&mut self) {}
    }

    #[test]
    fn a_terminated_server_disables_only_language_enrichment() {
        let root = std::env::temp_dir().join(format!(
            "cy-source-language-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(root.join("game")).unwrap();
        std::fs::write(root.join("game/Game.swift"), "let score = 1").unwrap();
        let incoming = Arc::new(Mutex::new(VecDeque::from([
            Ok(r#"{"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}}"#.to_string()),
            Err(ProcessFailure::Stopped),
        ])));
        let mut language = SourceLanguageService::new(&root);
        language
            .connect_with(&Factory(incoming), Duration::from_millis(10))
            .unwrap();
        language.synchronize("game/Game.swift", "let score = 1", Revision::INITIAL);
        language.pump();

        let LanguageServiceState::Unavailable { reason } = language.state() else {
            panic!("a stopped process must become unavailable")
        };
        assert!(reason.contains("editing, saving, Swift builds"));

        let mut sources = SourceWorkspaceService::new(&root);
        let snapshot = sources.open("game/Game.swift").unwrap();
        assert!(matches!(
            sources
                .save_if_unchanged("game/Game.swift", "let score = 2", snapshot.fingerprint)
                .unwrap(),
            SourceSave::Written { .. }
        ));
        let mut project = ProjectService::new(&root).with_builder(Arc::new(Builder));
        let (_label, build) = project.next_build().unwrap();
        let operation = cy_editor_core::progress::Operation::new("test build");
        build(&operation).unwrap();
        assert!(project.built().is_ok());
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn file_uris_escape_spaces_without_changing_slashes() {
        assert!(file_uri(Path::new("/tmp/Game Project")).ends_with("/tmp/Game%20Project"));
    }

    #[test]
    fn diagnostics_are_projected_with_exact_revision_line_and_column() {
        let incoming = Arc::new(Mutex::new(VecDeque::from([
            Ok(r#"{"jsonrpc":"2.0","id":1,"result":{"capabilities":{}}}"#.to_string()),
            Ok(r#"{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":"file:///project/game/Game.swift","version":0,"diagnostics":[{"range":{"start":{"line":3,"character":7},"end":{"line":3,"character":12}},"severity":1,"message":"cannot find name"}]}}"#.to_string()),
        ])));
        let mut language = SourceLanguageService::new("/project");
        language
            .connect_with(&Factory(incoming), Duration::from_millis(10))
            .unwrap();
        language.synchronize("game/Game.swift", "let score = missing", Revision::INITIAL);
        language.pump();

        let diagnostics = language.diagnostics("game/Game.swift");
        assert_eq!(diagnostics.len(), 1);
        assert_eq!((diagnostics[0].line, diagnostics[0].column), (3, 7));
        assert_eq!(diagnostics[0].revision, Revision::INITIAL);
        assert_eq!(diagnostics[0].severity, "error");
    }
}
