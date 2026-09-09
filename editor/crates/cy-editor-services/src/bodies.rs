//! Adding a physics body in the inspector: one transaction, and undo removes it. M8.a task 4.3.
//!
//! --- WHY A COMMAND AND NOT AN INSPECTOR WIDGET ---------------------------------------------------
//!
//! `editor-ui-ux` and `editor-agent-interface` between them settle this: "an action reachable only
//! through a specific widget SHALL be a defect", and every command a person can run is an agent
//! tool by construction. So "add a rigid body in the inspector" is `scene.add-body`, and the
//! inspector's own control is a caller of it. The artefact's step 3 — "add a rigid body to each, in
//! the inspector" — runs this.
//!
//! --- THE ONE DECISION WORTH ARGUING WITH: A BODY AND A COLLIDER ARE ONE TRANSACTION --------------
//!
//! `physics` specifies them as two components and the engine registers them as two. But
//! `cy::physics::validate(BodyDescription)` refuses a **dynamic body with no collider**, and it is
//! right to: such a body has no volume, therefore no derived mass, therefore an infinite
//! acceleration the first time gravity touches it. So an editor that added a `RigidBody` alone
//! would produce, for one edit, a world that cannot simulate — and the person would see a body
//! refused by name at the moment they pressed play rather than at the moment they made it.
//!
//! This command therefore writes both, in **one** transaction: one undo removes the body and its
//! collider together, which is also what a person means by "undo that". `shape=none` is the escape
//! hatch for a caller that wants the components separately, and `scene.add-collider` adds a
//! collider to something that already has a body.
//!
//! --- THE COMPONENT AND FIELD NAMES ARE A CONTRACT WITH THE ENGINE, PINNED FROM BOTH ENDS ---------
//!
//! `src/gameplay/play/src/session.cpp` reads these names out of the saved `.cyworld`'s own type
//! section, because physics' components are registered in the ECS **by name with no reflected type
//! behind them** — so `cy::scene::serialization::resolve_against` resolves them to nothing and
//! carries them, exactly as it does `MeshRenderer` for [`crate::primitives`].
//!
//! That makes the spelling load-bearing across a process and a language boundary, and a spelling
//! that drifted on one side would be a body the runtime silently does not simulate. It is pinned
//! the way `.cyprim` is: [`BodyBinding::COMPONENTS`] and [`ColliderBinding`] here,
//! `kRigidBody`/`kFieldMass`/… in session.cpp, and a golden world in each language's tests.
//!
//! **The day physics' components are reflected**, `resolve_against` fills in their engine types and
//! the C++ side reads them through `engine_type` instead of by name. Nothing here changes: the
//! names are the engine's own.

use cy_editor_commands::{
    Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::operation::Operation;
use cy_editor_documents::schema::DocumentSchema;

/// Register `scene.add-body`, `scene.add-collider` and `scene.remove-body`.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(add_body())?;
    registry.register(add_collider())?;
    registry.register(remove_body())?;
    Ok(())
}

// --- The bindings ---------------------------------------------------------------------------------

/// Which body a node carries.
///
/// Three components rather than one with a motion field, because that is what `physics` specifies
/// and what `cy::physics::PhysicsComponents` registers. An entity carrying two is an authoring
/// mistake that the bridge answers by precedence; this command refuses to make one.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BodyKind {
    /// Moved by the solver. `cy::physics::RigidBody`.
    Dynamic,
    /// Never moved. `cy::physics::StaticBody`.
    Static,
    /// Moved by code or animation, pushes and is not pushed. `cy::physics::KinematicBody`.
    Kinematic,
}

impl BodyKind {
    /// The component name, which is the engine's own with the namespace dropped — the same
    /// transformation `cy::scene::serialization::authoring_name_of` performs.
    #[must_use]
    pub const fn component(self) -> &'static str {
        match self {
            BodyKind::Dynamic => "RigidBody",
            BodyKind::Static => "StaticBody",
            BodyKind::Kinematic => "KinematicBody",
        }
    }

    /// The word the command takes.
    #[must_use]
    pub const fn keyword(self) -> &'static str {
        match self {
            BodyKind::Dynamic => "dynamic",
            BodyKind::Static => "static",
            BodyKind::Kinematic => "kinematic",
        }
    }

    /// Every kind, so a listing and a test cover all of them.
    pub const ALL: [BodyKind; 3] = [BodyKind::Dynamic, BodyKind::Static, BodyKind::Kinematic];

    /// The kind a word names.
    #[must_use]
    pub fn from_keyword(keyword: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|kind| kind.keyword() == keyword)
    }
}

/// The collider shapes an authored `Collider` can name.
///
/// The four the engine's `ShapeDescription` describes analytically and the play session reads back.
/// A mesh collider is not here: it is an asset reference, which is `asset.import`'s
/// `emit_collision`, not a value typed into an inspector.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ColliderShape {
    /// A rectangular box, sized by its half extents.
    Box,
    /// A sphere of one radius.
    Sphere,
    /// A cylinder of one radius capped by two hemispheres.
    Capsule,
    /// A capped cylinder of one radius and one height.
    Cylinder,
}

impl ColliderShape {
    /// The word written into the `shape` field, and read by `session.cpp`'s `shape_of`.
    #[must_use]
    pub const fn keyword(self) -> &'static str {
        match self {
            ColliderShape::Box => "box",
            ColliderShape::Sphere => "sphere",
            ColliderShape::Capsule => "capsule",
            ColliderShape::Cylinder => "cylinder",
        }
    }

    /// Every shape, so a listing and a test cover all of them.
    pub const ALL: [ColliderShape; 4] = [
        ColliderShape::Box,
        ColliderShape::Sphere,
        ColliderShape::Capsule,
        ColliderShape::Cylinder,
    ];

    /// The shape a word names.
    #[must_use]
    pub fn from_keyword(keyword: &str) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|shape| shape.keyword() == keyword)
    }
}

/// Where a body's authored values live in a document's schema.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct BodyBinding {
    /// The component.
    pub component: cy_editor_core::ids::TypeId,
    /// `mass`, on a dynamic body. Absent on the other two, which carry no authored value.
    pub mass: Option<cy_editor_core::ids::FieldId>,
    /// `gravity_scale`, on a dynamic body.
    pub gravity_scale: Option<cy_editor_core::ids::FieldId>,
}

impl BodyBinding {
    /// The three component names, so a caller looking for "does this node have a body" has one
    /// list rather than three literals.
    pub const COMPONENTS: [&'static str; 3] = ["RigidBody", "StaticBody", "KinematicBody"];
    /// The field a dynamic body's mass is written to. Zero means "derive it from the colliders",
    /// which is what `cy::physics::RigidBody::mass` means and why zero is the default here too.
    pub const MASS: &'static str = "mass";
    /// The field a dynamic body's gravity scale is written to.
    pub const GRAVITY_SCALE: &'static str = "gravity_scale";

    /// Find the binding for one kind in a schema, or answer that the document has never seen it.
    #[must_use]
    pub fn of_schema(schema: &DocumentSchema, kind: BodyKind) -> Option<Self> {
        let definition = schema.type_named(kind.component())?;
        Some(Self {
            component: definition.id,
            mass: definition.field_named(Self::MASS).map(|field| field.id),
            gravity_scale: definition
                .field_named(Self::GRAVITY_SCALE)
                .map(|field| field.id),
        })
    }

    /// Find it, or declare it. Idempotent for the reason [`crate::primitives::MeshBinding::declare`]
    /// gives: a second declaration would give one name a second identity, and every history entry
    /// addressing the first would stop applying.
    pub fn declare(schema: &mut DocumentSchema, kind: BodyKind) -> Self {
        if let Some(found) = Self::of_schema(schema, kind) {
            return found;
        }
        let component = schema.declare_type(kind.component(), false);
        if kind != BodyKind::Dynamic {
            // A static or kinematic body has NO authored value: what it is, is its placement and
            // its colliders, and both are other components. A field invented to avoid an empty
            // component would be a value a person could set and nothing would read.
            return Self {
                component,
                mass: None,
                gravity_scale: None,
            };
        }
        let mass = schema.declare_field(
            component,
            Self::MASS,
            ValueKind::Float,
            "Kilograms. Zero derives the mass from the colliders' volumes and densities, which is \
             what a body usually wants.",
        );
        let gravity_scale = schema.declare_field(
            component,
            Self::GRAVITY_SCALE,
            ValueKind::Float,
            "How strongly gravity acts on this body. One is normal, zero is weightless.",
        );
        Self {
            component,
            mass: mass.ok(),
            gravity_scale: gravity_scale.ok(),
        }
    }
}

/// Where a collider's authored values live.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ColliderBinding {
    /// The component holding the collider.
    pub component: cy_editor_core::ids::TypeId,
    /// The field naming which shape it is.
    pub shape: cy_editor_core::ids::FieldId,
    /// A box's half extents.
    pub extent: cy_editor_core::ids::FieldId,
    /// A sphere's, capsule's or cylinder's radius.
    pub radius: cy_editor_core::ids::FieldId,
    /// A capsule's or cylinder's total height.
    pub height: cy_editor_core::ids::FieldId,
}

impl ColliderBinding {
    /// The component's name, as the engine spells it with the namespace dropped.
    pub const COMPONENT: &'static str = "Collider";
    /// The field naming the shape. `session.cpp`'s `kFieldShape`.
    pub const SHAPE: &'static str = "shape";
    /// The field holding a box's half extents. `session.cpp`'s `kFieldExtent`.
    pub const EXTENT: &'static str = "extent";
    /// The field holding a radius. `session.cpp`'s `kFieldRadius`.
    pub const RADIUS: &'static str = "radius";
    /// The field holding a total height. `session.cpp`'s `kFieldHeight`.
    pub const HEIGHT: &'static str = "height";

    /// Find the binding in a document's schema, or answer that there is none.
    #[must_use]
    pub fn of_schema(schema: &DocumentSchema) -> Option<Self> {
        let definition = schema.type_named(Self::COMPONENT)?;
        Some(Self {
            component: definition.id,
            shape: definition.field_named(Self::SHAPE)?.id,
            extent: definition.field_named(Self::EXTENT)?.id,
            radius: definition.field_named(Self::RADIUS)?.id,
            height: definition.field_named(Self::HEIGHT)?.id,
        })
    }

    /// Find it, or declare all four fields.
    ///
    /// **All four, on every shape**, rather than only the ones this shape uses. That is the
    /// opposite of `.cyprim`'s rule, and deliberately: a `.cyprim` is a source asset whose bytes
    /// are hashed into a derivation key, so a parameter that does nothing changes the cache key; a
    /// document schema is a type declaration shared by every collider in the world, and one whose
    /// fields depended on the first collider somebody added would be a schema that differs between
    /// two worlds holding the same colliders.
    pub fn declare(schema: &mut DocumentSchema) -> Self {
        if let Some(found) = Self::of_schema(schema) {
            return found;
        }
        let component = schema.declare_type(Self::COMPONENT, false);
        let declare = |schema: &mut DocumentSchema, name: &str, kind: ValueKind, help: &str| {
            schema
                .declare_field(component, name, kind, help)
                .expect("the type was declared on the line above")
        };
        Self {
            component,
            shape: declare(
                schema,
                Self::SHAPE,
                ValueKind::Text,
                "Which shape: box, sphere, capsule or cylinder.",
            ),
            extent: declare(
                schema,
                Self::EXTENT,
                ValueKind::Vec3,
                "A box's half extents, in metres.",
            ),
            radius: declare(
                schema,
                Self::RADIUS,
                ValueKind::Float,
                "A sphere's, capsule's or cylinder's radius, in metres.",
            ),
            height: declare(
                schema,
                Self::HEIGHT,
                ValueKind::Float,
                "A capsule's or cylinder's TOTAL height, in metres, including the capsule's caps.",
            ),
        }
    }
}

/// Which body component a node carries, if any.
#[must_use]
pub fn body_of(document: &Document, node: NodeId) -> Option<BodyKind> {
    BodyKind::ALL.into_iter().find(|kind| {
        BodyBinding::of_schema(document.schema(), *kind)
            .is_some_and(|binding| document.content().has_component(node, binding.component))
    })
}

/// Whether a node carries a collider.
#[must_use]
pub fn has_collider(document: &Document, node: NodeId) -> bool {
    ColliderBinding::of_schema(document.schema())
        .is_some_and(|binding| document.content().has_component(node, binding.component))
}

// --- Writing them ----------------------------------------------------------------------------------

/// The collider a caller described.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct ColliderSpec {
    /// Which shape.
    pub shape: ColliderShape,
    /// A box's half extents, in metres.
    pub extent: [f32; 3],
    /// A sphere's, capsule's or cylinder's radius, in metres.
    pub radius: f32,
    /// A capsule's or cylinder's total height, in metres.
    pub height: f32,
}

impl Default for ColliderSpec {
    fn default() -> Self {
        Self {
            shape: ColliderShape::Box,
            extent: [0.5, 0.5, 0.5],
            radius: 0.5,
            height: 2.0,
        }
    }
}

/// Add a collider to `node`, inside the open transaction.
///
/// # Errors
///
/// When the document refuses the operation — no transaction is open, or the node is unknown.
pub fn add_collider_to(document: &mut Document, node: NodeId, spec: ColliderSpec) -> Result<()> {
    let binding = ColliderBinding::declare(document.schema_mut());
    document.add_component(
        node,
        binding.component,
        vec![
            (binding.shape, Value::Text(spec.shape.keyword().to_string())),
            (binding.extent, Value::Vec3(spec.extent)),
            (binding.radius, Value::Float(spec.radius)),
            (binding.height, Value::Float(spec.height)),
        ],
    )
}

/// Add a body to `node`, inside the open transaction.
///
/// # Errors
///
/// When the document refuses the operation.
pub fn add_body_to(document: &mut Document, node: NodeId, kind: BodyKind, mass: f32) -> Result<()> {
    let binding = BodyBinding::declare(document.schema_mut(), kind);
    let mut fields = Vec::new();
    if let Some(field) = binding.mass {
        fields.push((field, Value::Float(mass)));
    }
    if let Some(field) = binding.gravity_scale {
        fields.push((field, Value::Float(1.0)));
    }
    document.add_component(node, binding.component, fields)
}

// --- The commands ----------------------------------------------------------------------------------

fn active(context: &dyn CommandContext) -> Result<DocumentId> {
    context.active_document().ok_or_else(|| {
        Problem::new("add a body", "no document is open").with_remedy("open a world first")
    })
}

/// The node a command acts on: the one named, or the single selected one.
fn target(context: &dyn CommandContext, named: &str) -> Result<NodeId> {
    let named = named.trim();
    if !named.is_empty() {
        return u128::from_str_radix(named, 16)
            .map(NodeId::from_u128)
            .map_err(|_| {
                Problem::new(
                    format!("read the entity {named:?}"),
                    "it is not an entity identity",
                )
                .with_remedy("use the identity a command's result printed, which is 32 hex digits")
            });
    }
    let selection = context.selection();
    let mut nodes = selection.nodes();
    match (nodes.next(), nodes.next()) {
        (Some(only), None) => Ok(only),
        (Some(_), Some(_)) => Err(
            Problem::new("add a body", "more than one entity is selected")
                .with_remedy("name one with entity=, or select a single entity"),
        ),
        _ => Err(Problem::new("add a body", "nothing is selected")
            .with_remedy("select an entity, or name one with entity=")),
    }
}

fn spec_of(arguments: &cy_editor_commands::Arguments) -> Result<Option<ColliderSpec>> {
    let keyword = arguments
        .text("shape")
        .unwrap_or_default()
        .trim()
        .to_string();
    if keyword == "none" {
        return Ok(None);
    }
    let shape = ColliderShape::from_keyword(&keyword).ok_or_else(|| {
        Problem::new(
            "add a collider",
            format!("{keyword:?} is not a collider shape"),
        )
        .with_remedy(format!(
            "name one of: {}, or 'none' for a body with no collider",
            ColliderShape::ALL.map(ColliderShape::keyword).join(", ")
        ))
    })?;
    let defaults = ColliderSpec::default();
    Ok(Some(ColliderSpec {
        shape,
        extent: match arguments.get("extent") {
            Some(Value::Vec3(lanes)) => *lanes,
            _ => defaults.extent,
        },
        radius: match arguments.get("radius") {
            Some(Value::Float(value)) => *value,
            _ => defaults.radius,
        },
        height: match arguments.get("height") {
            Some(Value::Float(value)) => *value,
            _ => defaults.height,
        },
    }))
}

/// The four collider parameters, declared once so the two commands that take them cannot drift.
fn with_collider_parameters(metadata: Metadata) -> Metadata {
    metadata
        .with(ParameterSpec::optional(
            "shape",
            ValueKind::Text,
            "The collider's shape: box, sphere, capsule or cylinder — or 'none' to add the body \
             without one, which is what a caller wants when the collider comes from an imported \
             mesh. A box when omitted.",
            Value::Text("box".to_string()),
        ))
        .with(ParameterSpec::optional(
            "extent",
            ValueKind::Vec3,
            "A box collider's half extents, in metres. Half a metre each way when omitted, which \
             is a one-metre cube.",
            Value::Vec3([0.5, 0.5, 0.5]),
        ))
        .with(ParameterSpec::optional(
            "radius",
            ValueKind::Float,
            "A sphere, capsule or cylinder collider's radius, in metres.",
            Value::Float(0.5),
        ))
        .with(ParameterSpec::optional(
            "height",
            ValueKind::Float,
            "A capsule or cylinder collider's TOTAL height, in metres, including a capsule's caps.",
            Value::Float(2.0),
        ))
}

fn entity_parameter(metadata: Metadata) -> Metadata {
    metadata.with(ParameterSpec::optional(
        "entity",
        ValueKind::Text,
        "The identity of the entity to act on, as a create or select command printed it. The \
         selected entity when omitted.",
        Value::Text(String::new()),
    ))
}

fn add_body() -> Command {
    Command::new(
        with_collider_parameters(entity_parameter(
            Metadata::new(
                "scene.add-body",
                "Add Physics Body",
                "Scene",
                "Adds a physics body — and, unless shape=none, a collider — to an entity, as ONE \
                 undoable transaction. Undo removes both. A dynamic body with no collider has no \
                 volume and therefore no mass, which the engine refuses by name at the moment play \
                 is pressed, so the two are written together on purpose. The body simulates when \
                 play is pressed and the entity returns to where it was authored when play stops.",
                EffectClass::ReversibleMutation,
            )
            .with(ParameterSpec::optional(
                "kind",
                ValueKind::Text,
                "Which body: dynamic (moved by the solver), static (never moved) or kinematic \
                 (moved by code, pushes and is not pushed). Dynamic when omitted.",
                Value::Text("dynamic".to_string()),
            ))
            .with(ParameterSpec::optional(
                "mass",
                ValueKind::Float,
                "A dynamic body's mass in kilograms. Zero — the default — derives it from the \
                 colliders' volumes and densities, which is what a body usually wants.",
                Value::Float(0.0),
            ))
            .bound_to("Ctrl+Shift+P"),
        )),
        |context, arguments| {
            let keyword = arguments
                .text("kind")
                .unwrap_or_default()
                .trim()
                .to_string();
            let kind = BodyKind::from_keyword(&keyword).ok_or_else(|| {
                Problem::new("add a body", format!("{keyword:?} is not a body kind")).with_remedy(
                    format!(
                        "name one of: {}",
                        BodyKind::ALL.map(BodyKind::keyword).join(", ")
                    ),
                )
            })?;
            let spec = spec_of(arguments)?;
            let mass = match arguments.get("mass") {
                Some(Value::Float(value)) => *value,
                _ => 0.0,
            };
            if mass < 0.0 {
                return Err(Problem::new("add a body", "a mass cannot be negative")
                    .with_remedy("pass zero to derive it from the colliders, or a positive mass"));
            }
            let node = target(context, arguments.text("entity").unwrap_or_default())?;
            let document_id = active(context)?;
            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the active document"))?;

            if let Some(existing) = body_of(document, node) {
                return Err(Problem::new(
                    "add a body",
                    format!("that entity already has a {} body", existing.keyword()),
                )
                .with_remedy(
                    "remove it with scene.remove-body first; an entity with two body components is \
                     one the solver has to break a tie for",
                ));
            }

            document.with_transaction(
                format!("Add {} body", kind.keyword()),
                actor,
                |document| {
                    add_body_to(document, node, kind, mass)?;
                    if let Some(spec) = spec {
                        add_collider_to(document, node, spec)?;
                    }
                    Ok(())
                },
            )?;

            Ok(Outcome::new(match spec {
                Some(spec) => format!(
                    "Added a {} body and a {} collider",
                    kind.keyword(),
                    spec.shape.keyword()
                ),
                None => format!("Added a {} body with no collider", kind.keyword()),
            })
            .with("entity", Value::Text(node.to_string()))
            .with("kind", Value::Text(kind.keyword().to_string()))
            .with(
                "collider",
                Value::Text(
                    spec.map_or_else(|| "none".to_string(), |s| s.shape.keyword().to_string()),
                ),
            ))
        },
    )
}

fn add_collider() -> Command {
    Command::new(
        with_collider_parameters(entity_parameter(Metadata::new(
            "scene.add-collider",
            "Add Collider",
            "Scene",
            "Adds a collider to an entity without adding a body, as one undoable transaction. A \
             collider on an entity with no body is collision geometry the solver ignores until one \
             is added, which is what a caller building an entity in pieces wants; scene.add-body \
             writes both together and is the ordinary way in.",
            EffectClass::ReversibleMutation,
        ))),
        |context, arguments| {
            let spec = spec_of(arguments)?.ok_or_else(|| {
                Problem::new("add a collider", "shape=none adds nothing")
                    .with_remedy("name a shape: box, sphere, capsule or cylinder")
            })?;
            let node = target(context, arguments.text("entity").unwrap_or_default())?;
            let document_id = active(context)?;
            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            if has_collider(document, node) {
                return Err(
                    Problem::new("add a collider", "that entity already has one").with_remedy(
                        "remove it first; several colliders on one body is a compound shape, which \
                         needs a buffer component the document model does not have yet",
                    ),
                );
            }
            document.with_transaction("Add collider", actor, |document| {
                add_collider_to(document, node, spec)
            })?;
            Ok(
                Outcome::new(format!("Added a {} collider", spec.shape.keyword()))
                    .with("entity", Value::Text(node.to_string()))
                    .with("collider", Value::Text(spec.shape.keyword().to_string())),
            )
        },
    )
}

fn remove_body() -> Command {
    Command::new(
        entity_parameter(Metadata::new(
            "scene.remove-body",
            "Remove Physics Body",
            "Scene",
            "Removes an entity's physics body, as one undoable transaction. The collider stays, \
             because a collider is geometry and removing it is a separate decision; undo restores \
             the body with the values it had.",
            EffectClass::ReversibleMutation,
        )),
        |context, arguments| {
            let node = target(context, arguments.text("entity").unwrap_or_default())?;
            let document_id = active(context)?;
            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            let kind = body_of(document, node).ok_or_else(|| {
                Problem::new("remove a body", "that entity has none")
                    .with_remedy("add one with scene.add-body")
            })?;
            let binding = BodyBinding::of_schema(document.schema(), kind)
                .ok_or_else(|| Problem::not_found("the body component"))?;
            // The values it held, read out of the document, so undo restores the body it removed
            // rather than a default one. The same argument `Document::set_field` makes for reading
            // the before value itself: a caller that passed the wrong one would produce an undo
            // that silently restored something else.
            let before: Vec<(cy_editor_core::ids::FieldId, Value)> = document
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
            document.with_transaction(
                format!("Remove {} body", kind.keyword()),
                actor,
                |document| {
                    document.record(Operation::RemoveComponent {
                        node,
                        component: binding.component,
                        before,
                    })
                },
            )?;
            Ok(Outcome::new(format!("Removed the {} body", kind.keyword()))
                .with("entity", Value::Text(node.to_string()))
                .with("kind", Value::Text(kind.keyword().to_string())))
        },
    )
}
