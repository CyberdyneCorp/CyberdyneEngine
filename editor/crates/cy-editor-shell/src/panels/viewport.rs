//! The viewport: the engine's image, the chrome that floats over it, and what a pointer does to it.
//! Tasks 1.7, 2.1, 2.2, 2.3, 2.4.7, 2.5.
//!
//! --- CHROME IS OVERLAY, NOT A SECOND TOOLBAR --------------------------------------------------------
//!
//! `editor-visual-language`: *"Projection, rendering mode and show flags float **in** the viewport.
//! No second full-width toolbar. Every pixel not spent on chrome is viewport."* So every control
//! here is drawn inside the viewport's own rectangle, in the corner
//! `cy_editor_visual::chrome::Overlay::corner` assigns it, and there is no row of buttons above the
//! image. `Chrome::shown_for_capture` is what a screenshot uses, so an overlay cannot end up in a
//! capture that is meant to show the render.
//!
//! --- WHAT IS DRAWN HERE AND WHAT IS NOT -------------------------------------------------------------
//!
//! Drawn here: the engine's frame or the sentence explaining its absence, the overlay controls, the
//! ambient performance readout, the manipulation's numbers, and the **view-orientation widget** —
//! which `editor-viewport-and-gizmos` lists among *viewport controls*, so it is interface chrome and
//! the editor draws it.
//!
//! **Not drawn here: the transform gizmo.** The capability specification assigns "gizmo geometry
//! generation, depth handling, and drawing" to the engine, and an editor that drew its own arrows
//! would be the second renderer the same specification forbids — with the extra failure that its
//! arrows would disagree with the picking that grabs them. So the gizmo appears when a runtime draws
//! one, and the handles this panel hit-tests are the ones the runtime published with the frame
//! (`cy_editor_viewport::layout`). With no runtime there is no gizmo, exactly as there is no image,
//! and for the same reason.
//!
//! --- AND NOTHING HERE DECIDES ANYTHING --------------------------------------------------------------
//!
//! Pointer and key handling is `cy_editor_viewport::interaction`, which has no toolkit and is tested
//! with none; this file translates events in and draws outcomes out. The one thing it does decide is
//! where the *panel* is, which is the one thing a model cannot know.

use cy_editor_services::notifications::Notification;
use cy_editor_viewport::interaction::{Context, Outcome};
use cy_editor_viewport::overlay::{
    OrientationWidget, ViewPreset, WIDGET_DEFAULT_SIZE, WidgetGesture, WidgetState, widget_size,
};
// Two halves of one widget, deliberately in two crates: `cy_editor_visual` says what it looks like
// — the stubs, their colours, how much quieter than a transform gizmo it is — and
// `cy_editor_viewport` says what it does. Aliased rather than renamed because they are the same
// widget, and a reader should see that.
use cy_editor_viewport::state::ViewportRect;
use cy_editor_visual::chrome::{Corner, Overlay};
use cy_editor_visual::colour::{Semantic, Surface};
use cy_editor_visual::density::TextRole;
use cy_editor_visual::orientation::OrientationWidget as WidgetAppearance;

use super::{Panels, numeric, secondary};
use crate::theme;

/// The widget's screen size in points.
///
/// `editor-visual-language` fixes the range and the default: "constant screen size, 56–96 px with 72
/// the default". Constant is the load-bearing half — a widget that shrank with the camera would be a
/// widget that is hardest to read exactly when orientation is hardest to judge.
const ORIENTATION_SIZE: f32 = WIDGET_DEFAULT_SIZE;

/// Draw the viewport, and let it be driven.
pub(super) fn show(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let theme = panels.shell.theme;
    let rect = ui.available_rect_before_wrap();
    ui.painter().rect_filled(
        rect,
        egui::CornerRadius::ZERO,
        theme::surface(theme, Surface::Sunken),
    );

    // The image, or the sentence — AND THE SENTENCE OVER THE IMAGE when a runtime that was
    // delivering frames has stopped. M5.5's gate found the third case missing: after a runtime dies
    // the last complete frame is deliberately retained, so `texture()` still answers `Some` and the
    // `else` branch below is never reached. The user got a frozen picture and no explanation, which
    // is the silent-fallback shape this project has now paid for twice.
    let drawn = if let Some(texture) = panels.link.texture() {
        ui.painter().image(
            texture,
            rect,
            egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
            egui::Color32::WHITE,
        );
        if says_why_over_the_image(true, panels.link.is_live()) {
            let condition = panels.link.condition().clone();
            let metrics = panels.shell.metrics();
            let font = egui::FontId::proportional(metrics.text(TextRole::Body));
            let galley = ui.painter().layout(
                condition.message.clone(),
                font,
                theme::role(theme, condition.role),
                rect.width() - (metrics.gap() * 4.0),
            );
            // A band rather than bare text: the retained frame underneath is arbitrary content and
            // a sentence drawn straight onto it is unreadable against the wrong image.
            let band = egui::Rect::from_min_size(
                rect.left_top() + egui::vec2(0.0, 0.0),
                egui::vec2(rect.width(), galley.size().y + (metrics.gap() * 2.0)),
            );
            ui.painter().rect_filled(
                band,
                egui::CornerRadius::ZERO,
                theme::surface(theme, Surface::Raised).gamma_multiply(0.92),
            );
            ui.painter().galley(
                egui::pos2(
                    band.center().x - (galley.size().x / 2.0),
                    band.top() + metrics.gap(),
                ),
                galley,
                theme::role(theme, condition.role),
            );
        }
        true
    } else {
        let condition = panels.link.condition().clone();
        let metrics = panels.shell.metrics();
        ui.painter().text(
            rect.center(),
            egui::Align2::CENTER_CENTER,
            &condition.message,
            egui::FontId::proportional(metrics.text(TextRole::Body)),
            theme::role(theme, condition.role),
        );
        false
    };

    let response = ui.interact(
        rect,
        ui.id().with("cy-viewport-surface"),
        egui::Sense::click_and_drag(),
    );
    drive(panels, ui, rect, &response);
    overlays(panels, ui, rect, drawn);
}

/// Feed the frame's pointer and keys to the viewport's own interaction model, and act on what it says.
fn drive(panels: &mut Panels<'_>, ui: &mut egui::Ui, rect: egui::Rect, response: &egui::Response) {
    let events = panels.inputs.viewport.events(ui, rect, response);
    // The gizmo the runtime drew into the frame now on screen, so that a press grabs the handle the
    // user is looking at. See `published_gizmo`.
    panels
        .inputs
        .interaction
        .set_layout(published_gizmo(panels.editor));
    let actor = cy_editor_commands::CommandContext::actor(panels.editor);
    let document_id = panels.editor.workspace.active();

    // The panel knows how big it is and the model does not, so the viewport's rectangle is refreshed
    // here — before any event is resolved, because a ray built from last frame's size points
    // somewhere the user is not looking.
    // The origin stays at zero: the viewport model reasons in the viewport's own pixels, and
    // `crate::viewport_input` has already subtracted the panel's position from the pointer.
    let size = ViewportRect {
        x: 0,
        y: 0,
        width: to_pixels(rect.width()),
        height: to_pixels(rect.height()),
    };
    let cy_editor_services::Editor {
        documents,
        selection,
        viewports,
        notifications,
        ..
    } = &mut *panels.editor;
    let (focused, gizmos) = viewports.interacting();
    focused.state.viewport = size;
    // What the editor is asking to see, carried to the runtime so that the frame it sends back is
    // labelled with the view it was rendered for. See `ViewportLink::publish_view_state`.
    panels.link.publish_view_state(focused.state.clone());

    // With no document open there is nothing to manipulate, but the camera still moves: an empty
    // editor whose viewport was frozen would be one a user cannot tell from a broken one.
    let nodes: Vec<cy_editor_core::ids::NodeId> = selection.get().nodes().collect();
    let Some(document) = document_id.and_then(|id| documents.get_mut(id)) else {
        let mut context = Context {
            viewport: focused,
            document: &mut panels.inputs.no_world,
            registry: gizmos,
            binding: None,
            nodes: &[],
            actor,
        };
        for event in events {
            panels.inputs.interaction.handle(&mut context, event);
        }
        return;
    };

    let binding = cy_editor_viewport::gizmo::TransformBinding::of_schema(document.schema());
    let mut context = Context {
        viewport: focused,
        document,
        registry: gizmos,
        binding,
        nodes: &nodes,
        actor,
    };
    let mut outcomes = Vec::new();
    for event in events {
        outcomes.push(panels.inputs.interaction.handle(&mut context, event));
    }
    for outcome in outcomes {
        report(outcome, notifications);
    }
}

/// The gizmo geometry the runtime published with the frame now on screen.
///
/// **Read, never computed.** `editor-viewport-and-gizmos` assigns gizmo geometry to the engine so
/// that what is grabbed is what was drawn; a layout computed here would be a second geometry, and
/// the first time it disagreed with the picture the user would grab one handle and drag another.
///
/// It arrives through `cy_editor_services::RuntimeMirror`, which asks the runtime once a frame for
/// the gizmo on the current selection and refuses an answer that names a frame the viewport is not
/// showing. `None` means one of three ordinary things — no runtime is attached, no frame has
/// arrived yet, or nothing is selected — and all three correctly draw no gizmo.
fn published_gizmo(editor: &cy_editor_services::Editor) -> Option<cy_editor_viewport::GizmoLayout> {
    editor.mirror.layout().cloned()
}

/// What the window does with what an interaction produced.
///
/// Deliberately little. A camera move is not news, a hover is not news, and a manipulation reports
/// itself in the viewport's own corner rather than as a notification — `editor-ui-ux` requires that
/// notifications not interrupt, and a toast per drag would be a wall of them. Only a refusal and an
/// unanswerable pick are worth saying out loud.
fn report(outcome: Outcome, notifications: &mut cy_editor_services::NotificationService) {
    match outcome {
        Outcome::Refused(problem) => {
            notifications.post(Notification::error(problem.what.clone(), *problem));
        }
        Outcome::Pick(_request, _mode) => {
            // The request is built and carries the frame it was aimed at; what resolves it is the
            // engine, because picking is engine-side so that what is picked is what was rendered.
            // Until a runtime answers, saying so once is the honest outcome — inventing a hit from
            // the editor's own camera is the forbidden pattern this whole path avoids.
            notifications.post(Notification::info(
                "Picking is resolved by the runtime; none is attached to answer this click.",
            ));
        }
        Outcome::NothingToPick => notifications.post(Notification::info(
            "No frame has arrived yet, so there is nothing on screen to have clicked.",
        )),
        Outcome::Nothing
        | Outcome::CameraMoved
        | Outcome::HoverChanged(_)
        | Outcome::DragBegan(_)
        | Outcome::DragAdvanced(_)
        | Outcome::DragFinished { .. }
        | Outcome::DragCancelled => {}
    }
}

/// Logical points to whole pixels, which is what a viewport rectangle counts.
fn to_pixels(points: f32) -> u32 {
    #[allow(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "a panel is between zero and a few thousand points across"
    )]
    let pixels = points.max(0.0).round() as u32;
    pixels.max(1)
}

/// The floating controls, each in the corner the visual language assigns it.
fn overlays(panels: &mut Panels<'_>, ui: &mut egui::Ui, rect: egui::Rect, drawn: bool) {
    let metrics = panels.shell.metrics();
    let inset = metrics.padding() * 1.5;
    let shown = panels.shell.chrome.shown();

    for corner in [
        Corner::TopLeft,
        Corner::TopRight,
        Corner::BottomLeft,
        Corner::BottomRight,
    ] {
        let group: Vec<Overlay> = shown
            .iter()
            .copied()
            .filter(|overlay| overlay.corner() == corner)
            .collect();
        // An empty overlay is a box with nothing in it, which reads as a defect. The bottom-right
        // corner is drawn only when there is a manipulation, a hovered handle or a selection to
        // type numbers into.
        if group.is_empty() && !(corner == Corner::BottomRight && manipulation_has_content(panels))
        {
            continue;
        }
        let (anchor, align) = match corner {
            Corner::TopLeft => (
                rect.left_top() + egui::vec2(inset, inset),
                egui::Align2::LEFT_TOP,
            ),
            Corner::TopRight => (
                rect.right_top() + egui::vec2(-inset, inset),
                egui::Align2::RIGHT_TOP,
            ),
            Corner::BottomLeft => (
                rect.left_bottom() + egui::vec2(inset, -inset),
                egui::Align2::LEFT_BOTTOM,
            ),
            Corner::BottomRight => (
                rect.right_bottom() + egui::vec2(-inset, -inset),
                egui::Align2::RIGHT_BOTTOM,
            ),
        };
        egui::Area::new(egui::Id::new(("viewport-overlay", corner)))
            .fixed_pos(anchor)
            .pivot(align)
            .order(egui::Order::Foreground)
            .show(ui.ctx(), |ui| {
                ui.set_max_width(rect.width() * 0.4);
                // Flat and charcoal: the overlay is a surface step over the viewport, not a card.
                egui::Frame::NONE
                    .fill(theme::overlay_fill(panels.shell.theme))
                    .corner_radius(egui::CornerRadius::same(3))
                    .inner_margin(egui::Margin::symmetric(
                        theme::margin(metrics.padding()),
                        theme::margin(metrics.padding() * 0.5),
                    ))
                    .show(ui, |ui| match corner {
                        Corner::TopRight => orientation(panels, ui),
                        Corner::BottomLeft => performance(panels, ui, drawn),
                        Corner::BottomRight if manipulation_has_content(panels) => {
                            manipulation(panels, ui);
                        }
                        Corner::TopLeft => state_of_the_view(panels, ui),
                        Corner::BottomRight => labels(panels, ui, &group),
                    });
            });
    }
}

/// What the viewport is showing: the projection, the debug view, and the transform tools in force.
///
/// The reference's top-left group. Text rather than icons because these are *states* — "Wireframe",
/// "Local", "Bounds" — and a state drawn as an icon is a state a user has to remember the meaning of.
fn state_of_the_view(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let shell = &*panels.shell;
    let viewport = panels.editor.viewports.focused();
    let mut line = vec![
        if viewport.state.projection.is_orthographic() {
            "Orthographic".to_string()
        } else {
            "Perspective".to_string()
        },
        viewport.state.view_mode.label().to_string(),
        viewport.gizmo_mode.name().to_string(),
        viewport.gizmo_space.label().to_string(),
        viewport.gizmo_pivot.label().to_string(),
    ];
    if viewport.snap.modes.grid {
        line.push(format!("Snap {:.2} m", viewport.snap.grid));
    }
    // One line, not two. The overlays enabled in this corner — projection, render mode, show
    // flags, debug visualisation — are exactly the things this line reports the *value* of, and a
    // second line naming them would say "Projection" above the word "Perspective".
    ui.label(secondary(shell, line.join(" · ")));
}

/// The ambient performance readout: is this frame affordable, and is the image honest.
///
/// The per-subsystem breakdown belongs to the Profiler panel — see `panels::diagnostics`, and the
/// second thing the reference images get wrong. What is added here is the **degradation** line, which
/// `editor-viewport-and-gizmos` requires: "a lower-quality image is never mistaken for the real one".
fn performance(panels: &mut Panels<'_>, ui: &mut egui::Ui, drawn: bool) {
    let cost = panels.shell.frame_cost();
    let advisory = panels
        .editor
        .viewports
        .focused()
        .advisory(monotonic_micros());
    ui.vertical(|ui| {
        ui.label(numeric(
            panels.shell,
            format!("Frame {:>6.2} ms", cost.total().as_secs_f64() * 1_000.0),
        ));
        if let Some(advisory) = advisory {
            // Warning rather than secondary: a stale or degraded image is a thing to notice, and the
            // requirement is that it is never mistaken for the shipping appearance.
            ui.label(
                egui::RichText::new(advisory)
                    .size(panels.shell.metrics().text(TextRole::Secondary))
                    .color(theme::role(panels.shell.theme, Semantic::Warning)),
            );
        }
        if let Some(counters) = panels.link.counters() {
            ui.label(secondary(panels.shell, counters));
        } else if !drawn {
            ui.label(secondary(panels.shell, "No engine frame"));
        }
    });
}

/// The manipulation's numbers, while one is happening.
///
/// "A manipulation SHALL show numeric feedback of the delta **and** the resulting value." Both, in
/// the corner nearest the hand, and nothing at all when nothing is being manipulated — an overlay
/// that is always there is an overlay that is never read.
fn manipulation(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let shell = &*panels.shell;
    let interaction = &*panels.inputs.interaction;
    let Some(feedback) = interaction.feedback().cloned() else {
        if let Some(handle) = interaction.hovered() {
            ui.label(secondary(shell, format!("{} handle", handle.name())));
        }
        numeric_entry(panels, ui);
        return;
    };
    ui.vertical(|ui| {
        ui.label(numeric(shell, feedback.delta.clone()));
        ui.label(numeric(shell, feedback.value.clone()));
        let lock = interaction.lock().label();
        if !lock.is_empty() {
            ui.label(secondary(shell, format!("Locked to {lock}")));
        }
    });
}

/// The overlays enabled in a corner, named. What a corner falls back to when it has no reading of
/// its own to show — a box with nothing in it reads as a defect.
fn labels(panels: &Panels<'_>, ui: &mut egui::Ui, group: &[Overlay]) {
    let text = group
        .iter()
        .map(|overlay| overlay.label())
        .collect::<Vec<_>>()
        .join(" · ");
    ui.label(secondary(panels.shell, text));
}

/// Whether the bottom-right corner has anything to say this frame.
fn manipulation_has_content(panels: &Panels<'_>) -> bool {
    if panels.inputs.interaction.feedback().is_some()
        || panels.inputs.interaction.hovered().is_some()
    {
        return true;
    }
    let Some(id) = panels.editor.workspace.active() else {
        return false;
    };
    let Some(document) = panels.editor.documents.get(id) else {
        return false;
    };
    !panels.editor.selection.get().is_empty()
        && cy_editor_viewport::gizmo::TransformBinding::of_schema(document.schema()).is_some()
}

/// The reference's **Numeric Input**: nine fields, three rows, one transaction each.
///
/// "Numeric entry SHALL be available for **every** manipulation, and SHALL accept expressions and
/// units." It is drawn beside the manipulation's own numbers rather than in a panel of its own,
/// because a viewport control belongs *in* the viewport — and it is only drawn when there is
/// something selected to type about, so an empty scene spends no chrome on it.
///
/// Everything it does is `cy_editor_viewport::entry`: reading a mixed selection, parsing an
/// expression with units, converting a quaternion to three degrees and back, and writing through the
/// same transaction path a drag uses. Nothing here computes a value.
fn numeric_entry(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    use cy_editor_viewport::entry::{self, Row};

    let Some(document_id) = panels.editor.workspace.active() else {
        return;
    };
    let nodes: Vec<cy_editor_core::ids::NodeId> = panels.editor.selection.get().nodes().collect();
    if nodes.is_empty() {
        return;
    }
    let Some(document) = panels.editor.documents.get(document_id) else {
        return;
    };
    let Some(binding) = cy_editor_viewport::gizmo::TransformBinding::of_schema(document.schema())
    else {
        return;
    };
    let fields = entry::read(document, binding, &nodes);
    let metrics = panels.shell.metrics();
    let mut committed: Option<(Row, usize, String)> = None;

    for row in Row::ALL {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = metrics.padding() * 0.5;
            ui.label(secondary(panels.shell, row.label()));
            for (axis, field) in row_fields(&fields, row).into_iter().enumerate() {
                let editing = panels
                    .inputs
                    .transform_entry
                    .as_ref()
                    .is_some_and(|(held, index, _)| *held == row && *index == axis);
                let mut text = if editing {
                    panels
                        .inputs
                        .transform_entry
                        .as_ref()
                        .map(|(_, _, text)| text.clone())
                        .unwrap_or_default()
                } else {
                    field.text(row)
                };
                let response = ui.add(
                    egui::TextEdit::singleline(&mut text)
                        .desired_width(FIELD_WIDTH)
                        .font(egui::FontId::monospace(metrics.text(TextRole::Body)))
                        .hint_text(["X", "Y", "Z"][axis]),
                );
                if response.changed() {
                    panels.inputs.transform_entry = Some((row, axis, text.clone()));
                }
                // Enter or leaving the field applies it, which is what every numeric field in every
                // tool does; anything else means a typed value that silently did not happen.
                if editing
                    && (response.lost_focus()
                        || ui.input(|input| input.key_pressed(egui::Key::Enter)))
                {
                    committed = Some((row, axis, text));
                }
            }
        });
    }

    let Some((row, axis, text)) = committed else {
        return;
    };
    panels.inputs.transform_entry = None;
    let actor = cy_editor_commands::CommandContext::actor(panels.editor);
    let Some(document) = panels.editor.documents.get_mut(document_id) else {
        return;
    };
    match entry::apply(document, binding, &nodes, row, axis, &text, actor) {
        Ok(_changed) => {}
        Err(problem) => panels
            .editor
            .notifications
            .post(Notification::error(problem.what.clone(), problem)),
    }
}

/// How wide a numeric field is, in points. Three of them and a label fit the overlay's width.
const FIELD_WIDTH: f32 = 64.0;

/// One row's three fields.
fn row_fields(
    fields: &cy_editor_viewport::entry::Fields,
    row: cy_editor_viewport::entry::Row,
) -> [cy_editor_viewport::entry::Field; 3] {
    fields.row(row)
}

/// The view-orientation widget: three axes, their labels, the current view, and no manipulator's form.
///
/// Interactive, per `docs/design/images/scene-orientation-gizmo.png`: click an axis to snap the
/// camera, drag anywhere to orbit, scroll to zoom, modifier-drag to pan, and cycle the view with the
/// arrows beside its name. **Every one of those moves the camera and records no transaction** — the
/// widget is handed a `Navigator` and a `ViewState` and has nothing else it could touch.
fn orientation(panels: &mut Panels<'_>, ui: &mut egui::Ui) {
    let widget = WidgetAppearance::new();
    let theme = panels.shell.theme;
    let metrics = panels.shell.metrics();
    let size = widget_size(ORIENTATION_SIZE);
    let (rect, response) =
        ui.allocate_exact_size(egui::vec2(size, size), egui::Sense::click_and_drag());
    let centre = rect.center();
    let reach = size * 0.34;

    // The stubs follow the camera. A widget drawn at a fixed isometric angle is a decoration: the
    // one question it exists to answer is "which way am I looking", and a picture that is the same
    // from every camera cannot answer it. The projection is orthographic — the camera's rotation
    // inverted, then X right and Y up — because a perspective divide on a 72-pixel widget buys
    // nothing and makes the near axis swing about.
    let camera = panels.editor.viewports.focused().state.camera.rotation;
    let inverse = camera.inverse();
    let axes = [
        cy_editor_viewport::Vec3::X,
        cy_editor_viewport::Vec3::Y,
        cy_editor_viewport::Vec3::Z,
    ];
    let projected = axes.map(|axis| inverse.rotate(axis));
    let screen = projected.map(|axis| egui::vec2(axis.x, -axis.y));
    let stubs = widget.stubs();
    // Farthest first, so the axis pointing away is drawn under the ones in front of it — the
    // widget's own depth handling, which it needs because it is drawn with a painter and not by
    // the engine.
    let mut order: Vec<usize> = (0..stubs.len().min(3)).collect();
    order.sort_by(|left, right| {
        projected[*left]
            .z
            .partial_cmp(&projected[*right].z)
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    let hovered = response.hover_pos().and_then(|position| {
        // Nearest the camera first, so a click where two stubs overlap takes the one in front.
        order.iter().rev().copied().find(|index| {
            let tip = centre + screen[*index] * reach;
            position.distance(tip) <= HANDLE_RADIUS
        })
    });

    // The four states, in one place, so that "normal, hover, active and disabled" is a value rather
    // than three scattered conditions. Disabled is the viewport that is looking through a game
    // camera: the widget would move a camera that is not the editor's to move.
    let state = if !panels.editor.viewports.focused().attachment.is_detached() {
        WidgetState::Disabled
    } else if response.is_pointer_button_down_on() || response.dragged() {
        WidgetState::Active
    } else if hovered.is_some() || response.hovered() {
        WidgetState::Hovered
    } else {
        WidgetState::Normal
    };

    let painter = ui.painter();
    let emphasis = match state {
        WidgetState::Normal => widget.opacity(),
        WidgetState::Hovered => (widget.opacity() * 1.3).min(1.0),
        WidgetState::Active => 1.0,
        WidgetState::Disabled => widget.opacity() * 0.45,
    };
    // The body: a small cube face at the centre, which is what the reference draws the axes out of.
    painter.rect_filled(
        egui::Rect::from_center_size(centre, egui::vec2(size * 0.2, size * 0.2)),
        egui::CornerRadius::same(2),
        theme::role(theme, Semantic::SecondaryText).gamma_multiply(emphasis * 0.35),
    );
    for index in order.iter().copied() {
        let stub = stubs[index];
        let direction = screen[index];
        let mut colour = theme::colour(stub.colour(theme)).gamma_multiply(emphasis);
        if hovered == Some(index) {
            // Emphasis by luminance, never by recolouring: the hue identifies the axis, which is the
            // same rule the transform gizmo's active state follows.
            colour = colour.gamma_multiply(1.4);
        }
        let tip = centre + direction * reach;
        painter.line_segment([centre, tip], egui::Stroke::new(2.0, colour));
        painter.circle_filled(tip, HANDLE_RADIUS * 0.5, colour);
        painter.text(
            tip + direction * (metrics.text(TextRole::Secondary) * 0.8),
            egui::Align2::CENTER_CENTER,
            stub.label(),
            egui::FontId::proportional(metrics.text(TextRole::Secondary)),
            colour,
        );
    }

    let gesture = gesture(&response, hovered, &stubs, ui.ctx());
    if state.accepts_input()
        && let Some(gesture) = gesture
    {
        let viewport = panels.editor.viewports.focused_mut();
        let (navigator, view) = (&mut viewport.navigator, &mut viewport.state);
        OrientationWidget.perform(navigator, view, gesture);
    }

    view_text(panels, ui, state);
}

/// How near a stub's tip a click counts, in points. The same "acquirable without precision" rule the
/// gizmo's handles follow.
const HANDLE_RADIUS: f32 = 10.0;

/// What the pointer did to the widget.
fn gesture(
    response: &egui::Response,
    hovered: Option<usize>,
    stubs: &[cy_editor_visual::orientation::Stub],
    ctx: &egui::Context,
) -> Option<WidgetGesture> {
    if response.clicked()
        && let Some(index) = hovered
        && let Some(stub) = stubs.get(index)
    {
        // The positive end of an axis looks *back* along it: clicking Y views from the top.
        return Some(WidgetGesture::ClickAxis(axis_view(*stub)));
    }
    if response.dragged() {
        let delta = response.drag_delta();
        let alt = ctx.input(|input| input.modifiers.alt);
        return Some(if alt {
            WidgetGesture::Pan {
                dx: delta.x,
                dy: delta.y,
            }
        } else {
            WidgetGesture::Orbit {
                dx: delta.x,
                dy: delta.y,
            }
        });
    }
    if response.hovered() {
        let notches = ctx.input(|input| input.smooth_scroll_delta.y) / 50.0;
        if notches != 0.0 {
            return Some(WidgetGesture::Zoom { notches });
        }
    }
    None
}

/// The view a stub's positive end corresponds to.
fn axis_view(stub: cy_editor_visual::orientation::Stub) -> cy_editor_viewport::ViewAxis {
    use cy_editor_viewport::ViewAxis;
    use cy_editor_visual::axis::Axis;
    match (stub.axis, stub.negative) {
        (Axis::X, false) => ViewAxis::Right,
        (Axis::X, true) => ViewAxis::Left,
        (Axis::Y, false) => ViewAxis::Top,
        (Axis::Y, true) => ViewAxis::Bottom,
        (Axis::Z, false) => ViewAxis::Front,
        (Axis::Z, true) => ViewAxis::Back,
    }
}

/// The current view as cycleable text: `‹ Persp ›`.
///
/// Text rather than a menu because it is a *readout* first — the reference's "Current View" — and the
/// arrows are how it becomes a control without becoming a second widget.
fn view_text(panels: &mut Panels<'_>, ui: &mut egui::Ui, state: WidgetState) {
    let metrics = panels.shell.metrics();
    let preset = ViewPreset::of_view(&panels.editor.viewports.focused().state);
    let mut cycle: Option<bool> = None;
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = metrics.padding() * 0.5;
        if ui.add_enabled(state.accepts_input(), arrow("‹")).clicked() {
            cycle = Some(false);
        }
        ui.label(
            egui::RichText::new(preset.label())
                .size(metrics.text(TextRole::Secondary))
                .color(theme::role(panels.shell.theme, Semantic::PrimaryText)),
        );
        if ui.add_enabled(state.accepts_input(), arrow("›")).clicked() {
            cycle = Some(true);
        }
    });
    if let Some(forward) = cycle {
        let viewport = panels.editor.viewports.focused_mut();
        let (navigator, view) = (&mut viewport.navigator, &mut viewport.state);
        OrientationWidget.perform(navigator, view, WidgetGesture::Cycle { forward });
    }
}

fn arrow(glyph: &str) -> egui::Button<'_> {
    egui::Button::new(glyph).frame(false)
}

/// The monotonic clock the transport reports its frames against.
/// The clock a frame's age is measured against.
///
/// **`CLOCK_MONOTONIC`, because that is the clock the announcement carries.** A runtime publishes
/// `submitted_nanos` from `clock_gettime(CLOCK_MONOTONIC)` — `cy_editor_viewport_transport::
/// session::monotonic_nanos` on the reference publisher's side and the same call in
/// `src/backends/viewport/` on the engine's — and `ViewportSession` turns it straight into
/// `PresentedFrame::produced_micros`. Subtracting it from a wall-clock reading is a subtraction of
/// two different epochs.
///
/// It read `SystemTime::now()` until M7, and the symptom is in every screenshot M5.5 and M6 took:
/// **"The runtime has not produced a frame for 1788285426327 ms; this image is stale"** over a
/// viewport that was in fact receiving sixty frames a second. That number is the Unix epoch in
/// milliseconds, which is what the difference between the two clocks is. The advisory that
/// `editor-viewport-and-gizmos` requires — "the editor SHALL surface when it is viewing a stale or
/// degraded stream" — was therefore on permanently, which is the same as being off: a warning that
/// is always showing is one nobody reads, and it would not have said anything when a runtime
/// really did stop.
///
/// `viewport_link::a_stale_advisory_uses_the_clock_the_announcement_carries` is the regression.
#[cfg(target_os = "linux")]
fn monotonic_micros() -> u64 {
    cy_editor_viewport_transport::session::monotonic_nanos() / 1_000
}

/// Elsewhere there is no transport and therefore no frame, so nothing is ever measured against
/// this. It is the wall clock so that the function exists and compiles; a platform that grows a
/// transport must give it that transport's clock.
#[cfg(not(target_os = "linux"))]
fn monotonic_micros() -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| {
            u64::try_from(elapsed.as_micros()).unwrap_or(u64::MAX)
        })
}

/// Whether the runtime's condition is drawn ON TOP of a retained image.
///
/// A predicate rather than an inline `if` because the wrong answer is invisible: M5.5's gate killed
/// a runtime, watched the editor hold its last complete frame exactly as designed, and found that
/// nothing on screen said so. The image is retained deliberately, so `has_image` stays true and the
/// "no image" branch that carries the sentence is never reached.
#[must_use]
pub(crate) const fn says_why_over_the_image(has_image: bool, is_live: bool) -> bool {
    has_image && !is_live
}

#[cfg(test)]
mod tests {
    use super::says_why_over_the_image;

    /// REGRESSION, M7 task 5b.1: the clock a frame's age is measured against.
    ///
    /// `monotonic_micros` read `SystemTime::now()` while a frame's `produced_micros` comes from the
    /// runtime's `CLOCK_MONOTONIC`, so every viewport in M5.5's and M6's screenshots carried "the
    /// runtime has not produced a frame for 1788285426327 ms" — the Unix epoch, in milliseconds —
    /// over an image that was arriving sixty times a second.
    ///
    /// The check is that the two clocks are the SAME clock, which is the whole of the defect. It is
    /// written as a bound rather than an equality because the two readings are taken a few hundred
    /// nanoseconds apart, and as a bound far below the stale budget (50 ms) so that a failure means
    /// the epochs differ rather than that the machine hiccuped.
    #[cfg(target_os = "linux")]
    #[test]
    fn a_frames_age_is_measured_on_the_clock_the_announcement_carries() {
        let announced = cy_editor_viewport_transport::session::monotonic_nanos() / 1_000;
        let measured = super::monotonic_micros();
        let difference = measured.abs_diff(announced);
        assert!(
            difference < 10_000,
            "the age clock and the announcement clock are {difference} us apart; a wall clock and \
             a monotonic one differ by an epoch"
        );
    }

    /// REGRESSION, M5.5's gate: the row that was wrong is `(true, false)`.
    #[test]
    fn a_retained_frame_from_a_dead_runtime_is_explained() {
        // A live runtime's image needs no sentence over it.
        assert!(!says_why_over_the_image(true, true));
        // THE DEFECT: an image is retained after the runtime died, and nothing said so.
        assert!(says_why_over_the_image(true, false));
        // With no image the panel draws the sentence in the middle instead, not over anything.
        assert!(!says_why_over_the_image(false, false));
        assert!(!says_why_over_the_image(false, true));
    }
}
