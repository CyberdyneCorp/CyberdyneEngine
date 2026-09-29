// SPDX-License-Identifier: MIT
//! Navigation authoring: the Nav* document schema, its read model and the `navigation.*` commands.
//! Issue #28, tasks 4.1 to 4.3.
//!
//! --- THE EDITOR STORES INTENT; THE ENGINE COMPUTES ----------------------------------------------
//!
//! A navigation world's settings, its NavMeshSurface, NavObstacle, NavArea and NavLink components
//! and the identity of its accepted bake are authored data, so they live in the scene document and
//! every edit is one transaction: undo, redo, the journal and MCP come with that for free. The
//! bake, the geometry fingerprint, the stale check, path and flow-field searches and navmesh picks
//! are the engine's (`src/editor_backend` `NavigationService`); the commands here send a request
//! through [`cy_editor_commands::NavmeshHost`] and read back what the service answered. Nothing in
//! this file hashes geometry, voxelises or tests a ray.
//!
//! --- THE COMPONENT AND FIELD NAMES ARE A CONTRACT WITH THE ENGINE, PINNED FROM BOTH ENDS ---------
//!
//! The runtime host (`samples/05b-editor-window/runtime/nav_runtime.cpp`) reads these components by
//! name from the synced `.cyworld`, as `crate::bodies` explains for physics. The names are the
//! engine's (`cy.navigation.NavMeshSurface` and the others) with the namespace dropped, and the
//! field names are the ones `nav_runtime.cpp` reads. The test
//! `the_schema_matches_the_runtime_test_map` checks this table against the runtime's own fixture,
//! `samples/05b-editor-window/runtime/tests/data/nav_test_map.cyworld`, which the C++ runtime tests
//! bake. A spelling that drifted on either side turns one of the two suites red.
//!
//! --- ONE PARSER FOR EVERY EDIT -------------------------------------------------------------------
//!
//! Every `add`, `set` and `settings.set` command takes `values`: `field=value` pairs separated by
//! `;`, such as `shape.offset=4, 0, 4; shape.radius=1`. Each value is read as its field's declared
//! kind. One gesture that changes three fields is then one command and one transaction, from the
//! panel and from an agent alike.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, NavmeshHost, Outcome, ParameterSpec,
    Registry,
};
use cy_editor_core::ids::{DocumentId, FieldId, NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::operation::Operation;
use cy_editor_documents::schema::DocumentSchema;
use cy_editor_documents::selection::Selection;

use crate::backend::{
    NAVIGATION_FLOWFIELD_OPERATION, NAVIGATION_PATH_OPERATION, NAVIGATION_PICK_OPERATION,
    NAVIGATION_STATUS_OPERATION,
};
use crate::nav_bake::{
    NavBackend, NavBakeReport, NavSettingsBlock, bake_request, flow_request, path_request,
    pick_request, status_request,
};

/// The navigation world's settings and accepted bake. `cy::navigation` has no component of this
/// name: it stands for the `NavBakeSettings` plus the recorded identity the runtime reads.
pub const NAVIGATION_WORLD: &str = "NavigationWorld";
/// `cy.navigation.NavMeshSurface`.
pub const NAV_MESH_SURFACE: &str = "NavMeshSurface";
/// `cy.navigation.NavObstacle`.
pub const NAV_OBSTACLE: &str = "NavObstacle";
/// `cy.navigation.NavArea`.
pub const NAV_AREA: &str = "NavArea";
/// `cy.navigation.NavLink`.
pub const NAV_LINK: &str = "NavLink";

/// The engine's `kAreaNull`: an obstacle with this area blocks the polygons it covers.
const AREA_NULL: i64 = 63;
/// The engine's `kAreaCount`.
const AREA_COUNT: i64 = 64;
/// Every `NavDebugFlags` bit.
const OVERLAY_ALL: i64 = 0x1FF;

/// One authored field: its name, kind, default, and whether `values` may set it.
pub struct FieldSpec {
    /// The field name, as the runtime reads it.
    pub name: &'static str,
    /// Its kind.
    pub kind: ValueKind,
    /// The value a new component starts with.
    pub default: Value,
    /// Whether an `add`, `set` or `settings.set` may write it. The world a component belongs to is
    /// a command parameter, and the bake fields are written only by a completed bake.
    pub settable: bool,
    /// What it means, for the Inspector and a tool description.
    pub description: &'static str,
}

/// One authored component: its name, the command noun for it, and its fields.
pub struct ComponentSpec {
    /// The component name, as the runtime reads it.
    pub name: &'static str,
    /// The noun its commands use: `surface`, `obstacle`, `area`, `link`, or `world`.
    pub noun: &'static str,
    /// The name a node created for it gets.
    pub label: &'static str,
    /// Its fields.
    pub fields: &'static [FieldSpec],
}

const fn field(
    name: &'static str,
    kind: ValueKind,
    default: Value,
    description: &'static str,
) -> FieldSpec {
    FieldSpec {
        name,
        kind,
        default,
        settable: true,
        description,
    }
}

const fn fixed(
    name: &'static str,
    kind: ValueKind,
    default: Value,
    description: &'static str,
) -> FieldSpec {
    FieldSpec {
        name,
        kind,
        default,
        settable: false,
        description,
    }
}

const WORLD_ID: FieldSpec = fixed(
    "world",
    ValueKind::Int,
    Value::Int(1),
    "The navigation world this belongs to.",
);

static WORLD_FIELDS: [FieldSpec; 16] = [
    WORLD_ID,
    field(
        "agent_radius",
        ValueKind::Float,
        Value::Float(0.5),
        "Agent radius, metres.",
    ),
    field(
        "agent_height",
        ValueKind::Float,
        Value::Float(2.0),
        "Agent height, metres.",
    ),
    field(
        "max_slope",
        ValueKind::Float,
        Value::Float(45.0),
        "Steepest walkable slope, degrees.",
    ),
    field(
        "step_height",
        ValueKind::Float,
        Value::Float(0.4),
        "Highest step an agent climbs, metres.",
    ),
    field(
        "cell_size",
        ValueKind::Float,
        Value::Float(0.3),
        "Voxel cell size, metres.",
    ),
    field(
        "cell_height",
        ValueKind::Float,
        Value::Float(0.2),
        "Voxel cell height, metres.",
    ),
    field(
        "tile_size",
        ValueKind::Float,
        Value::Float(16.0),
        "Tile size, metres.",
    ),
    field(
        "layers",
        ValueKind::Int,
        Value::Int(-1),
        "Source layer mask; -1 is every layer.",
    ),
    field(
        "tags",
        ValueKind::Int,
        Value::Int(-1),
        "Source tag mask; -1 is every tag.",
    ),
    field(
        "backend",
        ValueKind::Int,
        Value::Int(1),
        "Voxeliser: 1 engine, 2 recast.",
    ),
    fixed(
        "overlay",
        ValueKind::Int,
        Value::Int(0),
        "Viewport overlay flags (NavDebugFlags bits).",
    ),
    fixed(
        "bake_identity",
        ValueKind::Int,
        Value::Int(0),
        "Identity of the accepted bake; 0 when unbaked.",
    ),
    fixed(
        "source_fingerprint",
        ValueKind::Int,
        Value::Int(0),
        "Engine fingerprint of the accepted bake's inputs.",
    ),
    fixed(
        "tile_count",
        ValueKind::Int,
        Value::Int(0),
        "Non-empty tiles in the accepted bake.",
    ),
    fixed(
        "sidecar",
        ValueKind::Text,
        Value::Text(String::new()),
        "Project-relative .cynavmesh of the accepted bake.",
    ),
];

static SURFACE_FIELDS: [FieldSpec; 4] = [
    WORLD_ID,
    field(
        "bounds.min",
        ValueKind::Vec3,
        Value::Vec3([-8.0, -1.0, -8.0]),
        "Volume minimum, node space.",
    ),
    field(
        "bounds.max",
        ValueKind::Vec3,
        Value::Vec3([8.0, 4.0, 8.0]),
        "Volume maximum, node space.",
    ),
    field(
        "exclude",
        ValueKind::Bool,
        Value::Bool(false),
        "Carve this volume out instead of baking it.",
    ),
];

static AREA_FIELDS: [FieldSpec; 5] = [
    WORLD_ID,
    field(
        "bounds.min",
        ValueKind::Vec3,
        Value::Vec3([-0.5, -0.5, -0.5]),
        "Volume minimum, node space.",
    ),
    field(
        "bounds.max",
        ValueKind::Vec3,
        Value::Vec3([0.5, 0.5, 0.5]),
        "Volume maximum, node space.",
    ),
    field(
        "area",
        ValueKind::Int,
        Value::Int(1),
        "Area type 0-63 given to triangles inside.",
    ),
    field(
        "cost",
        ValueKind::Float,
        Value::Float(1.0),
        "Traversal cost of the area type.",
    ),
];

static OBSTACLE_FIELDS: [FieldSpec; 6] = [
    WORLD_ID,
    field(
        "shape.offset",
        ValueKind::Vec3,
        Value::Vec3([0.0, 0.0, 0.0]),
        "Centre offset from the node, world space.",
    ),
    field(
        "shape.radius",
        ValueKind::Float,
        Value::Float(0.5),
        "Cylinder radius, metres.",
    ),
    field(
        "shape.height",
        ValueKind::Float,
        Value::Float(1.0),
        "Cylinder height, metres.",
    ),
    field(
        "shape.half_extents",
        ValueKind::Vec3,
        Value::Vec3([0.5, 0.5, 0.5]),
        "Footprint box half extents.",
    ),
    field(
        "area",
        ValueKind::Int,
        Value::Int(AREA_NULL),
        "Area it marks; 63 blocks.",
    ),
];

static LINK_FIELDS: [FieldSpec; 8] = [
    WORLD_ID,
    field(
        "from",
        ValueKind::Vec3,
        Value::Vec3([0.0, 0.0, 0.0]),
        "Start point, node space.",
    ),
    field(
        "to",
        ValueKind::Vec3,
        Value::Vec3([0.0, 0.0, 1.0]),
        "End point, node space.",
    ),
    field(
        "cost",
        ValueKind::Float,
        Value::Float(1.0),
        "Traversal cost.",
    ),
    field("area", ValueKind::Int, Value::Int(0), "Area type 0-63."),
    field(
        "bidirectional",
        ValueKind::Bool,
        Value::Bool(true),
        "Whether agents cross both ways.",
    ),
    field(
        "requires_capabilities",
        ValueKind::Int,
        Value::Int(0),
        "Capability bits an agent needs.",
    ),
    field(
        "action",
        ValueKind::Text,
        Value::Text(String::new()),
        "Named traversal action, such as jump.",
    ),
];

/// The navigation world component.
pub static WORLD: ComponentSpec = ComponentSpec {
    name: NAVIGATION_WORLD,
    noun: "world",
    label: "Navigation World",
    fields: &WORLD_FIELDS,
};

/// The four placeable components, in the order their commands are registered.
pub static PLACEABLE: [ComponentSpec; 4] = [
    ComponentSpec {
        name: NAV_MESH_SURFACE,
        noun: "surface",
        label: "Nav Mesh Surface",
        fields: &SURFACE_FIELDS,
    },
    ComponentSpec {
        name: NAV_OBSTACLE,
        noun: "obstacle",
        label: "Nav Obstacle",
        fields: &OBSTACLE_FIELDS,
    },
    ComponentSpec {
        name: NAV_AREA,
        noun: "area",
        label: "Nav Area",
        fields: &AREA_FIELDS,
    },
    ComponentSpec {
        name: NAV_LINK,
        noun: "link",
        label: "Nav Link",
        fields: &LINK_FIELDS,
    },
];

impl ComponentSpec {
    fn field(&self, name: &str) -> Option<&FieldSpec> {
        self.fields.iter().find(|field| field.name == name)
    }
}

/// Every navigation component, the world first.
pub fn components() -> impl Iterator<Item = &'static ComponentSpec> {
    std::iter::once(&WORLD).chain(PLACEABLE.iter())
}

fn spec_named(name: &str) -> Option<&'static ComponentSpec> {
    components().find(|spec| spec.name == name || spec.noun == name)
}

// --- Schema ------------------------------------------------------------------------------------

/// Declare every navigation component and field. Idempotent, so every command calls it first and
/// the Inspector shows the Nav* types once any navigation command has run.
pub fn declare(schema: &mut DocumentSchema) -> Result<()> {
    for spec in components() {
        let component = match schema.type_named(spec.name) {
            Some(found) => found.id,
            None => schema.declare_type(spec.name, false),
        };
        for field in spec.fields {
            let declared = schema
                .type_of(component)
                .and_then(|definition| definition.field_named(field.name))
                .is_some();
            if !declared {
                schema.declare_field(component, field.name, field.kind, field.description)?;
            }
        }
    }
    Ok(())
}

fn type_id(schema: &DocumentSchema, name: &str) -> Result<TypeId> {
    schema
        .type_named(name)
        .map(|definition| definition.id)
        .ok_or_else(|| Problem::not_found(format!("the {name} component")))
}

fn field_id(schema: &DocumentSchema, component: &str, name: &str) -> Result<FieldId> {
    schema
        .type_named(component)
        .and_then(|definition| definition.field_named(name))
        .map(|field| field.id)
        .ok_or_else(|| Problem::not_found(format!("the {component} field {name}")))
}

fn read_field<'a>(
    document: &'a Document,
    node: NodeId,
    component: &str,
    name: &str,
) -> Option<&'a Value> {
    let schema = document.schema();
    let component_id = schema.type_named(component)?.id;
    let field = schema.type_named(component)?.field_named(name)?.id;
    document.content().field(node, component_id, field)
}

// --- Values ------------------------------------------------------------------------------------

fn parse_int(text: &str) -> Option<i64> {
    match text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
        Some(hex) => u64::from_str_radix(hex, 16)
            .ok()
            .map(|bits| i64::from_ne_bytes(bits.to_ne_bytes())),
        None => text.parse().ok(),
    }
}

fn parse_vec3(text: &str) -> Option<[f32; 3]> {
    let inner = text
        .trim_start_matches(['(', '['])
        .trim_end_matches([')', ']']);
    let lanes: Vec<f32> = inner
        .split([',', ' '])
        .filter(|lane| !lane.is_empty())
        .map(str::parse)
        .collect::<std::result::Result<_, _>>()
        .ok()?;
    let lanes: [f32; 3] = lanes.try_into().ok()?;
    lanes.iter().all(|lane| lane.is_finite()).then_some(lanes)
}

fn parse_bool(text: &str) -> Option<bool> {
    match text {
        "true" | "1" | "yes" | "on" => Some(true),
        "false" | "0" | "no" | "off" => Some(false),
        _ => None,
    }
}

/// Read `text` as `field`'s kind. `backend` also takes `engine` and `recast`.
fn parse_value(field: &FieldSpec, text: &str) -> Result<Value> {
    let text = text.trim();
    let value = match field.kind {
        ValueKind::Int if field.name == "backend" => {
            NavBackend::parse(text).map(|backend| Value::Int(backend.code()))
        }
        ValueKind::Float => text
            .parse::<f32>()
            .ok()
            .filter(|value| value.is_finite())
            .map(Value::Float),
        ValueKind::Int => parse_int(text).map(Value::Int),
        ValueKind::Bool => parse_bool(text).map(Value::Bool),
        ValueKind::Vec3 => parse_vec3(text).map(Value::Vec3),
        ValueKind::Text => Some(Value::Text(text.to_string())),
        _ => None,
    };
    let value = value.ok_or_else(|| {
        Problem::new(
            format!("set {}", field.name),
            format!("{text:?} is not a {}", field.kind),
        )
        .with_remedy(field.description)
    })?;
    check_range(field, &value)?;
    Ok(value)
}

fn check_range(field: &FieldSpec, value: &Value) -> Result<()> {
    if field.name == "area"
        && let Value::Int(area) = value
        && !(0..AREA_COUNT).contains(area)
    {
        return Err(Problem::new(
            "set area",
            format!("area {area} is outside 0 to 63"),
        ));
    }
    Ok(())
}

/// `field=value; field=value`, each read as its declared kind.
fn parse_values(spec: &ComponentSpec, text: &str) -> Result<Vec<(&'static str, Value)>> {
    text.split(';')
        .map(str::trim)
        .filter(|pair| !pair.is_empty())
        .map(|pair| {
            let (name, value) = pair.split_once('=').ok_or_else(|| {
                Problem::new(
                    format!("edit {}", spec.name),
                    format!("{pair:?} is not field=value"),
                )
                .with_remedy("separate pairs with ';', such as shape.radius=1; area=63")
            })?;
            let field = settable_field(spec, name.trim())?;
            Ok((field.name, parse_value(field, value)?))
        })
        .collect()
}

fn settable_field<'a>(spec: &'a ComponentSpec, name: &str) -> Result<&'a FieldSpec> {
    let names: Vec<&str> = spec
        .fields
        .iter()
        .filter(|field| field.settable)
        .map(|field| field.name)
        .collect();
    spec.field(name)
        .filter(|field| field.settable)
        .ok_or_else(|| {
            Problem::new(
                format!("edit {}", spec.name),
                format!("{name:?} is not a field an edit may set"),
            )
            .with_remedy(format!("use one of {}", names.join(", ")))
        })
}

fn world_argument(arguments: &Arguments) -> Result<u32> {
    let world = match arguments.get("world") {
        Some(Value::Int(world)) => *world,
        _ => 1,
    };
    u32::try_from(world).map_err(|_| {
        Problem::new(
            "name a navigation world",
            format!("{world} is not a navigation world identity"),
        )
    })
}

fn vec3_argument(arguments: &Arguments, name: &str) -> [f32; 3] {
    match arguments.get(name) {
        Some(Value::Vec3(value)) => *value,
        _ => [0.0; 3],
    }
}

fn float_argument(arguments: &Arguments, name: &str) -> f32 {
    match arguments.get(name) {
        Some(Value::Float(value)) => *value,
        _ => 0.0,
    }
}

fn node_argument(arguments: &Arguments, name: &str) -> Result<Option<NodeId>> {
    let text = arguments.text(name).unwrap_or_default().trim();
    if text.is_empty() {
        return Ok(None);
    }
    u128::from_str_radix(text, 16)
        .map(|bits| Some(NodeId::from_u128(bits)))
        .map_err(|_| {
            Problem::new(
                format!("read the {name} identity"),
                "it is not a 32-digit hexadecimal node identity",
            )
        })
}

fn required_node(arguments: &Arguments, name: &str) -> Result<NodeId> {
    node_argument(arguments, name)?.ok_or_else(|| {
        Problem::new(
            "edit a navigation component",
            format!("no {name} identity was given"),
        )
    })
}

fn bits(value: u64) -> Value {
    Value::Int(i64::from_ne_bytes(value.to_ne_bytes()))
}

fn word(value: Option<&Value>) -> u64 {
    match value {
        Some(Value::Int(bits)) => u64::from_ne_bytes(bits.to_ne_bytes()),
        _ => 0,
    }
}

fn hex(value: u64) -> Value {
    Value::Text(format!("0x{value:016x}"))
}

// --- The read model ----------------------------------------------------------------------------

/// One navigation world as the document states it: what the panel shows and what a bake sends.
#[derive(Clone, PartialEq, Debug)]
pub struct NavmeshSettings {
    /// The node carrying the `NavigationWorld` component.
    pub node: NodeId,
    /// The world identity components name.
    pub world: u32,
    /// The profile and build settings.
    pub settings: NavSettingsBlock,
    /// The overlay flags.
    pub overlay: u32,
    /// The accepted bake's identity; 0 when unbaked.
    pub bake_identity: u64,
    /// The engine's fingerprint of the accepted bake's inputs.
    pub source_fingerprint: u64,
    /// Non-empty tiles in the accepted bake.
    pub tile_count: u32,
    /// Its `.cynavmesh` sidecar.
    pub sidecar: String,
}

impl NavmeshSettings {
    /// Every navigation world in the document, by world identity.
    #[must_use]
    pub fn read(document: &Document) -> Vec<Self> {
        let Some(component) = document.schema().type_named(NAVIGATION_WORLD).map(|t| t.id) else {
            return Vec::new();
        };
        let mut worlds: Vec<Self> = document
            .content()
            .nodes()
            .filter(|node| document.content().has_component(*node, component))
            .map(|node| Self::of_node(document, node))
            .collect();
        worlds.sort_by_key(|world| world.world);
        worlds
    }

    /// The navigation world `world`, when the document declares it.
    #[must_use]
    pub fn find(document: &Document, world: u32) -> Option<Self> {
        Self::read(document)
            .into_iter()
            .find(|settings| settings.world == world)
    }

    fn of_node(document: &Document, node: NodeId) -> Self {
        let value = |name: &str| read_field(document, node, NAVIGATION_WORLD, name);
        let real = |name: &str, fallback: f32| match value(name) {
            Some(Value::Float(real)) => *real,
            _ => fallback,
        };
        let integer = |name: &str| match value(name) {
            Some(Value::Int(integer)) => *integer,
            _ => 0,
        };
        let defaults = NavSettingsBlock::DEFAULT;
        Self {
            node,
            world: u32::try_from(integer("world")).unwrap_or(0),
            settings: NavSettingsBlock {
                agent_radius: real("agent_radius", defaults.agent_radius),
                agent_height: real("agent_height", defaults.agent_height),
                max_slope: real("max_slope", defaults.max_slope),
                step_height: real("step_height", defaults.step_height),
                cell_size: real("cell_size", defaults.cell_size),
                cell_height: real("cell_height", defaults.cell_height),
                tile_size: real("tile_size", defaults.tile_size),
                layers: word(value("layers")),
                tags: word(value("tags")),
                backend: NavBackend::from_code(integer("backend")).unwrap_or(NavBackend::Engine),
            },
            overlay: u32::try_from(integer("overlay")).unwrap_or(0),
            bake_identity: word(value("bake_identity")),
            source_fingerprint: word(value("source_fingerprint")),
            tile_count: u32::try_from(integer("tile_count")).unwrap_or(0),
            sidecar: match value("sidecar") {
                Some(Value::Text(text)) => text.clone(),
                _ => String::new(),
            },
        }
    }

    /// Refuse a world whose stored back end is not Engine or Recast, then the engine's rules.
    fn validate(&self, document: &Document) -> Result<()> {
        let code = match read_field(document, self.node, NAVIGATION_WORLD, "backend") {
            Some(Value::Int(code)) => *code,
            _ => 0,
        };
        if NavBackend::from_code(code).is_none() {
            return Err(Problem::new(
                "use these navigation settings",
                "a bake must name its back end: engine or recast",
            ));
        }
        self.settings.validate()
    }

    fn outcome(&self, summary: String) -> Outcome {
        let block = &self.settings;
        Outcome::new(summary)
            .with("node", Value::Text(self.node.to_string()))
            .with("world", Value::Int(i64::from(self.world)))
            .with("agent_radius", Value::Float(block.agent_radius))
            .with("agent_height", Value::Float(block.agent_height))
            .with("max_slope", Value::Float(block.max_slope))
            .with("step_height", Value::Float(block.step_height))
            .with("cell_size", Value::Float(block.cell_size))
            .with("cell_height", Value::Float(block.cell_height))
            .with("tile_size", Value::Float(block.tile_size))
            .with("layers", hex(block.layers))
            .with("tags", hex(block.tags))
            .with("backend", Value::Text(block.backend.keyword().into()))
            .with("overlay", Value::Int(i64::from(self.overlay)))
            .with("bake_identity", hex(self.bake_identity))
            .with("source_fingerprint", hex(self.source_fingerprint))
            .with("tile_count", Value::Int(i64::from(self.tile_count)))
            .with("sidecar", Value::Text(self.sidecar.clone()))
    }
}

/// Record a completed bake on its world's node: ONE transaction setting the bake identity, the
/// source fingerprint, the tile count and the sidecar. Undo restores the previous identity, which
/// the runtime answers by reloading that bake's sidecar.
pub fn record_bake(
    document: &mut Document,
    node: NodeId,
    report: &NavBakeReport,
    actor: cy_editor_core::Actor,
) -> Result<()> {
    let settings = NavmeshSettings::of_node(document, node);
    if settings.world != report.world
        || !document
            .content()
            .has_component(node, type_id(document.schema(), NAVIGATION_WORLD)?)
    {
        return Err(Problem::new(
            "record a navigation bake",
            "its navigation world is no longer in the document",
        ));
    }
    let schema = document.schema();
    let component = type_id(schema, NAVIGATION_WORLD)?;
    let fields = [
        ("bake_identity", bits(report.identity)),
        ("source_fingerprint", bits(report.fingerprint)),
        (
            "tile_count",
            Value::Int(i64::try_from(report.tile_count()).unwrap_or(i64::MAX)),
        ),
        ("sidecar", Value::Text(report.sidecar.clone())),
    ]
    .into_iter()
    .map(|(name, value)| Ok((field_id(schema, NAVIGATION_WORLD, name)?, value)))
    .collect::<Result<Vec<_>>>()?;
    document.with_transaction(
        format!("Bake navigation world {}", report.world),
        actor,
        |document| {
            for (field, value) in fields {
                document.set_field(node, component, field, value)?;
            }
            Ok(())
        },
    )
}

// --- Registration ------------------------------------------------------------------------------

/// Register every `navigation.*` command. Eighteen: thirteen reversible document edits and five
/// reads.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(create_world())?;
    registry.register(settings_get())?;
    registry.register(settings_set())?;
    registry.register(overlay_set())?;
    registry.register(bake())?;
    registry.register(bake_status())?;
    for spec in &PLACEABLE {
        registry.register(add_component(spec))?;
        registry.register(set_component(spec))?;
    }
    registry.register(remove_component())?;
    registry.register(path_query())?;
    registry.register(flowfield_query())?;
    registry.register(point_pick())?;
    Ok(())
}

fn world_parameter() -> ParameterSpec {
    ParameterSpec::optional(
        "world",
        ValueKind::Int,
        "Navigation world identity.",
        Value::Int(1),
    )
}

fn values_parameter(required: bool) -> ParameterSpec {
    let description = "field=value pairs separated by ';', each read as the field's kind.";
    if required {
        ParameterSpec::required("values", ValueKind::Text, description)
    } else {
        ParameterSpec::optional(
            "values",
            ValueKind::Text,
            description,
            Value::Text(String::new()),
        )
    }
}

fn active(context: &dyn CommandContext) -> Result<DocumentId> {
    context.active_document().ok_or_else(|| {
        Problem::new("edit navigation", "no world is open").with_remedy("open a world first")
    })
}

fn document_mut(context: &mut dyn CommandContext) -> Result<&mut Document> {
    let id = active(context)?;
    context
        .document_mut(id)
        .ok_or_else(|| Problem::not_found("the active document"))
}

fn document(context: &dyn CommandContext) -> Result<&Document> {
    let id = active(context)?;
    context
        .document(id)
        .ok_or_else(|| Problem::not_found("the active document"))
}

fn require_world(document: &Document, world: u32) -> Result<NavmeshSettings> {
    NavmeshSettings::find(document, world).ok_or_else(|| {
        Problem::new(
            "use a navigation world",
            format!("the document declares no navigation world {world}"),
        )
        .with_remedy("create one with navigation.world.create")
    })
}

fn navmesh(context: &mut dyn CommandContext) -> Result<&mut dyn NavmeshHost> {
    context.navmesh().ok_or_else(|| {
        Problem::new("ask the engine's navigation service", "this host has none")
            .with_remedy("invoke this through the editor with a runtime attached")
    })
}

fn select(context: &mut dyn CommandContext, node: NodeId) {
    let mut selection = Selection::new();
    selection.add_node(node);
    context.set_selection(selection);
}

// --- Worlds and settings -----------------------------------------------------------------------

fn next_world(document: &Document) -> u32 {
    NavmeshSettings::read(document)
        .iter()
        .map(|settings| settings.world)
        .max()
        .map_or(1, |world| world.saturating_add(1))
}

fn create_world() -> Command {
    Command::new(
        Metadata::new(
            "navigation.world.create",
            "Create Navigation World",
            "Navigation",
            "Creates a navigation world with the engine's default agent and build settings, as one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::optional("world", ValueKind::Int, "World identity; 0 picks the next free one.", Value::Int(0)))
        .with(values_parameter(false)),
        |context, arguments| {
            let actor = context.actor();
            let document = document_mut(context)?;
            declare(document.schema_mut())?;
            let requested = world_argument(arguments)?;
            let world = if requested == 0 { next_world(document) } else { requested };
            if NavmeshSettings::find(document, world).is_some() {
                return Err(Problem::new("create a navigation world", format!("navigation world {world} already exists"))
                    .with_remedy("pass another world identity, or 0 for the next free one"));
            }
            let overrides = parse_values(&WORLD, arguments.text("values").unwrap_or_default())?;
            let node = document.with_transaction(format!("Create navigation world {world}"), actor, |document| {
                let node = document.create_node(None)?;
                document.set_name(node, format!("{} {world}", WORLD.label))?;
                let fields = initial_fields(document.schema(), &WORLD, world, &overrides)?;
                document.add_component(node, type_id(document.schema(), WORLD.name)?, fields)?;
                NavmeshSettings::of_node(document, node).validate(document)?;
                Ok(node)
            })?;
            select(context, node);
            Ok(Outcome::new(format!("Created navigation world {world}"))
                .with("node", Value::Text(node.to_string()))
                .with("world", Value::Int(i64::from(world))))
        },
    )
}

fn initial_fields(
    schema: &DocumentSchema,
    spec: &ComponentSpec,
    world: u32,
    overrides: &[(&str, Value)],
) -> Result<Vec<(FieldId, Value)>> {
    spec.fields
        .iter()
        .map(|field| {
            let value = if field.name == "world" {
                Value::Int(i64::from(world))
            } else {
                overrides
                    .iter()
                    .rev()
                    .find(|(name, _)| *name == field.name)
                    .map_or_else(|| field.default.clone(), |(_, value)| value.clone())
            };
            Ok((field_id(schema, spec.name, field.name)?, value))
        })
        .collect()
}

fn settings_get() -> Command {
    Command::new(
        Metadata::new(
            "navigation.settings.get",
            "Navigation Settings",
            "Navigation",
            "Reports a navigation world's agent and build settings, overlay flags and the accepted bake it records.",
            EffectClass::Read,
        )
        .with(world_parameter()),
        |context, arguments| {
            let world = world_argument(arguments)?;
            let settings = require_world(document(context)?, world)?;
            Ok(settings.outcome(format!("Navigation world {world}")))
        },
    )
}

fn settings_set() -> Command {
    Command::new(
        Metadata::new(
            "navigation.settings.set",
            "Set Navigation Settings",
            "Navigation",
            "Sets agent radius, height, max slope, step height, cell size, cell height, tile size, layers, tags or backend (engine or recast) as one undoable transaction, refusing settings the engine would refuse.",
            EffectClass::ReversibleMutation,
        )
        .with(world_parameter())
        .with(values_parameter(true)),
        |context, arguments| {
            let world = world_argument(arguments)?;
            let actor = context.actor();
            let document = document_mut(context)?;
            declare(document.schema_mut())?;
            let node = require_world(document, world)?.node;
            let values = parse_values(&WORLD, arguments.text("values").unwrap_or_default())?;
            write_fields(document, node, &WORLD, &values, format!("Set navigation world {world} settings"), actor, |document| {
                NavmeshSettings::of_node(document, node).validate(document)
            })?;
            let settings = require_world(document, world)?;
            Ok(settings.outcome(format!("Set {} navigation setting(s)", values.len())))
        },
    )
}

/// Set `values` on `node`'s `spec` component in ONE transaction, then run `check` inside it: a
/// refusal rolls the whole gesture back.
fn write_fields(
    document: &mut Document,
    node: NodeId,
    spec: &ComponentSpec,
    values: &[(&str, Value)],
    label: String,
    actor: cy_editor_core::Actor,
    check: impl FnOnce(&Document) -> Result<()>,
) -> Result<()> {
    if values.is_empty() {
        return Err(Problem::new(
            format!("edit {}", spec.name),
            "no field=value pair was given",
        ));
    }
    let component = type_id(document.schema(), spec.name)?;
    let fields = values
        .iter()
        .map(|(name, value)| Ok((field_id(document.schema(), spec.name, name)?, value.clone())))
        .collect::<Result<Vec<_>>>()?;
    document.with_transaction(label, actor, |document| {
        for (field, value) in fields {
            document.set_field(node, component, field, value)?;
        }
        check(document)
    })
}

/// The overlay words and their `NavDebugFlags` bits.
pub const OVERLAYS: [(&str, i64); 9] = [
    ("polygons", 1),
    ("tiles", 1 << 1),
    ("adjacency", 1 << 2),
    ("links", 1 << 3),
    ("corridors", 1 << 4),
    ("paths", 1 << 5),
    ("avoidance", 1 << 6),
    ("neighbours", 1 << 7),
    ("obstacles", 1 << 8),
];

fn parse_overlays(text: &str) -> Result<i64> {
    text.split([',', ' ', ';'])
        .map(str::trim)
        .filter(|word| !word.is_empty())
        .try_fold(0_i64, |flags, word| {
            let bit = match word {
                "all" => Some(OVERLAY_ALL),
                "none" => Some(0),
                _ => OVERLAYS
                    .iter()
                    .find(|(name, _)| *name == word)
                    .map(|(_, bit)| *bit),
            };
            bit.map(|bit| flags | bit).ok_or_else(|| {
                Problem::new(
                    "set the navigation overlay",
                    format!("{word:?} is not an overlay"),
                )
                .with_remedy(format!(
                    "use all, none, or any of {}",
                    OVERLAYS.map(|(name, _)| name).join(", ")
                ))
            })
        })
}

fn overlay_set() -> Command {
    Command::new(
        Metadata::new(
            "navigation.overlay.set",
            "Set Navigation Overlay",
            "Navigation",
            "Chooses which of a world's navigation overlays the viewport draws (polygons, tiles, adjacency, links, corridors, paths, avoidance, neighbours, obstacles, all or none), as one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(world_parameter())
        .with(ParameterSpec::required("overlays", ValueKind::Text, "Comma-separated overlay names, all, or none.")),
        |context, arguments| {
            let world = world_argument(arguments)?;
            let flags = parse_overlays(arguments.text("overlays").unwrap_or_default())?;
            let actor = context.actor();
            let document = document_mut(context)?;
            declare(document.schema_mut())?;
            let node = require_world(document, world)?.node;
            let component = type_id(document.schema(), NAVIGATION_WORLD)?;
            let field = field_id(document.schema(), NAVIGATION_WORLD, "overlay")?;
            document.with_transaction(format!("Set navigation world {world} overlay"), actor, |document| {
                document.set_field(node, component, field, Value::Int(flags))
            })?;
            Ok(Outcome::new(format!("Navigation world {world} overlay set")).with("overlay", Value::Int(flags)))
        },
    )
}

// --- Bake --------------------------------------------------------------------------------------

fn bake() -> Command {
    Command::new(
        Metadata::new(
            "navigation.bake",
            "Bake Navigation",
            "Navigation",
            "Asks the engine to bake a navigation world from its settings and sources. When the bake completes the editor records its identity as one undoable transaction; a failure records nothing and keeps the diagnostics in navigation.bake.status.",
            EffectClass::ReversibleMutation,
        )
        .with(world_parameter()),
        |context, arguments| {
            let world = world_argument(arguments)?;
            let document_id = active(context)?;
            let document = document(context)?;
            let settings = require_world(document, world)?;
            settings.validate(document)?;
            let payload = bake_request(world, &settings.settings);
            let request = navmesh(context)?.bake(document_id, settings.node, payload)?;
            Ok(Outcome::new(format!("Baking navigation world {world}"))
                .with("request", Value::Int(i64::try_from(request).unwrap_or(i64::MAX))))
        },
    )
}

fn bake_status() -> Command {
    Command::new(
        Metadata::new(
            "navigation.bake.status",
            "Navigation Bake Status",
            "Navigation",
            "Reports the pending navigation request and its progress, the last bake report or diagnostics, the accepted bake the document records, whether the engine found it stale, and the last path, flow-field and pick answers. refresh asks the engine to recheck staleness.",
            EffectClass::Read,
        )
        .with(world_parameter())
        .with(ParameterSpec::optional("refresh", ValueKind::Bool, "Send navigation.status to recompute the engine fingerprint.", Value::Bool(false))),
        |context, arguments| {
            let world = world_argument(arguments)?;
            let settings = require_world(document(context)?, world)?;
            let refresh = matches!(arguments.get("refresh"), Some(Value::Bool(true)));
            let host = navmesh(context)?;
            let mut outcome = host.status();
            if refresh {
                let payload = status_request(world, &settings.settings, settings.bake_identity, settings.source_fingerprint);
                let request = host.query(NAVIGATION_STATUS_OPERATION, payload)?;
                outcome = outcome.with("status_request", Value::Int(i64::try_from(request).unwrap_or(i64::MAX)));
            }
            Ok(outcome
                .with("bake_identity", hex(settings.bake_identity))
                .with("source_fingerprint", hex(settings.source_fingerprint))
                .with("tile_count", Value::Int(i64::from(settings.tile_count)))
                .with("sidecar", Value::Text(settings.sidecar)))
        },
    )
}

// --- Components --------------------------------------------------------------------------------

fn add_component(spec: &'static ComponentSpec) -> Command {
    Command::new(
        Metadata::new(
            format!("navigation.{}.add", spec.noun),
            format!("Add {}", spec.label),
            "Navigation",
            format!(
                "Adds a {} to a navigation world, on a new node or on `node`, with optional field=value overrides, as one undoable transaction.",
                spec.name
            ),
            EffectClass::ReversibleMutation,
        )
        .with(world_parameter())
        .with(ParameterSpec::optional("node", ValueKind::Text, "Existing node to carry it; empty creates one.", Value::Text(String::new())))
        .with(values_parameter(false)),
        move |context, arguments| {
            let world = world_argument(arguments)?;
            let target = node_argument(arguments, "node")?;
            let overrides = parse_values(spec, arguments.text("values").unwrap_or_default())?;
            let actor = context.actor();
            let document = document_mut(context)?;
            declare(document.schema_mut())?;
            require_world(document, world)?;
            let component = type_id(document.schema(), spec.name)?;
            if let Some(node) = target {
                refuse_duplicate(document, node, component, spec)?;
            }
            let node = document.with_transaction(format!("Add {}", spec.label), actor, |document| {
                let node = if let Some(node) = target {
                    node
                } else {
                    let node = document.create_node(None)?;
                    document.set_name(node, spec.label)?;
                    node
                };
                let fields = initial_fields(document.schema(), spec, world, &overrides)?;
                document.add_component(node, component, fields)?;
                Ok(node)
            })?;
            select(context, node);
            Ok(Outcome::new(format!("Added {}", spec.label)).with("node", Value::Text(node.to_string())))
        },
    )
}

fn refuse_duplicate(
    document: &Document,
    node: NodeId,
    component: TypeId,
    spec: &ComponentSpec,
) -> Result<()> {
    if document.content().node(node).is_none() {
        return Err(Problem::not_found("that node"));
    }
    if document.content().has_component(node, component) {
        return Err(Problem::new(
            format!("add a {}", spec.name),
            "the node already carries one",
        )
        .with_remedy(format!("edit it with navigation.{}.set", spec.noun)));
    }
    Ok(())
}

fn require_component(document: &Document, node: NodeId, spec: &ComponentSpec) -> Result<()> {
    let component = type_id(document.schema(), spec.name)?;
    if document.content().has_component(node, component) {
        Ok(())
    } else {
        Err(Problem::new(
            format!("edit a {}", spec.name),
            format!("the node carries no {}", spec.name),
        ))
    }
}

fn set_component(spec: &'static ComponentSpec) -> Command {
    Command::new(
        Metadata::new(
            format!("navigation.{}.set", spec.noun),
            format!("Edit {}", spec.label),
            "Navigation",
            format!(
                "Sets fields of a node's {} from field=value pairs as one undoable transaction.",
                spec.name
            ),
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "node",
            ValueKind::Text,
            "The node carrying the component.",
        ))
        .with(values_parameter(true)),
        move |context, arguments| {
            let node = required_node(arguments, "node")?;
            let values = parse_values(spec, arguments.text("values").unwrap_or_default())?;
            let actor = context.actor();
            let document = document_mut(context)?;
            declare(document.schema_mut())?;
            require_component(document, node, spec)?;
            write_fields(
                document,
                node,
                spec,
                &values,
                format!("Edit {}", spec.label),
                actor,
                |_| Ok(()),
            )?;
            Ok(Outcome::new(format!(
                "Edited {} field(s) of {}",
                values.len(),
                spec.label
            ))
            .with("node", Value::Text(node.to_string())))
        },
    )
}

fn remove_component() -> Command {
    Command::new(
        Metadata::new(
            "navigation.component.remove",
            "Remove Navigation Component",
            "Navigation",
            "Removes a navigation component (world, surface, obstacle, area or link) from a node, and the node too when that leaves it empty, as one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required("node", ValueKind::Text, "The node carrying the component."))
        .with(ParameterSpec::required("component", ValueKind::Text, "world, surface, obstacle, area, link, or the component name.")),
        |context, arguments| {
            let node = required_node(arguments, "node")?;
            let name = arguments.text("component").unwrap_or_default().trim().to_string();
            let spec = spec_named(&name).ok_or_else(|| {
                Problem::new("remove a navigation component", format!("{name:?} is not a navigation component"))
                    .with_remedy("use world, surface, obstacle, area or link")
            })?;
            let actor = context.actor();
            let document = document_mut(context)?;
            declare(document.schema_mut())?;
            require_component(document, node, spec)?;
            let removed = remove_from(document, node, spec, actor)?;
            Ok(Outcome::new(format!("Removed {}", spec.label))
                .with("node", Value::Text(node.to_string()))
                .with("node_removed", Value::Bool(removed)))
        },
    )
}

/// Remove `spec` from `node` in one transaction; delete the node when nothing is left on it.
fn remove_from(
    document: &mut Document,
    node: NodeId,
    spec: &ComponentSpec,
    actor: cy_editor_core::Actor,
) -> Result<bool> {
    let component = type_id(document.schema(), spec.name)?;
    let state = document
        .content()
        .node(node)
        .ok_or_else(|| Problem::not_found("that node"))?;
    let before: Vec<(FieldId, Value)> = state
        .components
        .get(&component)
        .map(|fields| {
            fields
                .iter()
                .map(|(id, value)| (*id, value.clone()))
                .collect()
        })
        .unwrap_or_default();
    let empties = state.components.len() == 1 && state.children.is_empty();
    document.with_transaction(format!("Remove {}", spec.label), actor, |document| {
        document.record(Operation::RemoveComponent {
            node,
            component,
            before,
        })?;
        if empties {
            document.delete_node(node)?;
        }
        Ok(empties)
    })
}

// --- Queries -----------------------------------------------------------------------------------

fn request_value(request: u64) -> Value {
    Value::Int(i64::try_from(request).unwrap_or(i64::MAX))
}

fn path_query() -> Command {
    Command::new(
        Metadata::new(
            "navigation.path.query",
            "Test Navigation Path",
            "Navigation",
            "Asks the engine for a test path between two world points on a baked navigation world; the answer appears in navigation.bake.status.",
            EffectClass::Read,
        )
        .with(world_parameter())
        .with(ParameterSpec::required("start", ValueKind::Vec3, "Start point, world space."))
        .with(ParameterSpec::required("end", ValueKind::Vec3, "End point, world space."))
        .with(ParameterSpec::optional("extents", ValueKind::Vec3, "Search box half extents for snapping the points.", Value::Vec3([1.0, 2.0, 1.0]))),
        |context, arguments| {
            let world = world_argument(arguments)?;
            let payload = path_request(world, vec3_argument(arguments, "start"), vec3_argument(arguments, "end"), vec3_argument(arguments, "extents"));
            let request = navmesh(context)?.query(NAVIGATION_PATH_OPERATION, payload)?;
            Ok(Outcome::new(format!("Querying a path on navigation world {world}")).with("request", request_value(request)))
        },
    )
}

fn flowfield_query() -> Command {
    Command::new(
        Metadata::new(
            "navigation.flowfield.query",
            "Test Navigation Flow Field",
            "Navigation",
            "Asks the engine for a flow field toward a target over a region of a baked navigation world; the answer appears in navigation.bake.status.",
            EffectClass::Read,
        )
        .with(world_parameter())
        .with(ParameterSpec::required("target", ValueKind::Vec3, "Target point, world space."))
        .with(ParameterSpec::required("min", ValueKind::Vec3, "Region minimum, world space."))
        .with(ParameterSpec::required("max", ValueKind::Vec3, "Region maximum, world space."))
        .with(ParameterSpec::optional("cell", ValueKind::Float, "Flow-field cell size, metres.", Value::Float(0.5))),
        |context, arguments| {
            let world = world_argument(arguments)?;
            let region = (vec3_argument(arguments, "min"), vec3_argument(arguments, "max"));
            let payload = flow_request(world, vec3_argument(arguments, "target"), region, float_argument(arguments, "cell"));
            let request = navmesh(context)?.query(NAVIGATION_FLOWFIELD_OPERATION, payload)?;
            Ok(Outcome::new(format!("Querying a flow field on navigation world {world}")).with("request", request_value(request)))
        },
    )
}

fn point_pick() -> Command {
    Command::new(
        Metadata::new(
            "navigation.point.pick",
            "Pick Navigation Point",
            "Navigation",
            "Asks the engine for the navmesh point under a pixel of a viewport frame it published; the answer appears in navigation.bake.status.",
            EffectClass::Read,
        )
        .with(world_parameter())
        .with(ParameterSpec::optional("viewport", ValueKind::Int, "Viewport the frame came from.", Value::Int(0)))
        .with(ParameterSpec::required("frame", ValueKind::Int, "The published frame the pixel belongs to."))
        .with(ParameterSpec::required("x", ValueKind::Float, "Pixel column in the frame."))
        .with(ParameterSpec::required("y", ValueKind::Float, "Pixel row in the frame.")),
        |context, arguments| {
            let world = world_argument(arguments)?;
            let integer = |name: &str| match arguments.get(name) {
                Some(Value::Int(value)) => *value,
                _ => 0,
            };
            let viewport = u32::try_from(integer("viewport")).map_err(|_| Problem::new("pick a navigation point", "the viewport identity is negative"))?;
            let frame = u64::try_from(integer("frame")).map_err(|_| Problem::new("pick a navigation point", "the frame identity is negative"))?;
            let pixel = [float_argument(arguments, "x"), float_argument(arguments, "y")];
            let request = navmesh(context)?.query(NAVIGATION_PICK_OPERATION, pick_request(world, viewport, frame, pixel))?;
            Ok(Outcome::new(format!("Picking a point on navigation world {world}")).with("request", request_value(request)))
        },
    )
}

#[cfg(test)]
mod tests {
    use cy_editor_commands::scope::Scope;
    use cy_editor_core::Actor;

    use super::*;
    use crate::editor::Editor;

    fn setup() -> (Editor, Registry, DocumentId) {
        let mut editor = Editor::new(Actor::human("designer"));
        let document = editor.open_document("worlds/nav.cyworld").unwrap();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        (editor, registry, document)
    }

    fn invoke(
        editor: &mut Editor,
        registry: &Registry,
        id: &str,
        arguments: &Arguments,
    ) -> Result<Outcome> {
        editor.invoke(registry, id, &Scope::unrestricted(), arguments)
    }

    fn history(editor: &Editor, document: DocumentId) -> usize {
        editor
            .documents
            .get(document)
            .unwrap()
            .history()
            .entries()
            .len()
    }

    fn text(outcome: &Outcome, name: &str) -> String {
        outcome.values[name].as_text().unwrap().to_string()
    }

    fn node_of(outcome: &Outcome) -> NodeId {
        NodeId::from_u128(u128::from_str_radix(&text(outcome, "node"), 16).unwrap())
    }

    fn field_of(
        editor: &Editor,
        document: DocumentId,
        node: NodeId,
        component: &str,
        name: &str,
    ) -> Option<Value> {
        read_field(
            editor.documents.get(document).unwrap(),
            node,
            component,
            name,
        )
        .cloned()
    }

    fn undo_redo(editor: &mut Editor, registry: &Registry) -> (Outcome, Outcome) {
        (
            invoke(editor, registry, "edit.undo", &Arguments::new()).unwrap(),
            invoke(editor, registry, "edit.redo", &Arguments::new()).unwrap(),
        )
    }

    fn create_world(editor: &mut Editor, registry: &Registry) -> NodeId {
        let created = invoke(
            editor,
            registry,
            "navigation.world.create",
            &Arguments::new(),
        )
        .unwrap();
        assert_eq!(created.values["world"], Value::Int(1));
        node_of(&created)
    }

    #[test]
    fn creating_a_world_is_one_transaction_and_undo_removes_it() {
        let (mut editor, registry, document) = setup();
        create_world(&mut editor, &registry);
        assert_eq!(history(&editor, document), 1);
        let world = NavmeshSettings::find(editor.documents.get(document).unwrap(), 1).unwrap();
        assert_eq!(world.settings, NavSettingsBlock::DEFAULT);
        invoke(&mut editor, &registry, "edit.undo", &Arguments::new()).unwrap();
        assert!(NavmeshSettings::read(editor.documents.get(document).unwrap()).is_empty());
        invoke(&mut editor, &registry, "edit.redo", &Arguments::new()).unwrap();
        assert!(NavmeshSettings::find(editor.documents.get(document).unwrap(), 1).is_some());
        let second = invoke(
            &mut editor,
            &registry,
            "navigation.world.create",
            &Arguments::new(),
        )
        .unwrap();
        assert_eq!(second.values["world"], Value::Int(2));
    }

    #[test]
    fn a_settings_edit_is_one_entry_and_undo_redo_restore_it() {
        let (mut editor, registry, document) = setup();
        create_world(&mut editor, &registry);
        let set = invoke(
            &mut editor,
            &registry,
            "navigation.settings.set",
            &Arguments::new().with(
                "values",
                Value::Text("agent_radius=0.25; backend=recast; tile_size=8".into()),
            ),
        )
        .unwrap();
        assert_eq!(set.values["backend"], Value::Text("recast".into()));
        assert_eq!(history(&editor, document), 2, "one entry per gesture");
        let read = |editor: &Editor| {
            NavmeshSettings::find(editor.documents.get(document).unwrap(), 1)
                .unwrap()
                .settings
        };
        assert_eq!(read(&editor).backend, NavBackend::Recast);
        assert!((read(&editor).agent_radius - 0.25).abs() < f32::EPSILON);
        invoke(&mut editor, &registry, "edit.undo", &Arguments::new()).unwrap();
        assert_eq!(read(&editor), NavSettingsBlock::DEFAULT);
        invoke(&mut editor, &registry, "edit.redo", &Arguments::new()).unwrap();
        assert!((read(&editor).tile_size - 8.0).abs() < f32::EPSILON);
    }

    #[test]
    fn settings_the_engine_would_refuse_record_nothing() {
        let (mut editor, registry, document) = setup();
        create_world(&mut editor, &registry);
        for values in [
            "cell_size=0",
            "agent_radius=-1",
            "backend=automatic",
            "bake_identity=5",
            "nonsense",
            "tile_size=x",
        ] {
            let refused = invoke(
                &mut editor,
                &registry,
                "navigation.settings.set",
                &Arguments::new().with("values", Value::Text(values.into())),
            );
            assert!(refused.is_err(), "{values} must be refused");
        }
        assert_eq!(history(&editor, document), 1);
        assert_eq!(
            NavmeshSettings::find(editor.documents.get(document).unwrap(), 1)
                .unwrap()
                .settings,
            NavSettingsBlock::DEFAULT
        );
    }

    #[test]
    fn an_overlay_edit_is_one_entry_and_undo_redo_restore_it() {
        let (mut editor, registry, document) = setup();
        let node = create_world(&mut editor, &registry);
        invoke(
            &mut editor,
            &registry,
            "navigation.overlay.set",
            &Arguments::new().with("overlays", Value::Text("polygons, tiles, obstacles".into())),
        )
        .unwrap();
        assert_eq!(history(&editor, document), 2);
        let overlay =
            |editor: &Editor| field_of(editor, document, node, NAVIGATION_WORLD, "overlay");
        assert_eq!(overlay(&editor), Some(Value::Int(1 | 2 | 256)));
        undo_redo(&mut editor, &registry);
        assert_eq!(overlay(&editor), Some(Value::Int(1 | 2 | 256)));
        invoke(&mut editor, &registry, "edit.undo", &Arguments::new()).unwrap();
        assert_eq!(overlay(&editor), Some(Value::Int(0)));
        assert!(
            invoke(
                &mut editor,
                &registry,
                "navigation.overlay.set",
                &Arguments::new().with("overlays", Value::Text("sparkles".into())),
            )
            .is_err()
        );
    }

    #[test]
    fn every_component_add_set_and_remove_is_one_entry_and_undoes() {
        for spec in &PLACEABLE {
            let (mut editor, registry, document) = setup();
            create_world(&mut editor, &registry);
            let added = invoke(
                &mut editor,
                &registry,
                &format!("navigation.{}.add", spec.noun),
                &Arguments::new(),
            )
            .unwrap();
            let node = node_of(&added);
            assert_eq!(history(&editor, document), 2, "{}", spec.name);
            let (name, text, expected) = sample_edit(spec);
            invoke(
                &mut editor,
                &registry,
                &format!("navigation.{}.set", spec.noun),
                &Arguments::new()
                    .with("node", Value::Text(node.to_string()))
                    .with("values", Value::Text(format!("{name}={text}"))),
            )
            .unwrap();
            assert_eq!(history(&editor, document), 3, "{}", spec.name);
            let current = |editor: &Editor| field_of(editor, document, node, spec.name, name);
            assert_eq!(current(&editor), Some(expected.clone()));
            invoke(&mut editor, &registry, "edit.undo", &Arguments::new()).unwrap();
            assert_eq!(
                current(&editor),
                Some(spec.field(name).unwrap().default.clone())
            );
            invoke(&mut editor, &registry, "edit.redo", &Arguments::new()).unwrap();
            assert_eq!(current(&editor), Some(expected));
            let removed = invoke(
                &mut editor,
                &registry,
                "navigation.component.remove",
                &Arguments::new()
                    .with("node", Value::Text(node.to_string()))
                    .with("component", Value::Text(spec.noun.into())),
            )
            .unwrap();
            assert_eq!(removed.values["node_removed"], Value::Bool(true));
            assert_eq!(history(&editor, document), 4, "{}", spec.name);
            assert!(
                editor
                    .documents
                    .get(document)
                    .unwrap()
                    .content()
                    .node(node)
                    .is_none()
            );
            invoke(&mut editor, &registry, "edit.undo", &Arguments::new()).unwrap();
            assert!(
                current(&editor).is_some(),
                "undo restores the {}",
                spec.name
            );
        }
    }

    fn sample_edit(spec: &ComponentSpec) -> (&'static str, &'static str, Value) {
        match spec.noun {
            "surface" => ("bounds.max", "(4, 2, 4)", Value::Vec3([4.0, 2.0, 4.0])),
            "obstacle" => ("shape.offset", "4, 0, 4", Value::Vec3([4.0, 0.0, 4.0])),
            "area" => ("cost", "3.5", Value::Float(3.5)),
            _ => ("action", "jump", Value::Text("jump".into())),
        }
    }

    #[test]
    fn a_component_on_an_existing_node_keeps_the_node_when_removed() {
        let (mut editor, registry, document) = setup();
        let world = create_world(&mut editor, &registry);
        let added = invoke(
            &mut editor,
            &registry,
            "navigation.obstacle.add",
            &Arguments::new().with("node", Value::Text(world.to_string())),
        )
        .unwrap();
        assert_eq!(node_of(&added), world);
        let duplicate = invoke(
            &mut editor,
            &registry,
            "navigation.obstacle.add",
            &Arguments::new().with("node", Value::Text(world.to_string())),
        );
        assert!(duplicate.is_err());
        let removed = invoke(
            &mut editor,
            &registry,
            "navigation.component.remove",
            &Arguments::new()
                .with("node", Value::Text(world.to_string()))
                .with("component", Value::Text(NAV_OBSTACLE.into())),
        )
        .unwrap();
        assert_eq!(removed.values["node_removed"], Value::Bool(false));
        assert!(NavmeshSettings::find(editor.documents.get(document).unwrap(), 1).is_some());
    }

    #[test]
    fn a_component_needs_its_world_and_valid_values() {
        let (mut editor, registry, document) = setup();
        let missing = invoke(
            &mut editor,
            &registry,
            "navigation.surface.add",
            &Arguments::new(),
        );
        assert!(
            missing
                .unwrap_err()
                .remedy
                .unwrap()
                .contains("navigation.world.create")
        );
        create_world(&mut editor, &registry);
        for values in [
            "area=64",
            "world=2",
            "shape.offset=1, 2",
            "shape.radius=wide",
        ] {
            let refused = invoke(
                &mut editor,
                &registry,
                "navigation.obstacle.add",
                &Arguments::new().with("values", Value::Text(values.into())),
            );
            assert!(refused.is_err(), "{values}");
        }
        assert_eq!(history(&editor, document), 1);
    }

    #[test]
    fn read_commands_record_nothing_and_queries_need_a_runtime() {
        let (mut editor, registry, document) = setup();
        create_world(&mut editor, &registry);
        let settings = invoke(
            &mut editor,
            &registry,
            "navigation.settings.get",
            &Arguments::new(),
        )
        .unwrap();
        assert_eq!(settings.values["backend"], Value::Text("engine".into()));
        assert_eq!(settings.values["bake_identity"], hex(0));
        let status = invoke(
            &mut editor,
            &registry,
            "navigation.bake.status",
            &Arguments::new(),
        )
        .unwrap();
        assert_eq!(status.values["bake"], Value::Text("idle".into()));
        for (id, arguments) in [
            ("navigation.bake", Arguments::new()),
            (
                "navigation.path.query",
                Arguments::new()
                    .with("start", Value::Vec3([0.0; 3]))
                    .with("end", Value::Vec3([1.0; 3])),
            ),
        ] {
            let refused = invoke(&mut editor, &registry, id, &arguments).unwrap_err();
            assert!(refused.because.contains("runtime"), "{id}: {refused}");
        }
        assert_eq!(history(&editor, document), 1);
    }

    #[test]
    fn every_navigation_mutation_is_reversible_and_every_query_a_read() {
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        assert_eq!(registry.len(), 18);
        let reads = [
            "navigation.settings.get",
            "navigation.bake.status",
            "navigation.path.query",
            "navigation.flowfield.query",
            "navigation.point.pick",
        ];
        for metadata in registry.all() {
            let expected = if reads.contains(&metadata.id.as_str()) {
                EffectClass::Read
            } else {
                EffectClass::ReversibleMutation
            };
            assert_eq!(metadata.effect, expected, "{}", metadata.id);
        }
    }

    /// The runtime reads these names from the synced `.cyworld`; its own fixture is the other end.
    #[test]
    fn the_schema_matches_the_runtime_test_map() {
        let map = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../samples/05b-editor-window/runtime/tests/data/nav_test_map.cyworld");
        let text = std::fs::read_to_string(&map).unwrap();
        let mut component: Option<&ComponentSpec> = None;
        let mut checked = 0;
        for line in text.lines() {
            let words: Vec<&str> = line.split_whitespace().collect();
            match words.as_slice() {
                ["type", _, _, name] => component = spec_named(name.trim_matches('"')),
                ["node", ..] => component = None,
                ["field", _, kind, name, ..] if component.is_some() => {
                    let spec = component.unwrap();
                    let name = name.trim_matches('"');
                    let field = spec
                        .field(name)
                        .unwrap_or_else(|| panic!("{} declares no {name}", spec.name));
                    assert_eq!(&field.kind.to_string(), kind, "{}.{name}", spec.name);
                    checked += 1;
                }
                _ => {}
            }
        }
        assert!(checked >= 30, "the map pins {checked} navigation fields");
    }
}
