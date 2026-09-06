//! The status bar: the one view model that summarises everything, and therefore watches everything.

use std::fmt::Write as _;

use cy_editor_core::observe::{Cursor, Watch};
use cy_editor_documents::Document;
use cy_editor_services::{Editor, HostingMode, Notification};

/// What the status bar shows.
#[derive(Debug, Default)]
pub struct StatusViewModel {
    /// The line to render.
    line: String,
    /// The most recent notification, when there is one worth showing.
    latest: Option<Notification>,
    cursor: Cursor,
    watch: Watch,
    rebuilds: u64,
}

impl StatusViewModel {
    /// An empty status bar.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild only if something a status bar shows has moved.
    pub fn refresh(&mut self, editor: &Editor) -> bool {
        if !self.watch.changed(editor.summary_revision()) {
            return false;
        }
        for notification in editor.notifications.drain_from(&mut self.cursor) {
            self.latest = Some(notification.clone());
        }
        self.line = summarise(editor);
        self.watch.accept(editor.summary_revision());
        self.rebuilds += 1;
        true
    }

    /// The line to render.
    #[must_use]
    pub fn line(&self) -> &str {
        &self.line
    }

    /// The most recent notification.
    #[must_use]
    pub const fn latest(&self) -> Option<&Notification> {
        self.latest.as_ref()
    }

    /// How many times this bar has rebuilt.
    #[must_use]
    pub const fn rebuilds(&self) -> u64 {
        self.rebuilds
    }
}

/// One line: how many documents, how many are dirty, what is selected, and where the engine is.
fn summarise(editor: &Editor) -> String {
    let documents = editor.documents.len();
    let dirty = editor
        .documents
        .ids()
        .filter(|id| editor.documents.get(*id).is_some_and(Document::is_dirty))
        .count();
    let selected = editor.selection.get().node_count();
    let running = editor.operations.running().count();

    let engine = match editor.hosting_mode() {
        HostingMode::NoRuntime => "no runtime",
        HostingMode::Embedded => "embedded runtime",
        HostingMode::Hosted => "hosted runtime",
    };

    let mut line =
        format!("{documents} document(s), {dirty} unsaved · {selected} selected · {engine}");
    if running > 0 {
        let _ = write!(line, " · {running} running");
    }
    line
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;

    use super::*;

    #[test]
    fn an_idle_status_bar_costs_nothing() {
        let mut editor = Editor::default();
        editor.open_document("worlds/city.cyworld").unwrap();

        let mut status = StatusViewModel::new();
        assert!(status.refresh(&editor));
        for _ in 0..1000 {
            editor.pump();
            assert!(!status.refresh(&editor), "nothing moved");
        }
        assert_eq!(status.rebuilds(), 1);
    }

    #[test]
    fn the_status_bar_says_where_the_engine_is() {
        let mut editor = Editor::default();
        editor.open_document("worlds/city.cyworld").unwrap();
        let mut status = StatusViewModel::new();
        status.refresh(&editor);
        assert!(status.line().contains("no runtime"), "{}", status.line());
        assert!(status.line().contains("0 unsaved"), "{}", status.line());

        let id = editor.workspace.active().unwrap();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();
        // A document's own edit does not move the summary revision — that is what a document's
        // revision is for — so the bar is asked to rebuild by the frame that also notices the edit.
        editor.selection.update(|_| {});
        status.refresh(&editor);
        assert!(status.line().contains("1 unsaved"), "{}", status.line());
    }
}
