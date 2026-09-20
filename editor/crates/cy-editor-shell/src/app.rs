//! The window itself: one device, one frame loop, and the order everything happens in. Task 1.1.
//!
//! --- ONE DEVICE, NOT TWO -------------------------------------------------------------------------
//!
//! The editor draws its interface with wgpu and imports the runtime's `VkImage` into wgpu, and those
//! have to be the **same** device: an image imported into one device cannot be sampled by another,
//! and the failure is a driver error with no explanation in it. `egui_wgpu::WgpuSetup::Existing` is
//! the supported way to say so, and `cy_editor_viewport_transport::Gpu::create` is what makes the
//! device — because it is the one that pushes `VK_KHR_external_semaphore_fd` before `vkCreateDevice`,
//! which nothing can add afterwards.
//!
//! It is attempted first and falls back to eframe's own device, with the reason logged. The fallback
//! is not a feature toggle: it is what keeps a machine with no Vulkan, or a driver that refuses the
//! extension, from being a machine where the editor does not open. The viewport then says the
//! transport is not ready, which is true.
//!
//! --- THE ORDER OF A FRAME, WHICH IS NOT ARBITRARY ---------------------------------------------------
//!
//!   1. `Editor::pump` — drain the runtime, forget settled operations. Bounded, non-blocking.
//!   2. `ViewportLink::begin_frame` — claim the newest engine frame and release the last one. Before
//!      anything is drawn, and before the queue is submitted, which is the protocol's requirement
//!      rather than a preference (`design.md` 1.0.1b).
//!   3. `Shell::refresh` — take what the editor said, rebuild what moved. It measures itself, which
//!      is what the Profiler panel reports.
//!   4. Keyboard, then chrome, then panels, then the palette and the toasts on top.
//!   5. Intents — every command a panel, a menu, a key or the palette asked for, applied once, in
//!      order, through `Registry::invoke`.
//!
//! Step 5 is deferred on purpose. Invoking a command moves a document revision, and a view model
//! that rebuilt while a panel was iterating its rows would draw the rest of the frame against rows
//! that no longer exist. Collecting the intents and applying them at the end makes that impossible
//! rather than unlikely.

use std::sync::Arc;
use std::time::Duration;
#[cfg(target_os = "linux")]
use std::time::Instant;

use cy_editor_commands::{Arguments, Registry, Scope};
use cy_editor_core::ids::DocumentId;
use cy_editor_core::observe::Revision;
use cy_editor_core::value::Value;
use cy_editor_interface::notifications::{Choice, Modal};
use cy_editor_interface::panels::{PanelKey, PanelTitles};
use cy_editor_interface::shell::{Shell, panel_title};
use cy_editor_interface::thumbnails::Thumbnails;
use cy_editor_reflection::Catalogue;
use cy_editor_services::notifications::Notification;
use cy_editor_services::{CloseDecision, CloseOutcome, Editor, WorkspaceStore};
use cy_editor_viewmodels::{
    AssetBrowserViewModel, DiffViewModel, DocumentTabsViewModel, HierarchyViewModel,
    HistoryViewModel, MergeViewModel, SettingsViewModel, SourceControlViewModel,
    SourceWorkspaceViewModel,
};
use cy_editor_visual::colour::{Mode, Vision};
use cy_editor_visual::density::{Density, Metrics, Scale};

use crate::keys::{Keys, Pressed};
use crate::palette::Palette;
use crate::panels::{Inputs, Intent, Panels};
use crate::view::ViewAction;
use crate::viewport_link::ViewportLink;
use crate::{chrome, dock, documents, identity, theme};

/// How many thumbnails the content browser keeps.
const THUMBNAIL_CACHE: usize = 512;

/// How often a viewport with no transport tries to attach again.
///
/// A runtime is usually started *after* the editor, so a link that tried once at start-up and never
/// again would mean restarting the editor to see a scene. Half a second is short enough not to be
/// noticed and long enough that a missing socket costs nothing measurable.
const REATTACH_INTERVAL: Duration = Duration::from_millis(500);

/// The editor, with a window.
pub struct EditorWindow {
    /// The authoritative state.
    pub editor: Editor,
    /// Every action the editor can perform.
    pub registry: Registry,
    /// What the person at this window may do. Unrestricted; an agent connection declares its own.
    pub scope: Scope,

    shell: Shell,
    documents: DocumentTabsViewModel,
    hierarchy: HierarchyViewModel,
    history: HistoryViewModel,
    settings: SettingsViewModel,
    source_control: SourceControlViewModel,
    asset_browser: AssetBrowserViewModel,
    source_workspace: SourceWorkspaceViewModel,
    agent: Option<cy_editor_agent::DesktopAgentHost>,
    diff: DiffViewModel,
    merge_view: MergeViewModel,
    thumbnails: Thumbnails,
    titles: PanelTitles,
    dock: egui_dock::DockState<PanelKey>,
    palette: Palette,
    keys: Keys,
    inputs: Inputs,
    link: ViewportLink,
    identity: Identity,
    /// What the inspector was last described from, so the catalogue is rebuilt when it moves and
    /// never at frame rate.
    described: Option<(DocumentId, Revision, usize)>,
    /// The dirty document whose close decision is currently blocking input.
    pending_close: Option<DocumentId>,
    /// Per-user restart state. Absent for embedders and tests that did not choose a store.
    workspace_store: Option<WorkspaceStore>,
    #[cfg(target_os = "linux")]
    last_attach: Option<Instant>,
    /// The device the window shares with the transport, when there is one.
    #[cfg(target_os = "linux")]
    gpu: Option<Arc<cy_editor_viewport_transport::Gpu>>,
}

/// The identity's artwork; a type alias so the field reads as what it is.
type Identity = identity::Identity;

impl EditorWindow {
    /// A window over an editor's state.
    ///
    /// Takes the three things a headless `Application` holds rather than the `Application` itself:
    /// this crate is *below* the binary, and a render crate that named the binary's type would have
    /// inverted the workspace's dependency direction to save one line.
    pub fn new(
        mut editor: Editor,
        registry: Registry,
        scope: Scope,
    ) -> cy_editor_core::problem::Result<Self> {
        let mut shell = Shell::new(&registry)?;
        if let Err(problem) = editor.asset_catalogue.refresh() {
            editor
                .notifications
                .post(Notification::error(problem.what.clone(), problem));
        }
        if let Err(problem) = editor.sources.refresh() {
            editor
                .notifications
                .post(Notification::error(problem.what.clone(), problem));
        }
        if let Err(problem) = shell.restore_layout(&editor) {
            editor
                .notifications
                .post(Notification::error(problem.what.clone(), problem));
        }
        bind_view_actions(&mut shell, &mut |problem| {
            // A conflict names both commands, and it is reported rather than swallowed: a binding
            // that silently lost is how a user learns their keymap is unreliable.
            eprintln!("cyberdyne-editor: {problem}");
        });
        shell.palette.ingest(ViewAction::ALL.map(ViewAction::entry));

        let mut titles = PanelTitles::new();
        for panel in shell.workspaces.current().panels() {
            titles.define(panel.as_str(), panel_title(&panel));
        }

        let dock = dock::to_dock_state(shell.workspaces.current());
        Ok(Self {
            editor,
            registry,
            scope,
            shell,
            documents: DocumentTabsViewModel::new(),
            hierarchy: HierarchyViewModel::new(),
            history: HistoryViewModel::new(),
            settings: SettingsViewModel::new(),
            source_control: SourceControlViewModel::new(),
            asset_browser: AssetBrowserViewModel::new(),
            source_workspace: SourceWorkspaceViewModel::new(),
            agent: None,
            diff: DiffViewModel::new(),
            merge_view: MergeViewModel::new(),
            thumbnails: Thumbnails::new(THUMBNAIL_CACHE),
            titles,
            dock,
            palette: Palette::new(),
            keys: Keys::new(),
            inputs: Inputs::default(),
            link: ViewportLink::idle(),
            identity: Identity::new(),
            described: None,
            pending_close: None,
            workspace_store: None,
            #[cfg(target_os = "linux")]
            last_attach: None,
            #[cfg(target_os = "linux")]
            gpu: None,
        })
    }

    /// Persist this window's restart state in the selected per-user store.
    #[must_use]
    pub fn with_workspace_store(mut self, store: WorkspaceStore) -> Self {
        self.workspace_store = Some(store);
        self
    }

    /// Host one MCP session alongside this window, drained on the interface owner.
    #[must_use]
    pub fn with_agent_host(mut self, agent: cy_editor_agent::DesktopAgentHost) -> Self {
        self.agent = Some(agent);
        self
    }

    fn persist_workspace(&mut self) {
        self.capture_layout();
        self.shell.persist_layout(&mut self.editor);
        if let Some(store) = &self.workspace_store
            && let Err(problem) = store.write(&self.editor)
        {
            eprintln!("cyberdyne-editor: {problem}");
        }
    }

    /// The window's title, which names the product and the world.
    #[must_use]
    pub fn window_title(&self) -> String {
        format!(
            "{} — {}",
            identity::WINDOW_TITLE,
            self.shell.header(&self.editor)
        )
    }

    /// Describe the active document's types to the inspector, when they have moved.
    ///
    /// `Catalogue::of_document` is the authoring path and works with no runtime, which is the mode
    /// most of a session is spent in. When a runtime is attached the engine's own registered types
    /// are the better source; that arrives with the SDK's world handle, and the inspector does not
    /// care which produced the catalogue — which is the point of the catalogue existing.
    fn describe(&mut self) {
        let Some(id) = self.editor.workspace.active() else {
            return;
        };
        let Some(document) = self.editor.documents.get(id) else {
            return;
        };
        let types = document.schema().types().count();
        let fingerprint = (id, document.revision(), types);
        if self.described == Some(fingerprint) {
            return;
        }
        self.shell
            .describe_with(Catalogue::of_document(document.schema()));
        self.described = Some(fingerprint);
    }

    /// Try to attach the viewport to a runtime, at most every [`REATTACH_INTERVAL`].
    #[cfg(target_os = "linux")]
    fn attach_viewport(&mut self) {
        let Some(gpu) = self.gpu.clone() else {
            return;
        };
        if self.link.is_attached() {
            return;
        }
        let now = Instant::now();
        if self
            .last_attach
            .is_some_and(|last| now.duration_since(last) < REATTACH_INTERVAL)
        {
            return;
        }
        self.last_attach = Some(now);
        self.link.attach(gpu);
    }

    /// Apply everything the frame asked for.
    fn apply(&mut self, intents: Vec<Intent>) {
        for intent in intents {
            match intent {
                Intent::Invoke(id, arguments) => self.invoke(&id, &arguments),
                Intent::OpenAsset(path) => {
                    if let Err(problem) = self.editor.open_document(&path) {
                        self.editor
                            .notifications
                            .post(Notification::error(problem.what.clone(), problem));
                    }
                }
                Intent::ActivateDocument(document) => {
                    if let Err(problem) = self.documents.activate(&mut self.editor, document) {
                        self.editor
                            .notifications
                            .post(Notification::error(problem.what.clone(), problem));
                    }
                }
                Intent::CloseDocument(document) => self.close_document(document, None),
                Intent::ResolveDocumentClose(document, decision) => {
                    self.close_document(document, Some(decision));
                }
                Intent::OpenSource(path) => {
                    if let Err(problem) = self.source_workspace.open(&self.editor.sources, &path) {
                        self.editor
                            .notifications
                            .post(Notification::error(problem.what.clone(), problem));
                    }
                }
                Intent::SaveSource => self.save_source(),
                Intent::NavigateSource { path, line, column } => {
                    if let Err(problem) =
                        self.source_workspace
                            .navigate(&self.editor.sources, &path, line, column)
                    {
                        self.editor
                            .notifications
                            .post(Notification::error(problem.what.clone(), problem));
                    }
                }
                Intent::PauseAgent => {
                    if let Some(agent) = self.agent.as_mut() {
                        agent.pause();
                    }
                }
                Intent::ResumeAgent => {
                    if let Some(agent) = self.agent.as_mut() {
                        agent.resume();
                    }
                }
                Intent::RevokeAgent => {
                    if let Some(agent) = self.agent.as_mut()
                        && let Err(problem) = agent.revoke(&mut self.editor)
                    {
                        self.editor
                            .notifications
                            .post(Notification::error(problem.what.clone(), problem));
                    }
                }
                Intent::DecideAgent {
                    ticket,
                    allow,
                    grant_millis,
                } => {
                    if let Some(agent) = self.agent.as_mut() {
                        agent.decide(
                            ticket,
                            if allow {
                                cy_editor_agent::Decision::Allow
                            } else {
                                cy_editor_agent::Decision::Refuse
                            },
                            grant_millis.map(Duration::from_millis),
                        );
                    }
                }
            }
        }
    }

    fn sync_source_language(&mut self) {
        let buffers: Vec<_> = self
            .source_workspace
            .buffers()
            .iter()
            .map(|buffer| {
                (
                    buffer.path().to_string(),
                    buffer.text().to_string(),
                    buffer.revision(),
                )
            })
            .collect();
        for (path, text, revision) in buffers {
            self.editor
                .source_language
                .synchronize(&path, &text, revision);
        }
        self.editor.source_language.pump();
        self.source_workspace
            .refresh_diagnostics(&self.editor.source_language);
    }

    /// Apply one stage of the guarded close flow.
    fn close_document(&mut self, document: DocumentId, decision: Option<CloseDecision>) {
        match self.documents.close(&mut self.editor, document, decision) {
            Ok(CloseOutcome::NeedsDecision) => self.pending_close = Some(document),
            Ok(CloseOutcome::Closed | CloseOutcome::Cancelled) => self.pending_close = None,
            Err(problem) => {
                // Keep the decision open after a failed save. The document service guarantees the
                // dirty document is still present, so retry, discard, and cancel are all possible.
                self.pending_close = Some(document);
                self.editor
                    .notifications
                    .post(Notification::error(problem.what.clone(), problem));
            }
        }
    }

    /// Invoke a command, or perform one of the window's own view actions.
    ///
    /// The two are told apart by identifier and never by shape: `ViewAction::of_id` answers, and
    /// anything it does not claim goes to the registry, which is the single action surface for
    /// everything that can change the project.
    fn invoke(&mut self, id: &str, arguments: &Arguments) {
        if let Some(action) = ViewAction::of_id(id) {
            self.perform(action);
            return;
        }
        match self
            .registry
            .invoke(id, &self.scope, &mut self.editor, arguments)
        {
            Ok(outcome) => self
                .editor
                .notifications
                .post(Notification::info(outcome.summary.clone())),
            Err(problem) => self
                .editor
                .notifications
                .post(Notification::error(problem.what.clone(), problem)),
        }
    }

    fn save_source(&mut self) {
        let Some(buffer) = self.source_workspace.active() else {
            return;
        };
        let path = buffer.path().to_string();
        let arguments = Arguments::new()
            .with("path", Value::Text(path.clone()))
            .with("contents", Value::Text(buffer.text().to_string()))
            .with(
                "expected_fingerprint",
                Value::Text(buffer.base_fingerprint().to_string()),
            )
            .with("base", Value::Text(buffer.base().to_string()));
        match self
            .registry
            .invoke("source.write", &self.scope, &mut self.editor, &arguments)
        {
            Ok(outcome) => {
                let conflict = matches!(outcome.values.get("conflict"), Some(Value::Bool(true)));
                if conflict {
                    let disk = outcome
                        .values
                        .get("disk")
                        .and_then(Value::as_text)
                        .map(str::to_string);
                    let exists =
                        matches!(outcome.values.get("disk_exists"), Some(Value::Bool(true)));
                    let fingerprint = outcome
                        .values
                        .get("disk_fingerprint")
                        .and_then(Value::as_text)
                        .and_then(|text| cy_editor_services::SourceFingerprint::parse(text).ok());
                    if let (Some(buffer), Some(fingerprint)) =
                        (self.source_workspace.active_mut(), fingerprint)
                    {
                        buffer.report_conflict(exists.then_some(disk).flatten(), fingerprint);
                    }
                } else if let Some(fingerprint) = outcome
                    .values
                    .get("fingerprint")
                    .and_then(Value::as_text)
                    .and_then(|text| cy_editor_services::SourceFingerprint::parse(text).ok())
                    && let Some(buffer) = self.source_workspace.active_mut()
                {
                    buffer.saved(fingerprint);
                }
                self.editor
                    .notifications
                    .post(Notification::info(outcome.summary));
            }
            Err(problem) => self
                .editor
                .notifications
                .post(Notification::error(problem.what.clone(), problem)),
        }
    }

    /// One of the window's own actions.
    fn perform(&mut self, action: ViewAction) {
        match action {
            ViewAction::CommandPalette => self.palette.open(),
            ViewAction::ToggleDensity => {
                self.shell.density = match self.shell.density {
                    Density::Compact => Density::Comfortable,
                    Density::Comfortable => Density::Compact,
                };
            }
            ViewAction::ToggleTheme => {
                self.shell.theme.mode = match self.shell.theme.mode {
                    Mode::Dark => Mode::Light,
                    Mode::Light => Mode::Dark,
                };
            }
            ViewAction::ToggleVision => {
                self.shell.theme.vision = match self.shell.theme.vision {
                    Vision::Standard => Vision::RedGreenSafe,
                    Vision::RedGreenSafe => Vision::Standard,
                };
            }
            ViewAction::ScaleUp => {
                self.shell.scale = Scale::new(self.shell.scale.factor() + 0.1);
            }
            ViewAction::ScaleDown => {
                self.shell.scale = Scale::new(self.shell.scale.factor() - 0.1);
            }
            ViewAction::SaveWorkspace => {
                self.capture_layout();
                let name = self.shell.workspaces.current_name().to_string();
                self.shell.workspaces.save_as(name.clone());
                self.shell.persist_layout(&mut self.editor);
                self.editor
                    .notifications
                    .post(Notification::info(format!("Saved the {name} workspace")));
            }
            ViewAction::ResetWorkspace => {
                // "A reset SHALL be available and SHALL restore the shipped arrangement." Nothing in
                // the project changes, which is why this is a view action and not a command.
                self.shell.workspaces.reset();
                self.dock = dock::to_dock_state(self.shell.workspaces.current());
                self.editor
                    .notifications
                    .post(Notification::info("Reset the workspace to its default"));
            }
            ViewAction::ToggleViewportChrome => {
                let showing = !self.shell.chrome.shown().is_empty();
                for overlay in cy_editor_visual::chrome::Overlay::ALL {
                    self.shell.chrome.set(overlay, !showing);
                }
            }
        }
    }

    /// Take the user's arrangement out of the dock manager and back into the editor's own layout.
    ///
    /// Called when the layout is about to be saved or persisted rather than every frame: the editor's
    /// `Layout` is authoritative and the dock state is a rendering of it, so the only moments the
    /// conversion has to happen are the ones where the authoritative copy is read.
    fn capture_layout(&mut self) {
        *self.shell.workspaces.current_mut() = dock::from_dock_state(&self.dock);
    }

    /// The palette, and what choosing a result asks for.
    ///
    /// `Invoke` and `OpenSetting` land in the same place on purpose: a view action and a registry
    /// command are told apart by their identifier in [`EditorWindow::invoke`], which is the one
    /// place that distinction is made. Two arms doing the same thing here would be two places.
    fn overlays(&mut self, ctx: &egui::Context, metrics: Metrics, intents: &mut Vec<Intent>) {
        use cy_editor_interface::palette::Action as Chosen;

        if let Some(document) = self.pending_close {
            let Some(open) = self.editor.documents.get(document) else {
                self.pending_close = None;
                return;
            };
            let asset = open
                .assets()
                .first()
                .map_or_else(|| "this document".to_string(), Clone::clone);
            let modal = Modal::decision(
                format!("Save changes to {asset}?"),
                format!(
                    "Save writes the changes to {asset}. Discard permanently loses the unsaved changes."
                ),
                vec![
                    Choice::new("Save"),
                    Choice::destructive("Discard"),
                    Choice::new("Cancel"),
                ],
            )
            .expect("the close decision has three choices and a stated consequence");
            if let Some(decision) = crate::notify::modal(ctx, &modal, self.shell.theme, metrics) {
                let choice = match decision.choice {
                    0 => CloseDecision::Save,
                    1 => CloseDecision::Discard,
                    _ => CloseDecision::Cancel,
                };
                intents.push(Intent::ResolveDocumentClose(document, choice));
            }
            return;
        }

        let Some(action) = self
            .palette
            .show(ctx, &self.shell.palette, self.shell.theme, metrics)
        else {
            return;
        };
        match action {
            Chosen::Invoke(id) | Chosen::OpenSetting(id) => {
                intents.push(Intent::Invoke(id, Arguments::new()));
            }
            Chosen::OpenAsset(path) => intents.push(Intent::OpenAsset(path)),
            Chosen::FocusNode(node) => self.hierarchy.select(
                &mut self.editor,
                node,
                cy_editor_viewmodels::SelectionIntent::Replace,
            ),
            Chosen::OpenDocumentation(page) => self
                .editor
                .notifications
                .post(Notification::info(format!("Documentation: {page}"))),
        }
    }

    /// The header, the toolbar and the footer.
    fn chrome(&mut self, root: &mut egui::Ui, pending_chord: &str, intents: &mut Vec<Intent>) {
        egui::Panel::top("cy-header")
            .frame(chrome::bar(&self.shell))
            .show(root, |ui| {
                chrome::header(
                    ui,
                    &self.shell,
                    &self.editor,
                    &self.registry,
                    &mut self.identity,
                    intents,
                );
            });
        egui::Panel::top("cy-toolbar")
            .frame(chrome::bar(&self.shell))
            .show(root, |ui| {
                chrome::toolbar(ui, &self.shell, &self.editor, &self.registry, intents);
            });
        egui::Panel::top("cy-document-tabs")
            .frame(chrome::bar(&self.shell))
            .show(root, |ui| {
                documents::strip(
                    ui,
                    &self.documents,
                    self.shell.theme,
                    self.shell.metrics(),
                    intents,
                );
            });
        egui::Panel::bottom("cy-footer")
            .frame(chrome::bar(&self.shell))
            .show(root, |ui| {
                chrome::footer(ui, &self.shell, &self.editor, pending_chord);
            });
    }

    /// The docked panels.
    fn dock_area(&mut self, root: &mut egui::Ui, intents: &mut Vec<Intent>) {
        egui::CentralPanel::default()
            .frame(egui::Frame::NONE.fill(theme::surface(
                self.shell.theme,
                cy_editor_visual::colour::Surface::Window,
            )))
            .show(root, |ui| {
                let style = dock::style(self.shell.theme, ui.style());
                // Destructured so the dock state and the panels' borrows are disjoint; the panels
                // cannot reach the dock state at all, which is what keeps "context changes content,
                // never position" true by construction rather than by care.
                let Self {
                    editor,
                    registry,
                    scope,
                    shell,
                    hierarchy,
                    history,
                    settings,
                    source_control,
                    asset_browser,
                    source_workspace,
                    agent,
                    diff,
                    merge_view,
                    thumbnails,
                    titles,
                    dock,
                    link,
                    inputs,
                    ..
                } = self;
                let mut panels = Panels {
                    editor,
                    registry,
                    scope,
                    shell,
                    hierarchy,
                    history,
                    settings,
                    source_control,
                    asset_browser,
                    source_workspace,
                    agent: agent.as_mut(),
                    diff,
                    merge: merge_view,
                    thumbnails,
                    link,
                    titles,
                    inputs,
                    intents,
                    tab_rects: Vec::new(),
                };
                egui_dock::DockArea::new(dock)
                    .style(style)
                    .show_leaf_close_all_buttons(false)
                    .show_leaf_collapse_buttons(false)
                    .show_add_buttons(false)
                    .show_inside(ui, &mut panels);
                dock::paint_tab_selection(ui.ctx(), dock, &panels.tab_rects, panels.shell.theme);
            });
    }

    /// Run one frame's keyboard input.
    fn keyboard(&mut self, ctx: &egui::Context, intents: &mut Vec<Intent>) -> String {
        if self.palette.is_open() || self.pending_close.is_some() {
            // The palette owns the keyboard while it is open, including `Escape`. A chord that was
            // half-typed is abandoned rather than completing behind a modal surface.
            self.keys.clear();
            return String::new();
        }
        match self
            .keys
            .pump(ctx, &self.shell.keymap, cy_editor_interface::keymap::GLOBAL)
        {
            Pressed::Command(id) => {
                intents.push(Intent::Invoke(id, Arguments::new()));
                String::new()
            }
            // A chord in progress spans frames and passes, and `Keys` reports it on every one of
            // them — which is what keeps "Ctrl+K …" on the footer until the second stroke.
            Pressed::Pending(chord) => chord,
            Pressed::Nothing => String::new(),
        }
    }
}

/// Bind the window's own actions into the shell's keymap, reporting any conflict.
fn bind_view_actions(shell: &mut Shell, report: &mut dyn FnMut(cy_editor_core::problem::Problem)) {
    for action in ViewAction::ALL {
        if let Err(problem) = shell.keymap.bind(
            cy_editor_interface::keymap::GLOBAL,
            action.binding(),
            action.id(),
        ) {
            report(problem);
        }
    }
}

impl eframe::App for EditorWindow {
    fn ui(&mut self, root: &mut egui::Ui, frame: &mut eframe::Frame) {
        let ctx = root.ctx().clone();
        let ctx = &ctx;

        // 1 and 2: the editor's housekeeping, then the engine's newest frame.
        self.editor.pump();
        #[cfg(target_os = "linux")]
        self.attach_viewport();
        if let Some(render_state) = frame.wgpu_render_state() {
            // The focused viewport is handed in because claiming a frame and LEARNING that one
            // arrived are the same event — see `ViewportLink::begin_frame`, which is where M5.5's
            // recorded defect was.
            self.link
                .begin_frame(render_state, self.editor.viewports.focused_mut());
        }

        // 3: what moved, rebuilt — and measured, which is what the Profiler panel reports.
        self.describe();
        self.shell.refresh(&self.editor);
        self.documents.refresh(&self.editor);
        self.history.refresh(&self.editor);
        self.settings.refresh(&self.editor);
        self.source_control
            .refresh(&self.editor.source_control, &self.editor);
        self.asset_browser.refresh(&self.editor.asset_catalogue);
        self.source_workspace.refresh(&self.editor.sources);
        self.source_workspace
            .refresh_build(&self.editor.project, &self.editor.operations);
        self.source_workspace.refresh_reload(
            self.editor.reload_revision(),
            self.editor.reload_report.as_ref(),
        );
        self.diff.refresh(&self.editor.semantic_merge);
        self.merge_view.refresh(&self.editor.semantic_merge);

        // Both of egui's own themes are set to ours, because which one it would otherwise pick comes
        // from the host's preference and the editor's theme is the editor's own decision.
        let style = theme::style(self.shell.theme, self.shell.metrics());
        ctx.all_styles_mut(|existing| *existing = style.clone());
        root.set_style(std::sync::Arc::new(style));
        let metrics = self.shell.metrics();
        let mut intents: Vec<Intent> = Vec::new();

        // 4: the keyboard first, so a key is not swallowed by whatever happens to be under the
        // pointer, then the chrome, then the panels.
        let pending_chord = self.keyboard(ctx, &mut intents);

        self.chrome(root, &pending_chord, &mut intents);
        self.dock_area(root, &mut intents);

        // The palette and the notifications, over everything.
        self.overlays(ctx, metrics, &mut intents);
        crate::notify::toasts(
            ctx,
            &mut self.shell.notifications,
            self.shell.theme,
            metrics,
            &mut intents,
        );

        // 5: everything the frame asked for, once, in order.
        self.apply(intents);
        self.sync_source_language();
        // Human intents are applied first. Agent work then receives a fixed slice of the frame, so
        // a saturated client cannot turn the window into its worker thread.
        if let Some(agent) = self.agent.as_mut() {
            agent.pump(&mut self.editor, &self.registry, 4);
        }

        ctx.send_viewport_cmd(egui::ViewportCommand::Title(self.window_title()));
        // A viewport that is streaming frames needs a repaint every frame; one that is not still
        // needs a slow tick, because a runtime started after the editor must be noticed.
        if self.link.is_attached() || self.agent.is_some() {
            ctx.request_repaint();
        } else {
            ctx.request_repaint_after(REATTACH_INTERVAL);
        }
    }

    fn save(&mut self, _storage: &mut dyn eframe::Storage) {
        // eframe's own persistence is deliberately not enabled; the layout is persisted through
        // `Workspace`, which is tested headlessly and is the editor's own format. This hook only
        // makes sure the arrangement on screen is the one that was written.
        self.persist_workspace();
    }
}

impl Drop for EditorWindow {
    fn drop(&mut self) {
        self.persist_workspace();
    }
}

/// The device the window and the viewport transport share, when one can be made.
#[cfg(target_os = "linux")]
fn shared_device() -> (Option<Arc<cy_editor_viewport_transport::Gpu>>, Vec<String>) {
    use cy_editor_viewport_transport::Gpu;
    use cy_editor_viewport_transport::session::preferred_adapter;

    match Gpu::create(&preferred_adapter(), true) {
        Ok(gpu) => {
            let notes = gpu.notes.clone();
            (Some(Arc::new(gpu)), notes)
        }
        Err(problem) => (
            None,
            vec![format!(
                "the shared device could not be created ({}); the interface will use its own and \
                 the viewport will say the transport is not ready",
                problem.because
            )],
        ),
    }
}

#[cfg(not(target_os = "linux"))]
fn shared_device() -> (Option<Arc<()>>, Vec<String>) {
    (
        None,
        vec![if cfg!(target_os = "macos") {
            "the macOS viewport imports IOSurfaces into the interface's Metal device".to_string()
        } else {
            "the viewport transport is unavailable; the interface uses its own device".to_string()
        }],
    )
}

/// Open the window.
///
/// The shared device is attempted first and the reason is logged either way, because "the viewport
/// shows nothing" is a question whose answer is almost always in those lines.
pub fn run(window: EditorWindow) -> eframe::Result<()> {
    let (gpu, notes) = shared_device();
    for note in &notes {
        eprintln!("cyberdyne-editor: {note}");
    }

    #[cfg(target_os = "linux")]
    let mut window = window;
    let wgpu_options = egui_wgpu::WgpuConfiguration::default();
    #[cfg(target_os = "linux")]
    let mut wgpu_options = wgpu_options;
    #[cfg(target_os = "linux")]
    if let Some(gpu) = &gpu {
        wgpu_options.wgpu_setup = egui_wgpu::WgpuSetupExisting {
            instance: gpu.instance.clone(),
            adapter: gpu.adapter.clone(),
            device: gpu.device.clone(),
            queue: gpu.queue.clone(),
        }
        .into();
        window.gpu = Some(Arc::clone(gpu));
    }
    #[cfg(not(target_os = "linux"))]
    let _ = &gpu;

    let title = window.window_title();
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title(title)
            .with_app_id("cyberengine-editor")
            .with_inner_size([1600.0, 950.0])
            .with_min_inner_size([960.0, 600.0])
            .with_icon(identity::window_icon()),
        wgpu_options,
        ..Default::default()
    };

    eframe::run_native(
        identity::WINDOW_TITLE,
        options,
        Box::new(move |_cc| Ok(Box::new(window))),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::Actor;

    fn scratch(name: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!("cy-editor-shell-{name}-{}", std::process::id()))
    }

    fn window() -> EditorWindow {
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry).unwrap();
        EditorWindow::new(
            Editor::new(Actor::human("designer")),
            registry,
            Scope::unrestricted(),
        )
        .unwrap()
    }

    #[test]
    fn the_windows_own_bindings_do_not_conflict_with_the_registrys() {
        // Both sets go into one keymap on purpose, so that `Ctrl+S` meaning `file.save` and a view
        // action wanting the same key is caught at construction with both commands named. If this
        // ever fails, the message says which two.
        let mut reported = Vec::new();
        let mut shell = Shell::new(&window().registry).unwrap();
        bind_view_actions(&mut shell, &mut |problem| reported.push(problem));
        assert!(
            reported.is_empty(),
            "{}",
            reported
                .iter()
                .map(ToString::to_string)
                .collect::<Vec<_>>()
                .join("\n")
        );
    }

    #[test]
    fn the_workspace_chords_resolve_through_the_window_s_own_keymap() {
        // A regression test for a defect found by driving the real window: `Ctrl+K Ctrl+S` ran
        // `file.save` instead of saving the workspace. A chord whose first stroke is dropped
        // silently becomes its second stroke, which is the worst possible failure — it invokes a
        // *different* command rather than none.
        use cy_editor_interface::keymap::{GLOBAL, Resolution, Stroke};
        let window = window();
        let keymap = &window.shell.keymap;

        for action in [ViewAction::SaveWorkspace, ViewAction::ResetWorkspace] {
            let strokes: Vec<Stroke> = action
                .binding()
                .split_whitespace()
                .map(|part| Stroke::parse(part).unwrap())
                .collect();
            assert_eq!(strokes.len(), 2, "{} is not a chord", action.id());
            assert_eq!(
                keymap.resolve(GLOBAL, &strokes[..1]),
                Resolution::Pending,
                "{}'s first stroke does not hold the chord open",
                action.id()
            );
            assert_eq!(
                keymap.resolve(GLOBAL, &strokes),
                Resolution::Command(action.id().to_string()),
                "{} does not resolve",
                action.id()
            );
        }
    }

    #[test]
    fn the_default_workspace_reaches_the_dock_manager_with_every_panel_intact() {
        let window = window();
        let expected = window.shell.workspaces.current().panels().len();
        assert_eq!(window.dock.iter_all_tabs().count(), expected);
        assert!(
            expected >= 8,
            "the default workspace lost panels: {expected}"
        );
    }

    #[test]
    fn resetting_the_workspace_rebuilds_the_dock_and_changes_no_document() {
        let mut window = window();
        window.editor.open_document("worlds/city.cyworld").unwrap();
        let before = window.editor.documents.any_dirty();
        window.perform(ViewAction::ResetWorkspace);
        assert_eq!(
            window.dock.iter_all_tabs().count(),
            window.shell.workspaces.current().panels().len()
        );
        assert_eq!(
            window.editor.documents.any_dirty(),
            before,
            "resetting the workspace dirtied a document"
        );
    }

    #[test]
    fn a_view_action_never_reaches_the_registry() {
        // The separation this window depends on: view actions change the window, commands change the
        // project. A view identifier that fell through to `Registry::invoke` would post a "no such
        // command" failure every time somebody pressed Ctrl+P.
        let window = window();
        for action in ViewAction::ALL {
            assert!(
                window.registry.metadata(action.id()).is_none(),
                "{} is both a view action and a registered command",
                action.id()
            );
            assert_eq!(ViewAction::of_id(action.id()), Some(action));
        }
    }

    #[test]
    fn the_inspector_is_described_from_the_documents_own_schema() {
        // Generated, from reflection. If a document declares no types the catalogue is still set —
        // an empty catalogue, which is what makes the panel say "nothing is described" rather than
        // "nothing is selected".
        let mut window = window();
        assert!(window.shell.inspector.catalogue().is_none());
        window.editor.open_document("worlds/city.cyworld").unwrap();
        window.describe();
        assert!(window.shell.inspector.catalogue().is_some());
    }

    #[test]
    fn density_theme_and_scale_are_view_actions_that_change_only_the_window() {
        let mut window = window();
        let density = window.shell.density;
        let mode = window.shell.theme.mode;
        let scale = window.shell.scale.factor();
        window.perform(ViewAction::ToggleDensity);
        window.perform(ViewAction::ToggleTheme);
        window.perform(ViewAction::ScaleUp);
        assert_ne!(window.shell.density, density);
        assert_ne!(window.shell.theme.mode, mode);
        assert!(window.shell.scale.factor() > scale);
        assert!(!window.editor.documents.any_dirty());
    }

    #[test]
    fn cancelling_a_tab_close_preserves_the_dirty_document() {
        let mut window = window();
        let city = window.editor.open_document("worlds/city.cyworld").unwrap();
        window
            .editor
            .documents
            .get_mut(city)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        window.apply(vec![Intent::CloseDocument(city)]);
        assert_eq!(window.pending_close, Some(city));
        window.apply(vec![Intent::ResolveDocumentClose(
            city,
            CloseDecision::Cancel,
        )]);

        assert_eq!(window.pending_close, None);
        assert!(window.editor.documents.get(city).unwrap().is_dirty());
        assert_eq!(window.editor.workspace.active(), Some(city));
    }

    #[test]
    fn a_failed_save_keeps_the_tab_open_and_the_close_decision_available() {
        let directory = scratch("close-save-failure");
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(directory.join("worlds"), "not a directory").unwrap();
        let mut window = window();
        window.editor.documents.rooted_at(&directory);
        let city = window.editor.open_document("worlds/city.cyworld").unwrap();
        window
            .editor
            .documents
            .get_mut(city)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        window.apply(vec![Intent::CloseDocument(city)]);
        window.apply(vec![Intent::ResolveDocumentClose(
            city,
            CloseDecision::Save,
        )]);

        assert_eq!(window.pending_close, Some(city));
        assert!(window.editor.documents.get(city).unwrap().is_dirty());
        assert_eq!(window.editor.workspace.active(), Some(city));
        std::fs::remove_dir_all(directory).unwrap();
    }
}
