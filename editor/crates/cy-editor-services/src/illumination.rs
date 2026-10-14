// SPDX-License-Identifier: MIT
//! Global illumination authoring (issue #35): the world's GI settings, GI volumes, each object's
//! illumination class and each material's emitter rule — the same commands for the Lighting
//! panel, the Inspector, scripts and agents — and the engine's GI report.
//!
//! Everything an author decides is a document edit in one transaction, so undo reverses it:
//!
//! | Command | Writes |
//! |---|---|
//! | `gi.settings.set` | `GiSettings` (mode, quality, tier, hardware ray tracing, budget) on the world's one "Global Illumination" node, made by the first call |
//! | `gi.volume.create` | a node with a `Transform` (its translation the box's centre, its scale the box's size) and a `GiVolume` |
//! | `gi.volume.set` | the volume's priority, blend distance, weight, the parameters it overrides (`sets`) and their values |
//! | `gi.object.set-class` | `GiObject` (class) on a placed object; `automatic` gives the decision back to its mobility and size |
//! | `gi.emission.set` | `GiEmission` (material, mode) on the world's one rule node for that material |
//!
//! The runtime reads the same document (`samples/05b-editor-window/runtime/gi_runtime.h` names
//! every field) and hands it to the engine's `IlluminationPreview`, which applies a settings,
//! volume, class or rule change IN PLACE through the calls a game makes through ABI 1.9 — so what
//! an author sees is what a game gets. The scale gizmo sizes a volume because its size is its
//! scale.
//!
//! `gi.report` asks the engine for its GI report (`illumination.report`), optionally explaining
//! the surface under a pixel of a published frame; `gi.status` reads the last answer.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::schema::DocumentSchema;
use cy_editor_documents::selection::Selection;
use cy_editor_viewport::gizmo::TransformBinding;

/// Component names are persisted: they are the join keys between a saved world and the runtime.
pub const SETTINGS: &str = "GiSettings";
/// A GI volume's component.
pub const VOLUME: &str = "GiVolume";
/// A placed object's illumination class.
pub const OBJECT: &str = "GiObject";
/// A material's emitter rule.
pub const EMISSION: &str = "GiEmission";

const CATEGORY: &str = "Lighting";
/// The world's one settings node.
const SETTINGS_NODE: &str = "Global Illumination";

/// The engine's GI modes, as the document spells them.
pub const MODES: [&str; 5] = ["none", "baked", "probe", "dynamic", "hybrid"];
/// The engine's GI qualities.
pub const QUALITIES: [&str; 4] = ["low", "medium", "high", "epic"];
/// The engine's tier preferences.
pub const TIERS: [&str; 3] = ["automatic", "software-only", "world-only"];
/// The illumination classes an author may choose, `automatic` first.
pub const CLASSES: [&str; 5] = [
    "automatic",
    "static",
    "slow-dynamic",
    "fast-dynamic",
    "transient",
];
/// What a rule makes of a material's emitters.
pub const EMISSION_MODES: [&str; 4] = ["automatic", "cache-only", "promote", "off"];

/// The parameters a volume overrides: one bit each in `sets`, `gi::gi_volume_bit`'s.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Override {
    /// The probe ray multiplier.
    Quality,
    /// Probe density.
    ProbeDensity,
    /// How fast its probes age.
    UpdateRate,
    /// Indirect diffuse scale.
    DiffuseIntensity,
    /// Indirect specular scale.
    SpecularIntensity,
    /// The tier its rays prefer.
    Tier,
    /// Where the far field begins.
    FarField,
}

impl Override {
    /// Every override, in bit order.
    pub const ALL: [Override; 7] = [
        Override::Quality,
        Override::ProbeDensity,
        Override::UpdateRate,
        Override::DiffuseIntensity,
        Override::SpecularIntensity,
        Override::Tier,
        Override::FarField,
    ];

    /// Its bit in `sets`.
    #[must_use]
    pub const fn bit(self) -> i64 {
        1 << (self as u32)
    }

    /// What a person reads.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Override::Quality => "Quality",
            Override::ProbeDensity => "Probe density",
            Override::UpdateRate => "Update rate",
            Override::DiffuseIntensity => "Diffuse intensity",
            Override::SpecularIntensity => "Specular intensity",
            Override::Tier => "Tier",
            Override::FarField => "Far field (m)",
        }
    }
}

/// Every override's bits.
const ALL_OVERRIDES: i64 = (1 << 7) - 1;

// --- The schema ----------------------------------------------------------------------------------

fn component(schema: &mut DocumentSchema, name: &str) -> TypeId {
    if let Some(found) = schema.type_named(name) {
        return found.id;
    }
    schema.declare_type(name, true)
}

fn field(
    schema: &mut DocumentSchema,
    component: TypeId,
    name: &str,
    kind: ValueKind,
    description: &str,
) -> FieldId {
    if let Some(found) = schema
        .type_of(component)
        .and_then(|definition| definition.field_named(name))
    {
        return found.id;
    }
    schema
        .declare_field(component, name, kind, description)
        .expect("the component was declared")
}

/// A component's fields by name, declared on first use.
struct Fields {
    component: TypeId,
    ids: Vec<(&'static str, FieldId)>,
}

impl Fields {
    fn declare(
        schema: &mut DocumentSchema,
        name: &str,
        fields: &[(&'static str, ValueKind, &'static str)],
    ) -> Self {
        let component = component(schema, name);
        let ids = fields
            .iter()
            .map(|(field_name, kind, description)| {
                (
                    *field_name,
                    field(schema, component, field_name, *kind, description),
                )
            })
            .collect();
        Self { component, ids }
    }

    fn id(&self, name: &str) -> FieldId {
        self.ids
            .iter()
            .find(|(field_name, _)| *field_name == name)
            .map(|(_, id)| *id)
            .expect("the field was declared")
    }
}

const SETTINGS_FIELDS: [(&str, ValueKind, &str); 5] = [
    (
        "mode",
        ValueKind::Text,
        "none, baked, probe, dynamic or hybrid.",
    ),
    (
        "quality",
        ValueKind::Text,
        "low, medium, high or epic: the global probe quality.",
    ),
    (
        "tier",
        ValueKind::Text,
        "automatic, software-only or world-only.",
    ),
    (
        "hardware_ray_tracing",
        ValueKind::Bool,
        "Use hardware ray tracing where the device has it.",
    ),
    (
        "budget_ms",
        ValueKind::Float,
        "The GI allocation, in milliseconds.",
    ),
];

const VOLUME_FIELDS: [(&str, ValueKind, &str); 11] = [
    (
        "priority",
        ValueKind::Int,
        "Higher wins where volumes overlap.",
    ),
    (
        "blend_distance",
        ValueKind::Float,
        "Metres outside the box the volume fades over.",
    ),
    (
        "weight",
        ValueKind::Float,
        "0 to 1: dials the whole volume down.",
    ),
    (
        "sets",
        ValueKind::Int,
        "The parameters the volume overrides, one bit each.",
    ),
    ("quality", ValueKind::Text, "low, medium, high or epic."),
    (
        "tier",
        ValueKind::Text,
        "automatic, software-only or world-only.",
    ),
    (
        "probe_density",
        ValueKind::Float,
        "Above 1 full density, below 1 the sparse lattice.",
    ),
    (
        "update_rate",
        ValueKind::Float,
        "How fast its probes age in the scheduler.",
    ),
    (
        "diffuse_intensity",
        ValueKind::Float,
        "Scales the indirect diffuse light.",
    ),
    (
        "specular_intensity",
        ValueKind::Float,
        "Scales the indirect specular light.",
    ),
    (
        "far_field",
        ValueKind::Float,
        "Metres from the camera at which the far field begins.",
    ),
];

const OBJECT_FIELDS: [(&str, ValueKind, &str); 1] = [(
    "class",
    ValueKind::Text,
    "automatic, static, slow-dynamic, fast-dynamic or transient.",
)];

const EMISSION_FIELDS: [(&str, ValueKind, &str); 2] = [
    (
        "material",
        ValueKind::Text,
        "The drawn material's reference; default for the grey.",
    ),
    (
        "mode",
        ValueKind::Text,
        "automatic, cache-only, promote or off.",
    ),
];

// --- The read model ------------------------------------------------------------------------------

/// The world's GI settings.
#[derive(Clone, PartialEq, Debug)]
pub struct Settings {
    /// `MODES`.
    pub mode: String,
    /// `QUALITIES`.
    pub quality: String,
    /// `TIERS`.
    pub tier: String,
    /// Hardware ray tracing allowed.
    pub hardware_ray_tracing: bool,
    /// Milliseconds.
    pub budget_ms: f32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            mode: "dynamic".into(),
            quality: "medium".into(),
            tier: "automatic".into(),
            hardware_ray_tracing: true,
            budget_ms: 3.0,
        }
    }
}

/// One GI volume.
#[derive(Clone, PartialEq, Debug)]
pub struct GiVolume {
    /// Its node.
    pub id: NodeId,
    /// Its name.
    pub name: String,
    /// The box's centre: the node's translation.
    pub centre: [f32; 3],
    /// The box's size: the node's scale.
    pub size: [f32; 3],
    /// Higher wins.
    pub priority: i64,
    /// Metres.
    pub blend_distance: f32,
    /// 0 to 1.
    pub weight: f32,
    /// `Override::bit`s.
    pub sets: i64,
    /// `QUALITIES`.
    pub quality: String,
    /// `TIERS`.
    pub tier: String,
    /// The numeric overrides.
    pub probe_density: f32,
    /// How fast its probes age.
    pub update_rate: f32,
    /// Indirect diffuse scale.
    pub diffuse_intensity: f32,
    /// Indirect specular scale.
    pub specular_intensity: f32,
    /// Metres.
    pub far_field: f32,
}

impl GiVolume {
    /// Whether it overrides `parameter`.
    #[must_use]
    pub const fn overrides(&self, parameter: Override) -> bool {
        self.sets & parameter.bit() != 0
    }
}

/// One material's emitter rule.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct EmissionRule {
    /// The rule's node.
    pub id: NodeId,
    /// The material reference.
    pub material: String,
    /// `EMISSION_MODES`.
    pub mode: String,
}

/// What a world holds for GI, read from its document. Presentation, never a second store.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct IlluminationScene {
    /// The settings node, when one was made.
    pub settings_node: Option<NodeId>,
    /// The settings: the engine's defaults with no node.
    pub settings: Settings,
    /// Every GI volume.
    pub volumes: Vec<GiVolume>,
    /// Every object whose class was set, and the class.
    pub classes: Vec<(NodeId, String)>,
    /// Every material rule.
    pub emission: Vec<EmissionRule>,
}

fn text_field(
    document: &Document,
    node: NodeId,
    component: TypeId,
    schema_name: &str,
    name: &str,
) -> Option<String> {
    let field = document
        .schema()
        .type_named(schema_name)?
        .field_named(name)?
        .id;
    document
        .content()
        .field(node, component, field)
        .and_then(Value::as_text)
        .map(str::to_owned)
}

fn float_field(
    document: &Document,
    node: NodeId,
    component: TypeId,
    schema_name: &str,
    name: &str,
    fallback: f32,
) -> f32 {
    document
        .schema()
        .type_named(schema_name)
        .and_then(|definition| definition.field_named(name))
        .and_then(|field| document.content().field(node, component, field.id))
        .and_then(Value::as_float)
        .unwrap_or(fallback)
}

fn int_field(
    document: &Document,
    node: NodeId,
    component: TypeId,
    schema_name: &str,
    name: &str,
    fallback: i64,
) -> i64 {
    document
        .schema()
        .type_named(schema_name)
        .and_then(|definition| definition.field_named(name))
        .and_then(|field| document.content().field(node, component, field.id))
        .and_then(Value::as_int)
        .unwrap_or(fallback)
}

fn bool_field(
    document: &Document,
    node: NodeId,
    component: TypeId,
    schema_name: &str,
    name: &str,
    fallback: bool,
) -> bool {
    match document
        .schema()
        .type_named(schema_name)
        .and_then(|definition| definition.field_named(name))
        .and_then(|field| document.content().field(node, component, field.id))
    {
        Some(Value::Bool(value)) => *value,
        _ => fallback,
    }
}

impl IlluminationScene {
    /// Read the document.
    #[must_use]
    pub fn read(document: &Document) -> Self {
        let mut scene = Self::default();
        let content = document.content();
        let schema = document.schema();
        let transform = TransformBinding::of_schema(schema);
        let name = |node: NodeId| {
            content
                .node(node)
                .map(|state| state.name.clone())
                .unwrap_or_default()
        };
        let type_of = |name: &str| schema.type_named(name).map(|found| found.id);
        let (settings, volume, object, emission) = (
            type_of(SETTINGS),
            type_of(VOLUME),
            type_of(OBJECT),
            type_of(EMISSION),
        );
        for node in content.nodes() {
            if let Some(component) = settings.filter(|id| content.has_component(node, *id)) {
                let read = |field: &str, fallback: &str| {
                    text_field(document, node, component, SETTINGS, field)
                        .filter(|value| !value.is_empty())
                        .unwrap_or_else(|| fallback.to_owned())
                };
                let defaults = Settings::default();
                scene.settings_node = Some(node);
                scene.settings = Settings {
                    mode: read("mode", &defaults.mode),
                    quality: read("quality", &defaults.quality),
                    tier: read("tier", &defaults.tier),
                    hardware_ray_tracing: bool_field(
                        document,
                        node,
                        component,
                        SETTINGS,
                        "hardware_ray_tracing",
                        true,
                    ),
                    budget_ms: float_field(
                        document,
                        node,
                        component,
                        SETTINGS,
                        "budget_ms",
                        defaults.budget_ms,
                    ),
                };
            }
            if let Some(component) = volume.filter(|id| content.has_component(node, *id)) {
                let vec3 = |field: Option<FieldId>, fallback: [f32; 3]| {
                    transform
                        .zip(field)
                        .and_then(|(binding, field)| content.field(node, binding.component, field))
                        .and_then(Value::as_vec3)
                        .unwrap_or(fallback)
                };
                let float = |field: &str, fallback: f32| {
                    float_field(document, node, component, VOLUME, field, fallback)
                };
                let text = |field: &str, fallback: &str| {
                    text_field(document, node, component, VOLUME, field)
                        .filter(|value| !value.is_empty())
                        .unwrap_or_else(|| fallback.to_owned())
                };
                scene.volumes.push(GiVolume {
                    id: node,
                    name: name(node),
                    centre: vec3(transform.map(|binding| binding.translation), [0.0; 3]),
                    size: vec3(transform.map(|binding| binding.scale), [1.0; 3]),
                    priority: int_field(document, node, component, VOLUME, "priority", 0),
                    blend_distance: float("blend_distance", 1.0),
                    weight: float("weight", 1.0),
                    sets: int_field(document, node, component, VOLUME, "sets", 0),
                    quality: text("quality", "medium"),
                    tier: text("tier", "automatic"),
                    probe_density: float("probe_density", 1.0),
                    update_rate: float("update_rate", 1.0),
                    diffuse_intensity: float("diffuse_intensity", 1.0),
                    specular_intensity: float("specular_intensity", 1.0),
                    far_field: float("far_field", 60.0),
                });
            }
            if let Some(component) = object.filter(|id| content.has_component(node, *id)) {
                let class = text_field(document, node, component, OBJECT, "class")
                    .unwrap_or_else(|| "automatic".into());
                scene.classes.push((node, class));
            }
            if let Some(component) = emission.filter(|id| content.has_component(node, *id)) {
                scene.emission.push(EmissionRule {
                    id: node,
                    material: text_field(document, node, component, EMISSION, "material")
                        .unwrap_or_default(),
                    mode: text_field(document, node, component, EMISSION, "mode")
                        .unwrap_or_else(|| "automatic".into()),
                });
            }
        }
        scene
    }

    /// The class an object's GI uses: `automatic` unless set.
    #[must_use]
    pub fn class_of(&self, node: NodeId) -> &str {
        self.classes
            .iter()
            .find(|(id, _)| *id == node)
            .map_or("automatic", |(_, class)| class.as_str())
    }

    /// The rule a material's emitters are under: `automatic` unless set.
    #[must_use]
    pub fn emission_of(&self, material: &str) -> &str {
        self.emission
            .iter()
            .find(|rule| rule.material == material)
            .map_or("automatic", |rule| rule.mode.as_str())
    }
}

// --- Arguments -----------------------------------------------------------------------------------

fn refuse(action: &str, why: impl Into<String>) -> Problem {
    Problem::new(action, why.into())
}

fn active(context: &dyn CommandContext, action: &str) -> Result<cy_editor_core::ids::DocumentId> {
    context
        .active_document()
        .ok_or_else(|| refuse(action, "no world is open").with_remedy("open a world first"))
}

fn node_argument(arguments: &Arguments, name: &str, action: &str) -> Result<NodeId> {
    let text = arguments.text(name).unwrap_or_default();
    u128::from_str_radix(text, 16)
        .map(NodeId::from_u128)
        .map_err(|_| {
            refuse(
                action,
                format!("{name} is not a 32-digit hexadecimal node identity"),
            )
        })
}

/// A keyword argument from `allowed`, or `fallback` when absent.
fn keyword(
    arguments: &Arguments,
    name: &str,
    allowed: &[&str],
    fallback: &str,
    action: &str,
) -> Result<String> {
    let given = arguments.text(name).unwrap_or(fallback);
    if allowed.contains(&given) {
        Ok(given.to_owned())
    } else {
        Err(refuse(action, format!("{given:?} is not a {name}"))
            .with_remedy(format!("use one of {}", allowed.join(", "))))
    }
}

fn float_argument(arguments: &Arguments, name: &str, fallback: f32) -> f32 {
    arguments
        .get(name)
        .and_then(Value::as_float)
        .unwrap_or(fallback)
}

fn int_argument(arguments: &Arguments, name: &str, fallback: i64) -> i64 {
    arguments
        .get(name)
        .and_then(Value::as_int)
        .unwrap_or(fallback)
}

fn finite_positive(value: f32) -> bool {
    value.is_finite() && value > 0.0
}

fn finite_non_negative(value: f32) -> bool {
    value.is_finite() && value >= 0.0
}

// --- Commands ------------------------------------------------------------------------------------

/// Register the GI authoring commands and the report's two read commands.
///
/// # Errors
///
/// When the registry refuses a command's metadata.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(set_settings())?;
    registry.register(create_volume())?;
    registry.register(set_volume())?;
    registry.register(set_class())?;
    registry.register(set_emission())?;
    registry.register(report())?;
    registry.register(status())?;
    Ok(())
}

fn set_settings() -> Command {
    let metadata = Metadata::new(
        "gi.settings.set",
        "Set GI Settings",
        CATEGORY,
        "Sets the open world's global illumination — mode, global probe quality, tier preference, \
         hardware ray tracing and the GI's budget in milliseconds — as one undoable transaction. \
         The engine applies it in place, as a game's options menu would.",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::optional(
        "mode",
        ValueKind::Text,
        "none, baked, probe, dynamic or hybrid.",
        Value::Text("dynamic".into()),
    ))
    .with(ParameterSpec::optional(
        "quality",
        ValueKind::Text,
        "low, medium, high or epic.",
        Value::Text("medium".into()),
    ))
    .with(ParameterSpec::optional(
        "tier",
        ValueKind::Text,
        "automatic, software-only or world-only.",
        Value::Text("automatic".into()),
    ))
    .with(ParameterSpec::optional(
        "hardware_ray_tracing",
        ValueKind::Bool,
        "Use hardware ray tracing where the device has it.",
        Value::Bool(true),
    ))
    .with(ParameterSpec::optional(
        "budget_ms",
        ValueKind::Float,
        "The GI allocation in milliseconds, positive.",
        Value::Float(3.0),
    ));
    Command::new(metadata, |context, arguments| {
        const ACTION: &str = "set the GI settings";
        let mode = keyword(arguments, "mode", &MODES, "dynamic", ACTION)?;
        let quality = keyword(arguments, "quality", &QUALITIES, "medium", ACTION)?;
        let tier = keyword(arguments, "tier", &TIERS, "automatic", ACTION)?;
        let hardware = !matches!(
            arguments.get("hardware_ray_tracing"),
            Some(Value::Bool(false))
        );
        let budget = float_argument(arguments, "budget_ms", 3.0);
        if !finite_positive(budget) {
            return Err(refuse(
                ACTION,
                "the budget must be a positive number of milliseconds",
            ));
        }
        let id = active(context, ACTION)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let existing = IlluminationScene::read(document).settings_node;
        let fields = Fields::declare(document.schema_mut(), SETTINGS, &SETTINGS_FIELDS);
        let values = vec![
            (fields.id("mode"), Value::Text(mode.clone())),
            (fields.id("quality"), Value::Text(quality.clone())),
            (fields.id("tier"), Value::Text(tier.clone())),
            (fields.id("hardware_ray_tracing"), Value::Bool(hardware)),
            (fields.id("budget_ms"), Value::Float(budget)),
        ];
        document.with_transaction("Set GI Settings", actor, |document| {
            if let Some(node) = existing {
                for (field, value) in values {
                    document.set_field(node, fields.component, field, value)?;
                }
                Ok(())
            } else {
                let node = document.create_node(None)?;
                document.set_name(node, SETTINGS_NODE)?;
                document.add_component(node, fields.component, values)
            }
        })?;
        Ok(Outcome::new(format!(
            "GI: {mode}, {quality} quality, {tier} tier, {budget} ms"
        ))
        .with("mode", Value::Text(mode))
        .with("quality", Value::Text(quality))
        .with("tier", Value::Text(tier))
        .with("hardware_ray_tracing", Value::Bool(hardware))
        .with("budget_ms", Value::Float(budget)))
    })
}

fn create_volume() -> Command {
    let metadata = Metadata::new(
        "gi.volume.create",
        "Create GI Volume",
        CATEGORY,
        "Places a GI volume — a box that overrides the GI's quality, density, update rate, \
         intensities, tier or far field inside it — at a world position and size, as one undoable \
         transaction, and selects it. Move it with the move gizmo and size it with the scale \
         gizmo: its translation is the box's centre and its scale the box's size. It overrides \
         nothing until gi.volume.set says what.",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::optional(
        "at",
        ValueKind::Vec3,
        "The box's centre in metres; the origin when omitted.",
        Value::Vec3([0.0; 3]),
    ))
    .with(ParameterSpec::optional(
        "size",
        ValueKind::Vec3,
        "The box's size in metres, every edge positive.",
        Value::Vec3([4.0, 3.0, 4.0]),
    ));
    Command::new(metadata, |context, arguments| {
        const ACTION: &str = "create a GI volume";
        let at = arguments
            .get("at")
            .and_then(Value::as_vec3)
            .unwrap_or([0.0; 3]);
        let size = arguments
            .get("size")
            .and_then(Value::as_vec3)
            .unwrap_or([4.0, 3.0, 4.0]);
        if !size.iter().all(|edge| finite_positive(*edge))
            || !at.iter().all(|lane| lane.is_finite())
        {
            return Err(refuse(
                ACTION,
                "a GI volume needs a finite centre and a positive size",
            ));
        }
        let id = active(context, ACTION)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let fields = Fields::declare(document.schema_mut(), VOLUME, &VOLUME_FIELDS);
        let values = vec![
            (fields.id("priority"), Value::Int(0)),
            (fields.id("blend_distance"), Value::Float(1.0)),
            (fields.id("weight"), Value::Float(1.0)),
            (fields.id("sets"), Value::Int(0)),
            (fields.id("quality"), Value::Text("medium".into())),
            (fields.id("tier"), Value::Text("automatic".into())),
            (fields.id("probe_density"), Value::Float(1.0)),
            (fields.id("update_rate"), Value::Float(1.0)),
            (fields.id("diffuse_intensity"), Value::Float(1.0)),
            (fields.id("specular_intensity"), Value::Float(1.0)),
            (fields.id("far_field"), Value::Float(60.0)),
        ];
        let node = document.with_transaction("Create GI Volume", actor, |document| {
            let node = document.create_node(None)?;
            document.set_name(node, "GI Volume")?;
            crate::scene_actors::place(document, node, at, [0.0, 0.0, 0.0, 1.0])?;
            let binding = TransformBinding::of_schema(document.schema()).expect("placed above");
            document.set_field(node, binding.component, binding.scale, Value::Vec3(size))?;
            document.add_component(node, fields.component, values)?;
            Ok(node)
        })?;
        let mut selection = Selection::new();
        selection.add_node(node);
        context.set_selection(selection);
        Ok(Outcome::new("Created GI volume").with("volume", Value::Text(node.to_string())))
    })
}

fn set_volume() -> Command {
    let metadata = Metadata::new(
        "gi.volume.set",
        "Edit GI Volume",
        CATEGORY,
        "Replaces what a GI volume overrides, as one undoable transaction: its priority, blend \
         distance and weight, the parameters it sets (`sets`, one bit each: 1 quality, 2 probe \
         density, 4 update rate, 8 diffuse intensity, 16 specular intensity, 32 tier, 64 far field) \
         and their values. A parameter whose bit is clear is left to the global settings.",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required("volume", ValueKind::Text, "Stable identity of the GI volume's node."))
    .with(ParameterSpec::optional("priority", ValueKind::Int, "Higher wins where volumes overlap.", Value::Int(0)))
    .with(ParameterSpec::optional("blend_distance", ValueKind::Float, "Metres outside the box it fades over, at least 0.", Value::Float(1.0)))
    .with(ParameterSpec::optional("weight", ValueKind::Float, "How strongly the whole volume applies, from 0 (not at all) to 1.", Value::Float(1.0)))
    .with(ParameterSpec::optional("sets", ValueKind::Int, "The parameters overridden, one bit each (see the description).", Value::Int(0)))
    .with(ParameterSpec::optional("quality", ValueKind::Text, "low, medium, high or epic.", Value::Text("medium".into())))
    .with(ParameterSpec::optional("tier", ValueKind::Text, "automatic, software-only or world-only.", Value::Text("automatic".into())))
    .with(ParameterSpec::optional("probe_density", ValueKind::Float, "Probe density multiplier, positive: above 1 full density, below 1 the sparse lattice.", Value::Float(1.0)))
    .with(ParameterSpec::optional("update_rate", ValueKind::Float, "How fast the volume's probes age in the scheduler, positive: 2 is about twice the updates.", Value::Float(1.0)))
    .with(ParameterSpec::optional("diffuse_intensity", ValueKind::Float, "Scale on the indirect diffuse light inside the volume, at least 0.", Value::Float(1.0)))
    .with(ParameterSpec::optional("specular_intensity", ValueKind::Float, "Scale on the indirect specular light inside the volume, at least 0.", Value::Float(1.0)))
    .with(ParameterSpec::optional("far_field", ValueKind::Float, "Metres from the camera at which the far field begins, positive.", Value::Float(60.0)));
    Command::new(metadata, |context, arguments| {
        const ACTION: &str = "edit a GI volume";
        let volume = node_argument(arguments, "volume", ACTION)?;
        let sets = int_argument(arguments, "sets", 0);
        if sets & !ALL_OVERRIDES != 0 || sets < 0 {
            return Err(refuse(
                ACTION,
                "sets names a parameter a GI volume does not have",
            ));
        }
        let quality = keyword(arguments, "quality", &QUALITIES, "medium", ACTION)?;
        let tier = keyword(arguments, "tier", &TIERS, "automatic", ACTION)?;
        let blend = float_argument(arguments, "blend_distance", 1.0);
        let weight = float_argument(arguments, "weight", 1.0);
        let positives = ["probe_density", "update_rate", "far_field"]
            .map(|name| float_argument(arguments, name, 1.0));
        let intensities = ["diffuse_intensity", "specular_intensity"]
            .map(|name| float_argument(arguments, name, 1.0));
        if !finite_non_negative(blend)
            || !(0.0..=1.0).contains(&weight)
            || !positives.iter().all(|value| finite_positive(*value))
            || !intensities.iter().all(|value| finite_non_negative(*value))
        {
            return Err(refuse(
                ACTION,
                "a blend distance or intensity is negative, a weight outside 0 to 1, or a density, \
                 rate or far field not positive",
            ));
        }
        let id = active(context, ACTION)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let is_volume = document
            .schema()
            .type_named(VOLUME)
            .is_some_and(|found| document.content().has_component(volume, found.id));
        if !is_volume {
            return Err(refuse(ACTION, "the node is not a GI volume")
                .with_remedy("create one with gi.volume.create"));
        }
        let fields = Fields::declare(document.schema_mut(), VOLUME, &VOLUME_FIELDS);
        let values = vec![
            (
                fields.id("priority"),
                Value::Int(int_argument(arguments, "priority", 0)),
            ),
            (fields.id("blend_distance"), Value::Float(blend)),
            (fields.id("weight"), Value::Float(weight)),
            (fields.id("sets"), Value::Int(sets)),
            (fields.id("quality"), Value::Text(quality)),
            (fields.id("tier"), Value::Text(tier)),
            (fields.id("probe_density"), Value::Float(positives[0])),
            (fields.id("update_rate"), Value::Float(positives[1])),
            (fields.id("far_field"), Value::Float(positives[2])),
            (fields.id("diffuse_intensity"), Value::Float(intensities[0])),
            (
                fields.id("specular_intensity"),
                Value::Float(intensities[1]),
            ),
        ];
        document.with_transaction("Edit GI Volume", actor, |document| {
            for (field, value) in values {
                document.set_field(volume, fields.component, field, value)?;
            }
            Ok(())
        })?;
        Ok(Outcome::new("Edited GI volume").with("sets", Value::Int(sets)))
    })
}

fn set_class() -> Command {
    let metadata = Metadata::new(
        "gi.object.set-class",
        "Set Illumination Class",
        CATEGORY,
        "Sets how a placed object takes part in GI, as one undoable transaction: static (in the \
         caches, every move invalidates), slow-dynamic (in the caches, moves coalesced), \
         fast-dynamic (a proxy that occludes and reflects), transient (ignored), or automatic \
         (derived from its mobility and size, as a game's is).",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "entity",
        ValueKind::Text,
        "Stable identity of the object's node.",
    ))
    .with(ParameterSpec::required(
        "class",
        ValueKind::Text,
        "automatic, static, slow-dynamic, fast-dynamic or transient.",
    ));
    Command::new(metadata, |context, arguments| {
        const ACTION: &str = "set an illumination class";
        let node = node_argument(arguments, "entity", ACTION)?;
        let class = keyword(arguments, "class", &CLASSES, "automatic", ACTION)?;
        let id = active(context, ACTION)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let placed = TransformBinding::of_schema(document.schema())
            .is_some_and(|binding| document.content().has_component(node, binding.component));
        if !placed {
            return Err(refuse(ACTION, "the node is not a placed object")
                .with_remedy("select an object with a transform"));
        }
        let fields = Fields::declare(document.schema_mut(), OBJECT, &OBJECT_FIELDS);
        let value = Value::Text(class.clone());
        document.with_transaction(
            format!("Set Illumination Class to {class}"),
            actor,
            |document| {
                if document.content().has_component(node, fields.component) {
                    document.set_field(node, fields.component, fields.id("class"), value)
                } else {
                    document.add_component(
                        node,
                        fields.component,
                        vec![(fields.id("class"), value)],
                    )
                }
            },
        )?;
        Ok(Outcome::new(format!("Set illumination class to {class}"))
            .with("class", Value::Text(class)))
    })
}

fn set_emission() -> Command {
    let metadata = Metadata::new(
        "gi.emission.set",
        "Set Emitter Rule",
        CATEGORY,
        "Sets what the GI makes of every emitter drawn with a material, as one undoable \
         transaction: automatic (the engine classifies it by radiant power and area), cache-only \
         (never a light), promote (a light whatever its size) or off. gi.status lists each \
         emitter with the class it ended up in.",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "material",
        ValueKind::Text,
        "The drawn material's reference, as gi.status names it; default for the grey.",
    ))
    .with(ParameterSpec::required(
        "mode",
        ValueKind::Text,
        "automatic, cache-only, promote or off.",
    ));
    Command::new(metadata, |context, arguments| {
        const ACTION: &str = "set an emitter rule";
        let material = arguments.text("material").unwrap_or_default().to_owned();
        if material.is_empty() {
            return Err(refuse(ACTION, "name the material the rule is for"));
        }
        let mode = keyword(arguments, "mode", &EMISSION_MODES, "automatic", ACTION)?;
        let id = active(context, ACTION)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let existing = IlluminationScene::read(document)
            .emission
            .into_iter()
            .find(|rule| rule.material == material)
            .map(|rule| rule.id);
        let fields = Fields::declare(document.schema_mut(), EMISSION, &EMISSION_FIELDS);
        let mode_value = Value::Text(mode.clone());
        document.with_transaction(format!("Set Emitter Rule to {mode}"), actor, |document| {
            if let Some(node) = existing {
                document.set_field(node, fields.component, fields.id("mode"), mode_value)
            } else {
                let node = document.create_node(None)?;
                document.set_name(node, format!("Emission: {material}"))?;
                document.add_component(
                    node,
                    fields.component,
                    vec![
                        (fields.id("material"), Value::Text(material.clone())),
                        (fields.id("mode"), mode_value),
                    ],
                )
            }
        })?;
        Ok(Outcome::new(format!("Emitters of {material}: {mode}")).with("mode", Value::Text(mode)))
    })
}

fn report() -> Command {
    Command::new(
        Metadata::new(
            "gi.report",
            "Refresh GI Report",
            CATEGORY,
            "Asks the engine for its GI report of the open world: settings, tier, convergence, \
             cost per stage, rays, cache hit rates, every emitter's class and every object's \
             class — and, given a pixel of a published frame, what lit the surface there and why \
             it is the colour it is. Read gi.status once it answers.",
            EffectClass::Read,
        )
        .with(ParameterSpec::optional(
            "viewport",
            ValueKind::Int,
            "Viewport the frame came from.",
            Value::Int(0),
        ))
        .with(ParameterSpec::optional(
            "frame",
            ValueKind::Int,
            "The published frame the pixel belongs to; 0 asks for no point.",
            Value::Int(0),
        ))
        .with(ParameterSpec::optional(
            "x",
            ValueKind::Float,
            "Pixel column in the frame.",
            Value::Float(0.0),
        ))
        .with(ParameterSpec::optional(
            "y",
            ValueKind::Float,
            "Pixel row in the frame.",
            Value::Float(0.0),
        )),
        |context, arguments| {
            const ACTION: &str = "ask for the GI report";
            let frame = u64::try_from(int_argument(arguments, "frame", 0))
                .map_err(|_| refuse(ACTION, "the frame identity is negative"))?;
            let viewport = u32::try_from(int_argument(arguments, "viewport", 0))
                .map_err(|_| refuse(ACTION, "the viewport identity is negative"))?;
            let pick = (frame != 0).then(|| crate::illumination_requests::Pick {
                viewport,
                frame,
                x: float_argument(arguments, "x", 0.0),
                y: float_argument(arguments, "y", 0.0),
            });
            let host = context
                .project()
                .ok_or_else(|| refuse(ACTION, "no project is open"))?;
            let request =
                host.illumination_request(crate::illumination_requests::report_payload(pick))?;
            Ok(Outcome::new("Asked the engine for its GI report")
                .with("request", Value::Text(request.to_string())))
        },
    )
}

fn status() -> Command {
    Command::new(
        Metadata::new(
            "gi.status",
            "GI Status",
            CATEGORY,
            "Reports the engine's last GI report: settings, tier, convergence, cost per stage, \
             rays and hit rates, every emitter (material, class, rule, power, area) and every \
             object's class, and the explained point when one was asked for.",
            EffectClass::Read,
        ),
        |context, _| {
            let host = context
                .project()
                .ok_or_else(|| refuse("read the GI report", "no project is open"))?;
            Ok(host.illumination_status())
        },
    )
}

#[cfg(test)]
#[allow(
    clippy::float_cmp,
    reason = "the values compared are the exact literals the commands were given"
)]
mod tests {
    use cy_editor_commands::Scope;

    use super::*;
    use crate::editor::Editor;

    fn setup() -> (Editor, Registry) {
        let mut editor = Editor::default();
        editor.open_document("worlds/gi.cyworld").unwrap();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        (editor, registry)
    }

    fn try_invoke(
        editor: &mut Editor,
        registry: &Registry,
        id: &str,
        arguments: &Arguments,
    ) -> Result<Outcome> {
        editor.invoke(registry, id, &Scope::unrestricted(), arguments)
    }

    fn invoke(
        editor: &mut Editor,
        registry: &Registry,
        id: &str,
        arguments: &Arguments,
    ) -> Outcome {
        try_invoke(editor, registry, id, arguments)
            .unwrap_or_else(|problem| panic!("{id}: {problem}"))
    }

    fn scene(editor: &Editor) -> IlluminationScene {
        IlluminationScene::read(
            editor
                .documents
                .get(editor.workspace.active().unwrap())
                .unwrap(),
        )
    }

    fn history(editor: &Editor) -> usize {
        editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap()
            .history()
            .entries()
            .len()
    }

    fn undo(editor: &mut Editor, registry: &Registry) {
        invoke(editor, registry, "edit.undo", &Arguments::new());
    }

    #[test]
    fn every_gi_command_is_undoable_and_reaches_agents() {
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        for id in [
            "gi.settings.set",
            "gi.volume.create",
            "gi.volume.set",
            "gi.object.set-class",
            "gi.emission.set",
        ] {
            let metadata = registry.metadata(id).unwrap();
            assert_eq!(metadata.effect, EffectClass::ReversibleMutation, "{id}");
        }
        for id in ["gi.report", "gi.status"] {
            assert_eq!(
                registry.metadata(id).unwrap().effect,
                EffectClass::Read,
                "{id}"
            );
        }
    }

    #[test]
    fn the_worlds_settings_are_made_once_set_in_place_and_undone() {
        let (mut editor, registry) = setup();
        assert_eq!(scene(&editor).settings, Settings::default());
        assert!(scene(&editor).settings_node.is_none());
        let before = history(&editor);
        invoke(
            &mut editor,
            &registry,
            "gi.settings.set",
            &Arguments::new()
                .with("quality", Value::Text("high".into()))
                .with("tier", Value::Text("software-only".into()))
                .with("hardware_ray_tracing", Value::Bool(false))
                .with("budget_ms", Value::Float(2.5)),
        );
        assert_eq!(history(&editor), before + 1);
        let first = scene(&editor);
        let node = first.settings_node.unwrap();
        assert_eq!(first.settings.quality, "high");
        assert_eq!(first.settings.tier, "software-only");
        assert!(!first.settings.hardware_ray_tracing);
        assert_eq!(first.settings.budget_ms, 2.5);

        invoke(
            &mut editor,
            &registry,
            "gi.settings.set",
            &Arguments::new().with("mode", Value::Text("hybrid".into())),
        );
        let second = scene(&editor);
        assert_eq!(
            second.settings_node,
            Some(node),
            "one settings node per world"
        );
        assert_eq!(second.settings.mode, "hybrid");
        assert_eq!(second.settings.quality, "medium");

        undo(&mut editor, &registry);
        assert_eq!(scene(&editor).settings.quality, "high");
        undo(&mut editor, &registry);
        assert!(scene(&editor).settings_node.is_none());

        // A refused value records nothing.
        let undone = history(&editor);
        for (name, value) in [
            ("quality", Value::Text("ultra".into())),
            ("mode", Value::Text("lumen".into())),
            ("budget_ms", Value::Float(0.0)),
            ("budget_ms", Value::Float(f32::NAN)),
        ] {
            assert!(
                try_invoke(
                    &mut editor,
                    &registry,
                    "gi.settings.set",
                    &Arguments::new().with(name, value)
                )
                .is_err(),
                "{name}"
            );
        }
        assert_eq!(history(&editor), undone);
        assert!(scene(&editor).settings_node.is_none());
    }

    #[test]
    fn a_volume_is_placed_and_sized_by_its_transform_and_its_overrides_set_and_undone() {
        let (mut editor, registry) = setup();
        let before = history(&editor);
        let volume = invoke(
            &mut editor,
            &registry,
            "gi.volume.create",
            &Arguments::new()
                .with("at", Value::Vec3([1.5, 0.5, 0.0]))
                .with("size", Value::Vec3([2.0, 2.0, 2.0])),
        )
        .values["volume"]
            .as_text()
            .unwrap()
            .to_string();
        assert_eq!(history(&editor), before + 1);
        let placed = scene(&editor).volumes;
        assert_eq!(placed.len(), 1);
        assert_eq!(placed[0].centre, [1.5, 0.5, 0.0]);
        assert_eq!(placed[0].size, [2.0, 2.0, 2.0]);
        assert_eq!(placed[0].sets, 0, "a new volume overrides nothing");
        assert_eq!(editor.selection.get().nodes().next(), Some(placed[0].id));

        let sets = Override::Quality.bit() | Override::DiffuseIntensity.bit();
        invoke(
            &mut editor,
            &registry,
            "gi.volume.set",
            &Arguments::new()
                .with("volume", Value::Text(volume.clone()))
                .with("priority", Value::Int(2))
                .with("blend_distance", Value::Float(0.5))
                .with("sets", Value::Int(sets))
                .with("quality", Value::Text("epic".into()))
                .with("diffuse_intensity", Value::Float(0.5)),
        );
        assert_eq!(history(&editor), before + 2);
        let edited = &scene(&editor).volumes[0];
        assert!(edited.overrides(Override::Quality));
        assert!(edited.overrides(Override::DiffuseIntensity));
        assert!(!edited.overrides(Override::Tier));
        assert_eq!((edited.priority, edited.blend_distance), (2, 0.5));
        assert_eq!(
            (edited.quality.as_str(), edited.diffuse_intensity),
            ("epic", 0.5)
        );

        for arguments in [
            Arguments::new().with("sets", Value::Int(128)),
            Arguments::new().with("weight", Value::Float(1.5)),
            Arguments::new().with("probe_density", Value::Float(0.0)),
            Arguments::new().with("diffuse_intensity", Value::Float(-1.0)),
            Arguments::new().with("tier", Value::Text("gpu".into())),
        ] {
            assert!(
                try_invoke(
                    &mut editor,
                    &registry,
                    "gi.volume.set",
                    &arguments.with("volume", Value::Text(volume.clone()))
                )
                .is_err()
            );
        }
        assert!(
            try_invoke(
                &mut editor,
                &registry,
                "gi.volume.create",
                &Arguments::new().with("size", Value::Vec3([1.0, 0.0, 1.0]))
            )
            .is_err()
        );
        assert_eq!(history(&editor), before + 2);

        undo(&mut editor, &registry);
        assert_eq!(scene(&editor).volumes[0].sets, 0);
        undo(&mut editor, &registry);
        assert!(scene(&editor).volumes.is_empty());
    }

    #[test]
    fn an_objects_class_and_a_materials_emitter_rule_are_set_and_undone() {
        let (mut editor, registry) = setup();
        // A light is a placed object as any other: what matters is its transform.
        let entity = invoke(
            &mut editor,
            &registry,
            "scene.create-light",
            &Arguments::new().with("kind", Value::Text("point".into())),
        )
        .values["entity"]
            .as_text()
            .unwrap()
            .to_string();
        let node = NodeId::from_u128(u128::from_str_radix(&entity, 16).unwrap());
        assert_eq!(scene(&editor).class_of(node), "automatic");
        let before = history(&editor);
        invoke(
            &mut editor,
            &registry,
            "gi.object.set-class",
            &Arguments::new()
                .with("entity", Value::Text(entity.clone()))
                .with("class", Value::Text("fast-dynamic".into())),
        );
        assert_eq!(scene(&editor).class_of(node), "fast-dynamic");
        invoke(
            &mut editor,
            &registry,
            "gi.object.set-class",
            &Arguments::new()
                .with("entity", Value::Text(entity.clone()))
                .with("class", Value::Text("static".into())),
        );
        assert_eq!(scene(&editor).class_of(node), "static");
        assert!(
            try_invoke(
                &mut editor,
                &registry,
                "gi.object.set-class",
                &Arguments::new()
                    .with("entity", Value::Text(entity.clone()))
                    .with("class", Value::Text("moving".into()))
            )
            .is_err()
        );

        invoke(
            &mut editor,
            &registry,
            "gi.emission.set",
            &Arguments::new()
                .with("material", Value::Text("materials/lamp-glass".into()))
                .with("mode", Value::Text("off".into())),
        );
        invoke(
            &mut editor,
            &registry,
            "gi.emission.set",
            &Arguments::new()
                .with("material", Value::Text("materials/lamp-glass".into()))
                .with("mode", Value::Text("promote".into())),
        );
        let rules = scene(&editor).emission;
        assert_eq!(rules.len(), 1, "one rule node per material");
        assert_eq!(
            scene(&editor).emission_of("materials/lamp-glass"),
            "promote"
        );
        assert_eq!(scene(&editor).emission_of("materials/stone"), "automatic");
        assert!(
            try_invoke(
                &mut editor,
                &registry,
                "gi.emission.set",
                &Arguments::new()
                    .with("material", Value::Text("x".into()))
                    .with("mode", Value::Text("dim".into()))
            )
            .is_err()
        );
        assert_eq!(history(&editor), before + 4);

        undo(&mut editor, &registry);
        assert_eq!(scene(&editor).emission_of("materials/lamp-glass"), "off");
        undo(&mut editor, &registry);
        assert!(scene(&editor).emission.is_empty());
        undo(&mut editor, &registry);
        assert_eq!(scene(&editor).class_of(node), "fast-dynamic");
        undo(&mut editor, &registry);
        assert_eq!(scene(&editor).class_of(node), "automatic");
    }

    #[test]
    fn the_report_needs_a_runtime_and_says_so() {
        let (mut editor, registry) = setup();
        let refused = try_invoke(&mut editor, &registry, "gi.report", &Arguments::new());
        assert!(refused.is_err());
        let status = invoke(&mut editor, &registry, "gi.status", &Arguments::new());
        assert_eq!(status.values["pending"], Value::Bool(false));
    }
}
