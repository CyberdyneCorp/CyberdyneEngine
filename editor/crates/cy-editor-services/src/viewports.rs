//! The viewports the editor is showing, and the commands that change them. Tasks 2.2, 2.4, 2.5.
//!
//! --- WHY A VIEWPORT CONTROL IS A COMMAND ------------------------------------------------------------
//!
//! Switching the transform mode changes nothing in the project, so the obvious place to put it is the
//! window — and the obvious place is wrong. `editor-rust-application`'s "One action, six entry
//! points" is not a statement about mutations; it is a statement about *actions*:
//!
//! > An action reachable only through a specific widget SHALL be a defect.
//!
//! `W` is a widget. So is the toolbar button, and so is the menu item. Registering these in the
//! command registry is what makes `viewport.transform-mode-rotate` reachable from the palette, from a
//! script, from a keymap the user rebound, and from an agent that has never seen the window — and it
//! is what makes the toolbar able to *derive* its buttons rather than hard-code them.
//!
//! Their effect class is [`EffectClass::Read`], which deserves a sentence because it looks wrong: they
//! change what the editor shows. They change nothing in the *project*, they record no transaction and
//! there is nothing for undo to reverse, so a caller weighing whether to confirm should not be asked
//! to. `editor-agent-interface`'s rule is that confirmation is rare and meaningful; a mode switch that
//! prompted would spend the user's attention on the cheapest thing in the editor.
//!
//! --- THE NAMES, AND WHY THEY ARE THE REFERENCE'S ----------------------------------------------------
//!
//! `docs/design/images/transform-gizmo.png` names the four modes Move, Rotate, Scale and Universal on
//! `W`, `E`, `R` and `T`, and the four pivot modes Pivot, Center, Bounds and Individual. Those are the
//! labels here, and the identifiers are the ones `cy_editor_interface::keymap::Keymap::unity` already
//! names — so the Unity preset and this table cannot disagree about what `W` does.

use cy_editor_commands::context::ViewportControls;
use cy_editor_commands::{Command, CommandContext, EffectClass, Metadata, Outcome, Registry};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_viewport::gizmo::{GizmoMode, GizmoRegistry, GizmoSpace, Pivot};
use cy_editor_viewport::math::{Bounds, Vec3};
use cy_editor_viewport::overlay::ViewPreset;
use cy_editor_viewport::transport::TransportKind;
use cy_editor_viewport::viewmode::{ALL_VIEW_MODES, ViewMode};
use cy_editor_viewport::viewport::{Viewport, ViewportId, Viewports};

/// The editor's viewports.
///
/// A service rather than a bare [`Viewports`] for one reason that is not ceremony: [`ViewportControls`]
/// is a trait in `cy-editor-commands` and [`Viewports`] is a type in `cy-editor-viewport`, and a crate
/// may not implement a foreign trait for a foreign type. The wrapper is where the editor's vocabulary —
/// "transform-mode", "pivot", "view-mode" — is turned into the viewport model's values, and it is the
/// only place that translation exists.
pub struct ViewportService {
    viewports: Viewports,
    focused: ViewportId,
    gizmos: GizmoRegistry,
}

impl Default for ViewportService {
    fn default() -> Self {
        Self::new()
    }
}

impl ViewportService {
    /// One perspective viewport, which is what an editor opens with.
    #[must_use]
    pub fn new() -> Self {
        let mut viewports = Viewports::new();
        // A shared texture: the runtime is another process and the image arrives without a copy.
        // The kind is what the viewport *asks* for; a transport that cannot provide it says so.
        let focused = viewports.open("Perspective", TransportKind::SharedTexture);
        viewports.focus(focused);
        Self {
            viewports,
            focused,
            gizmos: GizmoRegistry::with_builtins(),
        }
    }

    /// The manipulators available, including any a plugin registered.
    #[must_use]
    pub const fn gizmos(&self) -> &GizmoRegistry {
        &self.gizmos
    }

    /// Register a manipulator, which is how a plugin's gizmo becomes indistinguishable from a
    /// built-in one — see `cy_editor_viewport::gizmo::GizmoRegistry`.
    pub fn register_gizmo(&mut self, manipulator: Box<dyn cy_editor_viewport::gizmo::Manipulator>) {
        self.gizmos.register(manipulator);
    }

    /// The focused viewport and the manipulators together.
    ///
    /// One call rather than two because a caller needs both at once to drive an interaction, and
    /// two calls would borrow this service mutably and immutably at the same time.
    pub fn interacting(&mut self) -> (&mut Viewport, &GizmoRegistry) {
        let viewport = self
            .viewports
            .get_mut(self.focused)
            .expect("the focused viewport is never closed while it is focused");
        (viewport, &self.gizmos)
    }

    /// The viewports, for a panel that draws them.
    #[must_use]
    pub const fn all(&self) -> &Viewports {
        &self.viewports
    }

    /// The viewports, mutably.
    pub const fn all_mut(&mut self) -> &mut Viewports {
        &mut self.viewports
    }

    /// Which one has the focus.
    #[must_use]
    pub const fn focused_id(&self) -> ViewportId {
        self.focused
    }

    /// The focused viewport, which is the one a command acts on.
    #[must_use]
    pub fn focused(&self) -> &Viewport {
        self.viewports
            .get(self.focused)
            .expect("the focused viewport is never closed while it is focused")
    }

    /// The focused viewport, mutably.
    pub fn focused_mut(&mut self) -> &mut Viewport {
        self.viewports
            .get_mut(self.focused)
            .expect("the focused viewport is never closed while it is focused")
    }

    /// Move the focus, which a click in a panel does.
    pub fn focus(&mut self, id: ViewportId) {
        if self.viewports.get(id).is_some() {
            self.focused = id;
            self.viewports.focus(id);
        }
    }
}

/// The controls, by name. Every one of them changes what is shown and nothing in the project.
impl ViewportControls for ViewportService {
    fn set(&mut self, control: &str, value: &str) -> Result<String> {
        match control {
            "transform-mode" => {
                let mode = GizmoMode::ALL
                    .into_iter()
                    .find(|mode| mode.name() == value)
                    .ok_or_else(|| unknown(control, value, &self.controls()))?;
                self.focused_mut().gizmo_mode = mode;
                Ok(format!("Transform mode: {}", mode.name()))
            }
            "space" => {
                let space = GizmoSpace::of_id(value)
                    .ok_or_else(|| unknown(control, value, &self.controls()))?;
                self.focused_mut().gizmo_space = space;
                Ok(format!("Transform space: {}", space.label()))
            }
            "pivot" => {
                let pivot =
                    Pivot::of_id(value).ok_or_else(|| unknown(control, value, &self.controls()))?;
                self.focused_mut().gizmo_pivot = pivot;
                Ok(format!("Pivot: {}", pivot.label()))
            }
            "snapping" => {
                let on = parse_flag(control, value)?;
                let snap = &mut self.focused_mut().snap;
                snap.modes.grid = on;
                snap.modes.angle = on;
                snap.modes.scale = on;
                Ok(format!("Snapping {}", if on { "on" } else { "off" }))
            }
            "view-mode" => {
                let mode = ALL_VIEW_MODES
                    .into_iter()
                    .find(|mode| mode.engine_name() == value)
                    .ok_or_else(|| unknown(control, value, &self.controls()))?;
                self.focused_mut().state.view_mode = mode;
                Ok(format!("View mode: {}", mode.label()))
            }
            "view" => {
                let preset = ViewPreset::of_id(value)
                    .ok_or_else(|| unknown(control, value, &self.controls()))?;
                let viewport = self.focused_mut();
                preset.apply(&viewport.navigator, &mut viewport.state);
                Ok(format!("View: {}", preset.label()))
            }
            other => Err(unknown_control(other, &self.controls())),
        }
    }

    fn get(&self, control: &str) -> Option<String> {
        let viewport = self.focused();
        match control {
            "transform-mode" => Some(viewport.gizmo_mode.name().to_string()),
            "space" => Some(viewport.gizmo_space.id().to_string()),
            "pivot" => Some(viewport.gizmo_pivot.id().to_string()),
            "snapping" => Some(viewport.snap.modes.grid.to_string()),
            "view-mode" => Some(viewport.state.view_mode.engine_name().to_string()),
            "view" => Some(ViewPreset::of_view(&viewport.state).id().to_string()),
            _ => None,
        }
    }

    fn perform(&mut self, action: &str) -> Result<String> {
        match action {
            "toggle-space" => {
                let viewport = self.focused_mut();
                viewport.gizmo_space = match viewport.gizmo_space {
                    GizmoSpace::World => GizmoSpace::Local,
                    _ => GizmoSpace::World,
                };
                Ok(format!("Transform space: {}", viewport.gizmo_space.label()))
            }
            "toggle-snapping" => {
                let on = !self.focused().snap.modes.grid;
                self.set("snapping", if on { "on" } else { "off" })
            }
            "cycle-view" => {
                let viewport = self.focused_mut();
                let next = ViewPreset::of_view(&viewport.state).next();
                next.apply(&viewport.navigator, &mut viewport.state);
                Ok(format!("View: {}", next.label()))
            }
            other => Err(Problem::new(
                format!("perform {other} on the viewport"),
                "the viewport has no such action",
            )
            .with_remedy("toggle-space, toggle-snapping or cycle-view")),
        }
    }

    fn focus(&mut self, centre: [f32; 3], radius: f32) {
        let viewport = self.focused_mut();
        let centre = Vec3::from_array(centre);
        let extent = Vec3::new(radius, radius, radius);
        viewport.navigator.focus(
            &mut viewport.state,
            Bounds::from_center_extents(centre, extent),
        );
    }

    fn controls(&self) -> Vec<(&'static str, Vec<String>)> {
        vec![
            (
                "transform-mode",
                GizmoMode::ALL
                    .iter()
                    .map(|mode| mode.name().to_string())
                    .collect(),
            ),
            (
                "space",
                GizmoSpace::TOGGLED
                    .iter()
                    .map(|space| space.id().to_string())
                    .collect(),
            ),
            (
                "pivot",
                Pivot::ALL
                    .iter()
                    .map(|pivot| pivot.id().to_string())
                    .collect(),
            ),
            ("snapping", vec!["on".to_string(), "off".to_string()]),
            (
                "view-mode",
                ALL_VIEW_MODES
                    .iter()
                    .map(|mode| mode.engine_name().to_string())
                    .collect(),
            ),
            (
                "view",
                ViewPreset::ALL
                    .iter()
                    .map(|preset| preset.id().to_string())
                    .collect(),
            ),
        ]
    }
}

fn parse_flag(control: &str, value: &str) -> Result<bool> {
    match value {
        "on" | "true" | "1" => Ok(true),
        "off" | "false" | "0" => Ok(false),
        other => Err(
            Problem::new(format!("set {control} to {other}"), "it takes on or off")
                .with_remedy("pass on or off"),
        ),
    }
}

fn unknown(control: &str, value: &str, controls: &[(&'static str, Vec<String>)]) -> Problem {
    let accepted = controls
        .iter()
        .find(|(name, _)| *name == control)
        .map(|(_, values)| values.join(", "))
        .unwrap_or_default();
    Problem::new(
        format!("set {control} to {value}"),
        format!("the viewport's {control} does not take that value"),
    )
    .with_remedy(format!("it takes one of: {accepted}"))
}

fn unknown_control(control: &str, controls: &[(&'static str, Vec<String>)]) -> Problem {
    let names: Vec<&str> = controls.iter().map(|(name, _)| *name).collect();
    Problem::new(format!("set {control}"), "the viewport has no such control")
        .with_remedy(format!("it has: {}", names.join(", ")))
}

// --- The commands ----------------------------------------------------------------------------------

/// Register every viewport command.
///
/// Called by [`crate::builtin::register`], so an editor built anywhere has them. Twenty-nine of them
/// are generated — four transform modes, four pivots, seven view presets, and nineteen debug views —
/// because a table is the only way a set that size stays in step with the model it comes from.
///
/// # Errors
///
/// When two commands would claim the same identifier or a binding conflicts, which is a programming
/// error caught at start-up rather than a state a user can reach.
pub fn register(registry: &mut Registry) -> Result<()> {
    for mode in GizmoMode::ALL {
        registry.register(transform_mode(mode))?;
    }
    for pivot in Pivot::ALL {
        registry.register(pivot_command(pivot))?;
    }
    for preset in ViewPreset::ALL {
        registry.register(view_preset(preset))?;
    }
    for mode in ALL_VIEW_MODES {
        registry.register(view_mode(mode))?;
    }
    registry.register(toggle_space())?;
    registry.register(toggle_snapping())?;
    registry.register(frame_selection())?;
    Ok(())
}

/// The viewport a command acts on, or a problem explaining that this context has none.
fn controls(context: &mut dyn CommandContext) -> Result<&mut dyn ViewportControls> {
    context.viewport().ok_or_else(|| {
        Problem::new(
            "change the viewport",
            "this editor has no viewport — it is running headless",
        )
        .with_remedy("run the editor with a window, or drive the document commands instead")
    })
}

/// The label a mode carries in the menu and the toolbar, from the reference.
const fn mode_label(mode: GizmoMode) -> &'static str {
    match mode {
        GizmoMode::Translate => "Move",
        GizmoMode::Rotate => "Rotate",
        GizmoMode::Scale => "Scale",
        GizmoMode::Universal => "Universal",
    }
}

/// The identifier a mode's command carries. The ones `Keymap::unity` already names.
const fn mode_id(mode: GizmoMode) -> &'static str {
    match mode {
        GizmoMode::Translate => "viewport.transform-mode-move",
        GizmoMode::Rotate => "viewport.transform-mode-rotate",
        GizmoMode::Scale => "viewport.transform-mode-scale",
        GizmoMode::Universal => "viewport.transform-mode-universal",
    }
}

/// The key the reference's shortcut table puts a mode on.
const fn mode_key(mode: GizmoMode) -> &'static str {
    match mode {
        GizmoMode::Translate => "W",
        GizmoMode::Rotate => "E",
        GizmoMode::Scale => "R",
        GizmoMode::Universal => "T",
    }
}

fn transform_mode(mode: GizmoMode) -> Command {
    let name = mode.name();
    Command::new(
        Metadata::new(
            mode_id(mode),
            mode_label(mode),
            "Viewport",
            match mode {
                GizmoMode::Universal => {
                    "Shows the universal gizmo: arrows, rings and box handles at once. Which \
                     manipulation a drag performs is decided by the handle grabbed. Changes what is \
                     shown and nothing in the project."
                }
                GizmoMode::Translate => {
                    "Shows the move gizmo: axis arrows with planar handles at the axis pairs, and a \
                     centre circle for screen-space movement. Changes what is shown and nothing in \
                     the project."
                }
                GizmoMode::Rotate => {
                    "Shows the rotate gizmo: one ring per axis plus an outer screen-space ring. \
                     Changes what is shown and nothing in the project."
                }
                GizmoMode::Scale => {
                    "Shows the scale gizmo: box handles on the axes and a centre cube for uniform \
                     scale. Changes what is shown and nothing in the project."
                }
            },
            EffectClass::Read,
        )
        .bound_to(mode_key(mode)),
        move |context, _arguments| {
            let summary = controls(context)?.set("transform-mode", name)?;
            Ok(Outcome::new(summary).with("transform-mode", Value::Text(name.to_string())))
        },
    )
}

fn pivot_command(pivot: Pivot) -> Command {
    let id = pivot.id();
    Command::new(
        Metadata::new(
            format!("viewport.pivot-{id}"),
            format!("Pivot: {}", pivot.label()),
            "Viewport",
            match pivot {
                Pivot::Pivot => {
                    "Manipulates about the active object's own origin. With several objects \
                     selected they all move about that one."
                }
                Pivot::Center => "Manipulates about the mean of the selected objects' origins.",
                Pivot::Bounds => {
                    "Manipulates about the centre of the box containing the selection, which is not \
                     the same point as the mean unless the objects are evenly spread."
                }
                Pivot::Individual => {
                    "Manipulates each selected object about its own origin, so several objects turn \
                     in place rather than about a shared point."
                }
            },
            EffectClass::Read,
        ),
        move |context, _arguments| {
            let summary = controls(context)?.set("pivot", id)?;
            Ok(Outcome::new(summary).with("pivot", Value::Text(id.to_string())))
        },
    )
}

fn view_preset(preset: ViewPreset) -> Command {
    let id = preset.id();
    Command::new(
        Metadata::new(
            format!("viewport.view-{id}"),
            format!("View: {}", preset.label()),
            "Viewport",
            format!(
                "Points the focused viewport's camera at the {} view. Moves the camera and nothing \
                 else: no object transform changes and no transaction is recorded.",
                preset.label().to_lowercase()
            ),
            EffectClass::Read,
        ),
        move |context, _arguments| {
            let summary = controls(context)?.set("view", id)?;
            Ok(Outcome::new(summary).with("view", Value::Text(id.to_string())))
        },
    )
}

fn view_mode(mode: ViewMode) -> Command {
    let name = mode.engine_name();
    Command::new(
        Metadata::new(
            mode.command_id(),
            mode.label(),
            "Viewport",
            format!(
                "{} How to read it: {} The engine renders it; the editor asks for it.",
                mode.shows(),
                mode.how_to_read()
            ),
            EffectClass::Read,
        ),
        move |context, _arguments| {
            let summary = controls(context)?.set("view-mode", name)?;
            Ok(Outcome::new(summary).with("view-mode", Value::Text(name.to_string())))
        },
    )
}

fn toggle_space() -> Command {
    Command::new(
        Metadata::new(
            "viewport.toggle-space",
            "Toggle World/Local Space",
            "Viewport",
            "Switches the transform gizmo between world axes and the object's own. Changes what is \
             shown and nothing in the project.",
            EffectClass::Read,
        ),
        |context, _arguments| {
            let summary = controls(context)?.perform("toggle-space")?;
            Ok(Outcome::new(summary))
        },
    )
}

fn toggle_snapping() -> Command {
    Command::new(
        Metadata::new(
            "viewport.toggle-snapping",
            "Toggle Snapping",
            "Viewport",
            "Turns grid, angle and scale snapping on or off. Holding Ctrl during a manipulation \
             inverts whichever way this is set, so both a user who works on the grid and one who \
             does not have a way to leave it.",
            EffectClass::Read,
        ),
        |context, _arguments| {
            let summary = controls(context)?.perform("toggle-snapping")?;
            Ok(Outcome::new(summary))
        },
    )
}

fn frame_selection() -> Command {
    Command::new(
        Metadata::new(
            "viewport.frame-selection",
            "Frame Selection",
            "Viewport",
            "Moves the camera so the selection fills the viewport. Reads the selected objects' \
             positions from the document, so it works with no runtime attached; it changes the \
             camera and nothing in the project.",
            EffectClass::Read,
        )
        .bound_to("F"),
        |context, _arguments| {
            let bounds = selection_bounds(context)?;
            let radius = bounds.radius().max(0.5);
            controls(context)?.focus(bounds.center().to_array(), radius);
            Ok(Outcome::new("Framed the selection"))
        },
    )
}

/// The box containing the selection's origins, from the document.
///
/// **Origins, not extents**, and the description says so: a document knows where its objects are and
/// the renderer knows how big they are. When the runtime reports bounds the viewport uses those; this
/// is the answer the editor can give on its own, and it is the right one for the case that matters —
/// framing something in a project with no runtime running.
fn selection_bounds(context: &dyn CommandContext) -> Result<Bounds> {
    let id = context
        .active_document()
        .ok_or_else(|| Problem::not_found("an active document"))?;
    let document = context
        .document(id)
        .ok_or_else(|| Problem::not_found("the active document"))?;
    let binding = cy_editor_viewport::gizmo::TransformBinding::of_schema(document.schema())
        .ok_or_else(|| {
            Problem::new(
                "frame the selection",
                "this document's schema declares no transform component",
            )
            .with_remedy(
                "open a world whose schema declares Transform with translation, rotation and scale",
            )
        })?;

    let mut bounds: Option<Bounds> = None;
    for node in context.selection().nodes() {
        let Some(Value::Vec3(lanes)) =
            document
                .content()
                .field(node, binding.component, binding.translation)
        else {
            continue;
        };
        let point = Bounds::point(Vec3::from_array(*lanes));
        bounds = Some(bounds.map_or(point, |existing| existing.union(point)));
    }
    bounds.ok_or_else(|| {
        Problem::new("frame the selection", "nothing selected carries a position")
            .with_remedy("select an object in the hierarchy or the viewport")
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Editor;
    use cy_editor_commands::{Arguments, Scope};
    use cy_editor_core::Actor;

    fn editor() -> (Editor, Registry) {
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).expect("the built-in commands register");
        (Editor::new(Actor::human("designer")), registry)
    }

    #[test]
    fn the_four_transform_modes_are_commands_on_w_e_r_and_t() {
        // The reference's shortcut table, as a check. A mode reachable only from the toolbar would
        // be the "action reachable only through a specific widget" the specification calls a defect.
        let (mut editor, registry) = editor();
        for (id, key, expected) in [
            ("viewport.transform-mode-move", "W", GizmoMode::Translate),
            ("viewport.transform-mode-rotate", "E", GizmoMode::Rotate),
            ("viewport.transform-mode-scale", "R", GizmoMode::Scale),
            (
                "viewport.transform-mode-universal",
                "T",
                GizmoMode::Universal,
            ),
        ] {
            let metadata = registry.metadata(id).expect("a registered command");
            assert_eq!(metadata.default_binding.as_deref(), Some(key), "{id}");
            assert_eq!(metadata.effect, EffectClass::Read, "{id} claims to mutate");
            registry
                .invoke(id, &Scope::unrestricted(), &mut editor, &Arguments::new())
                .expect("it invokes");
            assert_eq!(editor.viewports.focused().gizmo_mode, expected);
        }
    }

    #[test]
    fn a_viewport_command_records_no_transaction_and_dirties_no_document() {
        let (mut editor, registry) = editor();
        editor
            .open_document("worlds/city.cyworld")
            .expect("a document");
        for id in [
            "viewport.transform-mode-rotate",
            "viewport.pivot-bounds",
            "viewport.toggle-space",
            "viewport.toggle-snapping",
            "viewport.view-top",
            "viewport.view-mode.wireframe",
        ] {
            registry
                .invoke(id, &Scope::unrestricted(), &mut editor, &Arguments::new())
                .unwrap_or_else(|problem| panic!("{id}: {problem}"));
        }
        assert!(
            !editor.documents.any_dirty(),
            "a viewport command dirtied a document"
        );
    }

    #[test]
    fn every_pivot_and_every_view_preset_has_a_command() {
        let (_editor, registry) = editor();
        for pivot in Pivot::ALL {
            assert!(
                registry
                    .metadata(&format!("viewport.pivot-{}", pivot.id()))
                    .is_some(),
                "{} has no command",
                pivot.label()
            );
        }
        for preset in ViewPreset::ALL {
            assert!(
                registry
                    .metadata(&format!("viewport.view-{}", preset.id()))
                    .is_some(),
                "{} has no command",
                preset.label()
            );
        }
        for mode in ALL_VIEW_MODES {
            assert!(
                registry.metadata(&mode.command_id()).is_some(),
                "{} has no command",
                mode.label()
            );
        }
    }

    #[test]
    fn an_unknown_value_is_refused_with_the_list_of_what_is_accepted() {
        let mut service = ViewportService::new();
        let problem = service
            .set("transform-mode", "move")
            .expect_err("the mode is called translate");
        assert!(problem.remedy.is_some(), "{problem}");
        assert!(
            problem
                .remedy
                .as_deref()
                .unwrap_or_default()
                .contains("translate"),
            "{problem}"
        );

        let problem = service.set("colour", "red").expect_err("no such control");
        assert!(
            problem
                .remedy
                .as_deref()
                .unwrap_or_default()
                .contains("transform-mode"),
            "{problem}"
        );
    }

    #[test]
    fn every_control_reads_back_what_it_was_set_to() {
        let mut service = ViewportService::new();
        for (control, values) in service.controls() {
            for value in values {
                service
                    .set(control, &value)
                    .unwrap_or_else(|problem| panic!("{control}={value}: {problem}"));
                if control == "snapping" {
                    // It reads back as a boolean rather than as the word it was set with, which is
                    // what a caller asking "is snapping on" wants.
                    continue;
                }
                assert_eq!(
                    service.get(control).as_deref(),
                    Some(value.as_str()),
                    "{control} did not keep {value}"
                );
            }
        }
    }

    #[test]
    fn framing_the_selection_moves_the_camera_toward_it_and_changes_no_document() {
        use cy_editor_core::value::ValueKind;
        let (mut editor, registry) = editor();
        let id = editor
            .open_document("worlds/city.cyworld")
            .expect("a document");
        let document = editor.documents.get_mut(id).expect("the document");
        let component = document.schema_mut().declare_type("Transform", false);
        let translation = document
            .schema_mut()
            .declare_field(component, "translation", ValueKind::Vec3, "where it is")
            .expect("a fresh schema");
        let rotation = document
            .schema_mut()
            .declare_field(component, "rotation", ValueKind::Quat, "which way it faces")
            .expect("a fresh schema");
        let scale = document
            .schema_mut()
            .declare_field(component, "scale", ValueKind::Vec3, "how big it is")
            .expect("a fresh schema");
        let node = document
            .with_transaction("Populate", Actor::human("designer"), |document| {
                let node = document.create_node(None)?;
                document.add_component(
                    node,
                    component,
                    vec![
                        (translation, Value::Vec3([40.0, 0.0, 0.0])),
                        (rotation, Value::Quat([0.0, 0.0, 0.0, 1.0])),
                        (scale, Value::Vec3([1.0, 1.0, 1.0])),
                    ],
                )?;
                Ok(node)
            })
            .expect("a node");
        let mut selection = cy_editor_documents::selection::Selection::new();
        selection.add_node(node);
        editor.selection.set(selection);
        editor
            .documents
            .get_mut(id)
            .expect("the document")
            .save(|_| Ok(()))
            .expect("saved");

        let before = editor.viewports.focused().state.camera.position;
        registry
            .invoke(
                "viewport.frame-selection",
                &Scope::unrestricted(),
                &mut editor,
                &Arguments::new(),
            )
            .expect("it frames");
        let after = editor.viewports.focused().state.camera.position;
        assert_ne!(
            before.to_array().map(f32::to_bits),
            after.to_array().map(f32::to_bits),
            "the camera did not move"
        );
        assert!(
            (after.x - 40.0).abs() < (before.x - 40.0).abs(),
            "the camera moved away from the selection"
        );
        assert!(!editor.documents.any_dirty(), "framing dirtied a document");
    }

    #[test]
    fn framing_nothing_is_refused_with_a_remedy_rather_than_moving_the_camera_somewhere() {
        let (mut editor, registry) = editor();
        editor
            .open_document("worlds/city.cyworld")
            .expect("a document");
        let problem = registry
            .invoke(
                "viewport.frame-selection",
                &Scope::unrestricted(),
                &mut editor,
                &Arguments::new(),
            )
            .expect_err("nothing is selected");
        assert!(problem.remedy.is_some(), "{problem}");
    }
}
