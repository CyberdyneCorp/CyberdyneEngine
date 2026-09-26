// SPDX-License-Identifier: MIT
//! Headless visual/accessibility matrix for the panels added by complete-editor-features-now.

use cy_editor_commands::{Registry, Scope};
use cy_editor_core::Actor;
use cy_editor_core::codec::Writer;
use cy_editor_interface::SpecialisedEditors;
use cy_editor_interface::panels::{PanelKey, PanelTitles};
use cy_editor_interface::shell::{Shell, panel_title};
use cy_editor_interface::thumbnails::Thumbnails;
use cy_editor_services::Editor;
use cy_editor_shell::panels::{Inputs, Intent, Panels};
use cy_editor_shell::viewport_link::ViewportLink;
use cy_editor_viewmodels::{
    AssetBrowserViewModel, DiffViewModel, HierarchyViewModel, HistoryViewModel, MergeViewModel,
    SettingsViewModel, SourceControlViewModel, SourceWorkspaceViewModel,
};
use cy_editor_visual::colour::Mode;
use cy_editor_visual::density::Density;
use egui_dock::TabViewer;

const NEW_PANELS: [(&str, &str); 10] = [
    ("undo-history", "Undo"),
    ("settings", "Apply"),
    ("source-control", "Refresh"),
    ("agent-sessions", "No agent is connected."),
    ("swift-workspace", "No Swift source is open."),
    ("editor-materials", "Engine catalogue"),
    ("editor-vfx-graph", "Engine catalogue"),
    ("editor-terrain", "No world is open."),
    ("semantic-diff", "Compare"),
    ("semantic-merge", "Compare"),
];

struct Harness {
    editor: Editor,
    registry: Registry,
    scope: Scope,
    shell: Shell,
    specialised: SpecialisedEditors,
    hierarchy: HierarchyViewModel,
    history: HistoryViewModel,
    settings: SettingsViewModel,
    source_control: SourceControlViewModel,
    asset_browser: AssetBrowserViewModel,
    source_workspace: SourceWorkspaceViewModel,
    diff: DiffViewModel,
    merge_view: MergeViewModel,
    thumbnails: Thumbnails,
    titles: PanelTitles,
    link: ViewportLink,
    inputs: Inputs,
    ctx: egui::Context,
}

impl Harness {
    fn new() -> Self {
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry).expect("built-in commands");
        let shell = Shell::new(&registry).expect("shell");
        let mut titles = PanelTitles::new();
        for panel in shell.workspaces.current().panels() {
            titles.define(panel.as_str(), panel_title(&panel));
        }
        let ctx = egui::Context::default();
        ctx.enable_accesskit();
        ctx.options_mut(|options| options.screen_reader = true);
        let mut specialised = SpecialisedEditors::new().expect("specialised editors");
        specialised
            .install_material_catalogue(&test_material_catalogue())
            .expect("engine material catalogue");
        specialised
            .install_vfx_catalogue(&test_vfx_catalogue())
            .expect("engine VFX catalogue");
        Self {
            editor: Editor::new(Actor::human("accessibility-auditor")),
            registry,
            scope: Scope::unrestricted(),
            shell,
            specialised,
            hierarchy: HierarchyViewModel::new(),
            history: HistoryViewModel::new(),
            settings: SettingsViewModel::new(),
            source_control: SourceControlViewModel::new(),
            asset_browser: AssetBrowserViewModel::new(),
            source_workspace: SourceWorkspaceViewModel::new(),
            diff: DiffViewModel::new(),
            merge_view: MergeViewModel::new(),
            thumbnails: Thumbnails::new(8),
            titles,
            link: ViewportLink::idle(),
            inputs: Inputs::default(),
            ctx,
        }
    }

    /// One frame of the default workspace through the real dock, returning the panels it drew.
    fn dock_frame(&mut self, size: egui::Vec2) -> Vec<(String, egui::Rect)> {
        let mut dock = cy_editor_shell::dock::to_dock_state(self.shell.workspaces.current());
        let raw = egui::RawInput {
            screen_rect: Some(egui::Rect::from_min_size(egui::Pos2::ZERO, size)),
            ..Default::default()
        };
        let Self {
            editor,
            registry,
            scope,
            shell,
            specialised,
            hierarchy,
            history,
            settings,
            source_control,
            asset_browser,
            source_workspace,
            diff,
            merge_view,
            thumbnails,
            titles,
            link,
            inputs,
            ctx,
        } = self;
        let mut intents: Vec<Intent> = Vec::new();
        let mut drawn = Vec::new();
        let mut output = ctx.run_ui(raw, |ui| {
            egui::CentralPanel::default().show(ui, |ui| {
                let mut panels = Panels {
                    editor,
                    registry,
                    scope,
                    shell,
                    specialised,
                    hierarchy,
                    history,
                    settings,
                    source_control,
                    asset_browser,
                    source_workspace,
                    agent: None,
                    diff,
                    merge: merge_view,
                    thumbnails,
                    link,
                    titles,
                    inputs,
                    intents: &mut intents,
                    tab_rects: Vec::new(),
                    panel_rects: Vec::new(),
                };
                egui_dock::DockArea::new(&mut dock).show_inside(ui, &mut panels);
                drawn = panels.panel_rects;
            });
        });
        // No renderer here to apply the font atlas to; egui insists that be said out loud.
        output.textures_delta.clear();
        drawn
    }

    fn frame(&mut self, panel: &str, size: egui::Vec2, events: Vec<egui::Event>) -> FrameEvidence {
        let style = cy_editor_shell::theme::style(self.shell.theme, self.shell.metrics());
        self.ctx
            .all_styles_mut(|existing| *existing = style.clone());
        self.settings.refresh(&self.editor);
        self.history.refresh(&self.editor);
        self.source_control
            .refresh(&self.editor.source_control, &self.editor);
        self.source_workspace.refresh(&self.editor.sources);
        self.diff.refresh(&self.editor.semantic_merge);
        self.merge_view.refresh(&self.editor.semantic_merge);

        let raw = egui::RawInput {
            screen_rect: Some(egui::Rect::from_min_size(egui::Pos2::ZERO, size)),
            events,
            ..Default::default()
        };
        let Self {
            editor,
            registry,
            scope,
            shell,
            specialised,
            hierarchy,
            history,
            settings,
            source_control,
            asset_browser,
            source_workspace,
            diff,
            merge_view,
            thumbnails,
            titles,
            link,
            inputs,
            ctx,
        } = self;
        let mut intents: Vec<Intent> = Vec::new();
        let mut output = ctx.run_ui(raw, |ui| {
            egui::CentralPanel::default().show(ui, |ui| {
                let mut panels = Panels {
                    editor,
                    registry,
                    scope,
                    shell,
                    specialised,
                    hierarchy,
                    history,
                    settings,
                    source_control,
                    asset_browser,
                    source_workspace,
                    agent: None,
                    diff,
                    merge: merge_view,
                    thumbnails,
                    link,
                    titles,
                    inputs,
                    intents: &mut intents,
                    tab_rects: Vec::new(),
                    panel_rects: Vec::new(),
                };
                let mut key = PanelKey::new(panel).expect("a tested built-in panel");
                panels.ui(ui, &mut key);
            });
        });
        let shapes = output.shapes.len();
        let update = output
            .platform_output
            .accesskit_update
            .take()
            .expect("AccessKit was enabled");
        let labels = update
            .nodes
            .iter()
            .filter_map(|(_, node)| node.label().or_else(|| node.value()).map(str::to_string))
            .collect();
        let actionable = update
            .nodes
            .iter()
            .filter(|(_, node)| {
                node.supports_action(egui::accesskit::Action::Click)
                    || node.supports_action(egui::accesskit::Action::Focus)
            })
            .count();
        output.textures_delta.clear();
        FrameEvidence {
            labels,
            actionable,
            shapes,
        }
    }
}

fn test_material_catalogue() -> Vec<u8> {
    let mut catalogue = Writer::new();
    catalogue.u32(1);
    catalogue.u32(1);
    catalogue.u32(1);
    catalogue.u32(42);
    catalogue.u32(1);
    catalogue.text("material.future");
    catalogue.u32(1);
    catalogue.u32(9);
    catalogue.u8(1);
    catalogue.text("out");
    catalogue.text("value");
    catalogue.u32(0);
    catalogue.finish()
}

fn test_vfx_catalogue() -> Vec<u8> {
    let mut catalogue = Writer::new();
    catalogue.u32(1);
    catalogue.u32(1);
    catalogue.u32(1);
    catalogue.u32(1001);
    catalogue.u32(1);
    catalogue.text("vfx.constant");
    catalogue.u32(1);
    catalogue.u32(1);
    catalogue.u8(1);
    catalogue.text("out");
    catalogue.text("float");
    catalogue.u32(0);
    catalogue.finish()
}

struct FrameEvidence {
    labels: Vec<String>,
    actionable: usize,
    shapes: usize,
}

fn tab_event() -> egui::Event {
    egui::Event::Key {
        key: egui::Key::Tab,
        physical_key: Some(egui::Key::Tab),
        pressed: true,
        repeat: false,
        modifiers: egui::Modifiers::NONE,
    }
}

#[test]
fn every_new_panel_survives_the_theme_density_width_matrix_with_accessible_names() {
    for mode in [Mode::Dark, Mode::Light] {
        for density in Density::ALL {
            for size in [egui::vec2(900.0, 600.0), egui::vec2(280.0, 360.0)] {
                for (panel, expected_label) in NEW_PANELS {
                    let mut harness = Harness::new();
                    harness.shell.theme.mode = mode;
                    harness.shell.density = density;
                    let evidence = harness.frame(panel, size, Vec::new());
                    assert!(
                        evidence.shapes > 0,
                        "{panel} painted nothing in {mode:?}/{density:?}/{size:?}"
                    );
                    assert!(
                        evidence
                            .labels
                            .iter()
                            .any(|label| label.contains(expected_label)),
                        "{panel} exposed no accessible {expected_label:?} label in {mode:?}/{density:?}/{size:?}; labels were {:?}",
                        evidence.labels
                    );
                }
            }
        }
    }
}

#[test]
fn enabled_new_panel_actions_are_keyboard_focusable_without_pointer_input() {
    for (panel, _) in NEW_PANELS {
        let mut harness = Harness::new();
        let initial = harness.frame(panel, egui::vec2(420.0, 360.0), Vec::new());
        let _ = harness.frame(panel, egui::vec2(420.0, 360.0), vec![tab_event()]);
        if initial.actionable > 0 {
            assert!(
                harness.ctx.memory(egui::Memory::focused).is_some(),
                "Tab did not focus an enabled action in {panel}"
            );
        }
    }
}

#[test]
fn vfx_metadata_sections_are_visible_on_an_open_engine_catalogue() {
    use cy_editor_interface::specialised::vfx::{Emitter, SimulationPath, Stage, VfxDocument};

    let mut harness = Harness::new();
    let mut document = VfxDocument::new("sparks").unwrap();
    document.emitters.push(Emitter {
        name: "smoke".into(),
        path: SimulationPath::GpuPreferred,
        renderer: "Sprite".into(),
        stages: Vec::new(),
        modules: Vec::new(),
        interfaces: Vec::new(),
        capacity: 1024,
        attributes: Vec::new(),
    });
    harness.specialised.start_vfx_document(document).unwrap();
    harness
        .specialised
        .select_vfx_stage(0, Stage::Spawn)
        .unwrap();
    let evidence = harness.frame("editor-vfx-graph", egui::vec2(900.0, 700.0), Vec::new());
    for section in [
        "System parameters",
        "Event channels",
        "Particle attributes",
        "Reusable VFX module",
    ] {
        assert!(
            evidence.labels.iter().any(|label| label.contains(section)),
            "missing {section} in {:?}",
            evidence.labels
        );
    }
}

#[test]
fn the_dock_reports_where_each_shown_panel_was_drawn() {
    // `editor:window?panel=<kind>` crops to these rectangles, so they must name the panels the
    // default workspace shows and lie inside the window; a panel behind another tab is absent.
    let size = egui::vec2(1600.0, 950.0);
    let drawn = Harness::new().dock_frame(size);
    let window = egui::Rect::from_min_size(egui::Pos2::ZERO, size);
    for kind in ["viewport", "hierarchy", "inspector"] {
        let (_, rect) = drawn
            .iter()
            .find(|(drawn_kind, _)| drawn_kind == kind)
            .unwrap_or_else(|| panic!("{kind} was not drawn: {drawn:?}"));
        assert!(rect.area() > 1000.0, "{kind} has no area: {rect:?}");
        assert!(
            window.contains_rect(*rect),
            "{kind} {rect:?} is outside the window"
        );
    }
    for (kind, _) in &drawn {
        assert!(
            cy_editor_interface::shell::BUILT_IN_PANEL_KINDS.contains(&kind.as_str()),
            "{kind} is not a built-in kind"
        );
    }
    let viewport = drawn.iter().find(|(kind, _)| kind == "viewport").unwrap().1;
    let hierarchy = drawn
        .iter()
        .find(|(kind, _)| kind == "hierarchy")
        .unwrap()
        .1;
    assert!(!viewport.intersects(hierarchy), "two panels share pixels");
}
