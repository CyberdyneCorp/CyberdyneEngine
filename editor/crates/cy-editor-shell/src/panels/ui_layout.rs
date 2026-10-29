// SPDX-License-Identifier: MIT
//! The UI layout editor: a `.cyui` document on a canvas the ENGINE draws, a widget palette, a
//! hierarchy, handles for anchors, offsets and size, and an inspector for properties, classes and
//! bindings. Issue #29, "UI layout", over issue #91's toolkit.
//!
//! **The canvas is the engine's image.** `ui.preview.render` makes the document into the toolkit's
//! widgets, lays it out at the previewed resolution and DPI, puts the chosen widget into the chosen
//! state, and draws it with the interface renderer's shader. The panel draws its selection and
//! handles over that image from the rectangles the engine's layout gave each line — it lays out
//! nothing itself — and asks again after every save, undo and redo.
//!
//! **Everything the panel changes is one registered command** (`ui.widget.*`, `ui.document.*`):
//! one undoable transaction that saves the file, which is what a running game's `DocumentHost`
//! reloads, and an MCP tool of the same name. A drag on the canvas is one command when it ends.

use cy_editor_commands::Arguments;
use cy_editor_core::value::Value;
use cy_editor_interface::Domain;
use cy_editor_interface::specialised::Session;
use cy_editor_services::ui_document::{LineKind, UiDocument, UiLine, UiValue, WidgetSummary};
use cy_editor_services::ui_layout::{
    DPI_SCALES, PreviewSettings, PreviewState, PropertyType, RESOLUTIONS, ScaleMode, UiCatalogue,
};
use cy_editor_visual::colour::Semantic;
use std::fmt::Write as _;

use super::specialised::{SpecialisedTool, ToolDiagnostic, ToolFrame};
use super::{Inputs, Intent, Panels, heading, nothing_here, secondary};
use crate::theme;

/// The panel's own presentation state.
#[derive(Default)]
pub struct UiLayoutInputs {
    /// Whether the panel drew this frame.
    pub seen: bool,
    /// The document on the canvas.
    pub reference: Option<String>,
    /// A new document's path.
    pub new_reference: String,
    /// The selected widget, by index.
    pub selected: Option<usize>,
    /// A canvas gesture in progress: committed as one command when it ends.
    pub drag: Option<CanvasDrag>,
    /// What an inspector field holds while it is typed into, by its key.
    pub fields: std::collections::BTreeMap<String, String>,
    /// The engine's image, keyed by the preview revision it was made from.
    pub texture: Option<(cy_editor_core::observe::Revision, egui::TextureHandle)>,
    /// The document and settings the panel last asked the engine to draw.
    pub asked: Option<(String, String)>,
    /// A refusal to show in the diagnostics area.
    pub problem: Option<String>,
}

/// What a canvas drag changes.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Handle {
    /// The whole widget: both offset corners move.
    Body,
    /// The bottom-right corner: the max offset moves.
    Resize,
    /// The min anchor.
    AnchorMin,
    /// The max anchor.
    AnchorMax,
}

/// A drag on the canvas, in reference units from where it started.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct CanvasDrag {
    /// The widget dragged.
    pub widget: usize,
    /// What part of it.
    pub handle: Handle,
    /// How far, in reference units.
    pub delta: [f32; 2],
}

/// What the panel edits this frame.
pub(crate) struct Target {
    reference: String,
    document: UiDocument,
    catalogue: UiCatalogue,
    settings: PreviewSettings,
    view: Option<CanvasView>,
    documents: Vec<String>,
    themes: Vec<String>,
}

/// The engine's last preview of this document, as the canvas needs it.
struct CanvasView {
    texture: egui::TextureHandle,
    size: [u32; 2],
    per_unit: f32,
    rects: Vec<Option<[f32; 4]>>,
    classes: Vec<String>,
    diagnostics: Vec<String>,
    stats: String,
}

/// The UI layout editor, drawn in the specialised-editor frame.
pub(crate) struct UiLayoutTool;

impl SpecialisedTool for UiLayoutTool {
    const DOMAIN: Domain = Domain::UiLayout;
    const TITLE: &'static str = "UI Layout";
    const COMMANDS: &'static [&'static str] = &[
        "ui.document.create",
        "ui.document.stylesheet",
        "ui.widget.add",
        "ui.widget.remove",
        "ui.widget.move",
        "ui.widget.set",
        "ui.widget.identity",
        "ui.widget.layout",
        "ui.widget.bind",
        "ui.preview",
        "ui.status",
    ];

    type Target = Target;

    fn target(panels: &mut Panels<'_>, ui: &mut egui::Ui) -> Option<Self::Target> {
        panels.inputs.ui_layout.seen = true;
        if panels.editor.workspace.active().is_none() {
            nothing_here(
                ui,
                panels.shell,
                "No world is open.",
                "Open a world; UI document edits undo in its history.",
            );
            return None;
        }
        panels.editor.backend.ui_layout.want();
        let Some(catalogue) = panels.editor.backend.ui_layout.catalogue().cloned() else {
            nothing_here(
                ui,
                panels.shell,
                "The engine has not declared its widgets yet.",
                "The palette and the canvas are the engine's own: attach a runtime \
                 (`just run-editor-live`) and the toolkit's vocabulary arrives.",
            );
            return None;
        };
        let documents = project_files(panels, ".cyui");
        let themes = project_files(panels, ".cyss");
        let reference = chosen_document(panels, ui, &documents)?;
        let source = panels.editor.project.read_source(&reference).ok()?;
        let document = match UiDocument::decode(&source) {
            Ok(document) => document,
            Err(problem) => {
                nothing_here(
                    ui,
                    panels.shell,
                    &format!("{reference} does not read as a UI document."),
                    &problem.to_string(),
                );
                return None;
            }
        };
        let settings = panels.editor.backend.ui_layout.settings().clone();
        ask_for_preview(panels, &reference, &settings);
        let view = canvas_view(panels, ui.ctx(), &reference, &document);
        Some(Target {
            reference,
            document,
            catalogue,
            settings,
            view,
            documents,
            themes,
        })
    }

    fn diagnostics(inputs: &Inputs) -> Vec<ToolDiagnostic> {
        inputs
            .ui_layout
            .problem
            .iter()
            .map(|problem| ToolDiagnostic::error(problem.clone()))
            .collect()
    }

    fn body(frame: &mut ToolFrame<'_>, _session: Session<'_>, target: Target, ui: &mut egui::Ui) {
        toolbar(frame, ui, &target);
        ui.separator();
        let height = ui.available_height();
        ui.horizontal_top(|ui| {
            let side = (ui.available_width() * 0.22).clamp(150.0, 260.0);
            // Each column top-down: inside `horizontal_top` a plain allocation would lay its
            // widgets out in a row.
            let column = egui::Layout::top_down(egui::Align::Min);
            ui.allocate_ui_with_layout(egui::vec2(side, height), column, |ui| {
                egui::ScrollArea::vertical()
                    .id_salt("ui-layout-left")
                    .show(ui, |ui| {
                        palette(frame, ui, &target);
                        hierarchy(frame, ui, &target);
                    });
            });
            let canvas_width = (ui.available_width() - side - 8.0).max(160.0);
            ui.allocate_ui_with_layout(egui::vec2(canvas_width, height), column, |ui| {
                canvas(frame, ui, &target);
            });
            ui.allocate_ui_with_layout(egui::vec2(ui.available_width(), height), column, |ui| {
                egui::ScrollArea::vertical()
                    .id_salt("ui-layout-inspector")
                    .show(ui, |ui| inspector(frame, ui, &target));
            });
        });
    }
}

fn project_files(panels: &Panels<'_>, extension: &str) -> Vec<String> {
    panels.editor.project.paths_with_extension(extension)
}

/// The document on the canvas: the one chosen, else the first the project has. Draws the empty
/// state, with a way to make one, when there is none.
fn chosen_document(
    panels: &mut Panels<'_>,
    ui: &mut egui::Ui,
    documents: &[String],
) -> Option<String> {
    let inputs = &mut panels.inputs.ui_layout;
    let chosen = inputs
        .reference
        .clone()
        .filter(|reference| documents.contains(reference))
        .or_else(|| documents.first().cloned());
    if chosen.is_none() {
        nothing_here(
            ui,
            panels.shell,
            "This project has no UI document yet.",
            "Name one below; it starts with a full-screen root panel.",
        );
        if inputs.new_reference.is_empty() {
            inputs.new_reference = "ui/main_menu.cyui".into();
        }
        ui.horizontal(|ui| {
            ui.text_edit_singleline(&mut inputs.new_reference);
            if ui.button("Create").clicked() {
                panels.intents.push(Intent::Invoke(
                    "ui.document.create".into(),
                    Arguments::new().with("reference", Value::Text(inputs.new_reference.clone())),
                ));
                inputs.reference = Some(inputs.new_reference.clone());
            }
        });
    }
    inputs.reference.clone_from(&chosen);
    chosen
}

/// Ask once per document and settings: after that the editor asks again on every save.
fn ask_for_preview(panels: &mut Panels<'_>, reference: &str, settings: &PreviewSettings) {
    let key = (reference.to_owned(), format!("{settings:?}"));
    let shown = panels.editor.backend.ui_layout.reference() == Some(reference);
    if (shown && panels.inputs.ui_layout.asked.as_ref() == Some(&key))
        || !panels.editor.runtime.is_connected()
    {
        return;
    }
    panels.inputs.ui_layout.asked = Some(key);
    panels.intents.push(Intent::Invoke(
        "ui.preview".into(),
        Arguments::new().with("reference", Value::Text(reference.to_owned())),
    ));
}

fn canvas_view(
    panels: &mut Panels<'_>,
    ctx: &egui::Context,
    reference: &str,
    document: &UiDocument,
) -> Option<CanvasView> {
    let requests = &panels.editor.backend.ui_layout;
    let (previewed, preview) = requests.last_preview()?;
    if previewed != reference {
        return None;
    }
    let revision = requests.preview_revision();
    let texture = match &panels.inputs.ui_layout.texture {
        Some((cached, texture)) if *cached == revision => texture.clone(),
        _ => {
            let image = egui::ColorImage::from_rgba_unmultiplied(
                [preview.width as usize, preview.height as usize],
                &preview.pixels,
            );
            let texture = ctx.load_texture("ui-layout-canvas", image, egui::TextureOptions::LINEAR);
            panels.inputs.ui_layout.texture = Some((revision, texture.clone()));
            texture
        }
    };
    let rects = (0..document.widget_count())
        .map(|index| {
            document
                .line_of(index)
                .and_then(|line| preview.rect_of_line(line))
        })
        .collect();
    Some(CanvasView {
        texture,
        size: [preview.width, preview.height],
        per_unit: preview.pixels_per_unit(),
        rects,
        classes: preview.classes.clone(),
        diagnostics: preview
            .diagnostics
            .iter()
            .map(|d| format!("line {}: {} {}", d.line, d.message, d.detail))
            .collect(),
        stats: format!(
            "{} widgets · {} primitives · {} draws · laid out in {:.2} ms, drawn in {:.2} ms",
            preview.widgets, preview.primitives, preview.draws, preview.layout_ms, preview.draw_ms
        ),
    })
}

fn invoke(frame: &mut ToolFrame<'_>, command: &str, arguments: Arguments) {
    frame
        .intents
        .push(Intent::Invoke(command.to_owned(), arguments));
}

fn text(value: &str) -> Value {
    Value::Text(value.to_owned())
}

fn on(target: &Target) -> Arguments {
    Arguments::new().with("reference", text(&target.reference))
}

fn preview(frame: &mut ToolFrame<'_>, target: &Target, name: &str, value: Value) {
    invoke(frame, "ui.preview", on(target).with(name, value));
}

// --- The toolbar ---------------------------------------------------------------------------------

fn toolbar(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    ui.horizontal_wrapped(|ui| {
        document_choice(frame, ui, target);
        resolution_choice(frame, ui, target);
        dpi_choice(frame, ui, target);
        scale_choice(frame, ui, target);
        theme_choice(frame, ui, target);
        state_choice(frame, ui, target);
    });
}

fn document_choice(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let mut chosen = target.reference.clone();
    egui::ComboBox::from_id_salt("ui-layout-document")
        .selected_text(&target.reference)
        .show_ui(ui, |ui| {
            for document in &target.documents {
                ui.selectable_value(&mut chosen, document.clone(), document);
            }
        });
    if chosen != target.reference {
        frame.inputs.ui_layout.reference = Some(chosen);
        frame.inputs.ui_layout.selected = None;
    }
}

fn resolution_choice(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let current = (target.settings.width, target.settings.height);
    let label = RESOLUTIONS
        .iter()
        .find(|(_, width, height)| (*width, *height) == current)
        .map_or_else(
            || format!("{}x{}", current.0, current.1),
            |(name, ..)| (*name).to_owned(),
        );
    let mut chosen = current;
    egui::ComboBox::from_id_salt("ui-layout-resolution")
        .selected_text(label)
        .show_ui(ui, |ui| {
            for (name, width, height) in RESOLUTIONS {
                ui.selectable_value(&mut chosen, (*width, *height), *name);
            }
        });
    if chosen != current {
        #[allow(clippy::cast_precision_loss, reason = "a resolution is at most 8192")]
        preview(
            frame,
            target,
            "size",
            Value::Vec2([chosen.0 as f32, chosen.1 as f32]),
        );
    }
}

fn dpi_choice(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let mut chosen = target.settings.dpi;
    egui::ComboBox::from_id_salt("ui-layout-dpi")
        .selected_text(format!("DPI {chosen}x"))
        .show_ui(ui, |ui| {
            for scale in DPI_SCALES {
                ui.selectable_value(&mut chosen, *scale, format!("{scale}x"));
            }
        });
    if chosen.total_cmp(&target.settings.dpi).is_ne() {
        preview(frame, target, "dpi", Value::Float(chosen));
    }
}

fn scale_choice(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let mut chosen = target.settings.scale_mode;
    egui::ComboBox::from_id_salt("ui-layout-scale")
        .selected_text(format!("Scale: {}", chosen.name()))
        .show_ui(ui, |ui| {
            for mode in ScaleMode::ALL {
                ui.selectable_value(&mut chosen, mode, mode.name());
            }
        });
    if chosen != target.settings.scale_mode {
        preview(frame, target, "scale_mode", text(chosen.name()));
    }
}

fn theme_choice(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let mut chosen = target.settings.theme.clone();
    let shown = if chosen.is_empty() {
        "Theme: built-in".to_owned()
    } else {
        format!("Theme: {chosen}")
    };
    egui::ComboBox::from_id_salt("ui-layout-theme")
        .selected_text(shown)
        .show_ui(ui, |ui| {
            ui.selectable_value(&mut chosen, String::new(), "built-in");
            for theme in &target.themes {
                ui.selectable_value(&mut chosen, theme.clone(), theme);
            }
        });
    if chosen != target.settings.theme {
        preview(frame, target, "theme", Value::Text(chosen));
    }
}

fn state_choice(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let selected = frame.inputs.ui_layout.selected;
    let current = if target.settings.state_widget == selected {
        target.settings.state
    } else {
        PreviewState::None
    };
    let mut chosen = current;
    ui.add_enabled_ui(selected.is_some(), |ui| {
        egui::ComboBox::from_id_salt("ui-layout-state")
            .selected_text(format!("State: {}", current.name()))
            .show_ui(ui, |ui| {
                for state in PreviewState::ALL {
                    ui.selectable_value(&mut chosen, state, state.name());
                }
            });
    });
    if chosen != current
        && let Some(widget) = selected
    {
        invoke(
            frame,
            "ui.preview",
            on(target)
                .with("state", text(chosen.name()))
                .with("state_widget", text(&widget.to_string())),
        );
    }
}

// --- Palette and hierarchy -----------------------------------------------------------------------

/// The payload a palette entry carries when dragged onto the canvas or a hierarchy row.
#[derive(Clone, Debug)]
struct NewWidget(String);

/// The payload a hierarchy row carries when dragged onto another.
#[derive(Clone, Copy, Debug)]
struct MovedWidget(usize);

fn palette(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    heading(ui, frame.shell, "Palette");
    ui.label(secondary(
        frame.shell,
        "Click to add into the selection, or drag onto the canvas or a row.",
    ));
    // Rows of a few kinds: a kind's name is one word on its button, never broken across lines,
    // and a column as narrow as the side panel has no room for a wrapped row to measure.
    for row in target.catalogue.kinds.chunks(PALETTE_COLUMNS) {
        ui.horizontal(|ui| {
            ui.style_mut().wrap_mode = Some(egui::TextWrapMode::Extend);
            for kind in row {
                palette_entry(frame, ui, target, &kind.name);
            }
        });
    }
}

/// Kinds a palette row holds.
const PALETTE_COLUMNS: usize = 3;

fn palette_entry(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target, kind: &str) {
    let id = egui::Id::new(("ui-palette", kind));
    let response = ui
        .dnd_drag_source(id, NewWidget(kind.to_owned()), |ui| {
            ui.add(egui::Button::new(kind).sense(egui::Sense::click()))
        })
        .inner;
    if response.clicked() {
        let parent = container_for(target, frame.inputs.ui_layout.selected);
        add_widget(frame, target, kind, parent, None);
    }
}

/// The container a new widget goes into: the selection when it holds children, else its parent.
fn container_for(target: &Target, selected: Option<usize>) -> Option<usize> {
    let widgets = target.document.widgets();
    let mut at = selected;
    while let Some(index) = at {
        let kind = &widgets[index].kind;
        if target.catalogue.kind(kind).is_some_and(|k| k.container) {
            return Some(index);
        }
        at = widgets[index].parent;
    }
    None
}

fn add_widget(
    frame: &mut ToolFrame<'_>,
    target: &Target,
    kind: &str,
    parent: Option<usize>,
    offset: Option<[f32; 4]>,
) {
    let parent_text = parent.map_or_else(String::new, |index| index.to_string());
    let mut arguments = on(target)
        .with("kind", text(kind))
        .with("parent", text(&parent_text));
    if let Some(caption) = caption_for(kind) {
        arguments = arguments.with("text", text(&caption));
    }
    invoke(frame, "ui.widget.add", arguments);
    if let Some(offset) = offset {
        // The new widget is the parent's last child; its index is known once it is saved, so the
        // placement is a second command on the same widget, named by where it lands.
        let index = landing_index(&target.document, parent);
        invoke(
            frame,
            "ui.widget.layout",
            on(target)
                .with("widget", text(&index.to_string()))
                .with("anchor", Value::Vec4([0.0, 0.0, 0.0, 0.0]))
                .with("offset", Value::Vec4(offset)),
        );
    }
}

/// The index a widget added last under `parent` will have.
fn landing_index(document: &UiDocument, parent: Option<usize>) -> usize {
    let Some(parent) = parent else {
        return document.widget_count();
    };
    let widgets = document.widgets();
    let mut end = parent + 1;
    while end < widgets.len() && is_under(&widgets, end, parent) {
        end += 1;
    }
    end
}

fn is_under(widgets: &[WidgetSummary], mut index: usize, ancestor: usize) -> bool {
    while let Some(parent) = widgets[index].parent {
        if parent == ancestor {
            return true;
        }
        index = parent;
    }
    false
}

/// What a new captioned widget says until the author changes it: its kind, capitalised.
fn caption_for(kind: &str) -> Option<String> {
    matches!(kind, "label" | "button" | "toggle" | "checkbox").then(|| {
        let mut characters = kind.chars();
        characters.next().map_or_else(String::new, |first| {
            first.to_uppercase().chain(characters).collect()
        })
    })
}

fn hierarchy(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    heading(ui, frame.shell, "Hierarchy");
    for summary in target.document.widgets() {
        hierarchy_row(frame, ui, target, &summary);
    }
}

fn hierarchy_row(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    target: &Target,
    summary: &WidgetSummary,
) {
    let line = target.document.widget(summary.index).expect("listed");
    let name = row_name(summary, line);
    let selected = frame.inputs.ui_layout.selected == Some(summary.index);
    let id = egui::Id::new(("ui-hierarchy", summary.index));
    let response = ui
        .horizontal(|ui| {
            #[allow(clippy::cast_precision_loss, reason = "a depth is small")]
            ui.add_space(12.0 * summary.depth as f32);
            ui.dnd_drag_source(id, MovedWidget(summary.index), |ui| {
                ui.selectable_label(selected, name)
            })
            .inner
        })
        .inner;
    if response.clicked() {
        frame.inputs.ui_layout.selected = Some(summary.index);
    }
    let container = target
        .catalogue
        .kind(&summary.kind)
        .is_some_and(|kind| kind.container);
    if let Some(moved) = response.dnd_release_payload::<MovedWidget>() {
        let (parent, position) = drop_place(target, summary, container);
        if moved.0 != summary.index {
            invoke(
                frame,
                "ui.widget.move",
                on(target)
                    .with("widget", text(&moved.0.to_string()))
                    .with(
                        "parent",
                        text(&parent.map_or_else(String::new, |p| p.to_string())),
                    )
                    .with(
                        "position",
                        Value::Int(position.map_or(-1, |p| i64::try_from(p).unwrap_or(-1))),
                    ),
            );
        }
    }
    if let Some(new) = response.dnd_release_payload::<NewWidget>() {
        let (parent, _) = drop_place(target, summary, container);
        add_widget(frame, target, &new.0, parent, None);
    }
}

/// Dropping on a container puts it inside, last; on anything else, just before it.
fn drop_place(
    target: &Target,
    summary: &WidgetSummary,
    container: bool,
) -> (Option<usize>, Option<usize>) {
    if container {
        return (Some(summary.index), None);
    }
    let siblings = target
        .document
        .widgets()
        .into_iter()
        .filter(|other| other.parent == summary.parent)
        .position(|other| other.index == summary.index);
    (summary.parent, siblings)
}

fn row_name(summary: &WidgetSummary, line: &UiLine) -> String {
    let mut name = summary.kind.clone();
    if !line.id.is_empty() {
        name.push_str(" #");
        name.push_str(&line.id);
    }
    if let Some(UiValue::Text(caption)) = line.property("text") {
        let _ = write!(name, " \"{caption}\"");
    }
    if let LineKind::Use(path) = &line.kind {
        let _ = write!(name, " {}", path.as_text());
    }
    name
}

// --- The canvas ----------------------------------------------------------------------------------

/// Where the engine's image is drawn, and how a point on it maps to reference units.
struct Placement {
    rect: egui::Rect,
    /// Screen points per reference unit.
    points_per_unit: f32,
}

impl Placement {
    fn to_screen(&self, rect: [f32; 4]) -> egui::Rect {
        let [x, y, width, height] = rect.map(|value| value * self.points_per_unit);
        egui::Rect::from_min_size(self.rect.min + egui::vec2(x, y), egui::vec2(width, height))
    }

    fn to_units(&self, point: egui::Pos2) -> [f32; 2] {
        let local = point - self.rect.min;
        [
            local.x / self.points_per_unit,
            local.y / self.points_per_unit,
        ]
    }
}

fn canvas(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    let Some(view) = &target.view else {
        ui.label(secondary(
            frame.shell,
            "Waiting for the engine to draw the document…",
        ));
        return;
    };
    #[allow(
        clippy::cast_precision_loss,
        reason = "an image is at most 8192 a side"
    )]
    let image = egui::vec2(view.size[0] as f32, view.size[1] as f32);
    let room = egui::vec2(
        ui.available_width(),
        (ui.available_height() - 40.0).max(80.0),
    );
    let fit = (room.x / image.x).min(room.y / image.y);
    let (rect, response) = ui.allocate_exact_size(image * fit, egui::Sense::click_and_drag());
    ui.painter().image(
        view.texture.id(),
        rect,
        egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
        egui::Color32::WHITE,
    );
    let placement = Placement {
        rect,
        points_per_unit: view.per_unit * fit,
    };
    canvas_gestures(frame, ui, target, view, &placement, &response);
    draw_selection(frame, ui, target, view, &placement);
    for diagnostic in &view.diagnostics {
        super::status(ui, frame.shell, Semantic::Error, diagnostic);
    }
    ui.label(secondary(frame.shell, &view.stats));
}

/// The smallest widget rectangle under a point: the innermost, which is what a click means.
fn widget_at(view: &CanvasView, units: [f32; 2]) -> Option<usize> {
    view.rects
        .iter()
        .enumerate()
        .filter_map(|(index, rect)| rect.map(|rect| (index, rect)))
        .filter(|(_, [x, y, width, height])| {
            units[0] >= *x && units[0] < x + width && units[1] >= *y && units[1] < y + height
        })
        .min_by(|(_, a), (_, b)| (a[2] * a[3]).total_cmp(&(b[2] * b[3])))
        .map(|(index, _)| index)
}

fn canvas_gestures(
    frame: &mut ToolFrame<'_>,
    ui: &egui::Ui,
    target: &Target,
    view: &CanvasView,
    placement: &Placement,
    response: &egui::Response,
) {
    let pointer = response.interact_pointer_pos().or(response.hover_pos());
    if let Some(new) = response.dnd_release_payload::<NewWidget>()
        && let Some(point) = pointer
    {
        let units = placement.to_units(point);
        let parent = container_for(target, widget_at(view, units));
        let origin = parent
            .and_then(|parent| view.rects[parent])
            .map_or([0.0, 0.0], |[x, y, ..]| [x, y]);
        let at = [units[0] - origin[0], units[1] - origin[1]];
        add_widget(
            frame,
            target,
            &new.0,
            parent,
            Some([at[0], at[1], at[0] + 120.0, at[1] + 28.0]),
        );
        return;
    }
    if response.drag_started()
        && let Some(point) = pointer
    {
        let units = placement.to_units(point);
        frame.inputs.ui_layout.drag = handle_at(frame, target, view, placement, point)
            .or_else(|| {
                widget_at(view, units).map(|widget| {
                    frame.inputs.ui_layout.selected = Some(widget);
                    (widget, Handle::Body)
                })
            })
            .map(|(widget, handle)| CanvasDrag {
                widget,
                handle,
                delta: [0.0, 0.0],
            });
    }
    if response.dragged()
        && let Some(drag) = &mut frame.inputs.ui_layout.drag
    {
        let moved = response.drag_delta() / placement.points_per_unit;
        drag.delta[0] += moved.x;
        drag.delta[1] += moved.y;
    }
    if response.drag_stopped()
        && let Some(drag) = frame.inputs.ui_layout.drag.take()
    {
        commit_drag(
            frame,
            target,
            view,
            drag,
            ui.input(|input| input.modifiers.shift),
        );
    }
    if response.clicked()
        && let Some(point) = pointer
    {
        frame.inputs.ui_layout.selected = widget_at(view, placement.to_units(point));
    }
}

/// The selection's handle under a point: its resize corner or one of its anchors.
fn handle_at(
    frame: &ToolFrame<'_>,
    target: &Target,
    view: &CanvasView,
    placement: &Placement,
    point: egui::Pos2,
) -> Option<(usize, Handle)> {
    let selected = frame.inputs.ui_layout.selected?;
    let rect = placement.to_screen(view.rects.get(selected).copied().flatten()?);
    let near = |at: egui::Pos2| at.distance(point) <= 7.0;
    if near(rect.right_bottom()) {
        return Some((selected, Handle::Resize));
    }
    let (min, max) = anchor_points(target, view, placement, selected)?;
    if near(min) {
        return Some((selected, Handle::AnchorMin));
    }
    near(max).then_some((selected, Handle::AnchorMax))
}

/// Where a widget's anchors sit on screen: fractions of its parent's rectangle.
fn anchor_points(
    target: &Target,
    view: &CanvasView,
    placement: &Placement,
    widget: usize,
) -> Option<(egui::Pos2, egui::Pos2)> {
    let line = target.document.widget(widget)?;
    let [min_x, min_y, max_x, max_y] = lanes(&numbers(line, "anchor")?);
    let parent =
        parent_rect(target, view, widget).map_or(placement.rect, |rect| placement.to_screen(rect));
    Some((
        parent.lerp_inside(egui::vec2(min_x, min_y)),
        parent.lerp_inside(egui::vec2(max_x, max_y)),
    ))
}

/// One command for one drag: offsets for a move or a resize, anchors for an anchor handle. Shift
/// snaps to whole units of eight, and an anchor to quarters.
fn commit_drag(
    frame: &mut ToolFrame<'_>,
    target: &Target,
    view: &CanvasView,
    drag: CanvasDrag,
    snap: bool,
) {
    let Some(line) = target.document.widget(drag.widget) else {
        return;
    };
    let rounded = |value: f32| {
        if snap {
            (value / 8.0).round() * 8.0
        } else {
            value.round()
        }
    };
    let delta = drag.delta.map(rounded);
    if delta == [0.0, 0.0] {
        return;
    }
    let anchor = numbers(line, "anchor").unwrap_or_else(|| vec![0.0, 0.0, 0.0, 0.0]);
    let offset = numbers(line, "offset").unwrap_or_else(|| {
        // A widget with no offsets yet takes the rectangle the engine laid it out at, relative to
        // its parent, so a first drag starts from where it is drawn.
        laid_out_offset(target, view, drag.widget)
    });
    let arguments = on(target).with("widget", text(&drag.widget.to_string()));
    let arguments = match drag.handle {
        Handle::Body => arguments.with("anchor", Value::Vec4(lanes(&anchor))).with(
            "offset",
            Value::Vec4([
                offset[0] + delta[0],
                offset[1] + delta[1],
                offset[2] + delta[0],
                offset[3] + delta[1],
            ]),
        ),
        Handle::Resize => arguments.with("anchor", Value::Vec4(lanes(&anchor))).with(
            "offset",
            Value::Vec4([
                offset[0],
                offset[1],
                offset[2] + delta[0],
                offset[3] + delta[1],
            ]),
        ),
        Handle::AnchorMin | Handle::AnchorMax => {
            let parent = parent_size(target, view, drag.widget);
            let quarter = |value: f32| {
                let value = value.clamp(0.0, 1.0);
                if snap {
                    (value * 4.0).round() / 4.0
                } else {
                    value
                }
            };
            let mut next = lanes(&anchor);
            let column = if drag.handle == Handle::AnchorMin {
                0
            } else {
                2
            };
            next[column] = quarter(next[column] + drag.delta[0] / parent[0]);
            next[column + 1] = quarter(next[column + 1] + drag.delta[1] / parent[1]);
            arguments.with("anchor", Value::Vec4(next))
        }
    };
    invoke(frame, "ui.widget.layout", arguments);
}

fn numbers(line: &UiLine, key: &str) -> Option<Vec<f32>> {
    line.property(key)
        .and_then(UiValue::as_numbers)
        .filter(|values| values.len() == 4)
}

fn lanes(values: &[f32]) -> [f32; 4] {
    [values[0], values[1], values[2], values[3]]
}

#[expect(
    clippy::cast_precision_loss,
    reason = "a preview is a few thousand pixels across"
)]
fn pixels(value: u32) -> f32 {
    value as f32
}

fn parent_rect(target: &Target, view: &CanvasView, widget: usize) -> Option<[f32; 4]> {
    let parent = target.document.widgets()[widget].parent?;
    view.rects.get(parent).copied().flatten()
}

fn parent_size(target: &Target, view: &CanvasView, widget: usize) -> [f32; 2] {
    parent_rect(target, view, widget).map_or_else(
        || {
            [
                pixels(view.size[0]) / view.per_unit,
                pixels(view.size[1]) / view.per_unit,
            ]
        },
        |[_, _, width, height]| [width.max(1.0), height.max(1.0)],
    )
}

fn laid_out_offset(target: &Target, view: &CanvasView, widget: usize) -> Vec<f32> {
    let Some([x, y, width, height]) = view.rects.get(widget).copied().flatten() else {
        return vec![0.0; 4];
    };
    let [px, py] = parent_rect(target, view, widget).map_or([0.0, 0.0], |[px, py, ..]| [px, py]);
    vec![x - px, y - py, x - px + width, y - py + height]
}

fn draw_selection(
    frame: &ToolFrame<'_>,
    ui: &egui::Ui,
    target: &Target,
    view: &CanvasView,
    placement: &Placement,
) {
    let Some(selected) = frame.inputs.ui_layout.selected else {
        return;
    };
    let Some(rect) = view.rects.get(selected).copied().flatten() else {
        return;
    };
    let gold = theme::role(frame.shell.theme, Semantic::Selection);
    let mut screen = placement.to_screen(rect);
    if let Some(drag) = frame
        .inputs
        .ui_layout
        .drag
        .filter(|drag| drag.widget == selected)
    {
        let delta = egui::vec2(drag.delta[0], drag.delta[1]) * placement.points_per_unit;
        match drag.handle {
            Handle::Body => screen = screen.translate(delta),
            Handle::Resize => screen.max += delta,
            Handle::AnchorMin | Handle::AnchorMax => {}
        }
    }
    let painter = ui.painter_at(placement.rect);
    painter.rect_stroke(
        screen,
        egui::CornerRadius::ZERO,
        egui::Stroke::new(1.5, gold),
        egui::StrokeKind::Outside,
    );
    painter.rect_filled(
        egui::Rect::from_center_size(screen.right_bottom(), egui::vec2(8.0, 8.0)),
        egui::CornerRadius::same(1),
        gold,
    );
    // The anchors: two triangles in the parent's rectangle.
    if let Some((min, max)) = anchor_points(target, view, placement, selected) {
        for point in [min, max] {
            painter.add(egui::Shape::convex_polygon(
                vec![
                    point,
                    point + egui::vec2(-5.0, -9.0),
                    point + egui::vec2(5.0, -9.0),
                ],
                gold,
                egui::Stroke::NONE,
            ));
        }
    }
}

// --- The inspector -------------------------------------------------------------------------------

fn inspector(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    heading(ui, frame.shell, "Inspector");
    let Some(selected) = frame.inputs.ui_layout.selected else {
        ui.label(secondary(
            frame.shell,
            "Select a widget on the canvas or in the hierarchy.",
        ));
        sheets(frame, ui, target);
        return;
    };
    let Some(line) = target.document.widget(selected).cloned() else {
        frame.inputs.ui_layout.selected = None;
        return;
    };
    let widget = selected.to_string();
    ui.label(format!("{} · widget {widget}", line.kind_name()));
    identity(frame, ui, target, &widget, &line);
    if let Some(kind) = target.catalogue.kind(line.kind_name()) {
        heading(ui, frame.shell, "Properties");
        egui::Grid::new(("ui-layout-properties", selected))
            .num_columns(2)
            .show(ui, |ui| {
                for (key, kind) in &kind.properties {
                    if matches!(key.as_str(), "anchor" | "offset" | "bind" | "bind-mode") {
                        continue;
                    }
                    ui.label(key);
                    property(frame, ui, target, &widget, &line, key, *kind);
                    ui.end_row();
                }
            });
    }
    layout(frame, ui, target, &widget, &line);
    binding(frame, ui, target, &widget, &line);
    inline_styles(frame, ui, target, &widget, &line);
    ui.add_space(8.0);
    if ui.button("Remove widget").clicked() {
        invoke(
            frame,
            "ui.widget.remove",
            on(target).with("widget", text(&widget)),
        );
        frame.inputs.ui_layout.selected = None;
    }
}

/// A text field committed as one command when it loses focus or Enter is pressed.
fn committed_text(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    key: &str,
    current: &str,
) -> Option<String> {
    let fields = &mut frame.inputs.ui_layout.fields;
    let mut value = fields
        .get(key)
        .cloned()
        .unwrap_or_else(|| current.to_owned());
    let response = ui.add(egui::TextEdit::singleline(&mut value).desired_width(150.0));
    if response.changed() {
        fields.insert(key.to_owned(), value.clone());
    }
    if response.lost_focus() {
        fields.remove(key);
        return (value != current).then_some(value);
    }
    None
}

fn identity(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    target: &Target,
    widget: &str,
    line: &UiLine,
) {
    egui::Grid::new(("ui-layout-identity", widget))
        .num_columns(2)
        .show(ui, |ui| {
            ui.label("id");
            let id = committed_text(frame, ui, &format!("{widget}:id"), &line.id);
            ui.end_row();
            ui.label("classes");
            let classes = committed_text(
                frame,
                ui,
                &format!("{widget}:classes"),
                &line.classes.join(" "),
            );
            ui.end_row();
            if id.is_some() || classes.is_some() {
                invoke(
                    frame,
                    "ui.widget.identity",
                    on(target)
                        .with("widget", text(widget))
                        .with("id", Value::Text(id.unwrap_or_else(|| line.id.clone())))
                        .with(
                            "classes",
                            Value::Text(classes.unwrap_or_else(|| line.classes.join(" "))),
                        ),
                );
            }
        });
    // The classes the theme and the sheets select, one click each.
    if let Some(view) = &target.view {
        ui.horizontal_wrapped(|ui| {
            for class in view
                .classes
                .iter()
                .filter(|class| !line.classes.contains(class))
                .take(16)
            {
                if ui.small_button(format!(".{class}")).clicked() && line.classes.len() < 4 {
                    let mut classes = line.classes.clone();
                    classes.push(class.clone());
                    invoke(
                        frame,
                        "ui.widget.identity",
                        on(target)
                            .with("widget", text(widget))
                            .with("id", text(&line.id))
                            .with("classes", Value::Text(classes.join(" "))),
                    );
                }
            }
        });
    }
}

fn set(frame: &mut ToolFrame<'_>, target: &Target, widget: &str, key: &str, value: &str) {
    invoke(
        frame,
        "ui.widget.set",
        on(target)
            .with("widget", text(widget))
            .with("key", text(key))
            .with("value", text(value)),
    );
}

fn property(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    target: &Target,
    widget: &str,
    line: &UiLine,
    key: &str,
    kind: PropertyType,
) {
    let current = line.property(key);
    match kind {
        PropertyType::Bool => {
            let was = match current {
                Some(UiValue::Word(word)) => word == "true",
                // Unwritten: what the toolkit makes the widget with.
                _ => bool_default(key),
            };
            let mut now = was;
            if ui.checkbox(&mut now, "").changed() {
                set(
                    frame,
                    target,
                    widget,
                    key,
                    if now { "true" } else { "false" },
                );
            }
        }
        PropertyType::Text => {
            let shown = current.map(UiValue::as_text).unwrap_or_default();
            if let Some(value) = committed_text(frame, ui, &format!("{widget}:{key}"), &shown) {
                let quoted = if value.is_empty() {
                    String::new()
                } else {
                    UiValue::Text(value).spelled()
                };
                set(frame, target, widget, key, &quoted);
            }
        }
        _ => {
            let shown = current.map(UiValue::spelled).unwrap_or_default();
            if let Some(value) = committed_text(frame, ui, &format!("{widget}:{key}"), &shown) {
                set(frame, target, widget, key, &value);
            }
        }
    }
}

/// A boolean property's value when the document does not write it: a widget is made enabled and
/// visible, and every other switch off.
fn bool_default(key: &str) -> bool {
    matches!(key, "enabled" | "visible")
}

fn layout(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    target: &Target,
    widget: &str,
    line: &UiLine,
) {
    heading(ui, frame.shell, "Layout");
    egui::Grid::new(("ui-layout-layout", widget))
        .num_columns(2)
        .show(ui, |ui| {
            for key in [
                "anchor",
                "offset",
                "width",
                "height",
                "margin-left",
                "margin-top",
                "gap",
            ] {
                ui.label(key);
                let shown = line.property(key).map(UiValue::spelled).unwrap_or_default();
                if let Some(value) = committed_text(frame, ui, &format!("{widget}:{key}"), &shown) {
                    set(frame, target, widget, key, &value);
                }
                ui.end_row();
            }
        });
    if ui.small_button("Fill parent").clicked() {
        invoke(
            frame,
            "ui.widget.layout",
            on(target)
                .with("widget", text(widget))
                .with("anchor", Value::Vec4([0.0, 0.0, 1.0, 1.0]))
                .with("offset", Value::Vec4([0.0, 0.0, 0.0, 0.0])),
        );
    }
}

fn binding(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    target: &Target,
    widget: &str,
    line: &UiLine,
) {
    heading(ui, frame.shell, "Binding");
    let name = line
        .property("bind")
        .map(UiValue::as_text)
        .unwrap_or_default();
    let mode = line
        .property("bind-mode")
        .map_or_else(|| "one-way".to_owned(), UiValue::as_text);
    ui.horizontal(|ui| {
        let typed = committed_text(frame, ui, &format!("{widget}:bind"), &name);
        let mut chosen = mode.clone();
        egui::ComboBox::from_id_salt(("ui-layout-bind-mode", widget))
            .selected_text(&chosen)
            .show_ui(ui, |ui| {
                for candidate in &target.catalogue.binding_modes {
                    ui.selectable_value(&mut chosen, candidate.clone(), candidate);
                }
            });
        if typed.is_some() || chosen != mode {
            invoke(
                frame,
                "ui.widget.bind",
                on(target)
                    .with("widget", text(widget))
                    .with("name", Value::Text(typed.unwrap_or(name)))
                    .with("mode", Value::Text(chosen)),
            );
        }
    });
}

fn inline_styles(
    frame: &mut ToolFrame<'_>,
    ui: &mut egui::Ui,
    target: &Target,
    widget: &str,
    line: &UiLine,
) {
    heading(ui, frame.shell, "Inline style");
    let styles: Vec<&String> = target
        .catalogue
        .style_properties
        .iter()
        .filter(|name| line.property(name).is_some())
        .collect();
    egui::Grid::new(("ui-layout-styles", widget))
        .num_columns(2)
        .show(ui, |ui| {
            for name in styles {
                ui.label(name);
                let shown = line
                    .property(name)
                    .map(UiValue::spelled)
                    .unwrap_or_default();
                if let Some(value) = committed_text(frame, ui, &format!("{widget}:{name}"), &shown)
                {
                    set(frame, target, widget, name, &value);
                }
                ui.end_row();
            }
        });
    let key = format!("{widget}:__new-style");
    let mut chosen = frame
        .inputs
        .ui_layout
        .fields
        .get(&key)
        .cloned()
        .unwrap_or_default();
    egui::ComboBox::from_id_salt(("ui-layout-new-style", widget))
        .selected_text(if chosen.is_empty() {
            "Add a style…"
        } else {
            &chosen
        })
        .show_ui(ui, |ui| {
            for name in &target.catalogue.style_properties {
                ui.selectable_value(&mut chosen, name.clone(), name);
            }
        });
    if !chosen.is_empty() {
        frame.inputs.ui_layout.fields.remove(&key);
        set(frame, target, widget, &chosen, "0");
    }
}

fn sheets(frame: &mut ToolFrame<'_>, ui: &mut egui::Ui, target: &Target) {
    heading(ui, frame.shell, "Style sheets");
    let named = target.document.stylesheets();
    for sheet in &target.themes {
        let mut on_document = named.contains(sheet);
        if ui.checkbox(&mut on_document, sheet).changed() {
            invoke(
                frame,
                "ui.document.stylesheet",
                on(target)
                    .with("sheet", text(sheet))
                    .with("named", Value::Bool(on_document)),
            );
        }
    }
}
