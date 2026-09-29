// SPDX-License-Identifier: MIT
//! Joint and constraint authoring: one component, three commands, each one transaction. Issue #29.
//!
//! --- WHAT A JOINT IS IN A DOCUMENT --------------------------------------------------------------------
//!
//! `cy::physics::Joint` holds a `ConstraintDescription` whose endpoints are runtime body handles,
//! which no document can hold. The authored form is a `Joint` component on the node that owns body
//! A: its [`JointKind`], the node carrying body B as an entity reference (none joins A to the
//! world), and the joint frame as an anchor and an axis in body A's rotated, unscaled frame. At play
//! `cy::gameplay::PlaySession` resolves the reference to a body, derives frame B so both anchors
//! coincide where the bodies were authored, and hands the joint to the bridge
//! (`src/gameplay/play/include/cy/gameplay/play/joints.h`). One joint per node, as there is one
//! collider per node, for the same reason: several would need a buffer component the document
//! model does not have.
//!
//! --- THE NAMES ARE A CONTRACT WITH THE ENGINE ---------------------------------------------------------
//!
//! The component and field names below are `joint_fields` in that header, and the kind words are
//! `cy::physics::constraint_type_name`'s. `tests::the_kinds_are_the_engines` reads the engine's
//! table; `tests/a_joint_is_a_transaction.rs` and `src/gameplay/play/tests/test_joints.cpp` hold
//! the same golden world, so a spelling that drifts on one side fails a test on that side.
//!
//! --- WHY THREE COMMANDS ------------------------------------------------------------------------------------
//!
//! `physics.joint.add`, `physics.joint.set` and `physics.joint.remove` are the whole write surface.
//! The physics panel's controls are callers of them, each control one `set` and one undo entry, and
//! every one is an MCP tool with the same behaviour, because tools are a projection of the registry.
//! `set` takes a field name and a value rather than one parameter per field: a parameter the
//! caller omitted would otherwise be indistinguishable from one set to its default, and a command
//! that wrote eighteen fields to change one would put eighteen changes in the history.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::ids::{DocumentId, FieldId, NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::operation::Operation;
use cy_editor_documents::schema::DocumentSchema;

use crate::bodies::body_of;
use crate::mirror::engine_identity;

/// Register `physics.joint.add`, `physics.joint.set` and `physics.joint.remove`.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(add_joint())?;
    registry.register(set_joint())?;
    registry.register(remove_joint())?;
    Ok(())
}

// --- The kinds -----------------------------------------------------------------------------------------

/// The ten constraint kinds the engine has (`cy::physics::ConstraintType`), in its order.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum JointKind {
    /// Welds the two bodies together.
    Fixed,
    /// A ball and socket: the anchors meet, rotation is free.
    Point,
    /// Rotation about one axis, with an optional range and motor.
    Hinge,
    /// Travel along one axis, with an optional range and motor.
    Slider,
    /// The anchors kept between two distances.
    Distance,
    /// Rotation within a cone about the axis.
    Cone,
    /// A cone of swing and a range of twist about the axis: a shoulder.
    SwingTwist,
    /// Six axes, each locked, limited or free.
    SixDof,
    /// A rack's travel driving a pinion's rotation.
    RackAndPinion,
    /// Two rotations driven at a ratio.
    Gear,
}

impl JointKind {
    /// Every kind, in the engine's order.
    pub const ALL: [JointKind; 10] = [
        JointKind::Fixed,
        JointKind::Point,
        JointKind::Hinge,
        JointKind::Slider,
        JointKind::Distance,
        JointKind::Cone,
        JointKind::SwingTwist,
        JointKind::SixDof,
        JointKind::RackAndPinion,
        JointKind::Gear,
    ];

    /// The word written into `kind`: `cy::physics::constraint_type_name`'s spelling.
    #[must_use]
    pub const fn keyword(self) -> &'static str {
        match self {
            JointKind::Fixed => "fixed",
            JointKind::Point => "point",
            JointKind::Hinge => "hinge",
            JointKind::Slider => "slider",
            JointKind::Distance => "distance",
            JointKind::Cone => "cone",
            JointKind::SwingTwist => "swing-twist",
            JointKind::SixDof => "six-dof",
            JointKind::RackAndPinion => "rack-and-pinion",
            JointKind::Gear => "gear",
        }
    }

    /// What a person reads.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            JointKind::Fixed => "Fixed",
            JointKind::Point => "Point (ball and socket)",
            JointKind::Hinge => "Hinge",
            JointKind::Slider => "Slider",
            JointKind::Distance => "Distance",
            JointKind::Cone => "Cone",
            JointKind::SwingTwist => "Swing-twist",
            JointKind::SixDof => "Six degrees of freedom",
            JointKind::RackAndPinion => "Rack and pinion",
            JointKind::Gear => "Gear",
        }
    }

    /// The kind a word names.
    #[must_use]
    pub fn from_keyword(word: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|kind| kind.keyword() == word)
    }

    /// Whether this kind reads a field. The panel shows exactly these, and the engine reads exactly
    /// these (`authored_joint` in `joints.cpp`); a field a kind ignores is kept, not cleared, so
    /// switching kinds and back loses nothing.
    #[must_use]
    pub const fn uses(self, field: JointField) -> bool {
        use JointField as F;
        use JointKind as K;
        match field {
            F::Kind
            | F::Target
            | F::Anchor
            | F::BreakForce
            | F::BreakTorque
            | F::CollideConnected => true,
            F::Axis => !matches!(self, K::Fixed | K::Point | K::Distance),
            F::LimitMin | F::LimitMax => {
                matches!(self, K::Hinge | K::Slider | K::SwingTwist | K::Distance)
            }
            F::SwingY | F::SwingZ => matches!(self, K::Cone | K::SwingTwist),
            F::LinearMin | F::LinearMax | F::AngularMin | F::AngularMax => {
                matches!(self, K::SixDof)
            }
            F::MotorVelocity | F::MotorMaxForce => matches!(self, K::Hinge | K::Slider),
            F::Ratio => matches!(self, K::RackAndPinion | K::Gear),
        }
    }

    /// What the one range means for this kind, for a label.
    #[must_use]
    pub const fn range_meaning(self) -> &'static str {
        match self {
            JointKind::Slider => "travel, in metres",
            JointKind::SwingTwist => "twist, in radians",
            JointKind::Distance => "distance, in metres",
            _ => "angle, in radians",
        }
    }
}

// --- The fields -------------------------------------------------------------------------------------------

/// One authored field of a joint.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum JointField {
    /// Which kind of joint.
    Kind,
    /// The node carrying body B, or none for the world.
    Target,
    /// The anchor, in body A's frame.
    Anchor,
    /// The joint axis, in body A's frame.
    Axis,
    /// The low end of the range.
    LimitMin,
    /// The high end of the range.
    LimitMax,
    /// The swing half-angle about the frame's Y.
    SwingY,
    /// The swing half-angle about the frame's Z.
    SwingZ,
    /// Six-axis: the low end of travel along X, Y and Z.
    LinearMin,
    /// Six-axis: the high end of travel.
    LinearMax,
    /// Six-axis: the low end of rotation about X, Y and Z.
    AngularMin,
    /// Six-axis: the high end of rotation.
    AngularMax,
    /// The motor's target speed.
    MotorVelocity,
    /// The motor's force or torque cap. Zero is no motor.
    MotorMaxForce,
    /// The gear or rack ratio.
    Ratio,
    /// The force above which the joint breaks. Zero never breaks.
    BreakForce,
    /// The torque above which the joint breaks. Zero never breaks.
    BreakTorque,
    /// Whether the two bodies still collide with each other.
    CollideConnected,
}

impl JointField {
    /// Every field, in the order the component declares them.
    pub const ALL: [JointField; 18] = [
        JointField::Kind,
        JointField::Target,
        JointField::Anchor,
        JointField::Axis,
        JointField::LimitMin,
        JointField::LimitMax,
        JointField::SwingY,
        JointField::SwingZ,
        JointField::LinearMin,
        JointField::LinearMax,
        JointField::AngularMin,
        JointField::AngularMax,
        JointField::MotorVelocity,
        JointField::MotorMaxForce,
        JointField::Ratio,
        JointField::BreakForce,
        JointField::BreakTorque,
        JointField::CollideConnected,
    ];

    /// The field's name, as the engine reads it.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            JointField::Kind => "kind",
            JointField::Target => "target",
            JointField::Anchor => "anchor",
            JointField::Axis => "axis",
            JointField::LimitMin => "limit_min",
            JointField::LimitMax => "limit_max",
            JointField::SwingY => "swing_y",
            JointField::SwingZ => "swing_z",
            JointField::LinearMin => "linear_min",
            JointField::LinearMax => "linear_max",
            JointField::AngularMin => "angular_min",
            JointField::AngularMax => "angular_max",
            JointField::MotorVelocity => "motor_velocity",
            JointField::MotorMaxForce => "motor_max_force",
            JointField::Ratio => "ratio",
            JointField::BreakForce => "break_force",
            JointField::BreakTorque => "break_torque",
            JointField::CollideConnected => "collide_connected",
        }
    }

    /// The field a name names.
    #[must_use]
    pub fn of_name(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|field| field.name() == name)
    }

    /// The kind of value it holds.
    #[must_use]
    pub const fn kind(self) -> ValueKind {
        match self {
            JointField::Kind => ValueKind::Text,
            JointField::Target => ValueKind::Entity,
            JointField::Anchor
            | JointField::Axis
            | JointField::LinearMin
            | JointField::LinearMax
            | JointField::AngularMin
            | JointField::AngularMax => ValueKind::Vec3,
            JointField::CollideConnected => ValueKind::Bool,
            _ => ValueKind::Float,
        }
    }

    /// What the field means, for the schema and for a caller that cannot see the panel.
    #[must_use]
    pub const fn help(self) -> &'static str {
        match self {
            JointField::Kind => {
                "Which joint: fixed, point, hinge, slider, distance, cone, swing-twist, six-dof, \
                 rack-and-pinion or gear."
            }
            JointField::Target => "The entity carrying the other body, or none for the world.",
            JointField::Anchor => "The anchor, in metres, in this body's rotated unscaled frame.",
            JointField::Axis => {
                "The joint axis in this body's frame: the hinge, slider and twist axis."
            }
            JointField::LimitMin => "The low end of the range. Above the high end means free.",
            JointField::LimitMax => "The high end of the range.",
            JointField::SwingY => "The swing half-angle about the frame's Y axis, in radians.",
            JointField::SwingZ => "The swing half-angle about the frame's Z axis, in radians.",
            JointField::LinearMin => "Six-axis: the low end of travel along X, Y and Z, in metres.",
            JointField::LinearMax => "Six-axis: the high end of travel, in metres.",
            JointField::AngularMin => "Six-axis: the low end of rotation about X, Y and Z.",
            JointField::AngularMax => "Six-axis: the high end of rotation, in radians.",
            JointField::MotorVelocity => "The motor's target speed, in metres or radians a second.",
            JointField::MotorMaxForce => "The motor's force or torque cap. Zero is no motor.",
            JointField::Ratio => "The ratio between the two bodies' motion. Never zero.",
            JointField::BreakForce => "The force above which the joint breaks. Zero never breaks.",
            JointField::BreakTorque => "The torque above which it breaks. Zero never breaks.",
            JointField::CollideConnected => {
                "Whether the two joined bodies collide with each other."
            }
        }
    }
}

// --- The spec -----------------------------------------------------------------------------------------------

/// One joint, as values.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct JointSpec {
    /// Which joint.
    pub kind: JointKind,
    /// The engine identity of body B's node, or zero for the world.
    pub target: u64,
    /// The anchor in body A's frame.
    pub anchor: [f32; 3],
    /// The axis in body A's frame.
    pub axis: [f32; 3],
    /// The range: angle, travel, twist or distance.
    pub limit: [f32; 2],
    /// The swing half-angles about Y and Z.
    pub swing: [f32; 2],
    /// Six-axis travel, low and high.
    pub linear: [[f32; 3]; 2],
    /// Six-axis rotation, low and high.
    pub angular: [[f32; 3]; 2],
    /// Motor target speed and force cap.
    pub motor: [f32; 2],
    /// Gear or rack ratio.
    pub ratio: f32,
    /// Break force and torque.
    pub breaking: [f32; 2],
    /// Whether the joined bodies collide.
    pub collide_connected: bool,
}

impl JointSpec {
    /// A joint of `kind` with the engine's defaults: free range, no motor, never breaks, and for
    /// six axes translation locked and rotation free. A distance joint starts between 0 and 1 m,
    /// because "free" is not a distance.
    #[must_use]
    pub const fn new(kind: JointKind) -> Self {
        let limit = if matches!(kind, JointKind::Distance) {
            [0.0, 1.0]
        } else {
            [1.0, -1.0]
        };
        Self {
            kind,
            target: 0,
            anchor: [0.0; 3],
            axis: [1.0, 0.0, 0.0],
            limit,
            swing: [0.0, 0.0],
            linear: [[0.0; 3], [0.0; 3]],
            angular: [[1.0; 3], [-1.0; 3]],
            motor: [0.0, 0.0],
            ratio: 1.0,
            breaking: [0.0, 0.0],
            collide_connected: false,
        }
    }

    /// The value one field holds.
    #[must_use]
    pub fn value(&self, field: JointField) -> Value {
        match field {
            JointField::Kind => Value::Text(self.kind.keyword().to_string()),
            JointField::Target => Value::Entity(self.target),
            JointField::Anchor => Value::Vec3(self.anchor),
            JointField::Axis => Value::Vec3(self.axis),
            JointField::LimitMin => Value::Float(self.limit[0]),
            JointField::LimitMax => Value::Float(self.limit[1]),
            JointField::SwingY => Value::Float(self.swing[0]),
            JointField::SwingZ => Value::Float(self.swing[1]),
            JointField::LinearMin => Value::Vec3(self.linear[0]),
            JointField::LinearMax => Value::Vec3(self.linear[1]),
            JointField::AngularMin => Value::Vec3(self.angular[0]),
            JointField::AngularMax => Value::Vec3(self.angular[1]),
            JointField::MotorVelocity => Value::Float(self.motor[0]),
            JointField::MotorMaxForce => Value::Float(self.motor[1]),
            JointField::Ratio => Value::Float(self.ratio),
            JointField::BreakForce => Value::Float(self.breaking[0]),
            JointField::BreakTorque => Value::Float(self.breaking[1]),
            JointField::CollideConnected => Value::Bool(self.collide_connected),
        }
    }

    /// Write one field from a value of its kind. A value of another kind leaves the field as it
    /// was, which is how a document written by an older build reads.
    pub fn assign(&mut self, field: JointField, value: &Value) {
        match (field, value) {
            (JointField::Kind, Value::Text(word)) => {
                if let Some(kind) = JointKind::from_keyword(word) {
                    self.kind = kind;
                }
            }
            (JointField::Target, Value::Entity(target)) => self.target = *target,
            (JointField::CollideConnected, Value::Bool(flag)) => self.collide_connected = *flag,
            (_, Value::Vec3(lanes)) => self.assign_vector(field, *lanes),
            (_, Value::Float(number)) => self.assign_number(field, *number),
            _ => {}
        }
    }

    fn assign_vector(&mut self, field: JointField, lanes: [f32; 3]) {
        match field {
            JointField::Anchor => self.anchor = lanes,
            JointField::Axis => self.axis = lanes,
            JointField::LinearMin => self.linear[0] = lanes,
            JointField::LinearMax => self.linear[1] = lanes,
            JointField::AngularMin => self.angular[0] = lanes,
            JointField::AngularMax => self.angular[1] = lanes,
            _ => {}
        }
    }

    fn assign_number(&mut self, field: JointField, number: f32) {
        match field {
            JointField::LimitMin => self.limit[0] = number,
            JointField::LimitMax => self.limit[1] = number,
            JointField::SwingY => self.swing[0] = number,
            JointField::SwingZ => self.swing[1] = number,
            JointField::MotorVelocity => self.motor[0] = number,
            JointField::MotorMaxForce => self.motor[1] = number,
            JointField::Ratio => self.ratio = number,
            JointField::BreakForce => self.breaking[0] = number,
            JointField::BreakTorque => self.breaking[1] = number,
            _ => {}
        }
    }

    /// What the engine's `cy::physics::validate` would refuse, refused here instead, at the moment
    /// the author makes the change rather than when play is pressed.
    ///
    /// # Errors
    ///
    /// Naming the field and the rule.
    pub fn validate(&self) -> Result<()> {
        let action = "author a joint";
        if self.breaking.iter().any(|value| *value < 0.0) {
            return Err(Problem::new(action, "a break threshold is negative")
                .with_remedy("zero never breaks; pass zero or a positive threshold"));
        }
        if self.motor[1] < 0.0 {
            return Err(Problem::new(action, "the motor's force cap is negative")
                .with_remedy("zero turns the motor off; pass zero or a positive cap"));
        }
        if self.kind == JointKind::Distance && self.limit[0] > self.limit[1] {
            return Err(Problem::new(
                action,
                "a distance joint's limit_min is above its limit_max",
            )
            .with_remedy(
                "a distance joint has no free range; set a minimum at or below the maximum",
            ));
        }
        if matches!(self.kind, JointKind::RackAndPinion | JointKind::Gear) && self.ratio == 0.0 {
            return Err(
                Problem::new(action, "a gear or rack ratio of zero drives nothing")
                    .with_remedy("pass a non-zero ratio; its sign is the direction"),
            );
        }
        if self.axis.iter().all(|lane| *lane == 0.0) {
            return Err(Problem::new(action, "the axis has no direction")
                .with_remedy("pass a non-zero axis, such as 1 0 0"));
        }
        Ok(())
    }
}

// --- The binding ----------------------------------------------------------------------------------------

/// Where a joint's fields live in a document's schema.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct JointBinding {
    /// The component.
    pub component: TypeId,
    /// Each field, in [`JointField::ALL`]'s order.
    pub fields: Vec<FieldId>,
}

impl JointBinding {
    /// The component's name, as the engine reads it.
    pub const COMPONENT: &'static str = "Joint";

    /// The field's identity in this binding.
    #[must_use]
    pub fn field(&self, field: JointField) -> FieldId {
        let index = JointField::ALL
            .iter()
            .position(|candidate| *candidate == field)
            .expect("every field is in ALL");
        self.fields[index]
    }

    /// Find the binding, or answer that the document has never seen a joint.
    #[must_use]
    pub fn of_schema(schema: &DocumentSchema) -> Option<Self> {
        let definition = schema.type_named(Self::COMPONENT)?;
        let fields = JointField::ALL
            .iter()
            .map(|field| definition.field_named(field.name()).map(|found| found.id))
            .collect::<Option<Vec<_>>>()?;
        Some(Self {
            component: definition.id,
            fields,
        })
    }

    /// Find it, or declare all eighteen fields, for the reason `ColliderBinding::declare` gives: a
    /// schema whose fields depended on the first joint somebody added would differ between two
    /// worlds holding the same joints.
    pub fn declare(schema: &mut DocumentSchema) -> Self {
        if let Some(found) = Self::of_schema(schema) {
            return found;
        }
        let component = schema.declare_type(Self::COMPONENT, false);
        let fields = JointField::ALL
            .iter()
            .map(|field| {
                schema
                    .declare_field(component, field.name(), field.kind(), field.help())
                    .expect("the type was declared on the line above")
            })
            .collect();
        Self { component, fields }
    }
}

/// The joint on `node`, if it carries one.
#[must_use]
pub fn joint_of(document: &Document, node: NodeId) -> Option<JointSpec> {
    let binding = JointBinding::of_schema(document.schema())?;
    let fields = document
        .content()
        .node(node)?
        .components
        .get(&binding.component)?;
    let mut spec = JointSpec::new(JointKind::Fixed);
    for field in JointField::ALL {
        if let Some(value) = fields.get(&binding.field(field)) {
            spec.assign(field, value);
        }
    }
    Some(spec)
}

/// The node an engine identity names in `document`, which is how a joint's target is shown.
#[must_use]
pub fn node_of_identity(document: &Document, identity: u64) -> Option<NodeId> {
    if identity == 0 {
        return None;
    }
    document
        .content()
        .nodes()
        .find(|node| engine_identity(*node) == identity)
}

// --- The commands ------------------------------------------------------------------------------------------

fn active(context: &dyn CommandContext, action: &str) -> Result<DocumentId> {
    context.active_document().ok_or_else(|| {
        Problem::new(action.to_string(), "no document is open").with_remedy("open a world first")
    })
}

fn parse_node(named: &str) -> Result<NodeId> {
    u128::from_str_radix(named, 16)
        .map(NodeId::from_u128)
        .map_err(|_| {
            Problem::new(
                format!("read the entity {named:?}"),
                "it is not an entity identity",
            )
            .with_remedy("use the identity a command's result printed, which is 32 hex digits")
        })
}

/// The node a command acts on: the one named, or the single selected one.
fn subject(context: &dyn CommandContext, arguments: &Arguments, action: &str) -> Result<NodeId> {
    let named = arguments.text("entity").unwrap_or_default().trim();
    if !named.is_empty() {
        return parse_node(named);
    }
    let selection = context.selection();
    let mut nodes = selection.nodes();
    match (nodes.next(), nodes.next()) {
        (Some(only), None) => Ok(only),
        (Some(_), Some(_)) => Err(Problem::new(action, "more than one entity is selected")
            .with_remedy("name one with entity=, or select a single entity")),
        _ => Err(Problem::new(action, "nothing is selected")
            .with_remedy("select the entity carrying body A, or name it with entity=")),
    }
}

/// The engine identity of the node `named` carries body B, checked: it exists, it has a body, and
/// it is not body A. Empty names the world.
fn resolve_target(document: &Document, subject: NodeId, named: &str, action: &str) -> Result<u64> {
    let named = named.trim();
    if named.is_empty() {
        return Ok(0);
    }
    let node = parse_node(named)?;
    if node == subject {
        return Err(Problem::new(action, "a body cannot be joined to itself")
            .with_remedy("name another entity, or leave target empty to join it to the world"));
    }
    if document.content().node(node).is_none() {
        return Err(Problem::not_found(format!("the target entity {named}")));
    }
    if body_of(document, node).is_none() {
        return Err(
            Problem::new(action, "the target entity has no physics body")
                .with_remedy("add one with scene.add-body, or leave target empty for the world"),
        );
    }
    Ok(engine_identity(node))
}

fn entity_parameter(metadata: Metadata) -> Metadata {
    metadata.with(ParameterSpec::optional(
        "entity",
        ValueKind::Text,
        "The entity carrying body A, as a create or select command printed its identity. The \
         selected entity when omitted.",
        Value::Text(String::new()),
    ))
}

fn add_joint() -> Command {
    Command::new(
        entity_parameter(
            Metadata::new(
                "physics.joint.add",
                "Add Joint",
                "Physics",
                "Adds a joint to an entity that has a physics body, as ONE undoable transaction: \
                 its kind, the entity carrying the other body (or none for the world), and the \
                 anchor and axis in this body's frame. At play the engine turns it into a \
                 constraint with both anchors where the bodies were authored. The engine draws \
                 the selected entity's joint as a gizmo: its anchors, its axis and its limits.",
                EffectClass::ReversibleMutation,
            )
            .with(ParameterSpec::optional(
                "kind",
                ValueKind::Text,
                "fixed, point, hinge, slider, distance, cone, swing-twist, six-dof, \
                 rack-and-pinion or gear. A hinge when omitted.",
                Value::Text("hinge".to_string()),
            ))
            .with(ParameterSpec::optional(
                "target",
                ValueKind::Text,
                "The identity of the entity carrying the other body. Empty joins this body to \
                 the world.",
                Value::Text(String::new()),
            ))
            .with(ParameterSpec::optional(
                "anchor",
                ValueKind::Vec3,
                "The anchor in this body's rotated, unscaled frame, in metres. Its origin when \
                 omitted.",
                Value::Vec3([0.0; 3]),
            ))
            .with(ParameterSpec::optional(
                "axis",
                ValueKind::Vec3,
                "The joint axis in this body's frame: the hinge, slider and twist axis. X when \
                 omitted.",
                Value::Vec3([1.0, 0.0, 0.0]),
            )),
        ),
        run_add,
    )
}

fn run_add(context: &mut dyn CommandContext, arguments: &Arguments) -> Result<Outcome> {
    let action = "add a joint";
    let word = arguments.text("kind").unwrap_or("hinge").trim().to_string();
    let kind = JointKind::from_keyword(&word).ok_or_else(|| unknown_kind(&word))?;
    let node = subject(context, arguments, action)?;
    let document_id = active(context, action)?;
    let actor = context.actor();
    let document = context
        .document_mut(document_id)
        .ok_or_else(|| Problem::not_found("the active document"))?;
    if body_of(document, node).is_none() {
        return Err(Problem::new(action, "that entity has no physics body")
            .with_remedy("add one with scene.add-body first; a joint joins two bodies"));
    }
    if joint_of(document, node).is_some() {
        return Err(
            Problem::new(action, "that entity already has a joint").with_remedy(
                "change it with physics.joint.set, or remove it first; one joint per entity",
            ),
        );
    }
    let mut spec = JointSpec::new(kind);
    spec.target = resolve_target(
        document,
        node,
        arguments.text("target").unwrap_or_default(),
        action,
    )?;
    if let Some(Value::Vec3(anchor)) = arguments.get("anchor") {
        spec.anchor = *anchor;
    }
    if let Some(Value::Vec3(axis)) = arguments.get("axis") {
        spec.axis = *axis;
    }
    spec.validate()?;
    document.with_transaction(format!("Add {} joint", kind.keyword()), actor, |document| {
        let binding = JointBinding::declare(document.schema_mut());
        let fields = JointField::ALL
            .iter()
            .map(|field| (binding.field(*field), spec.value(*field)))
            .collect();
        document.add_component(node, binding.component, fields)
    })?;
    Ok(Outcome::new(format!("Added a {} joint", kind.keyword()))
        .with("entity", Value::Text(node.to_string()))
        .with("kind", Value::Text(kind.keyword().to_string())))
}

fn unknown_kind(word: &str) -> Problem {
    Problem::new("author a joint", format!("{word:?} is not a joint kind")).with_remedy(format!(
        "name one of: {}",
        JointKind::ALL.map(JointKind::keyword).join(", ")
    ))
}

fn set_joint() -> Command {
    Command::new(
        entity_parameter(
            Metadata::new(
                "physics.joint.set",
                "Set Joint Field",
                "Physics",
                "Changes one field of an entity's joint, as one undoable transaction. The fields \
                 are kind, target (an entity identity, or empty for the world), anchor and axis \
                 (three numbers), limit_min and limit_max (the hinge angle, slider travel, twist or \
                 distance range; min above max is free), swing_y and swing_z (radians), \
                 linear_min, linear_max, angular_min and angular_max (six-dof, three numbers \
                 each), motor_velocity and motor_max_force, ratio, break_force and break_torque \
                 (zero never breaks) and collide_connected (true or false). A change the engine \
                 would refuse at play is refused now, naming the rule.",
                EffectClass::ReversibleMutation,
            )
            .with(ParameterSpec::required(
                "field",
                ValueKind::Text,
                "The field to change, by the name the engine reads, such as limit_max.",
            ))
            .with(ParameterSpec::required(
                "value",
                ValueKind::Text,
                "The new value as text: a word, a number, three numbers separated by spaces, \
                 true or false, or an entity identity.",
            )),
        ),
        run_set,
    )
}

fn run_set(context: &mut dyn CommandContext, arguments: &Arguments) -> Result<Outcome> {
    let action = "change a joint";
    let name = arguments
        .text("field")
        .unwrap_or_default()
        .trim()
        .to_string();
    let field = JointField::of_name(&name).ok_or_else(|| {
        Problem::new(action, format!("a joint has no field {name:?}")).with_remedy(format!(
            "name one of: {}",
            JointField::ALL.map(JointField::name).join(", ")
        ))
    })?;
    let text = arguments.text("value").unwrap_or_default().to_string();
    let node = subject(context, arguments, action)?;
    let document_id = active(context, action)?;
    let actor = context.actor();
    let document = context
        .document_mut(document_id)
        .ok_or_else(|| Problem::not_found("the active document"))?;
    let before = joint_of(document, node).ok_or_else(|| {
        Problem::new(action, "that entity has no joint")
            .with_remedy("add one with physics.joint.add")
    })?;
    let value = parse_field(document, node, field, &text)?;
    let mut after = before;
    after.assign(field, &value);
    // A kind with no free range: a distance joint arriving from a free hinge takes the default span
    // in the same transaction, rather than being refused for a range the author never chose.
    if field == JointField::Kind
        && after.kind == JointKind::Distance
        && after.limit[0] > after.limit[1]
    {
        after.limit = JointSpec::new(JointKind::Distance).limit;
    }
    after.validate()?;
    let binding = JointBinding::of_schema(document.schema())
        .ok_or_else(|| Problem::not_found("the joint component"))?;
    document.with_transaction(format!("Set joint {}", field.name()), actor, |document| {
        for changed in JointField::ALL {
            if before.value(changed) != after.value(changed) {
                document.set_field(
                    node,
                    binding.component,
                    binding.field(changed),
                    after.value(changed),
                )?;
            }
        }
        Ok(())
    })?;
    Ok(Outcome::new(format!("Set the joint's {}", field.name()))
        .with("entity", Value::Text(node.to_string()))
        .with("field", Value::Text(field.name().to_string())))
}

/// A field's value from the text a caller passed, in the field's own kind.
fn parse_field(document: &Document, node: NodeId, field: JointField, text: &str) -> Result<Value> {
    let text = text.trim();
    let bad = |expected: &str| {
        Problem::new(
            format!("set the joint's {}", field.name()),
            format!("{text:?} is not {expected}"),
        )
    };
    match field.kind() {
        ValueKind::Text => JointKind::from_keyword(text)
            .map(|kind| Value::Text(kind.keyword().to_string()))
            .ok_or_else(|| unknown_kind(text)),
        ValueKind::Entity => {
            resolve_target(document, node, text, "change a joint").map(Value::Entity)
        }
        ValueKind::Bool => match text {
            "true" | "on" | "1" => Ok(Value::Bool(true)),
            "false" | "off" | "0" => Ok(Value::Bool(false)),
            _ => Err(bad("true or false")),
        },
        ValueKind::Vec3 => parse_vec3(text)
            .map(Value::Vec3)
            .ok_or_else(|| bad("three finite numbers")),
        _ => text
            .parse::<f32>()
            .ok()
            .filter(|number| number.is_finite())
            .map(Value::Float)
            .ok_or_else(|| bad("a finite number")),
    }
}

fn parse_vec3(text: &str) -> Option<[f32; 3]> {
    let lanes: Vec<f32> = text
        .split(|character: char| character.is_whitespace() || character == ',')
        .filter(|word| !word.is_empty())
        .map(str::parse::<f32>)
        .collect::<std::result::Result<_, _>>()
        .ok()?;
    let lanes: [f32; 3] = lanes.try_into().ok()?;
    lanes.iter().all(|lane| lane.is_finite()).then_some(lanes)
}

fn remove_joint() -> Command {
    Command::new(
        entity_parameter(Metadata::new(
            "physics.joint.remove",
            "Remove Joint",
            "Physics",
            "Removes an entity's joint, as one undoable transaction. The bodies stay; undo \
             restores the joint with every value it had.",
            EffectClass::ReversibleMutation,
        )),
        |context, arguments| {
            let action = "remove a joint";
            let node = subject(context, arguments, action)?;
            let document_id = active(context, action)?;
            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            let binding = JointBinding::of_schema(document.schema())
                .filter(|binding| document.content().has_component(node, binding.component))
                .ok_or_else(|| {
                    Problem::new(action, "that entity has no joint")
                        .with_remedy("add one with physics.joint.add")
                })?;
            // The values it held, so undo restores this joint rather than a default one.
            let before: Vec<(FieldId, Value)> = document
                .content()
                .node(node)
                .and_then(|state| state.components.get(&binding.component))
                .map(|fields| {
                    fields
                        .iter()
                        .map(|(field, value)| (*field, value.clone()))
                        .collect()
                })
                .unwrap_or_default();
            document.with_transaction("Remove joint", actor, |document| {
                document.record(Operation::RemoveComponent {
                    node,
                    component: binding.component,
                    before,
                })
            })?;
            Ok(Outcome::new("Removed the joint").with("entity", Value::Text(node.to_string())))
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_kinds_are_the_engines() {
        // `cy::physics::constraint_type_name`, read out of the engine's source: the ten words the
        // engine's `joint_kind_of` accepts, in `ConstraintType`'s order.
        let source = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(3)
            .expect("the crate is at editor/crates/<name>/")
            .join("src/servers/physics/src/constraints.cpp");
        let text = std::fs::read_to_string(&source).expect("the engine's constraint names");
        let body = text
            .split("const char* constraint_type_name(ConstraintType value) noexcept {")
            .nth(1)
            .expect("the name table")
            .split("return \"unknown\";")
            .next()
            .expect("the switch");
        let engine: Vec<&str> = body
            .lines()
            .filter_map(|line| line.trim().strip_prefix("return \""))
            .filter_map(|line| line.split('"').next())
            .collect();
        let editor: Vec<&str> = JointKind::ALL.iter().map(|kind| kind.keyword()).collect();
        assert_eq!(editor, engine);
    }

    #[test]
    fn a_spec_round_trips_through_its_values() {
        let mut spec = JointSpec::new(JointKind::SixDof);
        spec.anchor = [1.0, 2.0, 3.0];
        spec.linear[1] = [0.5, 0.0, 0.0];
        spec.collide_connected = true;
        let mut again = JointSpec::new(JointKind::Fixed);
        for field in JointField::ALL {
            again.assign(field, &spec.value(field));
        }
        assert_eq!(again, spec);
    }

    #[test]
    fn what_the_engine_would_refuse_is_refused_when_it_is_authored() {
        let mut spec = JointSpec::new(JointKind::Distance);
        spec.validate().expect("the default span is valid");
        spec.limit = [2.0, 1.0];
        assert!(spec.validate().is_err());
        let mut gear = JointSpec::new(JointKind::Gear);
        gear.ratio = 0.0;
        assert!(gear.validate().is_err());
        let mut hinge = JointSpec::new(JointKind::Hinge);
        hinge.breaking[0] = -1.0;
        assert!(hinge.validate().is_err());
        hinge.breaking[0] = 0.0;
        hinge.axis = [0.0; 3];
        assert!(hinge.validate().is_err());
    }

    #[test]
    fn each_kind_uses_the_fields_the_engine_reads_for_it() {
        assert!(JointKind::Hinge.uses(JointField::MotorMaxForce));
        assert!(!JointKind::Point.uses(JointField::Axis));
        assert!(JointKind::SwingTwist.uses(JointField::LimitMin));
        assert!(JointKind::SwingTwist.uses(JointField::SwingZ));
        assert!(!JointKind::Cone.uses(JointField::LimitMin));
        assert!(JointKind::SixDof.uses(JointField::AngularMax));
        assert!(!JointKind::Hinge.uses(JointField::LinearMin));
        assert!(JointKind::Gear.uses(JointField::Ratio));
        for kind in JointKind::ALL {
            assert!(kind.uses(JointField::Target), "{kind:?}");
        }
    }

    #[test]
    fn a_vector_is_three_finite_numbers() {
        assert_eq!(parse_vec3("1 0 -2.5"), Some([1.0, 0.0, -2.5]));
        assert_eq!(parse_vec3("1, 2, 3"), Some([1.0, 2.0, 3.0]));
        assert_eq!(parse_vec3("1 2"), None);
        assert_eq!(parse_vec3("1 2 inf"), None);
    }
}
