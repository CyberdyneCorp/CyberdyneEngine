//! Gizmos and manipulation: intent, captured state, and exactly one transaction.
//!
//! `editor-viewport-and-gizmos` — "Gizmos and manipulation":
//!
//! > The editor SHALL provide translate, rotate, and scale gizmos in world, local, parent, view, and
//! > custom spaces, with a configurable pivot — origin, centre, or active object. **Gizmo geometry,
//! > depth behaviour, occlusion handling, and screen-constant sizing SHALL be produced by the
//! > engine**; the editor SHALL supply intent and manipulation state. Manipulation SHALL be
//! > **numerically stable**: it SHALL operate on the state captured at drag start rather than
//! > accumulating per-frame deltas, so that a drag returning to its origin returns the exact original
//! > values. A manipulation SHALL produce **exactly one transaction**, SHALL be cancellable, and
//! > SHALL show numeric feedback of the delta and the resulting value.
//!
//! --- CAPTURED STATE, AND WHY IT IS NOT AN OPTIMISATION -----------------------------------------------
//!
//! [`Drag::begin`] copies every selected object's transform into [`DragStart`] and never reads them
//! again. Every frame of the drag computes the *whole* new transform from that copy and the current
//! ray — never from the value in the document, and never by adding a delta to the previous frame's
//! answer.
//!
//! The accumulating version is the obvious one and it is wrong in a way that hides: each frame's
//! rounding is folded into the next, so a drag out and back leaves the object a few ten-thousandths
//! from where it started. Nobody notices on one object. On a hundred objects nudged over a week, the
//! grid stops being a grid, and the cause is invisible because every individual movement was correct.
//! `tests::a_drag_out_and_back_restores_the_original_bits` compares `f32::to_bits`, which is the only
//! comparison that means "exact".
//!
//! --- WHAT THE EDITOR SENDS AND WHAT THE ENGINE DRAWS ------------------------------------------------
//!
//! Nothing in this module has a size in pixels, a colour, a depth test or a line width. [`GizmoIntent`]
//! is what the editor publishes — which gizmo, in which space, about which pivot, with which handle
//! under the cursor — and the engine produces the geometry, sizes it to be screen-constant, and draws
//! it through the same path it draws everything else. An editor that drew its own arrows would be the
//! forbidden "second renderer", and it would be one that disagreed with the picking that selects its
//! handles.
//!
//! --- PLUGIN GIZMOS ARE THE SAME MECHANISM, NOT A PARALLEL ONE ---------------------------------------
//!
//! > **WHEN** a plugin registers a gizmo **THEN** it SHALL render, pick, and undo identically to
//! > built-in gizmos.
//!
//! Translate, rotate and scale are three implementations of [`Manipulator`] in a [`GizmoRegistry`],
//! and a plugin's gizmo is a fourth. There is no branch anywhere below on whether a manipulator is
//! built in, so "identically" is a property of there being one path rather than of two paths being
//! kept in step. Undo is identical because a plugin's manipulator produces the same `SetField`
//! operations inside the same transaction; rendering is identical because the engine draws from
//! [`GizmoIntent`], which carries no code.

use cy_editor_core::Actor;
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_documents::Document;

use crate::math::{Bounds, Quat, Ray, Vec3};
use crate::snapping::{Quantity, SnapSettings};
use crate::state::ViewState;

/// Which gizmo is in force.
///
/// Four, on `W`, `E`, `R` and `T`, which is what `docs/design/images/transform-gizmo.png` shows and
/// what `cy_editor_visual::gizmo::GizmoMode` — the *appearance* half of the same decision — already
/// listed. The two enumerations are checked against each other by
/// `tests/the_two_gizmos_are_unmistakable.rs`, so a mode that exists in one and not the other is a
/// failing test rather than a toolbar button that does nothing.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum GizmoMode {
    /// Move.
    #[default]
    Translate,
    /// Turn.
    Rotate,
    /// Resize.
    Scale,
    /// All three at once. Which manipulation a drag performs is decided by the handle that was
    /// grabbed rather than by the mode, which is the whole of what makes it universal.
    Universal,
}

impl GizmoMode {
    /// Every mode, in the order the toolbar and the `W`/`E`/`R`/`T` bindings put them.
    pub const ALL: [GizmoMode; 4] = [
        GizmoMode::Translate,
        GizmoMode::Rotate,
        GizmoMode::Scale,
        GizmoMode::Universal,
    ];

    /// A name for the interface and for a command identifier.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            GizmoMode::Translate => "translate",
            GizmoMode::Rotate => "rotate",
            GizmoMode::Scale => "scale",
            GizmoMode::Universal => "universal",
        }
    }

    /// What quantity this mode's numeric entry takes.
    ///
    /// The universal gizmo has no single answer — its arrows move, its rings turn and its boxes
    /// resize — so it answers with the quantity of whichever handle is in question through
    /// [`GizmoMode::quantity_of`], and this returns the one its numeric panel opens on.
    #[must_use]
    pub const fn quantity(self) -> Quantity {
        match self {
            GizmoMode::Translate | GizmoMode::Universal => Quantity::Length,
            GizmoMode::Rotate => Quantity::Angle,
            GizmoMode::Scale => Quantity::Factor,
        }
    }

    /// What quantity a numeric entry takes for one handle of this mode.
    #[must_use]
    pub const fn quantity_of(self, handle: Handle) -> Quantity {
        match self {
            GizmoMode::Universal => handle.role().quantity(),
            other => other.quantity(),
        }
    }

    /// The handles this mode presents, in a stable order.
    ///
    /// This is the set `docs/design/images/transform-gizmo.png` is normative about: three axis
    /// handles, three planar handles at the axis pairs, three rotation rings plus the outer
    /// screen-space ring, and a centre carrying **three separately targetable affordances** —
    /// screen move, uniform scale and screen rotate.
    #[must_use]
    pub fn handles(self) -> Vec<Handle> {
        match self {
            GizmoMode::Translate => vec![
                Handle::AxisX,
                Handle::AxisY,
                Handle::AxisZ,
                Handle::PlaneYZ,
                Handle::PlaneZX,
                Handle::PlaneXY,
                Handle::Screen,
            ],
            GizmoMode::Rotate => vec![
                Handle::RingX,
                Handle::RingY,
                Handle::RingZ,
                Handle::ScreenRing,
            ],
            GizmoMode::Scale => vec![Handle::BoxX, Handle::BoxY, Handle::BoxZ, Handle::Uniform],
            GizmoMode::Universal => {
                let mut handles = GizmoMode::Translate.handles();
                handles.extend(GizmoMode::Rotate.handles());
                handles.extend(GizmoMode::Scale.handles());
                handles
            }
        }
    }
}

/// What a handle does when it is dragged.
///
/// The universal gizmo is the reason this exists as a value rather than as a mode: a drag on a ring
/// turns whatever mode is in force, and a drag on a box resizes, so the manipulator is chosen from
/// the handle. [`GizmoRegistry::for_handle`] is the one place that choice is made.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum HandleRole {
    /// Arrows, planar handles and the centre circle.
    Move,
    /// Rings, and the outer screen-space ring.
    Turn,
    /// Box handles, and the centre cube.
    Resize,
}

impl HandleRole {
    /// The manipulator identifier this role selects.
    #[must_use]
    pub const fn manipulator(self) -> &'static str {
        match self {
            HandleRole::Move => "translate",
            HandleRole::Turn => "rotate",
            HandleRole::Resize => "scale",
        }
    }

    /// The quantity its numeric entry takes.
    #[must_use]
    pub const fn quantity(self) -> Quantity {
        match self {
            HandleRole::Move => Quantity::Length,
            HandleRole::Turn => Quantity::Angle,
            HandleRole::Resize => Quantity::Factor,
        }
    }
}

/// The frame the gizmo's axes are expressed in.
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub enum GizmoSpace {
    /// The world's axes.
    #[default]
    World,
    /// The manipulated object's own axes.
    Local,
    /// The parent's axes, which is what a child of a rotated object is authored in.
    Parent,
    /// The camera's axes: right, up, and toward the viewer.
    View,
    /// A frame a tool supplies — a surface's tangent basis, a spline's frame, a plugin's.
    Custom(Quat),
}

impl GizmoSpace {
    /// The two the toolbar offers, which is what `World / Local Space` in the reference is.
    pub const TOGGLED: [GizmoSpace; 2] = [GizmoSpace::World, GizmoSpace::Local];

    /// The word the interface uses.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            GizmoSpace::World => "World",
            GizmoSpace::Local => "Local",
            GizmoSpace::Parent => "Parent",
            GizmoSpace::View => "View",
            GizmoSpace::Custom(_) => "Custom",
        }
    }

    /// The identifier a command uses.
    #[must_use]
    pub const fn id(self) -> &'static str {
        match self {
            GizmoSpace::World => "world",
            GizmoSpace::Local => "local",
            GizmoSpace::Parent => "parent",
            GizmoSpace::View => "view",
            GizmoSpace::Custom(_) => "custom",
        }
    }

    /// The space with this identifier. `custom` has no identifier form: it carries a rotation, and
    /// a command that named it would have nowhere to get one.
    #[must_use]
    pub fn of_id(id: &str) -> Option<Self> {
        [
            GizmoSpace::World,
            GizmoSpace::Local,
            GizmoSpace::Parent,
            GizmoSpace::View,
        ]
        .into_iter()
        .find(|space| space.id() == id)
    }
}

/// What the manipulation happens about.
///
/// The reference's four, named as it names them. They are genuinely four different answers and the
/// difference only shows with more than one object selected, which is why an editor that offers two
/// of them looks correct until the day it does not:
///
/// | Mode | The point | With several objects |
/// |---|---|---|
/// | [`Pivot::Pivot`] | the active object's own origin | they all turn about that one object |
/// | [`Pivot::Center`] | the mean of the origins | they turn about the middle of the group |
/// | [`Pivot::Bounds`] | the centre of the box containing them | they turn about the box, which is not the mean |
/// | [`Pivot::Individual`] | each object's own origin | each turns in place |
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Pivot {
    /// The active object's own origin — the last one selected, which is the one the gizmo is drawn
    /// on. The default, because it is the only one that behaves identically for one object and for
    /// many.
    #[default]
    Pivot,
    /// The mean of the selection's origins.
    Center,
    /// The centre of the axis-aligned box containing the selection's origins.
    ///
    /// **Origins, not extents.** A document knows where its objects are; how big they are is the
    /// renderer's answer, and asking for it here would make the pivot depend on a frame having
    /// arrived. When the runtime's bounds are available the caller passes them in as
    /// [`DragRequest::bounds`] and this becomes the true bounds centre.
    Bounds,
    /// Each object about its own origin. Several objects turn in place rather than about a shared
    /// point.
    Individual,
}

impl Pivot {
    /// Every mode, for a toolbar and for a command.
    pub const ALL: [Pivot; 4] = [
        Pivot::Pivot,
        Pivot::Center,
        Pivot::Bounds,
        Pivot::Individual,
    ];

    /// The word the interface uses, which is the reference's word.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Pivot::Pivot => "Pivot",
            Pivot::Center => "Center",
            Pivot::Bounds => "Bounds",
            Pivot::Individual => "Individual",
        }
    }

    /// The identifier a command and a keymap use.
    #[must_use]
    pub const fn id(self) -> &'static str {
        match self {
            Pivot::Pivot => "pivot",
            Pivot::Center => "center",
            Pivot::Bounds => "bounds",
            Pivot::Individual => "individual",
        }
    }

    /// The mode with this identifier.
    #[must_use]
    pub fn of_id(id: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|mode| mode.id() == id)
    }
}

/// Which part of the gizmo the cursor grabbed.
///
/// One enumeration for all four modes, because a handle is what a *drag* is about and a drag does
/// not know which mode drew it — see [`HandleRole`]. The vocabulary is the reference's:
///
/// | Reference | Here |
/// |---|---|
/// | axis arrows | [`Handle::AxisX`], `AxisY`, `AxisZ` |
/// | planar handles at the axis pairs | [`Handle::PlaneXY`], `PlaneYZ`, `PlaneZX` |
/// | rotation rings | [`Handle::RingX`], `RingY`, `RingZ` |
/// | the outer screen-space ring | [`Handle::ScreenRing`] |
/// | box handles | [`Handle::BoxX`], `BoxY`, `BoxZ` |
/// | the centre's three affordances | [`Handle::Screen`], [`Handle::Uniform`], [`Handle::ScreenRing`] |
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Debug)]
pub enum Handle {
    /// The first axis of the gizmo's space.
    AxisX,
    /// The second.
    AxisY,
    /// The third.
    AxisZ,
    /// The plane spanned by the first two axes.
    PlaneXY,
    /// The plane spanned by the second and third.
    PlaneYZ,
    /// The plane spanned by the third and first.
    PlaneZX,
    /// The ring about the first axis.
    RingX,
    /// The ring about the second.
    RingY,
    /// The ring about the third.
    RingZ,
    /// The outer ring, in the camera's plane: screen rotate. One of the centre's three affordances.
    ScreenRing,
    /// The box handle on the first axis.
    BoxX,
    /// The box handle on the second.
    BoxY,
    /// The box handle on the third.
    BoxZ,
    /// The plane facing the camera: free movement. The centre circle.
    Screen,
    /// Every axis at once, for a uniform scale. The centre cube.
    Uniform,
}

impl Handle {
    /// Every handle any mode can present, for a hit test and for a check.
    pub const ALL: [Handle; 15] = [
        Handle::AxisX,
        Handle::AxisY,
        Handle::AxisZ,
        Handle::PlaneXY,
        Handle::PlaneYZ,
        Handle::PlaneZX,
        Handle::RingX,
        Handle::RingY,
        Handle::RingZ,
        Handle::ScreenRing,
        Handle::BoxX,
        Handle::BoxY,
        Handle::BoxZ,
        Handle::Screen,
        Handle::Uniform,
    ];

    /// The axis this handle acts along or about, in the gizmo's own space.
    ///
    /// `None` for a plane, for the two screen handles and for the uniform one, which act in a plane
    /// or on everything at once rather than along a line.
    #[must_use]
    pub const fn axis(self) -> Option<Vec3> {
        match self {
            Handle::AxisX | Handle::RingX | Handle::BoxX => Some(Vec3::X),
            Handle::AxisY | Handle::RingY | Handle::BoxY => Some(Vec3::Y),
            Handle::AxisZ | Handle::RingZ | Handle::BoxZ => Some(Vec3::Z),
            _ => None,
        }
    }

    /// The normal of the plane this handle acts in, in the gizmo's own space.
    ///
    /// `None` for an axis handle and for the screen handles, whose plane is the camera's and is
    /// supplied at drag start. **A ring's plane normal is its own axis**, which is what makes a
    /// rotation about X a rotation about X whatever the camera is doing — the property
    /// `a_ring_turns_about_its_own_axis_whatever_the_camera_is_doing` holds.
    #[must_use]
    pub const fn plane_normal(self) -> Option<Vec3> {
        match self {
            Handle::PlaneYZ | Handle::RingX => Some(Vec3::X),
            Handle::PlaneZX | Handle::RingY => Some(Vec3::Y),
            Handle::PlaneXY | Handle::RingZ => Some(Vec3::Z),
            _ => None,
        }
    }

    /// What dragging this handle does.
    #[must_use]
    pub const fn role(self) -> HandleRole {
        match self {
            Handle::AxisX
            | Handle::AxisY
            | Handle::AxisZ
            | Handle::PlaneXY
            | Handle::PlaneYZ
            | Handle::PlaneZX
            | Handle::Screen => HandleRole::Move,
            Handle::RingX | Handle::RingY | Handle::RingZ | Handle::ScreenRing => HandleRole::Turn,
            Handle::BoxX | Handle::BoxY | Handle::BoxZ | Handle::Uniform => HandleRole::Resize,
        }
    }

    /// Which of the three centre affordances this is, if it is one.
    ///
    /// The reference draws them concentrically — circle, cube, outer ring — and requires them to be
    /// **separately targetable**. Three variants rather than one `Centre` is what makes that a
    /// property of the type instead of a note in a drawing routine.
    #[must_use]
    pub const fn is_centre(self) -> bool {
        matches!(self, Handle::Screen | Handle::Uniform | Handle::ScreenRing)
    }

    /// A name for the numeric feedback: "X", "XY", "screen".
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Handle::AxisX | Handle::RingX | Handle::BoxX => "X",
            Handle::AxisY | Handle::RingY | Handle::BoxY => "Y",
            Handle::AxisZ | Handle::RingZ | Handle::BoxZ => "Z",
            Handle::PlaneXY => "XY",
            Handle::PlaneYZ => "YZ",
            Handle::PlaneZX => "ZX",
            Handle::Screen => "screen",
            Handle::ScreenRing => "screen ring",
            Handle::Uniform => "uniform",
        }
    }
}

/// Which axes a manipulation is allowed to touch.
///
/// `X`/`Y`/`Z` during a drag, per the reference's shortcut table. It is *not* a modifier: it is a
/// state a keystroke toggles mid-drag, and pressing the same key again releases it — which is why it
/// lives beside the modifiers in [`DragInput`] rather than inside them.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct AxisLock {
    /// Whether the first axis may move.
    pub x: bool,
    /// The second.
    pub y: bool,
    /// The third.
    pub z: bool,
}

impl AxisLock {
    /// No lock: every axis is free.
    pub const NONE: Self = Self {
        x: false,
        y: false,
        z: false,
    };

    /// A lock on one axis alone.
    #[must_use]
    pub const fn only(axis: usize) -> Self {
        Self {
            x: axis == 0,
            y: axis == 1,
            z: axis == 2,
        }
    }

    /// Whether anything is locked at all.
    #[must_use]
    pub const fn constrains(self) -> bool {
        self.x || self.y || self.z
    }

    /// Toggle one axis, which is what pressing `X` twice does.
    #[must_use]
    pub const fn toggled(self, axis: usize) -> Self {
        let mut lock = self;
        match axis {
            0 => lock.x = !lock.x,
            1 => lock.y = !lock.y,
            _ => lock.z = !lock.z,
        }
        lock
    }

    /// Zero the components no axis lock admits. With no lock in force the value passes through
    /// **untouched**, which is what keeps the exactness property true when nothing is locked.
    #[must_use]
    pub fn constrain(self, value: Vec3) -> Vec3 {
        if !self.constrains() {
            return value;
        }
        Vec3::new(
            if self.x { value.x } else { 0.0 },
            if self.y { value.y } else { 0.0 },
            if self.z { value.z } else { 0.0 },
        )
    }

    /// The single locked axis, when exactly one is locked. A rotate drag uses it to override the
    /// ring it grabbed, which is what `E` then `Z` means.
    #[must_use]
    pub const fn single(self) -> Option<Vec3> {
        match (self.x, self.y, self.z) {
            (true, false, false) => Some(Vec3::X),
            (false, true, false) => Some(Vec3::Y),
            (false, false, true) => Some(Vec3::Z),
            _ => None,
        }
    }

    /// The lock as it is written in the interface: `X`, `XY`, or nothing.
    #[must_use]
    pub fn label(self) -> String {
        let mut label = String::new();
        for (held, name) in [(self.x, "X"), (self.y, "Y"), (self.z, "Z")] {
            if held {
                label.push_str(name);
            }
        }
        label
    }
}

/// How much of the cursor's movement a precision drag applies.
///
/// `Shift` in the reference's shortcut table. A tenth is the figure every tool this one will be
/// compared against uses, and the property that matters is that it is a *linear* factor: scaling the
/// offset cannot move a drag that returned to its origin away from zero.
pub const PRECISION_FACTOR: f32 = 0.1;

/// The keyboard state a drag is advanced with.
///
/// `Ctrl` is temporary snap, `Shift` is precision, and `X`/`Y`/`Z` are the axis lock. `Alt` —
/// duplicate and transform — is not here because it acts once, when the drag *begins*; it is
/// [`DragRequest::duplicate`].
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct DragInput {
    /// Whether the transient snap modifier is held. It **toggles** the snapping setting rather than
    /// enabling it, so holding it while snapping is on turns snapping off — see
    /// [`SnapSettings::active`].
    pub snap_modifier: bool,
    /// Whether the precision modifier is held.
    pub precision: bool,
    /// Which axes the manipulation is confined to.
    pub lock: AxisLock,
}

impl DragInput {
    /// Nothing held and nothing locked.
    pub const NONE: Self = Self {
        snap_modifier: false,
        precision: false,
        lock: AxisLock::NONE,
    };

    /// The snap modifier alone.
    #[must_use]
    pub const fn snapping() -> Self {
        Self {
            snap_modifier: true,
            ..Self::NONE
        }
    }

    /// The factor the cursor's movement is multiplied by.
    #[must_use]
    pub const fn precision_factor(self) -> f32 {
        if self.precision {
            PRECISION_FACTOR
        } else {
            1.0
        }
    }
}

/// Which fields of which component hold a transform.
///
/// A binding rather than three hard-coded names, so that a plugin's own transform-like component is
/// manipulable by the same gizmos. The document's schema owns the identities; this says which of them
/// mean what.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TransformBinding {
    /// The component that holds the transform.
    pub component: TypeId,
    /// The field holding a `Value::Vec3` position.
    pub translation: FieldId,
    /// The field holding a `Value::Quat` rotation.
    pub rotation: FieldId,
    /// The field holding a `Value::Vec3` scale.
    pub scale: FieldId,
}

impl TransformBinding {
    /// The name of the component a gizmo edits by default.
    pub const COMPONENT: &'static str = "Transform";

    /// The three field names, in the order [`TransformBinding`] holds them.
    pub const FIELDS: [&'static str; 3] = ["translation", "rotation", "scale"];

    /// Find the binding in a document's own schema, or answer that there is none.
    ///
    /// **By name, and only by name.** A document's schema is the editor's own — it exists before any
    /// runtime does — so the only thing that can identify a transform is what the schema calls it.
    /// A document whose transform component is called something else is not broken; it simply has no
    /// gizmo until a tool declares a binding for it, which is what [`TransformBinding`] being a value
    /// rather than three hard-coded names is for.
    #[must_use]
    pub fn of_schema(schema: &cy_editor_documents::schema::DocumentSchema) -> Option<Self> {
        let definition = schema.type_named(Self::COMPONENT)?;
        let [translation, rotation, scale] =
            Self::FIELDS.map(|name| definition.field_named(name).map(|field| field.id));
        Some(Self {
            component: definition.id,
            translation: translation?,
            rotation: rotation?,
            scale: scale?,
        })
    }
}

/// A transform, in the three parts a document stores.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Transform3 {
    /// Where it is.
    pub translation: Vec3,
    /// Which way it faces.
    pub rotation: Quat,
    /// How big it is.
    pub scale: Vec3,
}

impl Default for Transform3 {
    fn default() -> Self {
        Self {
            translation: Vec3::ZERO,
            rotation: Quat::IDENTITY,
            scale: Vec3::new(1.0, 1.0, 1.0),
        }
    }
}

/// One object's transform as it was when the drag began. Read once, never again.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct DragStart {
    /// Which object.
    pub node: NodeId,
    /// Its transform at drag start, and the only thing every frame of the drag is computed from.
    pub transform: Transform3,
}

/// The geometry the drag was set up with: the pivot, the handle's axis and plane in world space, and
/// where the ray first met them.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct DragFrame {
    /// The point the manipulation happens about, in world space.
    pub pivot: Vec3,
    /// The handle's axis, in world space. The screen and plane handles carry the plane's first
    /// tangent here so that a manipulator always has one.
    pub axis: Vec3,
    /// The plane the ray is intersected against, in world space.
    pub plane_normal: Vec3,
    /// Where the ray met the plane when the drag began.
    pub start_hit: Vec3,
    /// Where along the axis the ray was closest when the drag began.
    pub start_parameter: f32,
    /// Which handle was grabbed.
    pub handle: Handle,
    /// The rotation that takes the gizmo's own axes into world space.
    pub orientation: Quat,
    /// Whether each object turns about its own origin rather than about [`DragFrame::pivot`].
    pub individual: bool,
}

/// What the runtime needs in order to draw the gizmo.
///
/// Intent, and nothing else: no size, no colour, no depth mode, no line width. `editor-visual-language`
/// owns how it looks and the engine owns how it is drawn.
#[derive(Clone, PartialEq, Debug)]
pub struct GizmoIntent {
    /// Which gizmo.
    pub mode: GizmoMode,
    /// The frame its axes are in, resolved to a world-space rotation.
    pub orientation: Quat,
    /// Where it is drawn.
    pub pivot: Vec3,
    /// The handle under the cursor, for the engine's highlight. `None` when none is.
    pub hovered: Option<Handle>,
    /// The handle being dragged, if one is.
    pub active: Option<Handle>,
    /// The manipulator's identifier — `"translate"`, or a plugin's. The engine uses it to select the
    /// geometry it was registered with.
    pub manipulator: String,
}

/// What the user is told while dragging.
///
/// "SHALL show numeric feedback of the delta and the resulting value" — both, because either alone
/// is ambiguous. A delta with no value does not say where the object has ended up, and a value with
/// no delta does not say how far it has come.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Feedback {
    /// How far it has moved, turned or grown, with its unit.
    pub delta: String,
    /// Where it has ended up, with its unit.
    pub value: String,
}

/// A manipulation: how a ray becomes a transform, given what was captured at drag start.
///
/// Implemented three times here and once more by every plugin that registers a gizmo. **There is no
/// branch on whether an implementation is built in**, which is what makes a plugin gizmo undo,
/// coalesce and reconcile identically rather than nearly identically.
pub trait Manipulator: Send + Sync {
    /// The identifier the engine's gizmo geometry is registered under.
    fn id(&self) -> &'static str;

    /// Which of the three modes this behaves as, for the numeric field's units and the toolbar.
    fn mode(&self) -> GizmoMode;

    /// The transform for one object, computed from its captured start state and the current ray.
    ///
    /// **Must be a pure function of its arguments.** An implementation that remembered the previous
    /// frame would reintroduce exactly the accumulation this interface exists to prevent, and the
    /// symptom would not appear until somebody dragged something a long way.
    fn manipulate(&self, context: &ManipulationContext<'_>, start: &DragStart) -> Transform3;

    /// The delta this manipulation represents, for the numeric feedback. In base units — metres,
    /// radians, or a factor.
    fn magnitude(&self, context: &ManipulationContext<'_>) -> f32;
}

/// Everything a manipulator is given.
pub struct ManipulationContext<'a> {
    /// The drag's geometry, captured when it began.
    pub frame: &'a DragFrame,
    /// The ray under the cursor now.
    pub ray: Ray,
    /// The increments in force.
    pub snap: &'a SnapSettings,
    /// The modifiers held and the axes locked, this frame.
    pub input: DragInput,
}

impl ManipulationContext<'_> {
    /// Whether the transient snap modifier is held.
    #[must_use]
    pub const fn modifier_held(&self) -> bool {
        self.input.snap_modifier
    }

    /// The point one object turns or grows about.
    ///
    /// The same as the drag's pivot for three of the four modes, and the object's own captured
    /// origin for [`Pivot::Individual`]. It is a function of the *captured* start state, never of
    /// what the document holds now, for the same reason everything else here is.
    #[must_use]
    pub fn pivot_for(&self, start: &DragStart) -> Vec3 {
        if self.frame.individual {
            start.transform.translation
        } else {
            self.frame.pivot
        }
    }

    /// Where the ray meets the drag's plane, or the plane's start point when it does not.
    ///
    /// Falling back to the start point rather than to nothing is what keeps a drag that swings past
    /// the horizon from teleporting the object: the manipulation stops moving instead.
    #[must_use]
    pub fn plane_hit(&self) -> Vec3 {
        self.ray
            .intersect_plane(self.frame.pivot, self.frame.plane_normal)
            .unwrap_or(self.frame.start_hit)
    }

    /// Where along the drag's axis the ray is closest, or the starting parameter when the ray has
    /// become parallel to it.
    #[must_use]
    pub fn axis_parameter(&self) -> f32 {
        self.ray
            .closest_parameter_on_axis(self.frame.pivot, self.frame.axis)
            .unwrap_or(self.frame.start_parameter)
    }
}

/// Move along an axis, in a plane, or freely.
pub struct Translate;

impl Manipulator for Translate {
    fn id(&self) -> &'static str {
        "translate"
    }

    fn mode(&self) -> GizmoMode {
        GizmoMode::Translate
    }

    fn manipulate(&self, context: &ManipulationContext<'_>, start: &DragStart) -> Transform3 {
        let offset = translation_offset(context);
        // EXACTNESS. A drag that has come back to where it started produces a zero offset, and the
        // captured value is written back untouched rather than recomputed — which also means that a
        // returning drag is not moved onto the grid by a snap it never asked for. The precision
        // factor and the axis lock are both linear in the offset, so neither can turn a zero into
        // something else.
        if offset == Vec3::ZERO {
            return start.transform;
        }
        let moved = start.transform.translation + offset;
        Transform3 {
            translation: context.snap.position(moved, context.modifier_held()),
            ..start.transform
        }
    }

    fn magnitude(&self, context: &ManipulationContext<'_>) -> f32 {
        translation_offset(context).length()
    }
}

/// The world-space offset a translate drag represents.
///
/// The axis lock is applied to the **world-space** offset, which is what `X` means to a user looking
/// at a world-space gizmo. A local-space gizmo whose axis lock followed the object would be a second
/// rule to learn for no gain.
fn translation_offset(context: &ManipulationContext<'_>) -> Vec3 {
    let raw = match context.frame.handle {
        Handle::AxisX | Handle::AxisY | Handle::AxisZ => {
            context.frame.axis * (context.axis_parameter() - context.frame.start_parameter)
        }
        _ => context.plane_hit() - context.frame.start_hit,
    };
    context.input.lock.constrain(raw) * context.input.precision_factor()
}

/// Turn about an axis, or about the camera's axis for the screen handle.
pub struct Rotate;

impl Manipulator for Rotate {
    fn id(&self) -> &'static str {
        "rotate"
    }

    fn mode(&self) -> GizmoMode {
        GizmoMode::Rotate
    }

    fn manipulate(&self, context: &ManipulationContext<'_>, start: &DragStart) -> Transform3 {
        let angle = rotation_angle(context);
        if angle == 0.0 {
            return start.transform;
        }
        let snapped = context.snap.radians(angle, context.modifier_held());
        let turn = Quat::from_axis_angle(rotation_axis(context), snapped);
        let pivot = context.pivot_for(start);
        Transform3 {
            // The turn is applied in WORLD space — on the left — because the axis is already a
            // world-space direction. Applying it on the right would turn about the object's own
            // axis and a world-space gizmo would rotate objects about the wrong line.
            rotation: turn.after(start.transform.rotation).normalized(),
            // About a pivot that is not the object's own origin, a rotation also moves it. Computed
            // from the captured position rather than from the current one, like everything else.
            translation: pivot + turn.rotate(start.transform.translation - pivot),
            ..start.transform
        }
    }

    fn magnitude(&self, context: &ManipulationContext<'_>) -> f32 {
        context
            .snap
            .radians(rotation_angle(context), context.modifier_held())
    }
}

/// The world-space axis a rotate drag turns about.
///
/// The ring's own axis, unless exactly one axis is locked — `E` then `Z` — in which case the lock
/// wins. A lock naming two axes is ambiguous for a rotation and is ignored rather than guessed at.
fn rotation_axis(context: &ManipulationContext<'_>) -> Vec3 {
    context
        .input
        .lock
        .single()
        .map_or(context.frame.plane_normal, |axis| {
            context.frame.orientation.rotate(axis)
        })
}

/// The signed angle a rotate drag has swept in its plane.
fn rotation_angle(context: &ManipulationContext<'_>) -> f32 {
    let normal = context.frame.plane_normal;
    let from = (context.frame.start_hit - context.frame.pivot).normalized_or(Vec3::ZERO);
    let to = (context.plane_hit() - context.frame.pivot).normalized_or(Vec3::ZERO);
    if from == Vec3::ZERO || to == Vec3::ZERO {
        return 0.0;
    }
    // atan2 of the cross and the dot, rather than acos of the dot: acos loses all its precision near
    // zero, which is exactly where a drag that has barely moved lives, and it has no sign so a
    // gizmo built on it turns the same way whichever way the cursor goes.
    let sine = from.cross(to).dot(normal);
    let cosine = from.dot(to);
    sine.atan2(cosine) * context.input.precision_factor()
}

/// Resize along an axis or uniformly.
pub struct Scale;

impl Manipulator for Scale {
    fn id(&self) -> &'static str {
        "scale"
    }

    fn mode(&self) -> GizmoMode {
        GizmoMode::Scale
    }

    fn manipulate(&self, context: &ManipulationContext<'_>, start: &DragStart) -> Transform3 {
        let factor = scale_factor(context);
        // Compared as bits, for the same reason the translate case compares an offset against zero:
        // "the drag has not moved" is an exact question, and an epsilon here would make a very
        // small deliberate scale indistinguishable from no scale at all.
        if factor.to_bits() == 1.0_f32.to_bits() {
            return start.transform;
        }
        let along = scale_lanes(context.frame.handle, context.input.lock, factor);
        let scaled = start.transform.scale.component_mul(along);
        Transform3 {
            scale: context.snap.scale_factor(scaled, context.modifier_held()),
            ..start.transform
        }
    }

    fn magnitude(&self, context: &ManipulationContext<'_>) -> f32 {
        scale_factor(context)
    }
}

/// How much bigger a scale drag has made things: the ratio of the current distance from the pivot to
/// the distance at drag start.
///
/// A RATIO rather than a difference, because scale is multiplicative: a difference would make the
/// same cursor movement double a small object and barely change a large one.
fn scale_factor(context: &ManipulationContext<'_>) -> f32 {
    let start = (context.frame.start_hit - context.frame.pivot).length();
    if start <= 1e-4 {
        return 1.0;
    }
    let now = (context.plane_hit() - context.frame.pivot).length();
    let ratio = (now / start).max(1e-4);
    // Precision moves the ratio toward 1 rather than toward 0: a tenth of "twice as big" is "a tenth
    // bigger", not "a fifth the size". Multiplicative quantities have their own arithmetic and this
    // is the one place in the module where forgetting it would be silent.
    (1.0 + (ratio - 1.0) * context.input.precision_factor()).max(1e-4)
}

/// The shortest spoke a rotation or a scale can be measured from.
///
/// The same 1e-4 `scale_factor` refuses to divide by, stated once so the two cannot disagree.
const SPOKE_MINIMUM: f32 = 1e-4;

/// A ray that makes the manipulation come out at exactly `amount`.
///
/// Constructed by inverting what each manipulator reads, which is why it is beside them: a
/// translation along an axis reads [`ManipulationContext::axis_parameter`], and everything else
/// reads [`ManipulationContext::plane_hit`].
fn ray_for_amount(frame: &DragFrame, mode: GizmoMode, amount: f32) -> Ray {
    let normal = frame.plane_normal.normalized_or(Vec3::Z);
    let along_axis = matches!(frame.handle, Handle::AxisX | Handle::AxisY | Handle::AxisZ);
    if mode == GizmoMode::Translate && along_axis {
        // `closest_parameter_on_axis` is exact for a ray whose origin sits on the axis and whose
        // direction is perpendicular to it: the determinant is one and the result is the origin's
        // own parameter. The plane normal is perpendicular to the axis for an axis handle, by
        // construction in `world_plane`.
        return Ray {
            origin: frame.pivot + frame.axis * (frame.start_parameter + amount),
            direction: normal,
        };
    }
    let spoke = frame.start_hit - frame.pivot;
    let target = match mode {
        GizmoMode::Rotate => frame.pivot + Quat::from_axis_angle(normal, amount).rotate(spoke),
        GizmoMode::Scale => frame.pivot + spoke * amount,
        // A plane or screen translate moves in the plane; the direction is the spoke's, so a stated
        // amount is a distance along it. A universal drag is resolved by its handle before it gets
        // here, so this arm is the free-move case rather than an ambiguity.
        GizmoMode::Translate | GizmoMode::Universal => {
            frame.start_hit + spoke.normalized_or(Vec3::X) * amount
        }
    };
    // The plane intersection refuses a distance of zero, so the ray starts one unit behind the
    // target along the plane normal and travels toward it. `at(1)` is then the target.
    Ray {
        origin: target - normal,
        direction: normal,
    }
}

/// Which lanes a scale drag multiplies.
///
/// An axis lock overrides the handle: `R` then `Y` scales along Y whichever box was grabbed, which
/// is what makes the lock worth having on a gizmo that is nearly edge-on.
fn scale_lanes(handle: Handle, lock: AxisLock, factor: f32) -> Vec3 {
    if lock.constrains() {
        return Vec3::new(
            if lock.x { factor } else { 1.0 },
            if lock.y { factor } else { 1.0 },
            if lock.z { factor } else { 1.0 },
        );
    }
    match handle {
        Handle::AxisX | Handle::BoxX => Vec3::new(factor, 1.0, 1.0),
        Handle::AxisY | Handle::BoxY => Vec3::new(1.0, factor, 1.0),
        Handle::AxisZ | Handle::BoxZ => Vec3::new(1.0, 1.0, factor),
        _ => Vec3::new(factor, factor, factor),
    }
}

/// The manipulators available, built in and registered.
///
/// A plugin's gizmo is added here and is thereafter indistinguishable: [`Drag::begin`] looks its
/// manipulator up by identifier and knows nothing else about it.
pub struct GizmoRegistry {
    manipulators: Vec<Box<dyn Manipulator>>,
}

impl Default for GizmoRegistry {
    fn default() -> Self {
        Self::with_builtins()
    }
}

impl GizmoRegistry {
    /// Translate, rotate and scale.
    #[must_use]
    pub fn with_builtins() -> Self {
        Self {
            manipulators: vec![Box::new(Translate), Box::new(Rotate), Box::new(Scale)],
        }
    }

    /// Register a manipulator, replacing one with the same identifier.
    ///
    /// Replacing rather than refusing so that a plugin can substitute a better translate gizmo,
    /// which `project-and-plugins` requires of every extension point that is not a security
    /// boundary.
    pub fn register(&mut self, manipulator: Box<dyn Manipulator>) {
        let id = manipulator.id();
        self.manipulators.retain(|existing| existing.id() != id);
        self.manipulators.push(manipulator);
    }

    /// The manipulator with this identifier.
    #[must_use]
    pub fn get(&self, id: &str) -> Option<&dyn Manipulator> {
        self.manipulators
            .iter()
            .find(|manipulator| manipulator.id() == id)
            .map(AsRef::as_ref)
    }

    /// The manipulator a mode selects, which is the built-in unless a plugin replaced it.
    ///
    /// [`GizmoMode::Universal`] has no manipulator of its own — it is three gizmos drawn at once —
    /// so it answers with the one its **default** handle would select. Use
    /// [`GizmoRegistry::for_handle`] wherever a handle is known, which is everywhere a drag begins.
    #[must_use]
    pub fn for_mode(&self, mode: GizmoMode) -> Option<&dyn Manipulator> {
        match mode {
            GizmoMode::Universal => self.get(HandleRole::Move.manipulator()),
            other => self.get(other.name()),
        }
    }

    /// The manipulator a mode and a grabbed handle select.
    ///
    /// **This is where the universal gizmo happens**, and it is three lines rather than a mode of
    /// its own: a ring turns, a box resizes, an arrow moves, and the mode only decides which of them
    /// were drawn. A universal mode implemented as a fourth `Manipulator` would have had to
    /// re-implement all three and would have drifted from them.
    #[must_use]
    pub fn for_handle(&self, mode: GizmoMode, handle: Handle) -> Option<&dyn Manipulator> {
        match mode {
            GizmoMode::Universal => self.get(handle.role().manipulator()),
            other => self.get(other.name()),
        }
    }

    /// Every registered identifier, for a toolbar and for a test.
    #[must_use]
    pub fn ids(&self) -> Vec<&'static str> {
        self.manipulators
            .iter()
            .map(|manipulator| manipulator.id())
            .collect()
    }
}

/// A manipulation in progress: one transaction, the captured start state, and the geometry the drag
/// was set up with.
///
/// **A drag owns an open transaction.** It must be finished with [`Drag::commit`] or
/// [`Drag::cancel`]; dropping it leaves the document's scope open, which the document itself will
/// report the next time anything tries to commit. That is deliberate rather than a `Drop`
/// implementation: finishing needs the document, `Drop` cannot have it, and a silent rollback in a
/// destructor is a worse failure than a loud one at the next commit.
#[derive(Clone, PartialEq, Debug)]
pub struct Drag {
    manipulator_id: String,
    mode: GizmoMode,
    frame: DragFrame,
    starts: Vec<DragStart>,
    binding: TransformBinding,
    open: bool,
    /// Whether the drag created the objects it is moving, which decides whether a drag that ended
    /// where it began has anything to record.
    duplicated: bool,
}

/// What a drag needs to begin.
pub struct DragRequest<'a> {
    /// The manipulator's identifier.
    pub manipulator: &'a str,
    /// Which handle was grabbed.
    pub handle: Handle,
    /// The frame the gizmo's axes are in.
    pub space: GizmoSpace,
    /// What the manipulation happens about.
    pub pivot: Pivot,
    /// The objects being manipulated, in selection order; the last is the active one.
    pub nodes: &'a [NodeId],
    /// Which fields hold the transform.
    pub binding: TransformBinding,
    /// The view the drag started in.
    pub view: &'a ViewState,
    /// The pixel the drag started at.
    pub pixel: (f32, f32),
    /// Who is dragging. An agent's drag is a person's drag with a different name on the entry.
    pub actor: Actor,
    /// **Duplicate and transform** — `Alt` in the reference's shortcut table.
    ///
    /// The copies are made *inside the drag's own transaction*, so the whole gesture is one history
    /// entry and one undo puts the scene back exactly as it was. Duplicating first and dragging
    /// afterwards would be two entries, and the undo that removed the copy would leave the original
    /// moved.
    pub duplicate: bool,
    /// The selection's world bounds, when the runtime has reported them.
    ///
    /// Only [`Pivot::Bounds`] reads it, and it falls back to the box containing the objects' origins
    /// — which is the honest answer a document can give on its own. See [`Pivot::Bounds`].
    pub bounds: Option<Bounds>,
}

impl Drag {
    /// Capture the state and open the transaction.
    ///
    /// One [`Document::begin_interaction`] for the whole drag, with a key derived from the
    /// manipulator and the objects, so that the hundreds of `SetField`s a drag records collapse into
    /// one entry named for what the user did.
    pub fn begin(
        registry: &GizmoRegistry,
        document: &mut Document,
        request: &DragRequest<'_>,
    ) -> Result<Self> {
        let manipulator = registry.get(request.manipulator).ok_or_else(|| {
            Problem::new(
                format!("begin a {} drag", request.manipulator),
                "no manipulator is registered under that identifier",
            )
            .with_remedy("register the gizmo before using it, or use translate, rotate or scale")
        })?;

        let starts = capture(document, request.nodes, request.binding);
        if starts.is_empty() {
            return Err(Problem::new(
                "begin a drag",
                "nothing selected carries the transform this gizmo edits",
            )
            .with_remedy("select an object with that component, or choose another gizmo"));
        }

        let handle = normalise(request.handle, manipulator.mode());
        let frame = build_frame(request, handle, &starts);
        document.begin_interaction(
            format!(
                "{}{} {}",
                if request.duplicate {
                    "Duplicate and "
                } else {
                    ""
                },
                capitalised(manipulator.id()),
                handle.name()
            ),
            request.actor.clone(),
            format!("gizmo:{}:{}", manipulator.id(), starts[0].node),
        );
        let starts = if request.duplicate {
            match duplicate_all(document, &starts, request.binding) {
                Ok(copies) => copies,
                Err(problem) => {
                    // The transaction is already open, and leaving it open would make the *next*
                    // commit fail with a problem that names something else entirely.
                    document.cancel()?;
                    return Err(problem);
                }
            }
        } else {
            starts
        };
        Ok(Self {
            manipulator_id: manipulator.id().to_string(),
            mode: manipulator.mode(),
            frame,
            starts,
            binding: request.binding,
            open: true,
            duplicated: request.duplicate,
        })
    }

    /// Whether the transaction is still open.
    #[must_use]
    pub const fn is_open(&self) -> bool {
        self.open
    }

    /// The drag's geometry, for a test and for a tool that wants to drive it numerically.
    #[must_use]
    pub const fn frame(&self) -> &DragFrame {
        &self.frame
    }

    /// What the engine needs in order to draw the gizmo now.
    #[must_use]
    pub fn intent(&self) -> GizmoIntent {
        GizmoIntent {
            mode: self.mode,
            orientation: self.frame.orientation,
            pivot: self.frame.pivot,
            hovered: Some(self.frame.handle),
            active: Some(self.frame.handle),
            manipulator: self.manipulator_id.clone(),
        }
    }

    /// Advance the drag to a new cursor position and write the result into the document.
    ///
    /// Every object's transform is recomputed from its captured start state. Nothing reads the value
    /// the previous frame wrote, which is the property the module note is about.
    pub fn update(
        &mut self,
        registry: &GizmoRegistry,
        document: &mut Document,
        view: &ViewState,
        pixel: (f32, f32),
        snap: &SnapSettings,
        input: DragInput,
    ) -> Result<Feedback> {
        if !self.open {
            return Err(Problem::new(
                "continue a drag",
                "it has already been committed or cancelled",
            )
            .with_remedy("begin a new drag"));
        }
        let frame = self.frame;
        self.write_frame(
            registry,
            document,
            &frame,
            view.ray_through_pixel(pixel.0, pixel.1),
            snap,
            input,
        )
    }

    /// Advance the drag by a **stated amount** rather than by a cursor position.
    ///
    /// Metres along the handle's axis for a translate, radians about it for a rotate, a factor for a
    /// scale — the same units [`Manipulator::magnitude`] reports, so "move it back by what it just
    /// moved" is one negation rather than an inverse projection.
    ///
    /// --- WHY THIS IS HERE AND NOT IN THE CALLER -------------------------------------------------
    ///
    /// `editor-agent-interface` requires that an agent's translate, rotate and scale "execute
    /// through **the same manipulation implementation** a gizmo drag uses", inheriting its
    /// guarantees — start-state capture, one transaction, cancellability, and identical treatment of
    /// pivot, space, snapping and constraints. A caller outside this module can only satisfy that by
    /// synthesising a ray, and synthesising a ray needs the drag's private geometry: which plane, in
    /// which orientation, through which pivot. So the synthesis lives beside the geometry, and every
    /// caller — an agent, a script, a nudge key — gets the same manipulator, the same snapping and
    /// the same write path as a hand on a mouse.
    ///
    /// The frame is **conditioned** first; see [`Drag::conditioned_frame`].
    pub fn advance_by(
        &mut self,
        registry: &GizmoRegistry,
        document: &mut Document,
        amount: f32,
        snap: &SnapSettings,
        input: DragInput,
    ) -> Result<Feedback> {
        if !self.open {
            return Err(Problem::new(
                "continue a drag",
                "it has already been committed or cancelled",
            )
            .with_remedy("begin a new drag"));
        }
        let frame = self.conditioned_frame();
        let ray = ray_for_amount(&frame, self.mode, amount);
        self.write_frame(registry, document, &frame, ray, snap, input)
    }

    /// The drag's geometry with a usable spoke, for a stated-amount manipulation.
    ///
    /// A rotate and a scale are both measured from the vector between the pivot and where the ray
    /// first met the plane. That vector is zero in two cases a *cursor* never has to care about,
    /// because in both of them a drag simply does nothing: the ray ran parallel to the plane, or it
    /// met it exactly at the pivot. A stated amount has to work anyway — "turn it 90°" cannot depend
    /// on where a camera happens to be — so a degenerate spoke is replaced here, in a **copy**, and
    /// the drag's own frame is left exactly as the interactive path recorded it.
    fn conditioned_frame(&self) -> DragFrame {
        let mut frame = self.frame;
        if (frame.start_hit - frame.pivot).length() > SPOKE_MINIMUM {
            return frame;
        }
        let normal = frame.plane_normal.normalized_or(Vec3::Z);
        // Any unit vector in the plane will do: the amount is measured relative to this one, so the
        // result is the same whichever is chosen. Crossing with the least-aligned cardinal axis is
        // what keeps it from collapsing when the normal is itself cardinal, which it usually is.
        let seed = if normal.x.abs() < 0.9 {
            Vec3::X
        } else {
            Vec3::Y
        };
        frame.start_hit = frame.pivot + normal.cross(seed).normalized_or(Vec3::X);
        frame
    }

    /// One step of the manipulation, from whatever ray and whatever frame the caller resolved.
    ///
    /// The only place a transform is written, so the interactive path and the stated-amount path
    /// cannot drift apart: they differ in the ray and in nothing else.
    fn write_frame(
        &self,
        registry: &GizmoRegistry,
        document: &mut Document,
        frame: &DragFrame,
        ray: Ray,
        snap: &SnapSettings,
        input: DragInput,
    ) -> Result<Feedback> {
        let manipulator = registry.get(&self.manipulator_id).ok_or_else(|| {
            Problem::new(
                "continue a drag",
                "the manipulator it started with is no longer registered",
            )
            .with_remedy("cancel the drag; a plugin was unloaded while it was in progress")
        })?;

        let context = ManipulationContext {
            frame,
            ray,
            snap,
            input,
        };
        for start in &self.starts {
            let transform = manipulator.manipulate(&context, start);
            write_transform(document, start.node, self.binding, transform)?;
        }
        Ok(self.feedback(manipulator.magnitude(&context)))
    }

    /// Finish the drag. One transaction, or none when nothing actually changed.
    ///
    /// Returns whether an entry was recorded.
    ///
    /// **A drag that ended where it began records nothing.** That has to be decided here rather than
    /// left to the document: `Document::commit` drops a transaction with *no* operations, and a
    /// returning drag has operations — a run of `SetField`s that `Transaction::compact` collapses
    /// into one whose before and after are the same bits. Without this check, nudging an object and
    /// changing your mind leaves a "Translate X" in the history that undoes to the state it was
    /// already in, and a history full of those is a history nobody reads.
    ///
    /// A **duplicating** drag is the exception and is always recorded: the copies exist whether or
    /// not they were moved afterwards, and discarding them because the hand came back would throw
    /// away what the user asked for.
    pub fn commit(&mut self, document: &mut Document) -> Result<bool> {
        if !self.open {
            return Err(Problem::new("commit a drag", "it is already finished"));
        }
        self.open = false;
        if !self.duplicated && !self.moved_anything(document) {
            document.cancel()?;
            return Ok(false);
        }
        Ok(document.commit()?.is_some())
    }

    /// Whether anything the drag captured differs, bit for bit, from what the document holds now.
    fn moved_anything(&self, document: &Document) -> bool {
        self.starts.iter().any(|start| {
            read_transform(document, start.node, self.binding).is_none_or(|now| {
                now.translation.to_array().map(f32::to_bits)
                    != start.transform.translation.to_array().map(f32::to_bits)
                    || now.rotation.to_array().map(f32::to_bits)
                        != start.transform.rotation.to_array().map(f32::to_bits)
                    || now.scale.to_array().map(f32::to_bits)
                        != start.transform.scale.to_array().map(f32::to_bits)
            })
        })
    }

    /// Abandon the drag. The document returns to what it was and no entry is recorded.
    pub fn cancel(&mut self, document: &mut Document) -> Result<()> {
        if !self.open {
            return Err(Problem::new("cancel a drag", "it is already finished"));
        }
        self.open = false;
        document.cancel()
    }

    /// The objects the drag captured, and what they were.
    #[must_use]
    pub fn starts(&self) -> &[DragStart] {
        &self.starts
    }

    fn feedback(&self, magnitude: f32) -> Feedback {
        let active = self
            .starts
            .last()
            .map_or_else(Transform3::default, |start| start.transform);
        // The universal gizmo's feedback is the *handle's*, not the mode's: dragging a ring says
        // degrees whichever mode drew it.
        let reported = match self.mode {
            GizmoMode::Universal => match self.frame.handle.role() {
                HandleRole::Move => GizmoMode::Translate,
                HandleRole::Turn => GizmoMode::Rotate,
                HandleRole::Resize => GizmoMode::Scale,
            },
            other => other,
        };
        match reported {
            GizmoMode::Translate => Feedback {
                delta: format!("{magnitude:.3} m along {}", self.frame.handle.name()),
                value: format_vec3(active.translation, "m"),
            },
            GizmoMode::Rotate => Feedback {
                delta: format!(
                    "{:.2}° about {}",
                    magnitude.to_degrees(),
                    self.frame.handle.name()
                ),
                value: format!("{:.2}°", rotation_degrees(active.rotation)),
            },
            GizmoMode::Scale | GizmoMode::Universal => Feedback {
                delta: format!("×{magnitude:.3} on {}", self.frame.handle.name()),
                value: format_vec3(active.scale, ""),
            },
        }
    }
}

/// The handle a mode actually drags, given the one the caller named.
///
/// A rotate drag on `AxisX` is a drag on the **X ring**, and the difference is not cosmetic: a ring
/// turns about its own axis, while an axis handle is intersected against the plane most nearly
/// facing the camera. Without this, `rotate` on `AxisX` turned the object about whichever axis the
/// camera happened to make most visible — correct-looking on screen and wrong in the document.
const fn normalise(handle: Handle, mode: GizmoMode) -> Handle {
    match (mode, handle) {
        (GizmoMode::Rotate, Handle::AxisX) => Handle::RingX,
        (GizmoMode::Rotate, Handle::AxisY) => Handle::RingY,
        (GizmoMode::Rotate, Handle::AxisZ) => Handle::RingZ,
        (GizmoMode::Rotate, Handle::Screen) => Handle::ScreenRing,
        (GizmoMode::Scale, Handle::AxisX) => Handle::BoxX,
        (GizmoMode::Scale, Handle::AxisY) => Handle::BoxY,
        (GizmoMode::Scale, Handle::AxisZ) => Handle::BoxZ,
        _ => handle,
    }
}

/// Copy every captured object, and return the copies as the objects the drag will move.
///
/// Components and their fields, the layer, and the parent. Not the children: duplicating a subtree
/// is a command with its own name and its own semantics for prefabs, and doing half of it here would
/// be the worse kind of surprise.
fn duplicate_all(
    document: &mut Document,
    starts: &[DragStart],
    binding: TransformBinding,
) -> Result<Vec<DragStart>> {
    let mut copies = Vec::with_capacity(starts.len());
    for start in starts {
        let source = document
            .content()
            .node(start.node)
            .ok_or_else(|| {
                Problem::new(
                    "duplicate and transform",
                    "the object being dragged is no longer in the document",
                )
                .with_remedy("release the modifier and drag again")
            })?
            .clone();
        let copy = document.create_node(source.parent)?;
        for (component, fields) in source.components {
            document.add_component(copy, component, fields.into_iter().collect::<Vec<_>>())?;
        }
        copies.push(DragStart {
            node: copy,
            transform: start.transform,
        });
    }
    let _ = binding;
    Ok(copies)
}

/// The transform each node carries now, refusing a node that does not carry one.
fn capture(document: &Document, nodes: &[NodeId], binding: TransformBinding) -> Vec<DragStart> {
    let mut starts = Vec::new();
    for node in nodes {
        let Some(transform) = read_transform(document, *node, binding) else {
            continue;
        };
        starts.push(DragStart {
            node: *node,
            transform,
        });
    }
    starts
}

fn read_transform(
    document: &Document,
    node: NodeId,
    binding: TransformBinding,
) -> Option<Transform3> {
    let content = document.content();
    let translation = content.field(node, binding.component, binding.translation)?;
    let rotation = content.field(node, binding.component, binding.rotation)?;
    let scale = content.field(node, binding.component, binding.scale)?;
    Some(Transform3 {
        translation: Vec3::from_array(vec3_of(translation)?),
        rotation: Quat::from_array(quat_of(rotation)?),
        scale: Vec3::from_array(vec3_of(scale)?),
    })
}

fn vec3_of(value: &Value) -> Option<[f32; 3]> {
    match value {
        Value::Vec3(lanes) => Some(*lanes),
        _ => None,
    }
}

fn quat_of(value: &Value) -> Option<[f32; 4]> {
    match value {
        Value::Quat(lanes) => Some(*lanes),
        _ => None,
    }
}

/// Write a transform through the document's own write path, touching only what changed.
///
/// `set_field` reads the before value from the document itself, so a mis-recorded delta is
/// impossible — and this is the only way this module touches a document, which is what makes
/// "transactions are the only path for persistent mutation" true of gizmos.
///
/// UNCHANGED FIELDS ARE SKIPPED, AND IT MATTERS MORE THAN IT LOOKS. A translate drag does not touch
/// rotation or scale, so the transaction's operations are all `SetField` on the same field — which
/// is what `Transaction::compact` collapses, because it collapses **consecutive** operations that
/// address the same target. Writing all three every frame would interleave them
/// (translation, rotation, scale, translation, …), nothing would ever be consecutive, and a
/// two-hundred-frame drag would carry six hundred operations into history instead of one.
fn write_transform(
    document: &mut Document,
    node: NodeId,
    binding: TransformBinding,
    transform: Transform3,
) -> Result<()> {
    write_field(
        document,
        node,
        binding.component,
        binding.translation,
        Value::Vec3(transform.translation.to_array()),
    )?;
    write_field(
        document,
        node,
        binding.component,
        binding.rotation,
        Value::Quat(transform.rotation.to_array()),
    )?;
    write_field(
        document,
        node,
        binding.component,
        binding.scale,
        Value::Vec3(transform.scale.to_array()),
    )
}

/// Record a field only when the value differs from what the document already holds.
fn write_field(
    document: &mut Document,
    node: NodeId,
    component: TypeId,
    field: FieldId,
    value: Value,
) -> Result<()> {
    if document.content().field(node, component, field) == Some(&value) {
        return Ok(());
    }
    document.set_field(node, component, field, value)
}

/// Set the drag's geometry up once, from the state captured at drag start.
fn build_frame(request: &DragRequest<'_>, handle: Handle, starts: &[DragStart]) -> DragFrame {
    let active = starts.last().expect("callers check for an empty capture");
    let orientation = orientation_of(request.space, active.transform.rotation, request.view);
    let pivot = pivot_of(request.pivot, request.bounds, starts);
    let axis = world_axis(handle, orientation, request.view);
    let plane_normal = world_plane(handle, orientation, request.view, axis);

    let ray = request
        .view
        .ray_through_pixel(request.pixel.0, request.pixel.1);
    let start_hit = ray.intersect_plane(pivot, plane_normal).unwrap_or(pivot);
    let start_parameter = ray.closest_parameter_on_axis(pivot, axis).unwrap_or(0.0);

    DragFrame {
        pivot,
        axis,
        plane_normal,
        start_hit,
        start_parameter,
        handle,
        orientation,
        individual: request.pivot == Pivot::Individual,
    }
}

/// The rotation taking the gizmo's own axes into world space.
///
/// `Parent` resolves to the world's axes here, because the document's parent transform is not
/// something this module reads — the caller that knows the hierarchy passes `Custom` with the
/// parent's world rotation. Stated rather than silently equated: a parent-space gizmo on a rotated
/// parent needs that rotation, and there is no way to get it wrong quietly.
fn orientation_of(space: GizmoSpace, object: Quat, view: &ViewState) -> Quat {
    match space {
        GizmoSpace::World | GizmoSpace::Parent => Quat::IDENTITY,
        GizmoSpace::Local => object,
        GizmoSpace::View => view.camera.rotation,
        GizmoSpace::Custom(rotation) => rotation,
    }
}

fn pivot_of(pivot: Pivot, bounds: Option<Bounds>, starts: &[DragStart]) -> Vec3 {
    let active = starts.last().expect("callers check for an empty capture");
    match pivot {
        // Individual objects turn about their own origins, which `ManipulationContext::pivot_for`
        // resolves per object; the frame's pivot is the active object's, so that the gizmo is drawn
        // where the user grabbed it.
        Pivot::Pivot | Pivot::Individual => active.transform.translation,
        Pivot::Center => {
            let sum = starts.iter().fold(Vec3::ZERO, |total, start| {
                total + start.transform.translation
            });
            #[allow(
                clippy::cast_precision_loss,
                reason = "a selection is far below 2^24 objects"
            )]
            let count = starts.len() as f32;
            sum * (1.0 / count)
        }
        Pivot::Bounds => bounds.map_or_else(
            || {
                starts
                    .iter()
                    .map(|start| Bounds::point(start.transform.translation))
                    .reduce(Bounds::union)
                    .unwrap_or_else(|| Bounds::point(active.transform.translation))
                    .center()
            },
            Bounds::center,
        ),
    }
}

fn world_axis(handle: Handle, orientation: Quat, view: &ViewState) -> Vec3 {
    handle.axis().map_or_else(
        || {
            // A plane or screen handle still needs an axis for the manipulators that ask for one.
            // The camera's right is the least surprising choice: it is the direction a horizontal
            // cursor movement corresponds to.
            view.camera.right()
        },
        |axis| orientation.rotate(axis),
    )
}

fn world_plane(handle: Handle, orientation: Quat, view: &ViewState, axis: Vec3) -> Vec3 {
    if let Some(normal) = handle.plane_normal() {
        return orientation.rotate(normal);
    }
    match handle {
        // An axis drag is intersected against the plane containing the axis and most nearly facing
        // the camera, which is what keeps a nearly edge-on axis from becoming unusable.
        Handle::AxisX | Handle::AxisY | Handle::AxisZ => {
            let facing = view.facing_plane_normal();
            let tangent = axis.cross(facing).normalized_or(view.camera.up());
            tangent.cross(axis).normalized_or(facing)
        }
        _ => view.facing_plane_normal(),
    }
}

fn capitalised(text: &str) -> String {
    let mut characters = text.chars();
    characters.next().map_or_else(String::new, |first| {
        first.to_uppercase().collect::<String>() + characters.as_str()
    })
}

fn format_vec3(value: Vec3, unit: &str) -> String {
    let suffix = if unit.is_empty() {
        String::new()
    } else {
        format!(" {unit}")
    };
    format!("{:.3}, {:.3}, {:.3}{suffix}", value.x, value.y, value.z)
}

/// The angle of a rotation, in degrees, for the feedback line.
fn rotation_degrees(rotation: Quat) -> f32 {
    2.0 * rotation.w.clamp(-1.0, 1.0).acos().to_degrees()
}

#[cfg(test)]
mod tests {
    use cy_editor_core::value::ValueKind;

    use super::*;

    /// The fixture is destructured at every use, because `Drag::begin` takes the registry by
    /// reference and the document mutably at the same time — which a struct holding both cannot
    /// provide. Splitting it is what the borrow checker asks for and it reads no worse.
    struct Fixture {
        document: Document,
        node: NodeId,
        binding: TransformBinding,
        registry: GizmoRegistry,
        view: ViewState,
    }

    fn fixture() -> Fixture {
        let mut document = Document::new("worlds/city.cyworld");
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
        let binding = TransformBinding {
            component,
            translation,
            rotation,
            scale,
        };
        let node = document
            .with_transaction("Populate", Actor::human("designer"), |document| {
                let node = document.create_node(None)?;
                document.add_component(
                    node,
                    component,
                    vec![
                        (translation, Value::Vec3([1.7, 0.0, 0.0])),
                        (rotation, Value::Quat(Quat::IDENTITY.to_array())),
                        (scale, Value::Vec3([1.0, 1.0, 1.0])),
                    ],
                )?;
                Ok(node)
            })
            .expect("a transaction that creates one node");

        let mut view = ViewState::new();
        view.camera.position = Vec3::new(0.0, 0.0, 20.0);
        Fixture {
            document,
            node,
            binding,
            registry: GizmoRegistry::with_builtins(),
            view,
        }
    }

    fn request<'a>(
        view: &'a ViewState,
        binding: TransformBinding,
        nodes: &'a [NodeId],
        manipulator: &'a str,
        handle: Handle,
    ) -> DragRequest<'a> {
        DragRequest {
            manipulator,
            handle,
            space: GizmoSpace::World,
            pivot: Pivot::Individual,
            nodes,
            binding,
            view,
            pixel: (960.0, 540.0),
            actor: Actor::human("designer"),
            duplicate: false,
            bounds: None,
        }
    }

    /// Snapping turned off, for the tests that measure a distance rather than a grid.
    fn free_snapping() -> SnapSettings {
        SnapSettings {
            modes: crate::snapping::SnapModes {
                grid: false,
                angle: false,
                scale: false,
                vertex: false,
                surface: false,
            },
            ..SnapSettings::default()
        }
    }

    fn translation_of(document: &Document, binding: TransformBinding, node: NodeId) -> [f32; 3] {
        match document
            .content()
            .field(node, binding.component, binding.translation)
            .expect("the node has a translation")
        {
            Value::Vec3(lanes) => *lanes,
            other => panic!("expected a Vec3, got {other:?}"),
        }
    }

    /// A pixel offset from the centre of the viewport, as a drag would produce.
    #[allow(clippy::cast_precision_loss, reason = "a loop counter below 2^24")]
    fn pixel(step: u32) -> (f32, f32) {
        (960.0 + (step as f32) * 3.0, 540.0)
    }

    #[test]
    fn a_drag_out_and_back_restores_the_original_bits() {
        // THE REQUIREMENT: "a drag returning to its origin returns the exact original values".
        // Compared as bit patterns, because that is the only comparison that means "exact" — an
        // accumulating implementation passes an epsilon comparison and fails this one.
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let before = translation_of(&document, binding, node);
        let snap = SnapSettings::default();

        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect("a drag on a node with a transform");

        for step in 0..64 {
            drag.update(
                &registry,
                &mut document,
                &view,
                pixel(step),
                &snap,
                DragInput::NONE,
            )
            .expect("the drag continues");
        }
        // And back to exactly where it started.
        drag.update(
            &registry,
            &mut document,
            &view,
            (960.0, 540.0),
            &snap,
            DragInput::NONE,
        )
        .expect("the drag continues");
        drag.commit(&mut document).expect("the drag finishes");

        let after = translation_of(&document, binding, node);
        assert_eq!(
            before.map(f32::to_bits),
            after.map(f32::to_bits),
            "{before:?} became {after:?}"
        );
    }

    #[test]
    fn a_drag_that_ended_where_it_began_records_nothing() {
        // A REGRESSION TEST, and a claim this module used to make and not keep. `Document::commit`
        // drops a transaction with no operations; a returning drag has operations whose before and
        // after are identical, which is a different thing. Without the check in `Drag::commit`,
        // nudging an object and changing your mind leaves an entry in the history that undoes to
        // the state it was already in.
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let entries = document.history().entries().len();
        let snap = SnapSettings::default();
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect("a drag");
        for step in 0..12 {
            drag.update(
                &registry,
                &mut document,
                &view,
                pixel(step),
                &snap,
                DragInput::NONE,
            )
            .expect("the drag continues");
        }
        drag.update(
            &registry,
            &mut document,
            &view,
            (960.0, 540.0),
            &snap,
            DragInput::NONE,
        )
        .expect("the drag continues");

        assert!(
            !drag.commit(&mut document).expect("it finishes"),
            "a drag that moved nothing recorded an entry"
        );
        assert_eq!(document.history().entries().len(), entries);
        assert!(!document.is_transaction_open());
    }

    #[test]
    fn a_manipulation_produces_exactly_one_history_entry() {
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let before = document.history().entries().len();

        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect("a drag");
        let snap = SnapSettings::default();
        for step in 1..200 {
            drag.update(
                &registry,
                &mut document,
                &view,
                pixel(step),
                &snap,
                DragInput::NONE,
            )
            .expect("the drag continues");
        }
        assert!(drag.commit(&mut document).expect("it commits"));

        let entries = document.history().entries();
        assert_eq!(entries.len(), before + 1, "one entry for one drag");
        // A hundred and ninety-nine updates, one operation. A translate drag touches only the
        // translation, so every recorded operation addresses the same field and
        // `Transaction::compact` collapses the run — which it can only do because the writes are
        // consecutive. See `write_transform`.
        assert_eq!(
            entries.last().expect("the entry").operations.len(),
            1,
            "one operation, whatever the frame rate was"
        );
    }

    #[test]
    fn a_cancelled_drag_leaves_the_document_as_it_was_and_records_nothing() {
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let before = translation_of(&document, binding, node);
        let entries = document.history().entries().len();

        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect("a drag");
        drag.update(
            &registry,
            &mut document,
            &view,
            (1400.0, 540.0),
            &SnapSettings::default(),
            DragInput::NONE,
        )
        .expect("the drag continues");
        assert_ne!(
            translation_of(&document, binding, node).map(f32::to_bits),
            before.map(f32::to_bits),
            "it did move"
        );

        drag.cancel(&mut document).expect("it cancels");
        assert_eq!(
            translation_of(&document, binding, node).map(f32::to_bits),
            before.map(f32::to_bits)
        );
        assert_eq!(document.history().entries().len(), entries);
        assert!(!drag.is_open());
    }

    #[test]
    fn a_drag_reports_the_delta_and_the_resulting_value() {
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect("a drag");
        let feedback = drag
            .update(
                &registry,
                &mut document,
                &view,
                (1200.0, 540.0),
                &SnapSettings::default(),
                DragInput::NONE,
            )
            .expect("the drag continues");
        drag.cancel(&mut document).expect("it cancels");

        assert!(feedback.delta.contains('X'), "{feedback:?}");
        assert!(feedback.delta.contains('m'), "{feedback:?}");
        assert!(!feedback.value.is_empty(), "{feedback:?}");
    }

    #[test]
    fn manipulation_reads_the_captured_state_and_never_the_document() {
        // The property behind the exactness: two updates to the same pixel produce the same answer,
        // even though the first one already wrote a different value into the document. An
        // accumulating implementation gives two different answers here.
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect("a drag");
        let snap = SnapSettings::default();

        drag.update(
            &registry,
            &mut document,
            &view,
            (1100.0, 540.0),
            &snap,
            DragInput::NONE,
        )
        .expect("the drag continues");
        let first = translation_of(&document, binding, node);
        drag.update(
            &registry,
            &mut document,
            &view,
            (1100.0, 540.0),
            &snap,
            DragInput::NONE,
        )
        .expect("the drag continues");
        let second = translation_of(&document, binding, node);
        drag.cancel(&mut document).expect("it cancels");

        assert_eq!(first.map(f32::to_bits), second.map(f32::to_bits));
    }

    #[test]
    fn a_rotate_drag_about_an_objects_own_centre_turns_it_without_moving_it() {
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &DragRequest {
                pivot: Pivot::Center,
                ..request(&view, binding, &nodes, "rotate", Handle::AxisY)
            },
        )
        .expect("a drag");
        // One object, so its centre is its own origin and a rotation leaves it in place.
        let before = translation_of(&document, binding, node);
        drag.update(
            &registry,
            &mut document,
            &view,
            (1100.0, 640.0),
            &SnapSettings::default(),
            DragInput::NONE,
        )
        .expect("the drag continues");
        let after = translation_of(&document, binding, node);
        drag.cancel(&mut document).expect("it cancels");
        assert_eq!(before.map(f32::to_bits), after.map(f32::to_bits));
    }

    #[test]
    fn a_plugin_gizmo_is_registered_and_used_by_the_same_path() {
        // "WHEN a plugin registers a gizmo THEN it SHALL render, pick, and undo identically to
        // built-in gizmos." The check is that the drag path takes it with no branch: the same
        // begin, the same update, the same one transaction, the same undo.
        struct Nudge;
        impl Manipulator for Nudge {
            fn id(&self) -> &'static str {
                "plugin.nudge"
            }
            fn mode(&self) -> GizmoMode {
                GizmoMode::Translate
            }
            fn manipulate(
                &self,
                context: &ManipulationContext<'_>,
                start: &DragStart,
            ) -> Transform3 {
                Transform3 {
                    translation: start.transform.translation
                        + Vec3::Y * (context.axis_parameter() - context.frame.start_parameter),
                    ..start.transform
                }
            }
            fn magnitude(&self, context: &ManipulationContext<'_>) -> f32 {
                context.axis_parameter() - context.frame.start_parameter
            }
        }

        let Fixture {
            mut document,
            node,
            binding,
            mut registry,
            view,
        } = fixture();
        registry.register(Box::new(Nudge));
        assert!(registry.ids().contains(&"plugin.nudge"));

        let nodes = [node];
        let entries = document.history().entries().len();
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "plugin.nudge", Handle::AxisX),
        )
        .expect("a plugin drag begins like any other");
        drag.update(
            &registry,
            &mut document,
            &view,
            (1200.0, 540.0),
            &SnapSettings::default(),
            DragInput::NONE,
        )
        .expect("the drag continues");
        assert!(drag.commit(&mut document).expect("it commits"));
        assert_eq!(document.history().entries().len(), entries + 1);

        // And it undoes identically, because it produced the same operations.
        document.undo().expect("undo").expect("an entry to undo");
        assert_eq!(
            translation_of(&document, binding, node).map(f32::to_bits),
            [1.7_f32, 0.0, 0.0].map(f32::to_bits)
        );
    }

    #[test]
    fn a_plugin_may_replace_a_built_in_rather_than_shadowing_it() {
        struct BetterTranslate;
        impl Manipulator for BetterTranslate {
            fn id(&self) -> &'static str {
                "translate"
            }
            fn mode(&self) -> GizmoMode {
                GizmoMode::Translate
            }
            fn manipulate(&self, _: &ManipulationContext<'_>, start: &DragStart) -> Transform3 {
                start.transform
            }
            fn magnitude(&self, _: &ManipulationContext<'_>) -> f32 {
                0.0
            }
        }
        let mut registry = GizmoRegistry::with_builtins();
        let before = registry.ids().len();
        registry.register(Box::new(BetterTranslate));
        assert_eq!(registry.ids().len(), before, "replaced, not shadowed");
        assert!(registry.for_mode(GizmoMode::Translate).is_some());
    }

    #[test]
    fn an_unregistered_manipulator_is_refused_with_a_remedy() {
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let problem = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "plugin.missing", Handle::AxisX),
        )
        .expect_err("no such manipulator");
        assert!(problem.remedy.is_some(), "{problem}");
        assert!(!document.is_transaction_open(), "and nothing was opened");
    }

    #[test]
    fn dragging_something_with_no_transform_is_refused_before_a_transaction_opens() {
        let Fixture {
            mut document,
            binding,
            registry,
            view,
            ..
        } = fixture();
        let bare = document
            .with_transaction("Add", Actor::human("designer"), |document| {
                document.create_node(None)
            })
            .expect("a node with no components");
        let nodes = [bare];
        let problem = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect_err("nothing to drag");
        assert!(problem.remedy.is_some(), "{problem}");
        assert!(!document.is_transaction_open());
    }

    #[test]
    fn the_gizmo_intent_carries_no_geometry() {
        // The division of labour, as a property of the type: everything here is intent, and there is
        // nowhere to put a size, a colour or a depth mode.
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect("a drag");
        let intent = drag.intent();
        drag.cancel(&mut document).expect("it cancels");

        assert_eq!(intent.mode, GizmoMode::Translate);
        assert_eq!(intent.active, Some(Handle::AxisX));
        assert_eq!(intent.manipulator, "translate");
        assert!(intent.pivot.nearly_equals(Vec3::new(1.7, 0.0, 0.0), 1e-6));
    }

    #[test]
    fn continuing_a_finished_drag_is_refused_rather_than_writing_outside_a_transaction() {
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::AxisX),
        )
        .expect("a drag");
        drag.commit(&mut document).expect("it commits");
        let problem = drag
            .update(
                &registry,
                &mut document,
                &view,
                (1000.0, 540.0),
                &SnapSettings::default(),
                DragInput::NONE,
            )
            .expect_err("the drag is over");
        assert!(problem.remedy.is_some(), "{problem}");
    }

    #[test]
    fn a_ring_turns_about_its_own_axis_whatever_the_camera_is_doing() {
        // A REGRESSION TEST. `rotate` on `Handle::AxisX` used to be intersected against the plane
        // "most nearly facing the camera", which is right for an arrow and wrong for a ring: the
        // object turned about whichever axis the camera happened to make most visible. It looked
        // plausible on screen and was wrong in the document, which is the worst combination.
        for (handle, axis) in [
            (Handle::AxisX, Vec3::X),
            (Handle::AxisY, Vec3::Y),
            (Handle::AxisZ, Vec3::Z),
        ] {
            for camera in [
                Vec3::new(0.0, 0.0, 20.0),
                Vec3::new(20.0, 0.0, 0.0),
                Vec3::new(6.0, 9.0, 13.0),
            ] {
                let Fixture {
                    mut document,
                    node,
                    binding,
                    registry,
                    mut view,
                } = fixture();
                view.camera.position = camera;
                let nodes = [node];
                let drag = Drag::begin(
                    &registry,
                    &mut document,
                    &request(&view, binding, &nodes, "rotate", handle),
                )
                .expect("a drag");
                assert!(
                    drag.frame().plane_normal.nearly_equals(axis, 1e-6),
                    "{handle:?} from {camera:?} turns about {:?}",
                    drag.frame().plane_normal
                );
                assert_eq!(drag.frame().handle.role(), HandleRole::Turn);
            }
        }
    }

    #[test]
    fn the_universal_gizmo_chooses_its_manipulator_from_the_handle() {
        // The whole of what `T` is: one mode, three manipulations, decided by what was grabbed.
        let registry = GizmoRegistry::with_builtins();
        for (handle, expected) in [
            (Handle::AxisX, "translate"),
            (Handle::PlaneXY, "translate"),
            (Handle::Screen, "translate"),
            (Handle::RingY, "rotate"),
            (Handle::ScreenRing, "rotate"),
            (Handle::BoxZ, "scale"),
            (Handle::Uniform, "scale"),
        ] {
            let manipulator = registry
                .for_handle(GizmoMode::Universal, handle)
                .expect("a manipulator for every handle");
            assert_eq!(manipulator.id(), expected, "{handle:?}");
        }
        // And every handle any mode draws has one, so a universal gizmo can have no dead handle.
        for handle in Handle::ALL {
            assert!(
                registry.for_handle(GizmoMode::Universal, handle).is_some(),
                "{handle:?} has no manipulator"
            );
        }
    }

    #[test]
    fn the_centre_carries_three_separately_targetable_affordances() {
        // `docs/design/images/transform-gizmo.png`: "Screen Move (drag center circle), Uniform Scale
        // (center cube), Screen Rotate (outer ring)". Three handles, three roles, one place.
        let centre: Vec<Handle> = Handle::ALL
            .into_iter()
            .filter(|handle| handle.is_centre())
            .collect();
        assert_eq!(centre.len(), 3, "{centre:?}");
        let roles: Vec<HandleRole> = centre.iter().map(|handle| handle.role()).collect();
        assert!(roles.contains(&HandleRole::Move));
        assert!(roles.contains(&HandleRole::Turn));
        assert!(roles.contains(&HandleRole::Resize));
    }

    #[test]
    fn an_axis_lock_confines_the_movement_and_releasing_it_restores_the_original_bits() {
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let before = translation_of(&document, binding, node);
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "translate", Handle::Screen),
        )
        .expect("a drag");

        // Snapping off: this measures the constraint, and a grid would quantise the answer into
        // agreeing with it by accident.
        let free = free_snapping();
        let locked = DragInput {
            lock: AxisLock::only(1),
            ..DragInput::NONE
        };
        drag.update(
            &registry,
            &mut document,
            &view,
            (1400.0, 300.0),
            &free,
            locked,
        )
        .expect("the drag continues");
        let after = translation_of(&document, binding, node);
        assert_eq!(
            after[0].to_bits(),
            before[0].to_bits(),
            "X moved under a Y lock"
        );
        assert_eq!(after[2].to_bits(), before[2].to_bits(), "Z moved");
        assert_ne!(after[1].to_bits(), before[1].to_bits(), "Y did not move");

        // Back to the start with the lock still held: exactness survives the constraint, because it
        // is a linear function of the offset and a zero offset stays zero.
        drag.update(
            &registry,
            &mut document,
            &view,
            (960.0, 540.0),
            &free,
            locked,
        )
        .expect("the drag continues");
        assert_eq!(
            translation_of(&document, binding, node).map(f32::to_bits),
            before.map(f32::to_bits)
        );
        drag.cancel(&mut document).expect("it cancels");
    }

    #[test]
    fn the_precision_modifier_moves_a_tenth_as_far() {
        let mut travelled = Vec::new();
        for precision in [false, true] {
            let Fixture {
                mut document,
                node,
                binding,
                registry,
                view,
            } = fixture();
            let nodes = [node];
            let before = translation_of(&document, binding, node);
            let mut drag = Drag::begin(
                &registry,
                &mut document,
                &request(&view, binding, &nodes, "translate", Handle::AxisX),
            )
            .expect("a drag");
            drag.update(
                &registry,
                &mut document,
                &view,
                (1400.0, 540.0),
                &free_snapping(),
                DragInput {
                    precision,
                    ..DragInput::NONE
                },
            )
            .expect("the drag continues");
            let after = translation_of(&document, binding, node);
            drag.cancel(&mut document).expect("it cancels");
            travelled.push(after[0] - before[0]);
        }
        let ratio = travelled[1] / travelled[0];
        assert!(
            (ratio - PRECISION_FACTOR).abs() < 1e-4,
            "a precision drag travelled {ratio} of a normal one"
        );
    }

    #[test]
    fn duplicate_and_transform_leaves_the_original_where_it_was_and_undoes_as_one_entry() {
        // `Alt` in the reference's shortcut table. The property that decides the implementation:
        // ONE history entry. Duplicating in its own transaction and dragging in another would make
        // the first undo leave a moved original behind.
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let before = translation_of(&document, binding, node);
        let entries = document.history().entries().len();
        let count = document.content().node_count();

        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &DragRequest {
                duplicate: true,
                ..request(&view, binding, &nodes, "translate", Handle::AxisX)
            },
        )
        .expect("a duplicating drag");
        assert_eq!(
            document.content().node_count(),
            count + 1,
            "a copy was made"
        );
        let copy = drag.starts()[0].node;
        assert_ne!(copy, node, "the drag moves the copy, not the original");

        drag.update(
            &registry,
            &mut document,
            &view,
            (1400.0, 540.0),
            &SnapSettings::default(),
            DragInput::NONE,
        )
        .expect("the drag continues");
        assert!(drag.commit(&mut document).expect("it commits"));

        assert_eq!(
            translation_of(&document, binding, node).map(f32::to_bits),
            before.map(f32::to_bits),
            "the original moved"
        );
        assert_ne!(
            translation_of(&document, binding, copy).map(f32::to_bits),
            before.map(f32::to_bits),
            "the copy did not move"
        );
        assert_eq!(
            document.history().entries().len(),
            entries + 1,
            "one gesture, one entry"
        );

        document.undo().expect("undo").expect("an entry");
        assert_eq!(
            document.content().node_count(),
            count,
            "undo left the copy behind"
        );
    }

    #[test]
    fn the_four_pivot_modes_are_four_different_answers() {
        // Two objects, deliberately not symmetrical about their mean, so that Center and Bounds
        // differ — an editor offering both and computing one is indistinguishable from a correct one
        // until somebody selects three objects.
        let starts = [
            DragStart {
                node: NodeId::from_u128(1),
                transform: Transform3 {
                    translation: Vec3::new(0.0, 0.0, 0.0),
                    ..Transform3::default()
                },
            },
            DragStart {
                node: NodeId::from_u128(2),
                transform: Transform3 {
                    translation: Vec3::new(9.0, 0.0, 0.0),
                    ..Transform3::default()
                },
            },
            DragStart {
                node: NodeId::from_u128(3),
                transform: Transform3 {
                    translation: Vec3::new(12.0, 0.0, 0.0),
                    ..Transform3::default()
                },
            },
        ];
        assert_eq!(
            pivot_of(Pivot::Pivot, None, &starts),
            Vec3::new(12.0, 0.0, 0.0),
            "the active object is the last selected"
        );
        assert_eq!(
            pivot_of(Pivot::Center, None, &starts),
            Vec3::new(7.0, 0.0, 0.0)
        );
        assert_eq!(
            pivot_of(Pivot::Bounds, None, &starts),
            Vec3::new(6.0, 0.0, 0.0)
        );
        assert_eq!(
            pivot_of(Pivot::Individual, None, &starts),
            Vec3::new(12.0, 0.0, 0.0),
            "the gizmo is drawn on the active object even when each turns in place"
        );
        // And the runtime's bounds win over the box of origins when they are known.
        assert_eq!(
            pivot_of(
                Pivot::Bounds,
                Some(Bounds::from_center_extents(
                    Vec3::new(-4.0, 0.0, 0.0),
                    Vec3::new(1.0, 1.0, 1.0)
                )),
                &starts
            ),
            Vec3::new(-4.0, 0.0, 0.0)
        );
    }

    #[test]
    fn individual_pivots_turn_each_object_in_place() {
        let starts = [
            DragStart {
                node: NodeId::from_u128(1),
                transform: Transform3 {
                    translation: Vec3::new(-5.0, 0.0, 0.0),
                    ..Transform3::default()
                },
            },
            DragStart {
                node: NodeId::from_u128(2),
                transform: Transform3 {
                    translation: Vec3::new(5.0, 0.0, 0.0),
                    ..Transform3::default()
                },
            },
        ];
        let frame = DragFrame {
            pivot: Vec3::ZERO,
            axis: Vec3::Y,
            plane_normal: Vec3::Y,
            start_hit: Vec3::new(1.0, 0.0, 0.0),
            start_parameter: 0.0,
            handle: Handle::RingY,
            orientation: Quat::IDENTITY,
            individual: true,
        };
        let snap = SnapSettings::default();
        let context = ManipulationContext {
            frame: &frame,
            ray: ViewState::new().ray_through_pixel(960.0, 540.0),
            snap: &snap,
            input: DragInput::NONE,
        };
        for start in &starts {
            assert_eq!(context.pivot_for(start), start.transform.translation);
        }
        let shared = DragFrame {
            individual: false,
            ..frame
        };
        let context = ManipulationContext {
            frame: &shared,
            ray: ViewState::new().ray_through_pixel(960.0, 540.0),
            snap: &snap,
            input: DragInput::NONE,
        };
        for start in &starts {
            assert_eq!(context.pivot_for(start), Vec3::ZERO);
        }
    }

    #[test]
    fn a_scale_drag_is_multiplicative_so_it_suits_objects_of_any_size() {
        let Fixture {
            mut document,
            node,
            binding,
            registry,
            view,
        } = fixture();
        let nodes = [node];
        let mut drag = Drag::begin(
            &registry,
            &mut document,
            &request(&view, binding, &nodes, "scale", Handle::Uniform),
        )
        .expect("a drag");
        let feedback = drag
            .update(
                &registry,
                &mut document,
                &view,
                (1400.0, 540.0),
                &SnapSettings::default(),
                DragInput::NONE,
            )
            .expect("the drag continues");
        drag.cancel(&mut document).expect("it cancels");
        assert!(feedback.delta.starts_with('×'), "{feedback:?}");
    }
}
