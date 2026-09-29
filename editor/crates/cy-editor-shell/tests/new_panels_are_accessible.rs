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

const NEW_PANELS: [(&str, &str); 13] = [
    ("undo-history", "Undo"),
    ("physics", "No world is open."),
    ("settings", "Apply"),
    ("source-control", "Refresh"),
    ("agent-sessions", "No agent is connected."),
    ("swift-workspace", "No Swift source is open."),
    ("editor-materials", "Engine catalogue"),
    ("editor-vfx-graph", "Engine catalogue"),
    ("editor-terrain", "No world is open."),
    ("editor-lighting-and-lightmap-baking", "Bake lightmaps"),
    ("editor-audio-buses-and-mixing", "No world is open."),
    ("semantic-diff", "Compare"),
    ("semantic-merge", "Compare"),
    ("editor-navigation-baking", "No world is open."),
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
                    saved_vfx_document_reference: None,
                    saved_vfx_module_reference: None,
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
                    saved_vfx_document_reference: None,
                    saved_vfx_module_reference: None,
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
        let click_targets = click_targets(&update);
        let bounds = labelled_bounds(&update);
        output.textures_delta.clear();
        FrameEvidence {
            labels,
            bounds,
            actionable,
            shapes,
            click_targets,
            intents,
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
    /// Every labelled node's bounds, in points.
    bounds: Vec<(String, egui::accesskit::Rect)>,
    actionable: usize,
    shapes: usize,
    click_targets: Vec<(String, egui::accesskit::TreeId, egui::accesskit::NodeId)>,
    intents: Vec<Intent>,
}

fn labelled_bounds(update: &egui::accesskit::TreeUpdate) -> Vec<(String, egui::accesskit::Rect)> {
    update
        .nodes
        .iter()
        .filter_map(|(_, node)| Some((node.label()?.to_owned(), node.bounds()?)))
        .collect()
}

fn click_targets(
    update: &egui::accesskit::TreeUpdate,
) -> Vec<(String, egui::accesskit::TreeId, egui::accesskit::NodeId)> {
    update
        .nodes
        .iter()
        .filter(|(_, node)| node.supports_action(egui::accesskit::Action::Click))
        .filter_map(|(id, node)| {
            node.label()
                .map(|label| (label.to_owned(), update.tree_id, *id))
        })
        .collect()
}

fn click_named(evidence: &FrameEvidence, label: &str) -> egui::Event {
    let (_, target_tree, target_node) = evidence
        .click_targets
        .iter()
        .find(|(name, _, _)| name == label)
        .unwrap_or_else(|| panic!("{label} has no click target: {:?}", evidence.click_targets));
    egui::Event::AccessKitActionRequest(egui::accesskit::ActionRequest {
        action: egui::accesskit::Action::Click,
        target_tree: *target_tree,
        target_node: *target_node,
        data: None,
    })
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
fn vfx_creation_buttons_route_through_saved_commands() {
    let mut harness = Harness::new();
    let size = egui::vec2(900.0, 700.0);
    let initial = harness.frame("editor-vfx-graph", size, Vec::new());
    let created = harness.frame(
        "editor-vfx-graph",
        size,
        vec![click_named(&initial, "New VFX system")],
    );
    assert_eq!(
        created.intents,
        [Intent::CreateVfxDocument(
            harness.inputs.vfx_system_name.clone(),
            harness.inputs.vfx_reference.clone(),
        )]
    );

    let opened = harness.frame(
        "editor-vfx-graph",
        size,
        vec![click_named(&created, "Reusable VFX module")],
    );
    let module = harness.frame(
        "editor-vfx-graph",
        size,
        vec![click_named(&opened, "Create module")],
    );
    assert_eq!(
        module.intents,
        [Intent::CreateVfxModule(
            harness.inputs.vfx_module_name.clone(),
            harness.inputs.vfx_module_stage,
            harness.inputs.vfx_module_reference.clone(),
        )]
    );
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

/// A harness with an open world, one terrain root created through the registered command, and
/// that root selected — which is what the terrain command itself leaves behind.
fn terrain_harness() -> Harness {
    let mut harness = Harness::new();
    harness
        .editor
        .open_document("worlds/terrain.cyworld")
        .expect("a new world opens");
    harness
        .registry
        .invoke(
            "terrain.create",
            &harness.scope,
            &mut harness.editor,
            &cy_editor_commands::Arguments::new(),
        )
        .expect("terrain.create is one transaction");
    harness
}

#[test]
fn the_terrain_tool_draws_in_the_specialised_frame_with_undo_over_its_transactions() {
    let size = egui::vec2(900.0, 600.0);
    let mut harness = Harness::new();
    harness
        .editor
        .open_document("worlds/terrain.cyworld")
        .unwrap();
    let empty = harness.frame("editor-terrain", size, Vec::new());
    for label in ["Terrain", "Create terrain", "Undo", "Redo"] {
        assert!(
            empty.labels.iter().any(|drawn| drawn == label),
            "the scaffolded empty state lacks {label:?}: {:?}",
            empty.labels
        );
    }
    let created = harness.frame(
        "editor-terrain",
        size,
        vec![click_named(&empty, "Create terrain")],
    );
    assert!(matches!(
        created.intents.as_slice(),
        [Intent::Invoke(command, _)] if command == "terrain.create"
    ));

    let mut harness = terrain_harness();
    let first = harness.frame("editor-terrain", size, Vec::new());
    for label in [
        "Brush",
        "Material layers",
        "Modifier stack",
        "Terrain surface",
    ] {
        assert!(
            first.labels.iter().any(|drawn| drawn == label),
            "the terrain body lacks {label:?}: {:?}",
            first.labels
        );
    }
    let undone = harness.frame("editor-terrain", size, vec![click_named(&first, "Undo")]);
    assert!(
        matches!(
            undone.intents.as_slice(),
            [Intent::Invoke(command, _)] if command == "edit.undo"
        ),
        "the header's Undo is the history's undo: {:?}",
        undone.intents
    );
}

#[test]
fn a_terrain_refusal_is_shown_in_the_specialised_diagnostics_area() {
    let size = egui::vec2(900.0, 600.0);
    let mut harness = terrain_harness();
    let quiet = harness.frame("editor-terrain", size, Vec::new());
    let refusal = "Paint requires a material layer.";
    assert!(!quiet.labels.iter().any(|label| label.contains(refusal)));

    harness.inputs.terrain_problem = Some(refusal.into());
    let refused = harness.frame("editor-terrain", size, Vec::new());
    assert!(
        refused.labels.iter().any(|label| label == refusal),
        "the refusal is a readable row, not paint on the canvas: {:?}",
        refused.labels
    );
    assert!(
        refused.labels.iter().any(|label| label == "✕"),
        "the row carries its glyph as well as its colour: {:?}",
        refused.labels
    );
}

/// Regression: the terrain body laid its paint field out inside the horizontal row that holds the
/// controls, so the heading, the hint and the field sat side by side and the field was squeezed
/// into the strip left over at the right edge (about a sixth of the panel at 900 points).
#[test]
fn the_terrain_brush_field_fills_the_space_beside_the_controls() {
    let size = egui::vec2(900.0, 600.0);
    let mut harness = terrain_harness();
    let evidence = harness.frame("editor-terrain", size, Vec::new());
    let (_, field) = evidence
        .bounds
        .iter()
        .find(|(label, _)| label == "Terrain brush field")
        .unwrap_or_else(|| panic!("no brush field in {:?}", evidence.labels));
    let controls = 250.0_f64.min(f64::from(size.x) * 0.42);
    assert!(
        field.width() > (f64::from(size.x) - controls) * 0.8,
        "the brush field is {:.0} points wide beside {controls:.0} points of controls",
        field.width()
    );
    assert!(field.height() > 180.0, "{field:?}");
}

// --- The physics panel ------------------------------------------------------------------------------

/// An open world with a door and a frame, each carrying a dynamic body, and the door selected.
fn physics_harness() -> (
    Harness,
    cy_editor_core::ids::NodeId,
    cy_editor_core::ids::NodeId,
) {
    use cy_editor_commands::Arguments;
    use cy_editor_core::value::Value;

    let mut harness = Harness::new();
    harness
        .editor
        .open_document("worlds/joints.cyworld")
        .expect("a new world opens");
    let mut nodes = Vec::new();
    for _ in 0..2 {
        harness
            .registry
            .invoke(
                "scene.create-entity",
                &harness.scope,
                &mut harness.editor,
                &Arguments::new(),
            )
            .expect("an entity");
        let node = harness
            .editor
            .selection
            .get()
            .nodes()
            .next()
            .expect("the created entity is selected");
        harness
            .registry
            .invoke(
                "scene.add-body",
                &harness.scope,
                &mut harness.editor,
                &Arguments::new().with("entity", Value::Text(node.to_string())),
            )
            .expect("a body");
        nodes.push(node);
    }
    harness
        .registry
        .invoke(
            "edit.select",
            &harness.scope,
            &mut harness.editor,
            &Arguments::new().with("entity", Value::Text(nodes[0].to_string())),
        )
        .expect("select the door");
    (harness, nodes[0], nodes[1])
}

#[test]
fn the_physics_panel_toggles_an_engine_layer_through_its_registered_command() {
    let size = egui::vec2(900.0, 600.0);
    let mut harness = Harness::new();
    let first = harness.frame("physics", size, Vec::new());
    for label in [
        "Physics",
        "Undo",
        "Redo",
        "Viewport physics layers",
        "Colliders",
        "Joints",
    ] {
        assert!(
            first.labels.iter().any(|drawn| drawn == label),
            "the physics panel lacks {label:?}: {:?}",
            first.labels
        );
    }
    let toggled = harness.frame("physics", size, vec![click_named(&first, "Colliders")]);
    assert!(
        matches!(
            toggled.intents.as_slice(),
            [Intent::Invoke(command, arguments)]
                if command == "viewport.physics.colliders"
                    && arguments.text("state") == Some("on")
        ),
        "a layer is its command, not a flag the panel sets: {:?}",
        toggled.intents
    );
}

#[test]
fn the_physics_panel_scopes_the_ragdoll_tool_in_its_diagnostics_area() {
    let mut harness = Harness::new();
    let evidence = harness.frame("physics", egui::vec2(900.0, 600.0), Vec::new());
    assert!(
        evidence
            .labels
            .iter()
            .any(|label| label.contains("cannot load one yet")),
        "the ragdoll limit is stated rather than an empty section: {:?}",
        evidence.labels
    );
    assert!(evidence.labels.iter().any(|label| label == "▲"));
}

#[test]
fn a_selected_body_is_offered_a_joint_and_the_add_is_one_registered_command() {
    let size = egui::vec2(900.0, 700.0);
    let (mut harness, door, _) = physics_harness();
    let form = harness.frame("physics", size, Vec::new());
    assert!(
        form.labels.iter().any(|label| label == "Add joint"),
        "{:?}",
        form.labels
    );
    let added = harness.frame("physics", size, vec![click_named(&form, "Add joint")]);
    let [Intent::Invoke(command, arguments)] = added.intents.as_slice() else {
        panic!("one intent: {:?}", added.intents);
    };
    assert_eq!(command, "physics.joint.add");
    assert_eq!(arguments.text("entity"), Some(door.to_string().as_str()));
    assert_eq!(arguments.text("kind"), Some("hinge"));
}

#[test]
fn a_joined_body_shows_the_fields_its_kind_reads_and_removes_through_the_command() {
    use cy_editor_commands::Arguments;
    use cy_editor_core::value::Value;

    let size = egui::vec2(900.0, 900.0);
    let (mut harness, door, frame) = physics_harness();
    harness
        .registry
        .invoke(
            "physics.joint.add",
            &harness.scope,
            &mut harness.editor,
            &Arguments::new()
                .with("entity", Value::Text(door.to_string()))
                .with("kind", Value::Text("hinge".into()))
                .with("target", Value::Text(frame.to_string())),
        )
        .expect("a hinge");
    let evidence = harness.frame("physics", size, Vec::new());
    for label in [
        "Motor force cap",
        "Minimum angle, in radians",
        "Remove joint",
    ] {
        assert!(
            evidence.labels.iter().any(|drawn| drawn == label),
            "a hinge lacks {label:?}: {:?}",
            evidence.labels
        );
    }
    assert!(
        !evidence
            .labels
            .iter()
            .any(|drawn| drawn == "Travel minimum"),
        "a hinge offers six-axis fields it does not read"
    );
    let removed = harness.frame(
        "physics",
        size,
        vec![click_named(&evidence, "Remove joint")],
    );
    assert!(matches!(
        removed.intents.as_slice(),
        [Intent::Invoke(command, _)] if command == "physics.joint.remove"
    ));
}

fn pointer(position: egui::Pos2, pressed: Option<bool>) -> Vec<egui::Event> {
    let mut events = vec![egui::Event::PointerMoved(position)];
    if let Some(pressed) = pressed {
        events.push(egui::Event::PointerButton {
            pos: position,
            button: egui::PointerButton::Primary,
            pressed,
            modifiers: egui::Modifiers::NONE,
        });
    }
    events
}

/// Drag across the brush field and return the intents of the frame the gesture finished in.
#[expect(
    clippy::cast_possible_truncation,
    reason = "AccessKit reports bounds in f64 points; egui takes f32"
)]
fn drag_across_the_brush_field(harness: &mut Harness, size: egui::Vec2) -> Vec<Intent> {
    let evidence = harness.frame("editor-terrain", size, Vec::new());
    let (_, field) = evidence
        .bounds
        .iter()
        .find(|(label, _)| label == "Terrain brush field")
        .unwrap_or_else(|| panic!("no brush field in {:?}", evidence.labels));
    let at = |fraction: f64| {
        egui::pos2(
            (field.x0 + field.width() * fraction) as f32,
            (field.y0 + field.height() * 0.5) as f32,
        )
    };
    harness.frame("editor-terrain", size, pointer(at(0.3), Some(true)));
    harness.frame("editor-terrain", size, pointer(at(0.4), None));
    harness.frame("editor-terrain", size, pointer(at(0.5), None));
    harness
        .frame("editor-terrain", size, pointer(at(0.5), Some(false)))
        .intents
}

#[test]
fn the_terrain_brush_offers_every_tool_the_engine_applies() {
    let size = egui::vec2(900.0, 600.0);
    let mut harness = terrain_harness();
    let evidence = harness.frame("editor-terrain", size, Vec::new());
    for label in ["Raise", "Lower", "Smooth", "Flatten", "Paint", "Hole"] {
        assert!(
            evidence.labels.iter().any(|drawn| drawn == label),
            "the brush lacks {label:?}: {:?}",
            evidence.labels
        );
    }
    assert!(
        evidence
            .labels
            .iter()
            .any(|label| label.contains("No engine attached")),
        "without a runtime the field says why it shows no engine surface: {:?}",
        evidence.labels
    );
}

/// Regression: the panel keeps the first material layer selected whenever the stack has one, and
/// it sent that layer with every stroke — so once a layer existed every raise, lower, smooth and
/// flatten gesture was refused with "sculpt tools do not take a material layer".
#[test]
fn a_sculpt_stroke_with_a_layer_in_the_stack_names_no_layer_and_is_accepted() {
    let size = egui::vec2(900.0, 600.0);
    let mut harness = terrain_harness();
    let terrain = harness.editor.edited_terrain().expect("a terrain root");
    harness
        .registry
        .invoke(
            "terrain.layer.add",
            &harness.scope,
            &mut harness.editor,
            &cy_editor_commands::Arguments::new()
                .with(
                    "terrain",
                    cy_editor_core::value::Value::Text(terrain.to_string()),
                )
                .with("name", cy_editor_core::value::Value::Text("Rock".into()))
                .with(
                    "material",
                    cy_editor_core::value::Value::Text("materials/rock.cymat".into()),
                ),
        )
        .expect("a layer");

    for (tool, names_layer) in [("raise", false), ("hole", false), ("paint", true)] {
        harness.inputs.terrain_tool = tool.into();
        let intents = drag_across_the_brush_field(&mut harness, size);
        let [Intent::Invoke(command, arguments)] = intents.as_slice() else {
            panic!("one gesture is one command: {intents:?}");
        };
        assert_eq!(command, "terrain.stroke.commit");
        assert_eq!(
            arguments.text("layer").is_some(),
            names_layer,
            "{tool} and its layer"
        );
        harness
            .registry
            .invoke(command, &harness.scope, &mut harness.editor, arguments)
            .unwrap_or_else(|problem| panic!("{tool} stroke refused: {problem}"));
    }
}

// --- The audio mixer (#29) -------------------------------------------------------------------------

/// The engine's own audio fixtures, which `cy_test_integration_editor_backend_audio` produced.
fn engine_audio(name: &str) -> Vec<u8> {
    let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../src/editor_backend/tests/data")
        .join(name);
    std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
}

/// A project directory that removes itself.
struct Project(std::path::PathBuf);

impl Project {
    fn new(name: &str) -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos());
        let path =
            std::env::temp_dir().join(format!("cy-shell-{name}-{}-{unique}", std::process::id()));
        std::fs::create_dir_all(path.join("audio/cues")).unwrap();
        std::fs::create_dir_all(path.join("game/audio")).unwrap();
        Self(path)
    }
}

impl Drop for Project {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// Answer one audio request with an engine reply over a real session, the way the window does.
fn engine_answers(harness: &mut Harness, operation: &str, reply: Vec<u8>) {
    use cy_editor_protocol::{Message, ServiceEventKind, Session, write_frame};
    let (editor_reader, mut runtime_writer) = std::io::pipe().unwrap();
    let (_runtime_reader, editor_writer) = std::io::pipe().unwrap();
    harness.editor.runtime =
        cy_editor_services::RuntimeSession::over(Session::over(editor_reader, editor_writer));
    let request = harness
        .editor
        .backend
        .audio
        .request(&harness.editor.runtime, operation, Vec::new())
        .unwrap()
        .expect("nothing else is in flight");
    write_frame(
        &mut runtime_writer,
        &Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 1,
            payload: reply,
        }
        .encode(),
    )
    .unwrap();
    let mut notifications = cy_editor_services::NotificationService::new();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while harness.editor.backend.audio.pending() && std::time::Instant::now() < deadline {
        for message in harness.editor.runtime.pump(&mut notifications) {
            assert!(harness.editor.backend.accept(&message).is_none());
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    assert!(
        !harness.editor.backend.audio.pending(),
        "the engine's reply was not taken"
    );
}

/// A world, the canonical mixer on disk, a cue, the engine's vocabulary and its last state.
fn audio_harness(project: &Project) -> Harness {
    std::fs::write(
        project.0.join("game/audio/mixer.cymixer"),
        engine_audio("audio_mixer_v1.cymixer"),
    )
    .unwrap();
    std::fs::write(
        project.0.join("audio/cues/ping.cycue"),
        engine_audio("audio_cue_v1.cycue"),
    )
    .unwrap();
    let mut harness = Harness::new();
    harness.editor = Editor::new(Actor::human("sound-designer"))
        .with_project(cy_editor_services::ProjectService::new(&project.0));
    harness
        .editor
        .open_document("worlds/audio.cyworld")
        .unwrap();
    harness.specialised.install_audio_vocabulary(
        cy_editor_services::audio::AudioVocabulary::decode(&engine_audio(
            "audio_capabilities_v1.wire",
        ))
        .unwrap(),
    );
    engine_answers(
        &mut harness,
        "audio.state.get",
        engine_audio("audio_state_v1.wire"),
    );
    harness
}

const AUDIO: &str = "editor-audio-buses-and-mixing";

#[test]
fn the_audio_mixer_shows_the_engines_graph_and_levels() {
    let project = Project::new("mixer");
    let mut harness = audio_harness(&project);
    let evidence = harness.frame(AUDIO, egui::vec2(1000.0, 700.0), Vec::new());
    for label in [
        "Audio Mixer",
        "Buses",
        "Master",
        "Music",
        "SFX",
        "Reverb",
        "mute Music",
        "solo SFX",
        "SFX level",
        "Cues",
        "audio/cues/ping.cycue",
        "Undo",
    ] {
        assert!(
            evidence.labels.iter().any(|drawn| drawn == label),
            "the mixer lacks {label:?}: {:?}",
            evidence.labels
        );
    }
    assert!(
        evidence
            .labels
            .iter()
            .any(|label| label.starts_with("Engine mixer: null backend")),
        "the backend is named, so a null mix is never mistaken for a device: {:?}",
        evidence.labels
    );
    assert!(
        evidence.labels.iter().any(|label| label.ends_with(" dB")),
        "SFX mixed the preview, so its engine level is a number: {:?}",
        evidence.labels
    );
    assert!(
        harness.inputs.audio.seen,
        "the window keeps the meters live while it is drawn"
    );
}

#[test]
fn mixer_gestures_are_the_registered_audio_commands() {
    let project = Project::new("gestures");
    let size = egui::vec2(1000.0, 700.0);
    let mut harness = audio_harness(&project);
    let first = harness.frame(AUDIO, size, Vec::new());
    let muted = harness.frame(AUDIO, size, vec![click_named(&first, "mute Music")]);
    match muted.intents.as_slice() {
        [Intent::Invoke(command, arguments)] => {
            assert_eq!(command, "audio.bus.flag");
            assert_eq!(arguments.text("name"), Some("Music"));
            assert_eq!(arguments.text("flag"), Some("mute"));
            assert_eq!(
                arguments.get("enabled"),
                Some(&cy_editor_core::value::Value::Bool(true))
            );
        }
        other => panic!("one flag command, got {other:?}"),
    }
    let previewed = harness.frame(AUDIO, size, vec![click_named(&first, "Preview")]);
    assert!(
        matches!(
            previewed.intents.as_slice(),
            [Intent::Invoke(command, arguments)]
                if command == "audio.cue.preview"
                    && arguments.text("reference") == Some("audio/cues/ping.cycue")
        ),
        "{:?}",
        previewed.intents
    );
    harness.inputs.audio.new_bus = "Voice".into();
    let ready = harness.frame(AUDIO, size, Vec::new());
    let added = harness.frame(AUDIO, size, vec![click_named(&ready, "Add bus")]);
    assert!(
        matches!(
            added.intents.as_slice(),
            [Intent::Invoke(command, arguments)]
                if command == "audio.bus.add" && arguments.text("name") == Some("Voice")
        ),
        "{:?}",
        added.intents
    );
    let selected = harness.frame(AUDIO, size, vec![click_named(&first, "SFX")]);
    assert!(selected.intents.is_empty());
    let chain = harness.frame(AUDIO, size, Vec::new());
    for label in [
        "SFX sends",
        "Send to Reverb",
        "SFX effect chain",
        "low-pass",
    ] {
        assert!(
            chain.labels.iter().any(|drawn| drawn == label),
            "the selected bus lacks {label:?}: {:?}",
            chain.labels
        );
    }
}

#[test]
fn the_audio_mixer_empty_states_name_what_would_fill_them() {
    let project = Project::new("empty");
    let size = egui::vec2(900.0, 600.0);
    let mut harness = Harness::new();
    harness.editor = Editor::new(Actor::human("sound-designer"))
        .with_project(cy_editor_services::ProjectService::new(&project.0));
    harness
        .editor
        .open_document("worlds/audio.cyworld")
        .unwrap();
    let empty = harness.frame(AUDIO, size, Vec::new());
    let create = harness.frame(AUDIO, size, vec![click_named(&empty, "Create mixer")]);
    assert!(matches!(
        create.intents.as_slice(),
        [Intent::Invoke(command, _)] if command == "audio.mixer.create"
    ));
    std::fs::write(
        project.0.join("game/audio/mixer.cymixer"),
        engine_audio("audio_mixer_v1.cymixer"),
    )
    .unwrap();
    let refused = harness.frame(AUDIO, size, Vec::new());
    assert!(
        refused
            .labels
            .iter()
            .any(|label| label.contains("`audio` owes it")),
        "without the engine's vocabulary the mixer refuses by name: {:?}",
        refused.labels
    );
}
