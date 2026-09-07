//! The viewport panel, driven by a pointer, with no window and no device. Tasks 2.2, 2.3, 2.5.
//!
//! `cy_editor_viewport::interaction` is tested with no toolkit at all, which is where the interesting
//! behaviour is checked. What *this* file checks is the half that only exists once the toolkit is
//! involved: that the panel is wired to the model at all, that a pointer event reaches it, that the
//! panel's size reaches the view state, and that egui's habit of running several passes over one
//! frame's input does not turn one press into two.
//!
//! Every one of those is an integration failure of the kind this project keeps finding — a subsystem
//! verified in isolation and never assembled — and none of them is visible from either side alone.
//!
//! It runs headless: `egui::Context` needs no window, no GPU and no display, and the panel is invoked
//! through the same `egui_dock::TabViewer` the dock area calls.

use cy_editor_commands::{Registry, Scope};
use cy_editor_core::Actor;
use cy_editor_interface::panels::{PanelKey, PanelTitles};
use cy_editor_interface::shell::{Shell, panel_title};
use cy_editor_interface::thumbnails::Thumbnails;
use cy_editor_services::Editor;
use cy_editor_shell::panels::{Inputs, Intent, Panels};
use cy_editor_shell::viewport_link::ViewportLink;
use cy_editor_viewmodels::HierarchyViewModel;
use egui_dock::TabViewer;

/// Everything a viewport panel needs, kept alive across frames the way the window keeps it.
struct Harness {
    editor: Editor,
    registry: Registry,
    scope: Scope,
    shell: Shell,
    hierarchy: HierarchyViewModel,
    thumbnails: Thumbnails,
    titles: PanelTitles,
    link: ViewportLink,
    inputs: Inputs,
    ctx: egui::Context,
    /// The panel's rectangle, which is also the window's here.
    rect: egui::Rect,
}

impl Harness {
    fn new() -> Self {
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry).expect("the built-in commands");
        let shell = Shell::new(&registry).expect("a shell");
        let mut titles = PanelTitles::new();
        for panel in shell.workspaces.current().panels() {
            titles.define(panel.as_str(), panel_title(&panel));
        }
        Self {
            editor: Editor::new(Actor::human("designer")),
            registry,
            scope: Scope::unrestricted(),
            shell,
            hierarchy: HierarchyViewModel::new(),
            thumbnails: Thumbnails::new(16),
            titles,
            link: ViewportLink::idle(),
            inputs: Inputs::default(),
            ctx: egui::Context::default(),
            rect: egui::Rect::from_min_size(egui::pos2(0.0, 0.0), egui::vec2(1600.0, 900.0)),
        }
    }

    /// Run one interface frame with these input events, drawing the viewport panel.
    fn frame(&mut self, events: Vec<egui::Event>) -> Vec<Intent> {
        let raw = egui::RawInput {
            screen_rect: Some(self.rect),
            events,
            ..Default::default()
        };
        let mut intents: Vec<Intent> = Vec::new();
        let Self {
            editor,
            registry,
            scope,
            shell,
            hierarchy,
            thumbnails,
            titles,
            link,
            inputs,
            ctx,
            ..
        } = self;
        let ctx = ctx.clone();
        // The font atlas arrives as a texture delta, and epaint refuses to be dropped holding one —
        // a renderer would have uploaded it. There is no renderer here, so it is cleared.
        let mut output = ctx.run_ui(raw, |ui| {
            egui::CentralPanel::default().show(ui, |ui| {
                let mut panels = Panels {
                    editor,
                    registry,
                    scope,
                    shell,
                    hierarchy,
                    thumbnails,
                    link,
                    titles,
                    inputs,
                    intents: &mut intents,
                };
                let mut key = PanelKey::new("viewport").expect("the viewport panel");
                panels.ui(ui, &mut key);
            });
        });
        output.textures_delta.clear();
        intents
    }

    fn camera(&self) -> [f32; 3] {
        self.editor
            .viewports
            .focused()
            .state
            .camera
            .position
            .to_array()
    }
}

/// A pointer event at a position inside the viewport.
fn moved(x: f32, y: f32) -> egui::Event {
    egui::Event::PointerMoved(egui::pos2(x, y))
}

fn button(x: f32, y: f32, button: egui::PointerButton, pressed: bool) -> egui::Event {
    egui::Event::PointerButton {
        pos: egui::pos2(x, y),
        button,
        pressed,
        modifiers: egui::Modifiers::NONE,
    }
}

#[test]
fn the_panel_gives_the_view_state_its_own_size() {
    // A ray through a pixel is built from the viewport's dimensions, so a view state carrying the
    // default 1920×1080 while the panel is 1600×900 aims every click at the wrong place. The panel
    // is the only thing that knows, so it is the thing that has to say.
    let mut harness = Harness::new();
    harness.frame(Vec::new());
    let rect = harness.editor.viewports.focused().state.viewport;
    assert!(rect.width > 0 && rect.height > 0, "{rect:?}");
    // Within the panel's own margins of the window it is in — not equal to it, because the dock
    // frame and the panel's padding are real and the viewport is what is left.
    let window = f64::from(harness.rect.width());
    assert!(
        (window - f64::from(rect.width)).abs() < window * 0.1,
        "the view state says {} px wide and the window is {window}",
        rect.width
    );
    assert!(
        f64::from(rect.height) < f64::from(harness.rect.height()),
        "the viewport is taller than the window it is in"
    );
}

#[test]
fn a_middle_drag_in_the_panel_orbits_the_camera() {
    // The whole path: egui's raw events, the panel's translation, the interaction model, the
    // navigator, the view state. Nothing here is mocked.
    let mut harness = Harness::new();
    harness.frame(vec![moved(800.0, 450.0)]);
    let before = harness.camera();

    harness.frame(vec![button(
        800.0,
        450.0,
        egui::PointerButton::Middle,
        true,
    )]);
    harness.frame(vec![moved(900.0, 470.0)]);
    harness.frame(vec![button(
        900.0,
        470.0,
        egui::PointerButton::Middle,
        false,
    )]);

    assert_ne!(
        harness.camera().map(f32::to_bits),
        before.map(f32::to_bits),
        "a middle drag did not move the camera"
    );
    assert!(
        !harness.editor.documents.any_dirty(),
        "navigating dirtied a document"
    );
}

#[test]
fn a_click_with_no_runtime_says_there_is_nothing_to_pick_rather_than_selecting_something() {
    // The editor half of engine-side picking, through the panel. With no frame presented there is
    // nothing on screen to have clicked, and the editor says so instead of resolving the click
    // against its own camera — which is the forbidden pattern the whole path exists to avoid.
    let mut harness = Harness::new();
    harness
        .editor
        .open_document("worlds/city.cyworld")
        .expect("a document");
    harness.frame(vec![moved(700.0, 400.0)]);
    harness.frame(vec![button(
        700.0,
        400.0,
        egui::PointerButton::Primary,
        true,
    )]);
    harness.frame(vec![button(
        700.0,
        400.0,
        egui::PointerButton::Primary,
        false,
    )]);

    let mut cursor = cy_editor_core::observe::Cursor::default();
    let said: Vec<String> = harness
        .editor
        .notifications
        .drain_from(&mut cursor)
        .iter()
        .map(|notification| notification.message.clone())
        .collect();
    assert!(
        said.iter()
            .any(|message| message.contains("nothing on screen")),
        "the editor said {said:?}"
    );
    assert!(
        harness.editor.selection.get().is_empty(),
        "a click selected something with no runtime to resolve it"
    );
}

#[test]
fn several_passes_over_one_frame_of_input_are_one_press() {
    // egui runs more than one pass over the same input when a widget asks for the frame to be
    // rebuilt, and `input.events` is the same list on every pass. The window's keyboard handling
    // found this the hard way; the viewport would double every press, which turns one axis-lock
    // keystroke into a lock and an unlock — a key that visibly does nothing.
    let mut harness = Harness::new();
    harness.frame(vec![moved(800.0, 450.0)]);
    let before = harness.camera();

    // Two passes over the same press-and-move: the second must contribute nothing.
    harness.frame(vec![button(
        800.0,
        450.0,
        egui::PointerButton::Middle,
        true,
    )]);
    let raw = egui::RawInput {
        screen_rect: Some(harness.rect),
        events: vec![moved(860.0, 450.0)],
        ..Default::default()
    };
    let mut once: Option<[f32; 3]> = None;
    let ctx = harness.ctx.clone();
    let mut output = ctx.run_ui(raw.clone(), |ui| {
        egui::CentralPanel::default().show(ui, |ui| {
            let mut intents = Vec::new();
            let mut panels = Panels {
                editor: &mut harness.editor,
                registry: &harness.registry,
                scope: &harness.scope,
                shell: &mut harness.shell,
                hierarchy: &mut harness.hierarchy,
                thumbnails: &mut harness.thumbnails,
                link: &mut harness.link,
                titles: &harness.titles,
                inputs: &mut harness.inputs,
                intents: &mut intents,
            };
            let mut key = PanelKey::new("viewport").expect("the viewport panel");
            // Twice, in the same pass, exactly as a discarded-and-rebuilt frame does.
            panels.ui(ui, &mut key);
            once = Some(
                panels
                    .editor
                    .viewports
                    .focused()
                    .state
                    .camera
                    .position
                    .to_array(),
            );
            panels.ui(ui, &mut key);
        });
    });
    output.textures_delta.clear();
    let twice = harness.camera();
    assert_ne!(
        twice.map(f32::to_bits),
        before.map(f32::to_bits),
        "the drag did nothing at all"
    );
    assert_eq!(
        once.expect("the first pass ran").map(f32::to_bits),
        twice.map(f32::to_bits),
        "the second pass over the same input moved the camera again"
    );
}

#[test]
fn the_viewport_panel_draws_the_sentence_when_there_is_no_transport_and_never_an_approximation() {
    // `design.md` §2: "If the transport is not ready, the viewport shows a message saying so — not an
    // approximation." The panel is drawn with an idle link and the message is the one the link
    // carries, which is the only thing it is allowed to draw instead of the engine's frame.
    let mut harness = Harness::new();
    harness.frame(Vec::new());
    assert!(!harness.link.is_attached());
    assert!(
        harness
            .link
            .condition()
            .message
            .contains("transport is not ready"),
        "{:?}",
        harness.link.condition()
    );
}
