//! Translate, rotate and scale by a stated amount, through the path a gizmo drag takes. Task 3.5.
//!
//! `editor-agent-interface`, "Manipulation uses the manipulation path":
//!
//! > An agent's translate, rotate and scale SHALL execute through **the same manipulation
//! > implementation a gizmo drag uses**, and SHALL inherit its guarantees from
//! > `editor-viewport-and-gizmos`: operation on state captured at the start rather than accumulated
//! > per step, exactly one transaction per manipulation, cancellability, and identical treatment of
//! > pivot, space, snapping and constraints.
//!
//! --- HOW THAT IS TRUE HERE, RATHER THAN CLAIMED ------------------------------------------------------
//!
//! A relative manipulation opens a real [`Drag`] — the same type the viewport panel opens on a mouse
//! press — against the focused viewport's own view, space, pivot and increments, and advances it with
//! [`Drag::advance_by`], which synthesises the ray the manipulator reads. The manipulator is the same
//! object, the capture is the same capture, the write is the same write, and the commit is the same
//! commit including its rule that a manipulation which changed nothing records nothing.
//!
//! An **absolute** manipulation is the numeric panel's path, [`cy_editor_viewport::entry`], for the
//! same reason: that is what a person uses to type a value, and its module note already argues at
//! length that it is one transaction and the same operations rather than a shortcut.
//!
//! Neither is a second implementation, and there is nowhere in this file to put one.
//!
//! --- WHY THE VIEWPORT'S SETTINGS RATHER THAN THE REQUEST'S -------------------------------------------
//!
//! Space and pivot come from the focused viewport, not from parameters. That is what makes "an
//! agent's move is a human's move" literally true: both read `viewport.gizmo_space` and
//! `viewport.gizmo_pivot`, and an agent changes them with `viewport.toggle-space` and
//! `viewport.pivot-*` — the commands the toolbar and the keymap use. A parameter would be a second
//! place the space is decided, and the two would disagree the first time somebody changed one.
//!
//! Snapping is the exception and is a parameter, because it is the one a manipulation states per
//! manipulation: `Ctrl` does the same for a hand on a mouse.

use cy_editor_commands::{CommandContext, Manipulation, ManipulationKind};
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::Document;
use cy_editor_viewport::entry::{self, Row};
use cy_editor_viewport::gizmo::{
    Drag, DragInput, DragRequest, GizmoRegistry, Handle, TransformBinding,
};
use cy_editor_viewport::snapping::{SnapModes, SnapSettings};
use cy_editor_viewport::state::ViewState;

use crate::editor::Editor;

/// Perform one manipulation, as one transaction.
///
/// Returns the sentence the invocation reports. Refusals name what would have made it work, because
/// "nothing selected carries a transform" and "no document is open" call for different actions.
pub fn apply(editor: &mut Editor, request: &Manipulation) -> Result<String> {
    let nodes: Vec<NodeId> = editor.selection.get().nodes().collect();
    if nodes.is_empty() {
        return Err(Problem::new(
            format!("{} the selection", request.kind.name()),
            "nothing is selected",
        )
        .with_remedy("select something first — edit.select takes an entity identity"));
    }
    let document_id = editor.workspace.active().ok_or_else(|| {
        Problem::new(
            format!("{} the selection", request.kind.name()),
            "no document is active",
        )
        .with_remedy("open a document first")
    })?;
    let actor = editor.actor();

    // The viewport's own settings, copied out before the document is borrowed mutably. Copies
    // rather than borrows because the manipulation reads them once, at the start — which is the same
    // rule the drag itself follows about the objects it moves.
    let (view, snap, space, pivot) = {
        let viewport = editor.viewports.focused();
        (
            viewport.interaction_view().clone(),
            increments(viewport.snap, request.snap),
            viewport.gizmo_space,
            viewport.gizmo_pivot,
        )
    };

    let document = editor
        .documents
        .get_mut(document_id)
        .ok_or_else(|| Problem::not_found("the active document"))?;
    let binding = TransformBinding::of_schema(document.schema()).ok_or_else(|| {
        Problem::new(
            format!("{} the selection", request.kind.name()),
            format!(
                "this document's schema declares no {} component with {} fields",
                TransformBinding::COMPONENT,
                TransformBinding::FIELDS.join(", ")
            ),
        )
        .with_remedy(
            "open a world whose schema declares a transform, or declare a binding for the \
             component this one uses",
        )
    })?;

    // ONE transaction for the whole manipulation, however many axes it touches. The per-axis drags
    // and numeric writes below open nested scopes, which the document merges into this one rather
    // than recording separately — "a tool calling another tool produces one history entry named for
    // the user's intent".
    document.begin(label(request), actor.clone());
    let outcome = if request.absolute {
        absolute(document, binding, &nodes, request)
    } else {
        relative(
            document,
            editor.viewports.gizmos(),
            binding,
            &nodes,
            request,
            &view,
            &snap,
            space,
            pivot,
            &actor,
        )
    };
    match outcome {
        Ok(touched) => {
            document.commit()?;
            Ok(format!(
                "{}d {} object(s) by {:?} {}",
                capitalised(request.kind.name()),
                touched,
                request.amount,
                request.kind.unit()
            ))
        }
        Err(problem) => {
            document.cancel()?;
            Err(problem)
        }
    }
}

/// The increments in force for this manipulation.
///
/// The viewport's when the request asked for snapping, and none at all when it did not. `SnapModes`
/// is cleared rather than the increments zeroed, because a zero increment is a division waiting to
/// happen and "off" is a mode rather than a magnitude.
fn increments(viewport: SnapSettings, wanted: bool) -> SnapSettings {
    if wanted {
        return viewport;
    }
    SnapSettings {
        modes: SnapModes {
            grid: false,
            angle: false,
            scale: false,
            vertex: false,
            surface: false,
        },
        ..viewport
    }
}

/// The history entry's name: what the user asked for, not what it did to each field.
fn label(request: &Manipulation) -> String {
    format!(
        "{} {}",
        capitalised(request.kind.name()),
        if request.absolute { "to" } else { "by" }
    )
}

fn capitalised(word: &str) -> String {
    let mut characters = word.chars();
    characters.next().map_or_else(String::new, |first| {
        first.to_uppercase().collect::<String>() + characters.as_str()
    })
}

/// Set the value directly, through the numeric panel's own path.
fn absolute(
    document: &mut Document,
    binding: TransformBinding,
    nodes: &[NodeId],
    request: &Manipulation,
) -> Result<usize> {
    let row = match request.kind {
        ManipulationKind::Translate => Row::Position,
        ManipulationKind::Rotate => Row::Rotation,
        ManipulationKind::Scale => Row::Scale,
    };
    let actor = cy_editor_core::Actor::human("");
    let mut touched = 0;
    for (axis, amount) in request.amount.into_iter().enumerate() {
        // Radians for a rotation, because `entry::apply_value` takes base units and the request
        // speaks the ones a person types. `entry::apply` does the same conversion after parsing.
        let value = if request.kind == ManipulationKind::Rotate {
            amount.to_radians()
        } else {
            amount
        };
        touched = touched.max(entry::apply_value(
            document,
            binding,
            nodes,
            row,
            axis,
            value,
            actor.clone(),
        )?);
    }
    Ok(touched)
}

/// Change the value by the stated amount, through a real drag.
#[allow(
    clippy::too_many_arguments,
    reason = "every one of these is a thing the interactive path reads out of a viewport, and \
              bundling them into a struct would create a second vocabulary for the drag's own \
              request type, which already has one"
)]
fn relative(
    document: &mut Document,
    gizmos: &GizmoRegistry,
    binding: TransformBinding,
    nodes: &[NodeId],
    request: &Manipulation,
    view: &ViewState,
    snap: &SnapSettings,
    space: cy_editor_viewport::gizmo::GizmoSpace,
    pivot: cy_editor_viewport::gizmo::Pivot,
    actor: &cy_editor_core::Actor,
) -> Result<usize> {
    let identity = request.kind.identity();
    let mut touched = 0;
    for (axis, amount) in request.amount.into_iter().enumerate() {
        // Compared as bits, for the reason `Scale::manipulate` compares its own factor that way:
        // "this axis was not asked to move" is an exact question, and an epsilon would make a very
        // small deliberate nudge indistinguishable from none.
        if amount.to_bits() == identity.to_bits() {
            continue;
        }
        let handle = handle_for(request.kind, axis);
        let mut drag = Drag::begin(
            gizmos,
            document,
            &DragRequest {
                manipulator: request.kind.name(),
                handle,
                space,
                pivot,
                nodes,
                binding,
                view,
                // The centre of the view. Only the *ray* the frame is built from depends on it, and
                // `Drag::advance_by` replaces that ray with one it computes from the frame itself —
                // see `Drag::conditioned_frame` for the two degenerate cases that makes safe.
                pixel: centre_of(view),
                actor: actor.clone(),
                duplicate: false,
                bounds: None,
            },
        )?;
        // Degrees in, radians through: the manipulator measures a turn in radians, which is what
        // `Manipulator::magnitude` reports and what the snapping increment is in.
        let stated = if request.kind == ManipulationKind::Rotate {
            amount.to_radians()
        } else {
            amount
        };
        let advanced = drag.advance_by(gizmos, document, stated, snap, DragInput::NONE);
        if let Err(problem) = advanced {
            drag.cancel(document)?;
            return Err(problem);
        }
        drag.commit(document)?;
        touched = touched.max(drag.starts().len());
    }
    Ok(touched)
}

/// The middle of a view, in pixels.
///
/// Only the *ray* the drag's frame is built from depends on it, and `Drag::advance_by` replaces that
/// ray with one it computes from the frame itself.
#[allow(
    clippy::cast_precision_loss,
    reason = "a viewport is a few thousand pixels across, far below the 2^24 a f32 represents \
              exactly"
)]
fn centre_of(view: &ViewState) -> (f32, f32) {
    (
        view.viewport.width as f32 / 2.0,
        view.viewport.height as f32 / 2.0,
    )
}

/// Which handle a stated axis manipulation grabs.
///
/// The same handles a person grabs: an arrow to move along an axis, a ring to turn about it, a box
/// to resize along it. Naming them here rather than inventing an axis-only path is what makes the
/// two callers indistinguishable to everything below.
const fn handle_for(kind: ManipulationKind, axis: usize) -> Handle {
    match (kind, axis) {
        (ManipulationKind::Translate, 0) => Handle::AxisX,
        (ManipulationKind::Translate, 1) => Handle::AxisY,
        (ManipulationKind::Translate, _) => Handle::AxisZ,
        (ManipulationKind::Rotate, 0) => Handle::RingX,
        (ManipulationKind::Rotate, 1) => Handle::RingY,
        (ManipulationKind::Rotate, _) => Handle::RingZ,
        (ManipulationKind::Scale, 0) => Handle::BoxX,
        (ManipulationKind::Scale, 1) => Handle::BoxY,
        (ManipulationKind::Scale, _) => Handle::BoxZ,
    }
}

// --- The commands ---------------------------------------------------------------------------------
//
// One per quantity rather than one with a `kind` parameter, because a menu, a palette and a tool
// listing all show a command by name and "Move" is what a person is looking for. The parameters are
// identical by construction — they are generated from the same table — so the three cannot drift.

use cy_editor_commands::{Command, EffectClass, Metadata, Outcome, ParameterSpec, Registry};
use cy_editor_core::value::{Value, ValueKind};

/// Register `scene.translate`, `scene.rotate` and `scene.scale`.
pub fn register(registry: &mut Registry) -> Result<()> {
    for kind in [
        ManipulationKind::Translate,
        ManipulationKind::Rotate,
        ManipulationKind::Scale,
    ] {
        registry.register(command(kind))?;
    }
    Ok(())
}

/// What each command is called and what it says about itself.
const fn wording(kind: ManipulationKind) -> (&'static str, &'static str, &'static str) {
    match kind {
        ManipulationKind::Translate => (
            "scene.translate",
            "Move",
            "Moves the selection, as one undoable transaction, through the same manipulation a \
             gizmo drag performs — the same start-state capture, the same pivot, the same space, \
             and the same increments. Amounts are metres along the viewport's current transform \
             space.",
        ),
        ManipulationKind::Rotate => (
            "scene.rotate",
            "Rotate",
            "Turns the selection about the viewport's current pivot, as one undoable transaction \
             and through the same manipulation a ring drag performs. Amounts are degrees about the \
             X, Y and Z axes of the current transform space, applied in that order.",
        ),
        ManipulationKind::Scale => (
            "scene.scale",
            "Scale",
            "Resizes the selection about the viewport's current pivot, as one undoable \
             transaction and through the same manipulation a box drag performs. Amounts are \
             factors: 2 doubles, 0.5 halves, and 1 leaves an axis alone.",
        ),
    }
}

fn command(kind: ManipulationKind) -> Command {
    let (id, label, description) = wording(kind);
    let identity = kind.identity();
    Command::new(
        Metadata::new(
            id,
            label,
            "Transform",
            description,
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "amount",
            ValueKind::Vec3,
            match kind {
                ManipulationKind::Translate => {
                    "How far along X, Y and Z, in metres. Zero on an axis leaves it alone."
                }
                ManipulationKind::Rotate => {
                    "How far about X, Y and Z, in degrees. Zero on an axis leaves it alone."
                }
                ManipulationKind::Scale => {
                    "The factor on X, Y and Z. One on an axis leaves it alone."
                }
            },
        ))
        .with(ParameterSpec::optional(
            "absolute",
            ValueKind::Bool,
            "Whether the amount is the value to end at rather than the change to make; a \
                 change when omitted, which is what a drag does.",
            Value::Bool(false),
        ))
        .with(ParameterSpec::optional(
            "snap",
            ValueKind::Bool,
            "Whether the viewport's snapping increments apply, as holding the snap modifier \
                 during a drag would; off when omitted.",
            Value::Bool(false),
        )),
        move |context, arguments| {
            let amount = match arguments.get("amount") {
                Some(Value::Vec3(lanes)) => *lanes,
                _ => [identity; 3],
            };
            let request = Manipulation {
                kind,
                amount,
                absolute: matches!(arguments.get("absolute"), Some(Value::Bool(true))),
                snap: matches!(arguments.get("snap"), Some(Value::Bool(true))),
            };
            let said = context.manipulate(&request)?;
            Ok(Outcome::new(said).with("amount", Value::Vec3(amount)))
        },
    )
}
