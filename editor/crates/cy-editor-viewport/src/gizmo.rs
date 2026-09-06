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

use crate::math::{Quat, Ray, Vec3};
use crate::snapping::{Quantity, SnapSettings};
use crate::state::ViewState;

/// Which gizmo is in force.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum GizmoMode {
    /// Move.
    #[default]
    Translate,
    /// Turn.
    Rotate,
    /// Resize.
    Scale,
}

impl GizmoMode {
    /// A name for the interface and for a command identifier.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            GizmoMode::Translate => "translate",
            GizmoMode::Rotate => "rotate",
            GizmoMode::Scale => "scale",
        }
    }

    /// What quantity this mode's numeric entry takes.
    #[must_use]
    pub const fn quantity(self) -> Quantity {
        match self {
            GizmoMode::Translate => Quantity::Length,
            GizmoMode::Rotate => Quantity::Angle,
            GizmoMode::Scale => Quantity::Factor,
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

/// What the manipulation happens about.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Pivot {
    /// Each object about its own origin. Several objects rotate in place rather than about a shared
    /// point.
    #[default]
    Origin,
    /// The centre of the selection's bounds.
    Center,
    /// The last object selected, which is the one the gizmo is drawn on.
    Active,
}

/// Which part of the gizmo the cursor grabbed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
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
    /// The plane facing the camera: free movement, and the trackball rotation.
    Screen,
    /// Every axis at once, for a uniform scale.
    Uniform,
}

impl Handle {
    /// The axis this handle acts along, in the gizmo's own space. `None` for a plane or the screen
    /// handle, which act in a plane rather than along a line.
    #[must_use]
    pub const fn axis(self) -> Option<Vec3> {
        match self {
            Handle::AxisX => Some(Vec3::X),
            Handle::AxisY => Some(Vec3::Y),
            Handle::AxisZ => Some(Vec3::Z),
            _ => None,
        }
    }

    /// The normal of the plane this handle acts in, in the gizmo's own space. `None` for an axis
    /// handle and for the screen handle, whose plane is the camera's and is supplied at drag start.
    #[must_use]
    pub const fn plane_normal(self) -> Option<Vec3> {
        match self {
            Handle::PlaneXY => Some(Vec3::Z),
            Handle::PlaneYZ => Some(Vec3::X),
            Handle::PlaneZX => Some(Vec3::Y),
            _ => None,
        }
    }

    /// A name for the numeric feedback: "X", "XY", "screen".
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Handle::AxisX => "X",
            Handle::AxisY => "Y",
            Handle::AxisZ => "Z",
            Handle::PlaneXY => "XY",
            Handle::PlaneYZ => "YZ",
            Handle::PlaneZX => "ZX",
            Handle::Screen => "screen",
            Handle::Uniform => "uniform",
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
    /// Whether the transient snap modifier is held.
    pub modifier_held: bool,
}

impl ManipulationContext<'_> {
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
        // returning drag is not moved onto the grid by a snap it never asked for.
        if offset == Vec3::ZERO {
            return start.transform;
        }
        let moved = start.transform.translation + offset;
        Transform3 {
            translation: context.snap.position(moved, context.modifier_held),
            ..start.transform
        }
    }

    fn magnitude(&self, context: &ManipulationContext<'_>) -> f32 {
        translation_offset(context).length()
    }
}

/// The world-space offset a translate drag represents.
fn translation_offset(context: &ManipulationContext<'_>) -> Vec3 {
    match context.frame.handle {
        Handle::AxisX | Handle::AxisY | Handle::AxisZ => {
            context.frame.axis * (context.axis_parameter() - context.frame.start_parameter)
        }
        _ => context.plane_hit() - context.frame.start_hit,
    }
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
        let snapped = context.snap.radians(angle, context.modifier_held);
        let turn = Quat::from_axis_angle(context.frame.plane_normal, snapped);
        Transform3 {
            // The turn is applied in WORLD space — on the left — because the axis is already a
            // world-space direction. Applying it on the right would turn about the object's own
            // axis and a world-space gizmo would rotate objects about the wrong line.
            rotation: turn.after(start.transform.rotation).normalized(),
            // About a pivot that is not the object's own origin, a rotation also moves it. Computed
            // from the captured position rather than from the current one, like everything else.
            translation: context.frame.pivot
                + turn.rotate(start.transform.translation - context.frame.pivot),
            ..start.transform
        }
    }

    fn magnitude(&self, context: &ManipulationContext<'_>) -> f32 {
        context
            .snap
            .radians(rotation_angle(context), context.modifier_held)
    }
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
    sine.atan2(cosine)
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
        let along = match context.frame.handle {
            Handle::AxisX => Vec3::new(factor, 1.0, 1.0),
            Handle::AxisY => Vec3::new(1.0, factor, 1.0),
            Handle::AxisZ => Vec3::new(1.0, 1.0, factor),
            _ => Vec3::new(factor, factor, factor),
        };
        let scaled = start.transform.scale.component_mul(along);
        Transform3 {
            scale: context.snap.scale_factor(scaled, context.modifier_held),
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
    (now / start).max(1e-4)
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
    #[must_use]
    pub fn for_mode(&self, mode: GizmoMode) -> Option<&dyn Manipulator> {
        self.get(mode.name())
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

        let frame = build_frame(request, &starts);
        document.begin_interaction(
            format!(
                "{} {}",
                capitalised(manipulator.id()),
                request.handle.name()
            ),
            request.actor.clone(),
            format!("gizmo:{}:{}", manipulator.id(), starts[0].node),
        );
        Ok(Self {
            manipulator_id: manipulator.id().to_string(),
            mode: manipulator.mode(),
            frame,
            starts,
            binding: request.binding,
            open: true,
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
        modifier_held: bool,
    ) -> Result<Feedback> {
        if !self.open {
            return Err(Problem::new(
                "continue a drag",
                "it has already been committed or cancelled",
            )
            .with_remedy("begin a new drag"));
        }
        let manipulator = registry.get(&self.manipulator_id).ok_or_else(|| {
            Problem::new(
                "continue a drag",
                "the manipulator it started with is no longer registered",
            )
            .with_remedy("cancel the drag; a plugin was unloaded while it was in progress")
        })?;

        let context = ManipulationContext {
            frame: &self.frame,
            ray: view.ray_through_pixel(pixel.0, pixel.1),
            snap,
            modifier_held,
        };
        for start in &self.starts {
            let transform = manipulator.manipulate(&context, start);
            write_transform(document, start.node, self.binding, transform)?;
        }
        Ok(self.feedback(manipulator.magnitude(&context)))
    }

    /// Finish the drag. One transaction, or none when nothing actually changed.
    ///
    /// Returns whether an entry was recorded. A drag that ended where it began has recorded
    /// operations whose before and after are identical, and the document's own commit drops a
    /// transaction whose operations changed nothing — so "exactly one transaction" and "a history
    /// full of moves that moved nothing" do not have to be traded against each other.
    pub fn commit(&mut self, document: &mut Document) -> Result<bool> {
        if !self.open {
            return Err(Problem::new("commit a drag", "it is already finished"));
        }
        self.open = false;
        Ok(document.commit()?.is_some())
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
        match self.mode {
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
            GizmoMode::Scale => Feedback {
                delta: format!("×{magnitude:.3} on {}", self.frame.handle.name()),
                value: format_vec3(active.scale, ""),
            },
        }
    }
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
fn build_frame(request: &DragRequest<'_>, starts: &[DragStart]) -> DragFrame {
    let active = starts.last().expect("callers check for an empty capture");
    let orientation = orientation_of(request.space, active.transform.rotation, request.view);
    let pivot = pivot_of(request.pivot, starts);
    let axis = world_axis(request.handle, orientation, request.view);
    let plane_normal = world_plane(request.handle, orientation, request.view, axis);

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
        handle: request.handle,
        orientation,
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

fn pivot_of(pivot: Pivot, starts: &[DragStart]) -> Vec3 {
    let active = starts.last().expect("callers check for an empty capture");
    match pivot {
        Pivot::Origin | Pivot::Active => active.transform.translation,
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
            pivot: Pivot::Origin,
            nodes,
            binding,
            view,
            pixel: (960.0, 540.0),
            actor: Actor::human("designer"),
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
            drag.update(&registry, &mut document, &view, pixel(step), &snap, false)
                .expect("the drag continues");
        }
        // And back to exactly where it started.
        drag.update(
            &registry,
            &mut document,
            &view,
            (960.0, 540.0),
            &snap,
            false,
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
            drag.update(&registry, &mut document, &view, pixel(step), &snap, false)
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
            false,
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
                false,
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
            false,
        )
        .expect("the drag continues");
        let first = translation_of(&document, binding, node);
        drag.update(
            &registry,
            &mut document,
            &view,
            (1100.0, 540.0),
            &snap,
            false,
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
            false,
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
            false,
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
                false,
            )
            .expect_err("the drag is over");
        assert!(problem.remedy.is_some(), "{problem}");
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
                false,
            )
            .expect("the drag continues");
        drag.cancel(&mut document).expect("it cancels");
        assert!(feedback.delta.starts_with('×'), "{feedback:?}");
    }
}
