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

mod browser;
mod diagnostics;
mod hierarchy;
mod inspector;
mod pending;
mod viewport;

use cy_editor_commands::{Arguments, Registry, Scope};
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
    /// The content browser's permanent search.
    pub browser_filter: String,
    /// The console's command line.
    pub console: String,
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
            browser_filter: String::new(),
            console: String::new(),
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
    /// The hierarchy's presentation state.
    pub hierarchy: &'frame mut HierarchyViewModel,
    /// Asset previews, and typed placeholders until the engine has rendered one.
    pub thumbnails: &'frame mut Thumbnails,
    /// The viewport's link to the runtime's rendered image.
    pub link: &'frame mut ViewportLink,
    /// The titles a person reads, kept apart from the keys a layout stores.
    pub titles: &'frame PanelTitles,
    /// The text a person has typed into a panel's own field, which survives across frames.
    pub inputs: &'frame mut Inputs,
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

    fn ui(&mut self, ui: &mut egui::Ui, tab: &mut Self::Tab) {
        let padding = self.metrics().padding();
        egui::Frame::NONE
            .inner_margin(egui::Margin::same(theme::margin(padding)))
            .show(ui, |ui| match tab.kind() {
                "hierarchy" => hierarchy::show(self, ui),
                "inspector" => inspector::show(self, ui),
                "content-browser" => browser::show(self, ui),
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
    ui.label(secondary(shell, what));
    ui.add_space(shell.metrics().gap() * 0.5);
    ui.label(secondary(shell, remedy));
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
            .hint_text(secondary(shell, hint))
            .desired_width(f32::INFINITY),
    );
    response.changed()
}
