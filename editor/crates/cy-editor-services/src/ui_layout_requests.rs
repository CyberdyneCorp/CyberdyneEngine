// SPDX-License-Identifier: MIT
//! The editor's side of the `ui.*` operations (issue #29, "UI layout"): the engine's vocabulary,
//! asked for once something wants it, and the canvas — one preview in flight, the newest one
//! waiting behind it, and the engine's last answer kept.
//!
//! **The newest preview wins.** An author dragging a handle saves an edit a frame; the canvas needs
//! the last of them drawn, not every one in turn, so a preview asked for while another is in flight
//! replaces any that was waiting. The catalogue is never dropped for a preview.

use cy_editor_core::codec::Reader;
use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::runtime::RuntimeSession;
use crate::ui_document::UiDocument;
use crate::ui_layout::{
    CATALOGUE, PREVIEW, PreviewFile, PreviewSettings, UiCatalogue, UiPreview, preview_payload,
};

const SCHEMA: u32 = 1;
/// The most files one preview carries, as the engine refuses more.
const MAX_FILES: usize = 64;

/// The `ui.*` request state [`crate::backend::BackendServices`] holds.
#[derive(Debug, Default)]
pub struct UiLayoutRequests {
    wanted: bool,
    catalogue: Versioned<Option<UiCatalogue>>,
    catalogue_requested: bool,
    in_flight: Option<(RequestId, String, Option<String>)>,
    waiting: Option<(String, Vec<u8>)>,
    preview: Versioned<Option<UiPreview>>,
    previewed: Option<String>,
    settings: PreviewSettings,
    reference: Option<String>,
    problem: Option<String>,
}

impl UiLayoutRequests {
    /// Ask for the engine's vocabulary at the next [`Self::maintain`]. Nothing about interfaces
    /// reaches a runtime until a panel, a command or an agent asks.
    pub fn want(&mut self) {
        self.wanted = true;
    }

    /// The document the canvas shows, and how it is previewed.
    #[must_use]
    pub const fn settings(&self) -> &PreviewSettings {
        &self.settings
    }

    /// Change how the canvas previews. The caller asks for a new preview.
    pub fn set_settings(&mut self, settings: PreviewSettings) {
        self.settings = settings;
    }

    /// The document the canvas shows, once one was previewed.
    #[must_use]
    pub fn reference(&self) -> Option<&str> {
        self.reference.as_deref()
    }

    /// Ask the engine to draw `reference` with `source`: now, or after the preview in flight —
    /// replacing any preview already waiting.
    ///
    /// # Errors
    ///
    /// When no runtime is attached, or the request cannot be sent.
    pub fn preview(
        &mut self,
        runtime: &RuntimeSession,
        reference: &str,
        payload: Vec<u8>,
    ) -> Result<Option<RequestId>> {
        if !runtime.is_connected() {
            return Err(Problem::new(
                "preview a UI document",
                "no runtime is attached, and the editor does not lay out or draw interfaces itself",
            )
            .with_remedy("start the editor with a hosted runtime (`just run-editor-live`)"));
        }
        self.want();
        self.reference = Some(reference.to_owned());
        self.waiting = Some((reference.to_owned(), payload));
        self.send_next(runtime)
    }

    fn send_next(&mut self, runtime: &RuntimeSession) -> Result<Option<RequestId>> {
        if self.in_flight.is_some() {
            return Ok(None);
        }
        if self.wanted && !self.catalogue_requested {
            self.catalogue_requested = true;
            let request = runtime.service_request(SCHEMA, CATALOGUE, Vec::new())?;
            self.in_flight = Some((request, CATALOGUE.into(), None));
            return Ok(Some(request));
        }
        let Some((reference, payload)) = self.waiting.take() else {
            return Ok(None);
        };
        let request = runtime.service_request(SCHEMA, PREVIEW, payload)?;
        self.in_flight = Some((request, PREVIEW.into(), Some(reference)));
        Ok(Some(request))
    }

    /// Once a frame: the catalogue, then the waiting preview.
    pub fn maintain(&mut self, runtime: &RuntimeSession) -> Option<Problem> {
        if !runtime.is_connected() {
            if self.in_flight.take().is_some() {
                self.problem = Some("the runtime disconnected before drawing the preview".into());
            }
            self.catalogue_requested = false;
            return None;
        }
        if !self.wanted {
            return None;
        }
        self.send_next(runtime).err()
    }

    /// Take the engine's answer to the request in flight. `None` for a message that is not one.
    pub fn accept(&mut self, message: &Message) -> Option<Option<Problem>> {
        let Message::ServiceEvent {
            request,
            kind,
            schema_version,
            payload,
        } = message
        else {
            return None;
        };
        let (pending, operation, reference) = self.in_flight.as_ref()?;
        if pending != request {
            return None;
        }
        if matches!(
            kind,
            ServiceEventKind::Accepted | ServiceEventKind::Progress
        ) {
            return Some(None);
        }
        let operation = operation.clone();
        let reference = reference.clone();
        self.in_flight = None;
        let settled = match (kind, *schema_version) {
            (ServiceEventKind::Completed, SCHEMA) if operation == CATALOGUE => {
                UiCatalogue::decode(payload).map(|catalogue| self.catalogue.set(Some(catalogue)))
            }
            (ServiceEventKind::Completed, SCHEMA) => UiPreview::decode(payload).map(|preview| {
                self.preview.set(Some(preview));
                self.previewed = reference;
            }),
            (ServiceEventKind::Completed, _) => Err(Problem::new(
                format!("read the engine's answer to {operation}"),
                "the engine answered in a schema this editor does not know",
            )),
            _ => Err(Problem::new(
                format!("run {operation} in the engine"),
                failure_detail(payload),
            )),
        };
        Some(match settled {
            Ok(()) => {
                self.problem = None;
                None
            }
            Err(problem) => {
                self.problem = Some(problem.to_string());
                Some(problem)
            }
        })
    }

    /// The engine's vocabulary, once it has answered.
    #[must_use]
    pub fn catalogue(&self) -> Option<&UiCatalogue> {
        self.catalogue.get().as_ref()
    }

    /// Moves when the vocabulary arrives.
    #[must_use]
    pub const fn catalogue_revision(&self) -> Revision {
        self.catalogue.revision()
    }

    /// The engine's last preview, and the document it is of.
    #[must_use]
    pub fn last_preview(&self) -> Option<(&str, &UiPreview)> {
        Some((self.previewed.as_deref()?, self.preview.get().as_ref()?))
    }

    /// Moves when a preview arrives.
    #[must_use]
    pub const fn preview_revision(&self) -> Revision {
        self.preview.revision()
    }

    /// Whether a request is in flight or waiting.
    #[must_use]
    pub const fn pending(&self) -> bool {
        self.in_flight.is_some() || self.waiting.is_some()
    }

    /// The last refusal, in the engine's words.
    #[must_use]
    pub fn problem(&self) -> Option<&str> {
        self.problem.as_deref()
    }
}

/// The schema-1 failure payload's code and detail, as a sentence.
fn failure_detail(payload: &[u8]) -> String {
    let mut reader = Reader::new(payload);
    match (reader.u32(), reader.text(), reader.text()) {
        (Ok(1), Ok(code), Ok(detail)) => format!("{code}: {detail}"),
        _ => "the engine refused the request".into(),
    }
}

/// Every sheet and document `document` reaches, read with `read` (a project-relative path to its
/// text), each once: what the engine needs to make it into widgets without a file system.
#[must_use]
pub fn reached_files(
    document: &UiDocument,
    read: &dyn Fn(&str) -> Option<String>,
) -> Vec<PreviewFile> {
    let mut files: Vec<PreviewFile> = Vec::new();
    let mut queue: Vec<String> = document.dependencies();
    while let Some(path) = queue.pop() {
        if files.len() >= MAX_FILES || files.iter().any(|file| file.path == path) {
            continue;
        }
        let Some(bytes) = read(&path) else {
            // The engine names a file it cannot read; the editor does not invent one.
            continue;
        };
        if std::path::Path::new(&path)
            .extension()
            .and_then(|e| e.to_str())
            == Some(crate::ui_document::EXTENSION)
            && let Ok(used) = UiDocument::decode(&bytes)
        {
            queue.extend(used.dependencies());
        }
        files.push(PreviewFile { path, bytes });
    }
    files
}

/// The `ui.preview.render` payload for a document as it is saved, its files and the theme read
/// through `read`, and the widget the settings put into a state found on its line.
#[must_use]
pub fn payload_for(
    source: &str,
    settings: &PreviewSettings,
    read: &dyn Fn(&str) -> Option<String>,
) -> Vec<u8> {
    let document = UiDocument::decode(source).unwrap_or_default();
    let files = reached_files(&document, read);
    let theme = if settings.theme.is_empty() {
        String::new()
    } else {
        read(&settings.theme).unwrap_or_default()
    };
    let state_line = settings
        .state_widget
        .and_then(|widget| document.line_of(widget))
        .unwrap_or(0);
    preview_payload(source, &files, &theme, settings, state_line)
}

/// The settings `change` makes of `current`: what it leaves `None` stays.
#[must_use]
pub fn changed_settings(
    current: &PreviewSettings,
    change: &cy_editor_commands::UiPreviewChange,
) -> PreviewSettings {
    let mut settings = current.clone();
    if let Some((width, height)) = change.size {
        settings.width = width;
        settings.height = height;
    }
    if let Some(dpi) = change.dpi {
        settings.dpi = dpi;
    }
    if let Some(mode) = change
        .scale_mode
        .as_deref()
        .and_then(crate::ui_layout::ScaleMode::named)
    {
        settings.scale_mode = mode;
    }
    if let Some((width, height)) = change.reference {
        settings.reference = [width, height];
    }
    if let Some(theme) = &change.theme {
        settings.theme.clone_from(theme);
    }
    if let Some(widget) = change.state_widget {
        settings.state_widget = widget;
    }
    if let Some(state) = change
        .state
        .as_deref()
        .and_then(crate::ui_layout::PreviewState::named)
    {
        settings.state = state;
    }
    settings
}

/// `ui.status`: the canvas's last preview as a command outcome, for a person and for an agent.
#[must_use]
pub fn outcome(requests: &UiLayoutRequests, source: Option<&str>) -> cy_editor_commands::Outcome {
    use cy_editor_core::value::Value;
    let whole = |value: u32| Value::Int(i64::from(value));
    let settings = requests.settings();
    let mut outcome = cy_editor_commands::Outcome::new(match requests.last_preview() {
        Some((reference, _)) => format!("The engine's preview of {reference}"),
        None => "The engine has not drawn a preview".into(),
    })
    .with("pending", Value::Bool(requests.pending()))
    .with("catalogue", Value::Bool(requests.catalogue().is_some()))
    .with(
        "target",
        Value::Text(format!("{}x{}", settings.width, settings.height)),
    )
    .with("dpi", Value::Float(settings.dpi))
    .with("scale_mode", Value::Text(settings.scale_mode.name().into()))
    .with("theme", Value::Text(settings.theme.clone()))
    .with("state", Value::Text(settings.state.name().into()));
    if let Some(problem) = requests.problem() {
        outcome = outcome.with("problem", Value::Text(problem.to_owned()));
    }
    let Some((reference, preview)) = requests.last_preview() else {
        return outcome;
    };
    outcome = outcome
        .with("reference", Value::Text(reference.to_owned()))
        .with(
            "image",
            Value::Text(format!("{}x{}", preview.width, preview.height)),
        )
        .with("scale", Value::Float(preview.scale))
        .with("widgets", whole(preview.widgets))
        .with("primitives", whole(preview.primitives))
        .with("draws", whole(preview.draws))
        .with("layout_ms", Value::Float(narrow(preview.layout_ms)))
        .with("draw_ms", Value::Float(narrow(preview.draw_ms)))
        .with("classes", Value::Text(preview.classes.join(" ")))
        .with(
            "diagnostics",
            whole(u32::try_from(preview.diagnostics.len()).unwrap_or(0)),
        );
    for diagnostic in &preview.diagnostics {
        outcome = outcome.with(
            format!("diagnostic.{}.{}", diagnostic.line, diagnostic.column),
            Value::Text(format!("{} ({})", diagnostic.message, diagnostic.detail)),
        );
    }
    let document = source.and_then(|text| UiDocument::decode(text).ok());
    for node in &preview.nodes {
        let name = document
            .as_ref()
            .and_then(|document| document.widget_at_line(node.line))
            .map_or_else(
                || format!("line.{}", node.line),
                |index| format!("widget.{index}"),
            );
        let [x, y, width, height] = node.rect;
        outcome = outcome.with(
            name,
            Value::Text(if node.made {
                format!("{x} {y} {width} {height}")
            } else {
                "not made".into()
            }),
        );
    }
    outcome
}

#[allow(
    clippy::cast_possible_truncation,
    reason = "milliseconds shown to a person"
)]
const fn narrow(value: f64) -> f32 {
    value as f32
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    #[test]
    fn every_file_a_document_reaches_is_sent_once_and_a_missing_one_is_left_to_the_engine() {
        let files: BTreeMap<&str, &str> = [
            ("ui/menu.cyss", ".big { height: 40; }"),
            (
                "ui/stat.cyui",
                "cyui 1\nstylesheet \"ui/stat.cyss\"\nrow\n  use \"ui/bar.cyui\"\n",
            ),
            ("ui/stat.cyss", ".stat { gap: 4; }"),
            ("ui/bar.cyui", "cyui 1\nprogress-bar\n"),
        ]
        .into_iter()
        .collect();
        let read = |path: &str| files.get(path).map(|text| (*text).to_owned());
        let document = UiDocument::decode(
            "cyui 1\nstylesheet \"ui/menu.cyss\"\nstylesheet \"ui/missing.cyss\"\ncolumn\n  use \"ui/stat.cyui\"\n  use \"ui/stat.cyui\"\n",
        )
        .unwrap();
        let mut sent: Vec<String> = reached_files(&document, &read)
            .into_iter()
            .map(|file| file.path)
            .collect();
        sent.sort();
        assert_eq!(
            sent,
            vec![
                "ui/bar.cyui",
                "ui/menu.cyss",
                "ui/stat.cyss",
                "ui/stat.cyui"
            ]
        );
    }

    #[test]
    fn the_state_is_sent_on_the_line_of_the_widget_it_names() {
        let source = "cyui 1\nstylesheet \"t.cyss\"\ncolumn\n  // a note\n  button #play\n";
        let settings = PreviewSettings {
            state_widget: Some(1),
            state: crate::ui_layout::PreviewState::Hover,
            theme: "themes/dark.cyss".into(),
            ..PreviewSettings::default()
        };
        let read = |path: &str| (path == "themes/dark.cyss").then(|| "button {}".to_owned());
        let payload = payload_for(source, &settings, &read);
        // The tail: u32 state line, u8 state.
        let tail = &payload[payload.len() - 5..];
        assert_eq!(&tail[..4], &5_u32.to_le_bytes());
        assert_eq!(tail[4], 1);
        let theme = b"button {}";
        assert!(payload.windows(theme.len()).any(|window| window == theme));
    }
}
