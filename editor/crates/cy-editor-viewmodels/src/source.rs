//! In-progress Swift source buffers, separate from project state until an explicit save.

use cy_editor_core::observe::{Revision, Watch};
use cy_editor_core::problem::Result;
use cy_editor_services::{
    BuildState, OperationService, ProjectService, ReloadReport, SourceFingerprint,
    SourceLanguageService, SourceSnapshot, SourceWorkspaceService,
};

/// Structured build status rendered by the Swift Workspace.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct SourceBuildPresentation {
    /// Stable build state name.
    pub state: String,
    /// Hot-reload generation being built or last built.
    pub generation: u32,
    /// Progress fraction while the background operation can report one.
    pub fraction: Option<f32>,
    /// Current operation step or final build message.
    pub step: String,
    /// Compiler diagnostic when the build failed.
    pub diagnostic: Option<String>,
    /// Produced library, when successful.
    pub library: Option<String>,
}

/// A zero-based text position reported by the editor or language service.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct SourcePosition {
    /// UTF-8 byte offset into the buffer.
    pub offset: usize,
}

/// A diagnostic projected without carrying a SourceKit or LSP type into the view-model layer.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct SourceDiagnostic {
    /// Human-readable diagnostic.
    pub message: String,
    /// Zero-based start position.
    pub position: SourcePosition,
    /// Zero-based line reported by SourceKit-LSP.
    pub line: u32,
    /// Zero-based UTF-16 column reported by SourceKit-LSP.
    pub column: u32,
    /// `error`, `warning`, `information`, or `hint`.
    pub severity: String,
}

/// The three-way inputs retained when disk changed beneath an edited buffer.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct SourceConflict {
    /// Text the buffer originally opened from.
    pub base: String,
    /// Unsaved text the person or agent produced.
    pub buffer: String,
    /// Current disk text, or `None` when the file was externally removed.
    pub disk: Option<String>,
    /// Fingerprint of `disk`.
    pub disk_fingerprint: SourceFingerprint,
}

/// One source editor's presentation state.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct SourceBufferViewModel {
    path: String,
    base: String,
    base_fingerprint: SourceFingerprint,
    text: String,
    cursor: SourcePosition,
    cursor_request: Option<SourcePosition>,
    selection: (SourcePosition, SourcePosition),
    revision: Revision,
    diagnostics: Vec<SourceDiagnostic>,
    conflict: Option<SourceConflict>,
}

impl SourceBufferViewModel {
    /// Open a buffer from an exact source snapshot.
    #[must_use]
    pub fn open(snapshot: SourceSnapshot) -> Self {
        Self {
            path: snapshot.path,
            base: snapshot.text.clone(),
            base_fingerprint: snapshot.fingerprint,
            text: snapshot.text,
            cursor: SourcePosition::default(),
            cursor_request: None,
            selection: (SourcePosition::default(), SourcePosition::default()),
            revision: Revision::INITIAL,
            diagnostics: Vec::new(),
            conflict: None,
        }
    }

    /// Project-relative path.
    #[must_use]
    pub fn path(&self) -> &str {
        &self.path
    }

    /// In-progress text. It is not project state until the save command accepts it.
    #[must_use]
    pub fn text(&self) -> &str {
        &self.text
    }

    /// Text this buffer originally read from disk.
    #[must_use]
    pub fn base(&self) -> &str {
        &self.base
    }

    /// Fingerprint a save must still find on disk.
    #[must_use]
    pub const fn base_fingerprint(&self) -> SourceFingerprint {
        self.base_fingerprint
    }

    /// Whether buffer text differs from its disk base.
    #[must_use]
    pub fn is_dirty(&self) -> bool {
        self.text != self.base
    }

    /// Buffer revision for versioned diagnostics and future SourceKit requests.
    #[must_use]
    pub const fn revision(&self) -> Revision {
        self.revision
    }

    /// Cursor.
    #[must_use]
    pub const fn cursor(&self) -> SourcePosition {
        self.cursor
    }

    /// Ordered selection endpoints.
    #[must_use]
    pub const fn selection(&self) -> (SourcePosition, SourcePosition) {
        self.selection
    }

    /// Diagnostics for this exact buffer revision.
    #[must_use]
    pub fn diagnostics(&self) -> &[SourceDiagnostic] {
        &self.diagnostics
    }

    /// Active external-change conflict.
    #[must_use]
    pub const fn conflict(&self) -> Option<&SourceConflict> {
        self.conflict.as_ref()
    }

    /// Replace the in-progress text and advance its observable revision.
    pub fn edit(&mut self, text: impl Into<String>) {
        let text = text.into();
        if self.text == text {
            return;
        }
        self.text = text;
        self.advance();
        self.conflict = None;
    }

    /// Move cursor and selection without changing project or buffer text.
    pub fn select(&mut self, anchor: usize, cursor: usize) {
        let limit = self.text.len();
        self.cursor = SourcePosition {
            offset: cursor.min(limit),
        };
        self.selection = (
            SourcePosition {
                offset: anchor.min(limit),
            },
            self.cursor,
        );
        self.cursor_request = Some(self.cursor);
    }

    /// Consume a request for the rendered editor to move its visible caret.
    pub fn take_cursor_request(&mut self) -> Option<SourcePosition> {
        self.cursor_request.take()
    }

    /// Accept diagnostics only for the revision they describe.
    pub fn set_diagnostics(
        &mut self,
        revision: Revision,
        diagnostics: Vec<SourceDiagnostic>,
    ) -> bool {
        if revision != self.revision {
            return false;
        }
        self.diagnostics = diagnostics;
        true
    }

    /// Record the three versions a conflict decision must see.
    pub fn report_conflict(&mut self, disk: Option<String>, disk_fingerprint: SourceFingerprint) {
        self.conflict = Some(SourceConflict {
            base: self.base.clone(),
            buffer: self.text.clone(),
            disk,
            disk_fingerprint,
        });
    }

    /// Reload: abandon the unsaved buffer and accept disk.
    pub fn resolve_reload(&mut self) {
        let Some(conflict) = self.conflict.take() else {
            return;
        };
        self.base = conflict.disk.clone().unwrap_or_default();
        self.text = self.base.clone();
        self.base_fingerprint = conflict.disk_fingerprint;
        self.cursor = SourcePosition::default();
        self.cursor_request = Some(self.cursor);
        self.selection = (self.cursor, self.cursor);
        self.advance();
    }

    /// Keep: retain buffer text but explicitly adopt current disk as the next save base.
    pub fn resolve_keep(&mut self) {
        let Some(conflict) = self.conflict.take() else {
            return;
        };
        self.base = conflict.disk.unwrap_or_default();
        self.base_fingerprint = conflict.disk_fingerprint;
    }

    /// Merge: retain a caller-produced merge and adopt current disk as its base.
    pub fn resolve_merge(&mut self, merged: impl Into<String>) {
        let Some(conflict) = self.conflict.take() else {
            return;
        };
        self.base = conflict.disk.unwrap_or_default();
        self.base_fingerprint = conflict.disk_fingerprint;
        self.text = merged.into();
        self.advance();
    }

    /// Record a successful save and the fingerprint returned by the command.
    pub fn saved(&mut self, fingerprint: SourceFingerprint) {
        self.base = self.text.clone();
        self.base_fingerprint = fingerprint;
        self.conflict = None;
    }

    fn advance(&mut self) {
        self.revision = Revision::from_u64(self.revision.as_u64().saturating_add(1));
    }
}

/// File tree, open tabs, and active buffer for the Swift Workspace panel.
#[derive(Debug, Default)]
pub struct SourceWorkspaceViewModel {
    files: Vec<String>,
    buffers: Vec<SourceBufferViewModel>,
    active: Option<usize>,
    watch: Watch,
    build_watch: Watch,
    build: SourceBuildPresentation,
    reload_watch: Watch,
    reload: Option<ReloadReport>,
}

impl SourceWorkspaceViewModel {
    /// An empty workspace presentation.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Refresh the project-relative Swift file tree only when discovery changed.
    pub fn refresh(&mut self, sources: &SourceWorkspaceService) -> bool {
        if !self.watch.changed(sources.revision()) {
            return false;
        }
        self.files = sources
            .files()
            .iter()
            .map(|file| file.path.clone())
            .collect();
        self.watch.accept(sources.revision());
        true
    }

    /// Open or activate a source buffer without writing anything.
    pub fn open(&mut self, sources: &SourceWorkspaceService, path: &str) -> Result<()> {
        if let Some(index) = self.buffers.iter().position(|buffer| buffer.path() == path) {
            self.active = Some(index);
            return Ok(());
        }
        self.buffers
            .push(SourceBufferViewModel::open(sources.open(path)?));
        self.active = Some(self.buffers.len() - 1);
        Ok(())
    }

    /// Activate an open tab.
    pub fn activate(&mut self, index: usize) -> bool {
        if index >= self.buffers.len() {
            return false;
        }
        self.active = Some(index);
        true
    }

    /// Discovered Swift paths.
    #[must_use]
    pub fn files(&self) -> &[String] {
        &self.files
    }

    /// Open buffers.
    #[must_use]
    pub fn buffers(&self) -> &[SourceBufferViewModel] {
        &self.buffers
    }

    /// Active buffer.
    #[must_use]
    pub fn active(&self) -> Option<&SourceBufferViewModel> {
        self.active.and_then(|index| self.buffers.get(index))
    }

    /// Active buffer for presentation-state edits and conflict decisions.
    pub fn active_mut(&mut self) -> Option<&mut SourceBufferViewModel> {
        self.active.and_then(|index| self.buffers.get_mut(index))
    }

    /// Refresh structured build progress without touching any source buffer.
    pub fn refresh_build(
        &mut self,
        project: &ProjectService,
        operations: &OperationService,
    ) -> bool {
        let revision = project.build_revision();
        if self.build.state != "running" && !self.build_watch.changed(revision) {
            return false;
        }
        let record = project.record();
        let running = operations.all().iter().find_map(|operation| {
            operation
                .label()
                .starts_with("Build ")
                .then(|| operation.state())
        });
        let (fraction, step) = match running {
            Some(cy_editor_core::progress::OperationState::Running { fraction, step }) => {
                (fraction, step)
            }
            _ => (None, record.message.clone()),
        };
        let next = SourceBuildPresentation {
            state: record.state.name().to_string(),
            generation: record.generation,
            fraction,
            step,
            diagnostic: (record.state == BuildState::Failed).then_some(record.message),
            library: record.library.map(|path| path.display().to_string()),
        };
        if self.build == next {
            self.build_watch.accept(revision);
            return false;
        }
        self.build = next;
        self.build_watch.accept(revision);
        true
    }

    /// Current structured build progress and diagnostic.
    #[must_use]
    pub const fn build(&self) -> &SourceBuildPresentation {
        &self.build
    }

    /// Refresh the last structured module-reload report by revision.
    pub fn refresh_reload(&mut self, revision: Revision, report: Option<&ReloadReport>) -> bool {
        if !self.reload_watch.changed(revision) {
            return false;
        }
        self.reload = report.cloned();
        self.reload_watch.accept(revision);
        true
    }

    /// Pending or completed module-reload report.
    #[must_use]
    pub const fn reload(&self) -> Option<&ReloadReport> {
        self.reload.as_ref()
    }

    /// Project revision-matched language diagnostics into open buffers.
    pub fn refresh_diagnostics(&mut self, language: &SourceLanguageService) {
        for buffer in &mut self.buffers {
            let diagnostics = language
                .diagnostics(buffer.path())
                .iter()
                .map(|diagnostic| SourceDiagnostic {
                    message: diagnostic.message.clone(),
                    position: SourcePosition {
                        offset: offset_for_position(
                            buffer.text(),
                            diagnostic.line,
                            diagnostic.column,
                        ),
                    },
                    line: diagnostic.line,
                    column: diagnostic.column,
                    severity: diagnostic.severity.to_string(),
                })
                .collect();
            let _ = buffer.set_diagnostics(buffer.revision(), diagnostics);
        }
    }

    /// Open a diagnostic's file and place the cursor at its exact line and UTF-16 column.
    pub fn navigate(
        &mut self,
        sources: &SourceWorkspaceService,
        path: &str,
        line: u32,
        column: u32,
    ) -> Result<()> {
        self.open(sources, path)?;
        if let Some(buffer) = self.active_mut() {
            let offset = offset_for_position(buffer.text(), line, column);
            buffer.select(offset, offset);
        }
        Ok(())
    }
}

fn offset_for_position(text: &str, line: u32, utf16_column: u32) -> usize {
    let Ok(wanted_line) = usize::try_from(line) else {
        return text.len();
    };
    let mut line_start = 0;
    let mut selected = None;
    for (index, line_text) in text.split_inclusive('\n').enumerate() {
        if index == wanted_line {
            selected = Some(line_text);
            break;
        }
        line_start += line_text.len();
    }
    let Some(line_text) = selected else {
        return text.len();
    };
    let content = line_text.strip_suffix('\n').unwrap_or(line_text);
    let mut units = 0_u32;
    for (offset, character) in content.char_indices() {
        if units >= utf16_column {
            return line_start + offset;
        }
        units = units.saturating_add(u32::try_from(character.len_utf16()).unwrap_or(2));
    }
    line_start + content.len()
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;
    use std::sync::Arc;

    use cy_editor_core::problem::Problem;
    use cy_editor_services::project::BuildRequest;
    use cy_editor_services::{ModuleBuilder, ProjectService, SourceWorkspaceService};

    use super::*;

    struct Sandbox(PathBuf);

    impl Sandbox {
        fn source() -> Self {
            let unique = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_or(0, |elapsed| elapsed.as_nanos());
            let root = std::env::temp_dir()
                .join(format!("cy-source-buffer-{}-{unique}", std::process::id()));
            std::fs::create_dir_all(root.join("game")).unwrap();
            std::fs::write(root.join("game/Player.swift"), "struct Player {}").unwrap();
            Self(root)
        }
    }

    impl Drop for Sandbox {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn typing_changes_only_the_buffer() {
        let sandbox = Sandbox::source();
        let service = SourceWorkspaceService::new(&sandbox.0);
        let mut buffer = SourceBufferViewModel::open(service.open("game/Player.swift").unwrap());
        buffer.edit("struct Player { var hp = 3 }");
        buffer.select(7, 13);

        assert!(buffer.is_dirty());
        assert_eq!(buffer.selection().0.offset, 7);
        assert_eq!(
            std::fs::read_to_string(sandbox.0.join("game/Player.swift")).unwrap(),
            "struct Player {}",
            "in-progress text is presentation state"
        );
    }

    #[test]
    fn a_failed_build_preserves_every_unsaved_buffer_byte() {
        struct FailingBuilder;
        impl ModuleBuilder for FailingBuilder {
            fn describe(&self) -> String {
                "failing Swift compiler double".into()
            }

            fn build(&self, _request: &BuildRequest) -> cy_editor_core::problem::Result<PathBuf> {
                Err(Problem::new(
                    "build Swift module",
                    "Player.swift:1:8: error: expected declaration",
                ))
            }
        }

        let sandbox = Sandbox::source();
        let mut sources = SourceWorkspaceService::new(&sandbox.0);
        sources.refresh().unwrap();
        let mut workspace = SourceWorkspaceViewModel::new();
        workspace.open(&sources, "game/Player.swift").unwrap();
        let unsaved = "struct Player { let unfinished = }";
        workspace.active_mut().unwrap().edit(unsaved);

        let mut project = ProjectService::new(&sandbox.0).with_builder(Arc::new(FailingBuilder));
        let (_label, build) = project.next_build().unwrap();
        let operation = cy_editor_core::progress::Operation::new("test build");
        assert!(build(&operation).is_err());

        let buffer = workspace.active().unwrap();
        assert_eq!(buffer.text(), unsaved);
        assert!(buffer.is_dirty());
        assert_eq!(project.record().state, BuildState::Failed);
    }

    #[test]
    fn diagnostics_and_conflict_decisions_are_versioned_and_explicit() {
        let snapshot = SourceSnapshot {
            path: "game/Player.swift".into(),
            text: "base".into(),
            fingerprint: SourceFingerprint::of(b"base"),
        };
        let mut buffer = SourceBufferViewModel::open(snapshot);
        let old = buffer.revision();
        buffer.edit("buffer");
        assert!(!buffer.set_diagnostics(old, Vec::new()));
        assert!(buffer.set_diagnostics(
            buffer.revision(),
            vec![SourceDiagnostic {
                message: "expected declaration".into(),
                position: SourcePosition { offset: 2 },
                line: 0,
                column: 2,
                severity: "error".into(),
            }]
        ));

        let disk_fingerprint = SourceFingerprint::of(b"disk");
        buffer.report_conflict(Some("disk".into()), disk_fingerprint);
        let conflict = buffer.conflict().unwrap();
        assert_eq!(
            (&conflict.base, &conflict.buffer),
            (&"base".into(), &"buffer".into())
        );

        buffer.resolve_keep();
        assert_eq!(buffer.text(), "buffer");
        assert_eq!(buffer.base(), "disk");
        assert!(buffer.is_dirty());
        assert_eq!(buffer.base_fingerprint(), disk_fingerprint);
    }

    #[test]
    fn reload_and_merge_make_distinct_explicit_conflict_choices() {
        let snapshot = || SourceSnapshot {
            path: "game/Player.swift".into(),
            text: "base".into(),
            fingerprint: SourceFingerprint::of(b"base"),
        };
        let disk_fingerprint = SourceFingerprint::of(b"disk");

        let mut reloaded = SourceBufferViewModel::open(snapshot());
        reloaded.edit("buffer");
        reloaded.report_conflict(Some("disk".into()), disk_fingerprint);
        reloaded.resolve_reload();
        assert_eq!(reloaded.text(), "disk");
        assert_eq!(reloaded.base(), "disk");
        assert!(!reloaded.is_dirty());
        assert_eq!(reloaded.base_fingerprint(), disk_fingerprint);

        let mut merged = SourceBufferViewModel::open(snapshot());
        merged.edit("buffer");
        merged.report_conflict(Some("disk".into()), disk_fingerprint);
        merged.resolve_merge("merged");
        assert_eq!(merged.text(), "merged");
        assert_eq!(merged.base(), "disk");
        assert!(merged.is_dirty());
        assert_eq!(merged.base_fingerprint(), disk_fingerprint);
    }

    #[test]
    fn workspace_tabs_open_without_writing_and_reuse_existing_buffers() {
        let sandbox = Sandbox::source();
        let mut service = SourceWorkspaceService::new(&sandbox.0);
        service.refresh().unwrap();
        let mut workspace = SourceWorkspaceViewModel::new();
        assert!(workspace.refresh(&service));
        workspace.open(&service, "game/Player.swift").unwrap();
        workspace.active_mut().unwrap().edit("unsaved");
        workspace.open(&service, "game/Player.swift").unwrap();
        assert_eq!(workspace.buffers().len(), 1);
        assert_eq!(workspace.active().unwrap().text(), "unsaved");
        assert_eq!(
            std::fs::read_to_string(sandbox.0.join("game/Player.swift")).unwrap(),
            "struct Player {}"
        );
    }

    #[test]
    fn diagnostic_navigation_opens_the_file_at_line_and_utf16_column() {
        let sandbox = Sandbox::source();
        std::fs::write(
            sandbox.0.join("game/Player.swift"),
            "let icon = \"🚀\"\nlet score = icon.count\n",
        )
        .unwrap();
        let service = SourceWorkspaceService::new(&sandbox.0);
        let mut workspace = SourceWorkspaceViewModel::new();
        workspace
            .navigate(&service, "game/Player.swift", 1, 4)
            .unwrap();
        let active = workspace.active().unwrap();
        assert_eq!(
            &active.text()[active.cursor().offset..],
            "score = icon.count\n"
        );
        workspace
            .navigate(&service, "game/Player.swift", 0, 14)
            .unwrap();
        let active = workspace.active().unwrap();
        assert_eq!(
            &active.text()[active.cursor().offset..],
            "\"\nlet score = icon.count\n",
            "UTF-16 columns account for the two-unit rocket scalar"
        );
        let cursor = active.cursor();
        assert_eq!(
            workspace.active_mut().unwrap().take_cursor_request(),
            Some(cursor),
            "the shell receives the exact caret request once"
        );
        assert_eq!(workspace.active_mut().unwrap().take_cursor_request(), None);
    }
}
