//! The docked panels, and the one place a tab becomes a body. Tasks 1.2, 1.3, 1.5.
//!
//! --- WHAT A PANEL IS ALLOWED TO DO ------------------------------------------------------------------
//!
//! Read a model and draw it; ask for a command by identifier. That is the whole contract, and it is
//! what makes `editor-rust-application`'s "One action, six entry points" true rather than
//! aspirational: a button in a panel and a line in a script reach `Registry::invoke` by the same
//! call, so a panel cannot acquire a mutation the palette, the keyboard, a script and an agent do
//! not also have.
//!
//! It follows that no panel in this module holds state that another panel needs, and none of them
//! names another. Where two panels have to agree — which is only ever about the selection — they
//! read the selection service, exactly as `editor-rust-application` requires and for the reason it
//! gives: a hierarchy that notified an inspector would be a hierarchy the inspector cannot be
//! tested without.
//!
//! --- SPATIAL STABILITY, WHICH IS THE EASIEST RULE TO BREAK BY BEING HELPFUL --------------------------
//!
//! "Context changes content, never position." Selecting a light changes what the inspector shows and
//! must not move it, open a panel, close one, or bring one forward. Nothing in this module calls
//! anything on [`crate::dock`] — the dock state is changed by the user and by explicit workspace
//! commands, and a panel has no way to reach it. That is enforced by what [`Panels`] borrows rather
//! than by care.

mod agents;
mod browser;
mod diagnostics;
mod hierarchy;
mod history;
mod inspector;
mod material_graph;
mod pending;
mod semantic_merge;
mod settings;
mod source;
mod source_control;
mod terrain;
mod viewport;

use cy_editor_commands::{Arguments, Registry, Scope};
use cy_editor_core::ids::DocumentId;
use cy_editor_interface::SpecialisedEditors;
use cy_editor_interface::panels::{PanelKey, PanelTitles};
use cy_editor_interface::shell::Shell;
use cy_editor_interface::thumbnails::Thumbnails;
use cy_editor_services::Editor;
use cy_editor_viewmodels::HierarchyViewModel;
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::{Metrics, TextRole};

use crate::theme;
use crate::viewport_link::ViewportLink;

/// Something a panel asked the window to do that a panel may not do itself.
///
/// Deliberately small. Every entry is either a command invocation — which goes through the registry
/// like every other caller — or a change to the *workspace*, which is the user's own arrangement and
/// therefore not something a panel decides. There is no variant that moves a panel.
#[derive(Clone, PartialEq, Debug)]
pub enum Intent {
    /// Invoke a registered command.
    Invoke(String, Arguments),
    /// Open a document by asset path.
    OpenAsset(String),
    /// Stage and import files selected outside the project.
    ImportExternal {
        /// Native paths supplied by the chooser or operating-system drop.
        paths: Vec<std::path::PathBuf>,
        /// Project-relative Content Browser folder receiving the files.
        destination: String,
    },
    /// Make an already-open document active.
    ActivateDocument(DocumentId),
    /// Begin closing an open document, asking for a decision when it is dirty.
    CloseDocument(DocumentId),
    /// Finish a dirty-document close after the user makes an explicit choice.
    ResolveDocumentClose(DocumentId, cy_editor_services::CloseDecision),
    /// Open a Swift source in an editor-owned buffer.
    OpenSource(String),
    /// Resolve an authored Swift behaviour and open its source in the workspace.
    OpenBehaviourSource(String),
    /// Save the active Swift buffer through the registered conflict-safe command.
    SaveSource,
    /// Open a diagnostic source and move to its exact zero-based line and UTF-16 column.
    NavigateSource {
        /// Project-relative source path.
        path: String,
        /// Zero-based line.
        line: u32,
        /// Zero-based UTF-16 column.
        column: u32,
    },
    /// Pause the connected desktop agent before its next invocation.
    PauseAgent,
    /// Resume a paused desktop agent.
    ResumeAgent,
    /// Permanently revoke the connected desktop agent.
    RevokeAgent,
    /// Resolve an irreversible or external request waiting at the desktop.
    DecideAgent {
        /// Correlated desktop queue ticket.
        ticket: u64,
        /// Whether the human accepted the request.
        allow: bool,
        /// Optional monotonic grant duration; absent means this request only.
        grant_millis: Option<u64>,
    },
}

/// Build the one intent used by both the chooser and operating-system file drop.
pub(crate) fn external_import_intent(
    paths: Vec<std::path::PathBuf>,
    destination: impl Into<String>,
) -> Intent {
    Intent::ImportExternal {
        paths,
        destination: destination.into(),
    }
}

/// The text a person has typed into a panel's own field.
///
/// Presentation state, and it lives beside the panels rather than in a view model for the reason
/// `editor-rust-application` gives about view models: none of this is derived from the document, and
/// an in-progress edit is explicitly not document state. A field's contents surviving a frame is the
/// whole of what this struct is for.
pub struct Inputs {
    /// The outliner's permanent search.
    pub hierarchy_filter: String,
    /// Inline hierarchy rename: node, text, and whether initial focus was requested.
    pub hierarchy_rename: Option<(cy_editor_core::ids::NodeId, String, bool)>,
    /// Stable identity currently being dragged for reparenting.
    pub hierarchy_drag: Option<cy_editor_core::ids::NodeId>,
    /// The content browser's permanent search.
    pub browser_filter: String,
    /// Asset type filter; empty means every kind.
    pub browser_kind: String,
    /// Whether the Content Browser import chooser is visible.
    pub browser_import_open: bool,
    /// Native paths entered in the import chooser, one per line.
    pub browser_import_paths: String,
    /// Search text for the engine-owned material node palette.
    pub material_filter: String,
    /// Output pin selected as the source of the next material connection.
    ///
    /// Node and pin identities are stored alongside readable metadata so a catalogue rename does
    /// not silently retarget the in-progress gesture.
    pub material_link_source: Option<(u64, u32, String, String)>,
    /// The latest refused material-canvas gesture, kept visible until the next successful edit.
    pub material_link_problem: Option<String>,
    /// The latest typed material-property refusal, shown beside the generated controls.
    pub material_property_problem: Option<String>,
    /// Name emitted when the opened graph is compiled.
    pub material_name: String,
    /// Active terrain sculpt or paint tool keyword.
    pub terrain_tool: String,
    /// Stable material layer receiving paint gestures.
    pub terrain_layer: Option<cy_editor_core::ids::NodeId>,
    /// New terrain layer's author-facing name.
    pub terrain_layer_name: String,
    /// New terrain layer's material asset reference.
    pub terrain_layer_material: String,
    /// Most recent painting-surface refusal.
    pub terrain_problem: Option<String>,
    /// The console's command line.
    pub console: String,
    /// The Settings panel's permanent search.
    pub settings_filter: String,
    /// The platform whose overrides are being inspected.
    pub settings_platform: String,
    /// In-progress textual setting values.
    pub settings_entries: std::collections::BTreeMap<String, String>,
    /// Validation failures beside their fields.
    pub settings_errors: std::collections::BTreeMap<String, String>,
    /// Rows where platform override editing is selected.
    pub settings_platform_overrides: std::collections::BTreeSet<String>,
    /// Change description entered in Source Control.
    pub source_control_description: String,
    /// Source-control base revision entered in Diff/Merge.
    pub merge_base_revision: String,
    /// Source-control incoming revision entered in Diff/Merge.
    pub merge_incoming_revision: String,
    /// Typed replacement literals keyed by conflict index.
    pub merge_replacements: std::collections::BTreeMap<usize, String>,
    /// The viewport's pointer and keyboard translation, which needs to know what happened last
    /// frame in order to report a movement rather than a position.
    pub viewport: crate::viewport_input::ViewportInput,
    /// The transform field a person is typing into, and what they have typed so far.
    ///
    /// One field at a time, because that is how many a pointer can be in. Held rather than read back
    /// from the document every frame so that a half-typed "2m + 3" is not replaced by the value it
    /// does not evaluate to yet — an in-progress edit is explicitly not document state.
    pub transform_entry: Option<(cy_editor_viewport::Row, usize, String)>,
    /// A document with nothing in it, for the frames where no world is open.
    ///
    /// The viewport's interaction model is given a document because a manipulation writes to one;
    /// with no world open there is nothing to manipulate, but the camera still moves — an editor
    /// whose viewport froze until a world was opened would be one a user cannot tell from a broken
    /// one. Held here rather than made per frame, because a document allocated sixty times a second
    /// is sixty allocations a second for nothing.
    pub no_world: Box<cy_editor_documents::Document>,
    /// The viewport's interaction state: what is hovered, what is being dragged, what is locked.
    ///
    /// Presentation state in the same sense as a half-typed field: it is not derived from a
    /// document, it survives a frame, and it belongs to *this* window. The decisions it makes are
    /// `cy_editor_viewport::interaction`'s, which is tested with no toolkit at all.
    pub interaction: Box<cy_editor_viewport::Interaction>,
}

impl Default for Inputs {
    fn default() -> Self {
        Self {
            hierarchy_filter: String::new(),
            hierarchy_rename: None,
            hierarchy_drag: None,
            browser_filter: String::new(),
            browser_kind: String::new(),
            browser_import_open: false,
            browser_import_paths: String::new(),
            material_filter: String::new(),
            material_link_source: None,
            material_link_problem: None,
            material_property_problem: None,
            material_name: "editor_preview".into(),
            terrain_tool: "raise".into(),
            terrain_layer: None,
            terrain_layer_name: String::new(),
            terrain_layer_material: String::new(),
            terrain_problem: None,
            console: String::new(),
            settings_filter: String::new(),
            settings_platform: if cfg!(target_os = "macos") {
                "macos".into()
            } else if cfg!(target_os = "windows") {
                "windows".into()
            } else {
                "linux".into()
            },
            settings_entries: std::collections::BTreeMap::new(),
            settings_errors: std::collections::BTreeMap::new(),
            settings_platform_overrides: std::collections::BTreeSet::new(),
            source_control_description: String::new(),
            merge_base_revision: String::new(),
            merge_incoming_revision: String::new(),
            merge_replacements: std::collections::BTreeMap::new(),
            viewport: crate::viewport_input::ViewportInput::new(),
            transform_entry: None,
            no_world: Box::new(cy_editor_documents::Document::new("worlds/none.cyworld")),
            interaction: Box::new(cy_editor_viewport::Interaction::new()),
        }
    }
}

/// Everything the panels are allowed to touch, for the length of one frame.
pub struct Panels<'frame> {
    /// The authoritative state.
    pub editor: &'frame mut Editor,
    /// Every action the editor can perform.
    pub registry: &'frame Registry,
    /// What this caller may do.
    pub scope: &'frame Scope,
    /// The interface's own models: theme, density, palette, inspector, problems, notifications.
    pub shell: &'frame mut Shell,
    /// The one shared specialised-editor surface host.
    pub specialised: &'frame mut SpecialisedEditors,
    /// The hierarchy's presentation state.
    pub hierarchy: &'frame mut HierarchyViewModel,
    /// Attributed history for the active document.
    pub history: &'frame mut cy_editor_viewmodels::HistoryViewModel,
    /// Search and effective values for Settings.
    pub settings: &'frame mut cy_editor_viewmodels::SettingsViewModel,
    /// Cached provider state and capability availability.
    pub source_control: &'frame mut cy_editor_viewmodels::SourceControlViewModel,
    /// Deterministic Content Browser navigation and filters.
    pub asset_browser: &'frame mut cy_editor_viewmodels::AssetBrowserViewModel,
    /// Swift file tree and open source buffers.
    pub source_workspace: &'frame mut cy_editor_viewmodels::SourceWorkspaceViewModel,
    /// Desktop MCP session, when one was requested for this window.
    pub agent: Option<&'frame mut cy_editor_agent::DesktopAgentHost>,
    /// Semantic comparison rows for the current source-control revision inputs.
    pub diff: &'frame mut cy_editor_viewmodels::DiffViewModel,
    /// Pending typed conflicts and decisions.
    pub merge: &'frame mut cy_editor_viewmodels::MergeViewModel,
    /// Asset previews, and typed placeholders until the engine has rendered one.
    pub thumbnails: &'frame mut Thumbnails,
    /// The viewport's link to the runtime's rendered image.
    pub link: &'frame mut ViewportLink,
    /// The titles a person reads, kept apart from the keys a layout stores.
    pub titles: &'frame PanelTitles,
    /// The text a person has typed into a panel's own field, which survives across frames.
    pub inputs: &'frame mut Inputs,
    /// Tab rectangles collected for selection underlines after docking interaction resolves.
    pub tab_rects: Vec<(PanelKey, egui::Rect, egui::LayerId)>,
    /// What the panels asked for, applied after the frame is drawn.
    pub intents: &'frame mut Vec<Intent>,
}

impl Panels<'_> {
    /// The metrics in force.
    fn metrics(&self) -> Metrics {
        self.shell.metrics()
    }
}

impl egui_dock::TabViewer for Panels<'_> {
    type Tab = PanelKey;

    fn id(&mut self, tab: &mut Self::Tab) -> egui::Id {
        // Derived from the stable key, never from the title. See `crate::dock`.
        egui::Id::new(("cy-panel", tab.to_string()))
    }

    fn title(&mut self, tab: &mut Self::Tab) -> egui::WidgetText {
        self.titles.title(tab).into()
    }

    fn on_tab_button(&mut self, tab: &mut Self::Tab, response: &egui::Response) {
        self.tab_rects
            .push((tab.clone(), response.rect, response.layer_id));
    }

    fn ui(&mut self, ui: &mut egui::Ui, tab: &mut Self::Tab) {
        let padding = self.metrics().padding();
        egui::Frame::NONE
            .inner_margin(egui::Margin::same(theme::margin(padding)))
            .show(ui, |ui| match tab.kind() {
                "hierarchy" => hierarchy::show(self, ui),
                "undo-history" => history::show(self, ui),
                "settings" => settings::show(self, ui),
                "source-control" => source_control::show(self, ui),
                "agent-sessions" => agents::show(self, ui),
                "swift-workspace" => source::show(self, ui),
                "semantic-diff" => semantic_merge::show_diff(self, ui),
                "semantic-merge" => semantic_merge::show_merge(self, ui),
                "inspector" => inspector::show(self, ui),
                "content-browser" => browser::show(self, ui),
                "editor-materials" => material_graph::show(self, ui),
                "editor-terrain" => terrain::show(self, ui),
                "console" => diagnostics::console(self, ui),
                "problems" => diagnostics::problems(self, ui),
                "profiler" => diagnostics::profiler(self, ui),
                "viewport" => viewport::show(self, ui),
                other => pending::show(self, ui, other),
            });
    }

    fn clear_background(&self, tab: &Self::Tab) -> bool {
        // The viewport paints its own sunken ground and then the engine's image over it. Letting the
        // dock manager fill it first would be one full-screen fill per frame for nothing.
        tab.kind() != "viewport"
    }

    fn scroll_bars(&self, tab: &Self::Tab) -> [bool; 2] {
        // Every panel scrolls itself, so that a virtualised list is virtualised rather than being
        // built in full inside a scrolling parent. The viewport does not scroll at all.
        let _ = tab;
        [false, false]
    }

    fn closeable(&mut self, tab: &mut Self::Tab) -> bool {
        // The viewport is the subject of the interface; an editor with it closed is an editor with
        // nothing in it, and the recovery — reset the workspace — is not obvious from the state it
        // leaves behind.
        tab.kind() != "viewport"
    }
}

// --- The shared drawing vocabulary ----------------------------------------------------------------
//
// Everything below is used by more than one panel. A helper that only one panel needs lives in that
// panel's file, because a shared helpers module is where a colour decision eventually hides.

/// A panel's section heading: subtle, per `editor-visual-language`.
pub(crate) fn heading(ui: &mut egui::Ui, shell: &Shell, text: &str) {
    let metrics = shell.metrics();
    ui.add_space(metrics.gap() * 0.5);
    ui.label(
        egui::RichText::new(text)
            .size(metrics.text(TextRole::Section))
            .color(theme::role(shell.theme, Semantic::SecondaryText)),
    );
}

/// Secondary text: the same size as body text, separated by weight and luminance.
pub(crate) fn secondary(shell: &Shell, text: impl Into<String>) -> egui::RichText {
    egui::RichText::new(text.into())
        .size(shell.metrics().text(TextRole::Secondary))
        .color(theme::role(shell.theme, Semantic::SecondaryText))
}

/// A number, in the tabular family. See [`crate::theme::NUMERIC`].
pub(crate) fn numeric(shell: &Shell, text: impl Into<String>) -> egui::RichText {
    egui::RichText::new(text.into())
        .family(egui::FontFamily::Monospace)
        .size(shell.metrics().text(TextRole::Body))
}

/// A status marker: glyph, word and colour together.
///
/// `editor-visual-language`: "Colour is never the sole encoding." The glyph and the label come from
/// `Semantic` itself, so a status drawn through this function is legible in a colour-blind-safe
/// palette and in a monochrome screenshot without anything else being remembered.
pub(crate) fn status(ui: &mut egui::Ui, shell: &Shell, role: Semantic, text: &str) {
    let colour = theme::role(shell.theme, role);
    ui.horizontal(|ui| {
        ui.label(egui::RichText::new(role.glyph().to_string()).color(colour));
        ui.label(egui::RichText::new(text).color(colour));
    });
}

/// The empty state of a panel: what is not here, and what would put something here.
///
/// A panel that is empty and says nothing is indistinguishable from one that is broken. Every empty
/// state in this crate goes through here, and every one names the action that would fill it.
pub(crate) fn nothing_here(ui: &mut egui::Ui, shell: &Shell, what: &str, remedy: &str) {
    ui.add_space(shell.metrics().gap());
    ui.add(
        egui::Label::new(
            egui::RichText::new(what)
                .size(shell.metrics().text(TextRole::Body))
                .color(theme::role(shell.theme, Semantic::PrimaryText)),
        )
        .sense(egui::Sense::focusable_noninteractive()),
    );
    ui.add_space(shell.metrics().gap() * 0.5);
    ui.add(
        egui::Label::new(secondary(shell, remedy)).sense(egui::Sense::focusable_noninteractive()),
    );
}

/// A row's background: resting, hovered, or selected.
///
/// The selected fill is gold at low opacity rather than a saturated block, because the panel is
/// charcoal and the viewport carries the screen's colour. See [`crate::theme::selected_fill`].
pub(crate) fn row_background(
    ui: &egui::Ui,
    shell: &Shell,
    rect: egui::Rect,
    selected: bool,
    hovered: bool,
) {
    let fill = if selected {
        theme::selected_fill(shell.theme)
    } else if hovered {
        theme::lifted(shell.theme, Surface::Panel, 0.06)
    } else {
        return;
    };
    ui.painter()
        .rect_filled(rect, egui::CornerRadius::same(2), fill);
}

/// A permanent search field, as the hierarchy and the content browser both carry.
///
/// "Search everywhere — find capability instead of memorising where it was put." Permanent rather
/// than behind a key, because a search field that has to be summoned is one a user has to remember
/// exists.
pub(crate) fn search_field(
    ui: &mut egui::Ui,
    shell: &Shell,
    hint: &str,
    text: &mut String,
) -> bool {
    let response = ui.add(
        egui::TextEdit::singleline(text)
            .margin(egui::vec2(
                shell.metrics().gap(),
                shell.metrics().gap() * 0.5,
            ))
            .background_color(theme::surface(shell.theme, Surface::Raised))
            .hint_text(secondary(shell, hint))
            .desired_width(f32::INFINITY),
    );
    response.changed()
}
