// SPDX-License-Identifier: MIT
//! The Navigation panel and the armed viewport pick, headless. Issue #28, tasks 5.2 to 5.4.
//!
//! The panel is a client: every assertion here is about which `navigation.*` command a gesture
//! asks for, and what the registry then records. Engine answers arrive the way they do in the
//! window, as service events on the editor's runtime session, over a pipe with nobody on the far
//! side but this test.

use cy_editor_commands::{Arguments, Registry, Scope};
use cy_editor_core::Actor;
use cy_editor_core::codec::Writer;
use cy_editor_core::value::Value;
use cy_editor_interface::SpecialisedEditors;
use cy_editor_interface::panels::{PanelKey, PanelTitles};
use cy_editor_interface::shell::{Shell, panel_title};
use cy_editor_interface::thumbnails::Thumbnails;
use cy_editor_protocol::{FrameId, Message, ServiceEventKind, Session, read_frame};
use cy_editor_reflection::Catalogue;
use cy_editor_services::Editor;
use cy_editor_services::navmesh::{NAV_LINK, NAV_OBSTACLE, NavmeshSettings};
use cy_editor_services::runtime::RuntimeSession;
use cy_editor_shell::panels::navigation_baking::PickTarget;
use cy_editor_shell::panels::{Inputs, Intent, Panels};
use cy_editor_shell::viewport_link::ViewportLink;
use cy_editor_viewmodels::{
    AssetBrowserViewModel, DiffViewModel, HierarchyViewModel, HistoryViewModel, MergeViewModel,
    SettingsViewModel, SourceControlViewModel, SourceWorkspaceViewModel,
};
use cy_editor_viewport::{FrameImage, PresentedFrame, ViewState};
use egui_dock::TabViewer;

const PANEL: &str = "editor-navigation-baking";

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
    merge: MergeViewModel,
    thumbnails: Thumbnails,
    titles: PanelTitles,
    link: ViewportLink,
    inputs: Inputs,
    ctx: egui::Context,
    /// The far end of the runtime session, kept open so writes to it succeed.
    runtime: Option<(std::io::PipeReader, std::io::PipeWriter)>,
}

struct Frame {
    click_targets: Vec<(String, egui::accesskit::TreeId, egui::accesskit::NodeId)>,
    labels: Vec<String>,
    intents: Vec<Intent>,
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
        Self {
            editor: Editor::new(Actor::human("designer")),
            registry,
            scope: Scope::unrestricted(),
            shell,
            specialised: SpecialisedEditors::new().expect("specialised editors"),
            hierarchy: HierarchyViewModel::new(),
            history: HistoryViewModel::new(),
            settings: SettingsViewModel::new(),
            source_control: SourceControlViewModel::new(),
            asset_browser: AssetBrowserViewModel::new(),
            source_workspace: SourceWorkspaceViewModel::new(),
            diff: DiffViewModel::new(),
            merge: MergeViewModel::new(),
            thumbnails: Thumbnails::new(8),
            titles,
            link: ViewportLink::idle(),
            inputs: Inputs::default(),
            ctx,
            runtime: None,
        }
    }

    /// A harness with a world open and navigation world 1 created through the registry.
    fn with_world() -> Self {
        let mut harness = Self::new();
        harness
            .editor
            .open_document("worlds/nav.cyworld")
            .expect("a document");
        harness.invoke("navigation.world.create", &Arguments::new());
        harness
    }

    /// Attach a runtime session whose far end is this test.
    fn attach_runtime(&mut self) {
        let (editor_reader, runtime_writer) = std::io::pipe().unwrap();
        let (runtime_reader, editor_writer) = std::io::pipe().unwrap();
        self.editor.runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));
        self.runtime = Some((runtime_reader, runtime_writer));
    }

    fn invoke(&mut self, id: &str, arguments: &Arguments) {
        self.editor
            .invoke(&self.registry, id, &self.scope, arguments)
            .unwrap_or_else(|problem| panic!("{id}: {problem}"));
    }

    /// What the window does with a frame's intents: every `Invoke` goes through the registry.
    fn apply(&mut self, intents: Vec<Intent>) {
        for intent in intents {
            match intent {
                Intent::Invoke(id, arguments) => self.invoke(&id, &arguments),
                other => panic!("the navigation panel asked for {other:?}"),
            }
        }
    }

    fn frame(&mut self, panel: &str, events: Vec<egui::Event>) -> Frame {
        let raw = egui::RawInput {
            screen_rect: Some(egui::Rect::from_min_size(
                egui::Pos2::ZERO,
                egui::vec2(1200.0, 1600.0),
            )),
            events,
            ..Default::default()
        };
        let mut intents: Vec<Intent> = Vec::new();
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
            merge,
            thumbnails,
            titles,
            link,
            inputs,
            ctx,
            ..
        } = self;
        shell.inspector.refresh(editor);
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
                    merge,
                    thumbnails,
                    link,
                    titles,
                    inputs,
                    intents: &mut intents,
                    tab_rects: Vec::new(),
                    panel_rects: Vec::new(),
                };
                let mut key = PanelKey::new(panel).expect("a built-in panel");
                panels.ui(ui, &mut key);
            });
        });
        let update = output
            .platform_output
            .accesskit_update
            .take()
            .expect("AccessKit was enabled");
        output.textures_delta.clear();
        let labels = update
            .nodes
            .iter()
            .filter_map(|(_, node)| node.label().or_else(|| node.value()).map(str::to_string))
            .collect();
        let click_targets = update
            .nodes
            .iter()
            .filter(|(_, node)| node.supports_action(egui::accesskit::Action::Click))
            .filter_map(|(id, node)| {
                node.label()
                    .map(|label| (label.to_owned(), update.tree_id, *id))
            })
            .collect();
        Frame {
            click_targets,
            labels,
            intents,
        }
    }

    /// Draw the panel, then press the control labelled `label` and return what it asked for.
    fn press(&mut self, label: &str) -> Vec<Intent> {
        let shown = self.frame(PANEL, Vec::new());
        let (_, tree, node) = shown
            .click_targets
            .iter()
            .find(|(name, _, _)| name == label)
            .unwrap_or_else(|| panic!("{label} has no click target: {:?}", shown.click_targets))
            .clone();
        let click = egui::Event::AccessKitActionRequest(egui::accesskit::ActionRequest {
            action: egui::accesskit::Action::Click,
            target_tree: tree,
            target_node: node,
            data: None,
        });
        self.frame(PANEL, vec![click]).intents
    }

    /// Put a presented frame on the focused viewport, as the transport would.
    fn present_frame(&mut self) {
        let focused = self.editor.viewports.focused_mut();
        let state = ViewState::default();
        focused.stream.accept(
            PresentedFrame::new(FrameId::from_raw(7), state, FrameImage::Surface(1), 0),
            0,
        );
    }

    /// A primary click in the middle of the viewport panel.
    fn click_viewport(&mut self) -> Vec<Intent> {
        let at = egui::pos2(600.0, 700.0);
        let mut intents = self
            .frame("viewport", vec![egui::Event::PointerMoved(at)])
            .intents;
        for pressed in [true, false] {
            intents.extend(
                self.frame(
                    "viewport",
                    vec![egui::Event::PointerButton {
                        pos: at,
                        button: egui::PointerButton::Primary,
                        pressed,
                        modifiers: egui::Modifiers::NONE,
                    }],
                )
                .intents,
            );
        }
        intents
    }

    /// Answer the navigation.point.pick the editor just sent with a hit at `point`.
    fn answer_pick(&mut self, point: [f32; 3]) {
        let (reader, _) = self.runtime.as_mut().expect("a runtime");
        // Nothing else writes to the runtime here (the tests never pump), so the next frame must
        // be the pick. Reading on past anything else would wait forever on a pick never sent.
        let frame = read_frame(reader).unwrap().expect("a frame");
        let request = match Message::decode(&frame).unwrap() {
            Message::ServiceRequest {
                request, operation, ..
            } if operation == "navigation.point.pick" => request,
            other => panic!("the runtime was sent {other:?} instead of a navmesh pick"),
        };
        let mut answer = Writer::new();
        answer.u8(1);
        for lane in point {
            answer.f32(lane);
        }
        answer.u64(1);
        answer.f32(3.0);
        let event = Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 1,
            payload: answer.finish(),
        };
        assert!(self.editor.navmesh.accept(&event).is_none());
    }

    fn history(&self) -> Vec<String> {
        self.editor
            .documents
            .get(self.editor.workspace.active().unwrap())
            .unwrap()
            .history()
            .entries()
            .iter()
            .map(|entry| entry.name.clone())
            .collect()
    }
}

fn invoked(intents: &[Intent]) -> Vec<(&str, &Arguments)> {
    intents
        .iter()
        .map(|intent| match intent {
            Intent::Invoke(id, arguments) => (id.as_str(), arguments),
            other => panic!("the navigation panel asked for {other:?}"),
        })
        .collect()
}

fn only(intents: &[Intent], id: &str) -> Arguments {
    let calls = invoked(intents);
    assert_eq!(calls.len(), 1, "expected one {id}, got {intents:?}");
    assert_eq!(calls[0].0, id, "{intents:?}");
    calls[0].1.clone()
}

#[test]
fn with_no_world_open_the_panel_says_so_accessibly() {
    let mut harness = Harness::new();
    let shown = harness.frame(PANEL, Vec::new());
    assert!(
        shown
            .labels
            .iter()
            .any(|label| label.contains("No world is open.")),
        "{:?}",
        shown.labels
    );
    assert!(shown.intents.is_empty());
}

#[test]
fn bake_settings_overlays_and_add_buttons_push_their_navigation_commands() {
    let mut harness = Harness::with_world();

    let bake = only(&harness.press("Bake"), "navigation.bake");
    assert_eq!(bake.get("world"), Some(&Value::Int(1)));

    let status = only(
        &harness.press("Check for changes"),
        "navigation.bake.status",
    );
    assert_eq!(status.get("refresh"), Some(&Value::Bool(true)));

    let mut draft = NavmeshSettings::find(
        harness
            .editor
            .documents
            .get(harness.editor.workspace.active().unwrap())
            .unwrap(),
        1,
    )
    .unwrap()
    .settings;
    draft.agent_radius = 0.25;
    harness.inputs.navigation.draft = Some((1, draft));
    let settings = only(&harness.press("Apply settings"), "navigation.settings.set");
    assert_eq!(
        settings.text("values"),
        Some("agent_radius=0.25"),
        "the gesture sends exactly the edited setting"
    );

    let overlay = only(&harness.press("polygons"), "navigation.overlay.set");
    assert_eq!(overlay.text("overlays"), Some("polygons"));

    for (label, command) in [
        ("Add Nav Mesh Surface", "navigation.surface.add"),
        ("Add Nav Obstacle", "navigation.obstacle.add"),
        ("Add Nav Area", "navigation.area.add"),
        ("Add Nav Link", "navigation.link.add"),
    ] {
        let added = only(&harness.press(label), command);
        assert_eq!(added.get("world"), Some(&Value::Int(1)), "{label}");
    }
}

#[test]
fn a_world_without_navigation_offers_to_create_one() {
    let mut harness = Harness::new();
    harness
        .editor
        .open_document("worlds/nav.cyworld")
        .expect("a document");
    only(
        &harness.press("Create navigation world"),
        "navigation.world.create",
    );
}

#[test]
fn an_armed_viewport_click_picks_a_navmesh_point_instead_of_selecting() {
    let mut harness = Harness::with_world();
    harness.present_frame();
    assert!(harness.press("Pick start").is_empty());
    assert_eq!(harness.inputs.navigation.armed, Some(PickTarget::PathStart));

    let clicked = harness.click_viewport();
    let pick = only(&clicked, "navigation.point.pick");
    assert_eq!(pick.get("frame"), Some(&Value::Int(7)));
    assert_eq!(pick.get("world"), Some(&Value::Int(1)));
    assert!(
        matches!(pick.get("x"), Some(Value::Float(x)) if *x > 0.0),
        "{pick:?}"
    );
    assert_eq!(harness.inputs.navigation.armed, None, "one click, one pick");
    assert!(
        said(&mut harness).is_empty(),
        "the armed click also went to selection picking"
    );
}

/// What the editor has told the user. A selection pick with no runtime attached says so, which
/// is how these tests see that a click went to selection.
fn said(harness: &mut Harness) -> Vec<String> {
    let mut cursor = cy_editor_core::observe::Cursor::default();
    harness
        .editor
        .notifications
        .drain_from(&mut cursor)
        .iter()
        .map(|notification| notification.message.clone())
        .collect()
}

#[test]
fn an_unarmed_viewport_click_still_selects() {
    let mut harness = Harness::with_world();
    harness.present_frame();
    let clicked = harness.click_viewport();
    assert!(
        invoked(&clicked)
            .iter()
            .all(|(id, _)| *id != "navigation.point.pick"),
        "{clicked:?}"
    );
    assert!(
        !said(&mut harness).is_empty(),
        "an unarmed click did not reach selection picking"
    );
}

#[test]
fn picked_path_endpoints_feed_the_path_query() {
    let mut harness = Harness::with_world();
    harness.attach_runtime();
    harness.present_frame();
    for (button, point) in [
        ("Pick start", [1.0, 0.0, 1.0]),
        ("Pick end", [6.0, 0.0, 2.0]),
    ] {
        harness.press(button);
        let intents = harness.click_viewport();
        only(&intents, "navigation.point.pick");
        harness.apply(intents);
        // A frame drawn before the engine answers must keep waiting, not read an older point.
        harness.frame(PANEL, Vec::new());
        assert!(
            harness.inputs.navigation.awaiting.is_some(),
            "the panel settled {button} before the engine answered"
        );
        harness.answer_pick(point);
        let settled = harness.frame(PANEL, Vec::new());
        assert!(settled.intents.is_empty());
    }
    assert_eq!(harness.inputs.navigation.start, Some([1.0, 0.0, 1.0]));
    assert_eq!(harness.inputs.navigation.end, Some([6.0, 0.0, 2.0]));
    let query = only(&harness.press("Find path"), "navigation.path.query");
    assert_eq!(query.get("start"), Some(&Value::Vec3([1.0, 0.0, 1.0])));
    assert_eq!(query.get("end"), Some(&Value::Vec3([6.0, 0.0, 2.0])));
}

#[test]
fn two_picks_in_link_mode_record_one_link_add() {
    let mut harness = Harness::with_world();
    harness.attach_runtime();
    harness.present_frame();
    assert!(harness.press("Place link in viewport").is_empty());
    let before = harness.history().len();

    let first = harness.click_viewport();
    only(&first, "navigation.point.pick");
    harness.apply(first);
    harness.answer_pick([1.0, 0.0, 2.0]);
    let settled = harness.frame(PANEL, Vec::new());
    assert!(settled.intents.is_empty(), "one endpoint records nothing");
    assert_eq!(harness.inputs.navigation.armed, Some(PickTarget::LinkTo));

    let second = harness.click_viewport();
    only(&second, "navigation.point.pick");
    harness.apply(second);
    harness.answer_pick([5.0, 1.0, 6.0]);
    let settled = harness.frame(PANEL, Vec::new()).intents;
    let link = only(&settled, "navigation.link.add");
    assert_eq!(link.text("values"), Some("from=1, 0, 2; to=5, 1, 6"));
    harness.apply(settled);

    let history = harness.history();
    assert_eq!(history.len(), before + 1, "{history:?}");
    let document = harness
        .editor
        .documents
        .get(harness.editor.workspace.active().unwrap())
        .unwrap();
    let component = document.schema().type_named(NAV_LINK).unwrap();
    let from = component.field_named("from").unwrap().id;
    let to = component.field_named("to").unwrap().id;
    let component = component.id;
    let node = document
        .content()
        .nodes()
        .find(|node| document.content().has_component(*node, component))
        .expect("a NavLink node");
    let state = document.content().node(node).unwrap();
    let fields = &state.components[&component];
    assert_eq!(fields.get(&from), Some(&Value::Vec3([1.0, 0.0, 2.0])));
    assert_eq!(fields.get(&to), Some(&Value::Vec3([5.0, 1.0, 6.0])));
}

#[test]
fn the_inspector_shows_and_undoably_edits_a_nav_obstacle() {
    let mut harness = Harness::with_world();
    harness.invoke("navigation.obstacle.add", &Arguments::new());
    let catalogue = {
        let document = harness
            .editor
            .documents
            .get(harness.editor.workspace.active().unwrap())
            .unwrap();
        Catalogue::of_document(document.schema())
    };
    harness.shell.describe_with(catalogue);
    let shown = harness.frame("inspector", Vec::new());
    for label in [NAV_OBSTACLE, "shape.radius"] {
        assert!(
            shown.labels.iter().any(|shown| shown.contains(label)),
            "the Inspector does not show {label}: {:?}",
            shown.labels
        );
    }

    let row = harness
        .shell
        .inspector
        .rows()
        .into_iter()
        .find(|row| row.name == "shape.radius")
        .map(|row| (row.component, row.field))
        .expect("a shape.radius row");
    let before = harness.history().len();
    harness
        .shell
        .inspector
        .begin_edit(row.0, row.1, Value::Float(2.5));
    harness
        .shell
        .inspector
        .commit_edit(&mut harness.editor)
        .expect("the edit commits");
    assert_eq!(harness.history().len(), before + 1);
    let radius = |harness: &Harness| {
        let document = harness
            .editor
            .documents
            .get(harness.editor.workspace.active().unwrap())
            .unwrap();
        let node = harness.editor.selection.get().nodes().next().unwrap();
        document.content().node(node).unwrap().components[&row.0]
            .get(&row.1)
            .cloned()
    };
    assert_eq!(radius(&harness), Some(Value::Float(2.5)));
    harness.invoke("edit.undo", &Arguments::new());
    assert_ne!(
        radius(&harness),
        Some(Value::Float(2.5)),
        "undo kept the edit"
    );
}

#[test]
fn panel_gestures_record_the_history_the_same_gestures_record_over_mcp() {
    // The panel's gestures, applied the way the window applies them.
    let mut panel = Harness::new();
    panel
        .editor
        .open_document("worlds/nav.cyworld")
        .expect("a document");
    let created = panel.press("Create navigation world");
    panel.apply(created);
    panel.frame(PANEL, Vec::new());
    let mut draft = panel.inputs.navigation.draft.expect("a draft once shown").1;
    draft.agent_radius = 0.25;
    draft.tile_size = 8.0;
    panel.inputs.navigation.draft = Some((1, draft));
    let settings = panel.press("Apply settings");
    panel.apply(settings);
    let obstacle = panel.press("Add Nav Obstacle");
    panel.apply(obstacle);
    let overlay = panel.press("polygons");
    panel.apply(overlay);

    // The same gestures as an agent sends them: text arguments through the same registry, which
    // is what `tools/call` does (cy-editor-mcp's navigation wire tests pin that half).
    let mut agent = Harness::new();
    agent
        .editor
        .open_document("worlds/nav.cyworld")
        .expect("a document");
    for (id, arguments) in [
        ("navigation.world.create", vec![]),
        (
            "navigation.settings.set",
            vec![("values", "agent_radius=0.25; tile_size=8")],
        ),
        ("navigation.obstacle.add", vec![]),
        ("navigation.overlay.set", vec![("overlays", "polygons")]),
    ] {
        let arguments = arguments
            .into_iter()
            .fold(Arguments::new(), |arguments, (key, value)| {
                arguments.with(key, Value::Text(value.into()))
            });
        agent.invoke(id, &arguments);
    }

    assert_eq!(panel.history(), agent.history());
    let settings = |harness: &Harness| {
        NavmeshSettings::find(
            harness
                .editor
                .documents
                .get(harness.editor.workspace.active().unwrap())
                .unwrap(),
            1,
        )
        .unwrap()
    };
    let (from_panel, from_agent) = (settings(&panel), settings(&agent));
    assert_eq!(from_panel.settings, from_agent.settings);
    assert_eq!(from_panel.overlay, from_agent.overlay);
}

#[test]
fn the_panel_draws_in_the_specialised_frame_with_its_problem_in_the_diagnostics_area() {
    let mut harness = Harness::with_world();
    let quiet = harness.frame(PANEL, Vec::new());
    for label in ["Navigation", "Undo", "Redo", "Navigation world"] {
        assert!(
            quiet.labels.iter().any(|drawn| drawn == label),
            "the scaffolded panel lacks {label:?}: {:?}",
            quiet.labels
        );
    }
    let problem = "The click missed the navmesh, so nothing was placed.";
    assert!(!quiet.labels.iter().any(|label| label == problem));
    harness.inputs.navigation.problem = Some(problem.into());
    let shown = harness.frame(PANEL, Vec::new());
    assert!(
        shown.labels.iter().any(|label| label == problem),
        "the problem is a readable diagnostics row: {:?}",
        shown.labels
    );
}
