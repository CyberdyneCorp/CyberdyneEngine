// SPDX-License-Identifier: MIT
//! Lighting authoring: irradiance volumes, per-object lightmap resolution and light mobility — the
//! same commands for the Lighting panel, the Inspector, scripts and agents.
//!
//! Everything an author decides is a document edit in one transaction, so undo reverses it:
//!
//! | Command | Writes |
//! |---|---|
//! | `lighting.volume.create` | a node with a `Transform` and an `IrradianceVolume` (spacing, counts, rays) |
//! | `lighting.volume.set` | the volume's grid, in one transaction |
//! | `lighting.object.set-resolution` | `LightmapObject` (resolution scale, receives) on a placed object |
//! | `lighting.light.set-mobility` | `LightBakeMobility` on a `LightSource` |
//!
//! The three components are **authoring-only**: the runtime world never sees them. What the bake
//! needs of them reaches it through the level's `.cylightmap` description, which
//! [`crate::lightmap_description`] writes from this module's [`LightingScene`] on every bake
//! request, and which `lighting.bake-lightmaps` ([`crate::lightmaps`]) hands to `cy_build
//! lightmap`. The volume's placement is its `Transform`, so the ordinary move gizmo places it.

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

/// Component names are persisted: they are the join keys between a saved world and this module.
const VOLUME: &str = "IrradianceVolume";
const OBJECT: &str = "LightmapObject";
const MOBILITY: &str = "LightBakeMobility";
/// The light component `scene.create-light` writes.
pub const LIGHT: &str = "LightSource";

/// The resolution scale range the engine accepts.
const MAX_RESOLUTION_SCALE: f32 = 64.0;
/// Probes one volume may have, and capture rays per probe: the editor's bounds, inside the
/// engine's.
const MAX_PROBES: i64 = 4096;
const MAX_RAYS: i64 = 1024;

/// Register the lighting authoring commands.
///
/// # Errors
///
/// When the registry refuses a command's metadata.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(create_volume())?;
    registry.register(set_volume())?;
    registry.register(set_resolution())?;
    registry.register(set_mobility())?;
    Ok(())
}

// --- Mobility ------------------------------------------------------------------------------------

/// What a light's placement and intensity may do at runtime, and so what of it the bake may keep.
/// The engine's `gi::LightMobility`, and the word a `.cylightmap` light line carries.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Mobility {
    /// Fixed: its direct light and its bounce are baked.
    Static,
    /// Fixed position, runtime intensity: its bounce and a shadow-mask channel are baked.
    #[default]
    Stationary,
    /// Anything may change: nothing of it is baked.
    Movable,
}

impl Mobility {
    /// Every mobility, in the engine's order.
    pub const ALL: [Mobility; 3] = [Mobility::Static, Mobility::Stationary, Mobility::Movable];

    /// The keyword commands, documents and the description carry.
    #[must_use]
    pub const fn keyword(self) -> &'static str {
        match self {
            Mobility::Static => "static",
            Mobility::Stationary => "stationary",
            Mobility::Movable => "movable",
        }
    }

    /// What a person reads in the Inspector.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Mobility::Static => "Static",
            Mobility::Stationary => "Stationary",
            Mobility::Movable => "Movable",
        }
    }

    /// The mobility a keyword names.
    ///
    /// # Errors
    ///
    /// Anything but `static`, `stationary` or `movable`, naming the three.
    pub fn parse(keyword: &str) -> Result<Self> {
        Self::ALL
            .into_iter()
            .find(|mobility| mobility.keyword() == keyword)
            .ok_or_else(|| {
                Problem::new(
                    "set a light's mobility",
                    format!("{keyword:?} is not a mobility"),
                )
                .with_remedy("use static, stationary, or movable")
            })
    }
}

// --- The schema ----------------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct VolumeFields {
    component: TypeId,
    spacing: FieldId,
    count_x: FieldId,
    count_y: FieldId,
    count_z: FieldId,
    rays: FieldId,
}

#[derive(Clone, Copy)]
struct ObjectFields {
    component: TypeId,
    scale: FieldId,
    receives: FieldId,
}

#[derive(Clone, Copy)]
struct MobilityFields {
    component: TypeId,
    mobility: FieldId,
}

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

fn found_field(schema: &DocumentSchema, component: &str, name: &str) -> Option<(TypeId, FieldId)> {
    let definition = schema.type_named(component)?;
    Some((definition.id, definition.field_named(name)?.id))
}

impl VolumeFields {
    fn declare(schema: &mut DocumentSchema) -> Self {
        let component = component(schema, VOLUME);
        let count = |schema: &mut DocumentSchema, axis: &str| {
            field(
                schema,
                component,
                &format!("count_{axis}"),
                ValueKind::Int,
                "Probes along one axis of the volume's grid.",
            )
        };
        Self {
            component,
            spacing: field(
                schema,
                component,
                "spacing",
                ValueKind::Float,
                "Metres between neighbouring probes.",
            ),
            count_x: count(schema, "x"),
            count_y: count(schema, "y"),
            count_z: count(schema, "z"),
            rays: field(
                schema,
                component,
                "rays",
                ValueKind::Int,
                "Capture rays per probe.",
            ),
        }
    }

    fn find(schema: &DocumentSchema) -> Option<Self> {
        let (component, spacing) = found_field(schema, VOLUME, "spacing")?;
        Some(Self {
            component,
            spacing,
            count_x: found_field(schema, VOLUME, "count_x")?.1,
            count_y: found_field(schema, VOLUME, "count_y")?.1,
            count_z: found_field(schema, VOLUME, "count_z")?.1,
            rays: found_field(schema, VOLUME, "rays")?.1,
        })
    }
}

impl ObjectFields {
    fn declare(schema: &mut DocumentSchema) -> Self {
        let component = component(schema, OBJECT);
        Self {
            component,
            scale: field(
                schema,
                component,
                "resolution_scale",
                ValueKind::Float,
                "Lightmap resolution over the level's texel density.",
            ),
            receives: field(
                schema,
                component,
                "receives",
                ValueKind::Bool,
                "Whether the object owns a lightmap rectangle.",
            ),
        }
    }

    fn find(schema: &DocumentSchema) -> Option<Self> {
        let (component, scale) = found_field(schema, OBJECT, "resolution_scale")?;
        Some(Self {
            component,
            scale,
            receives: found_field(schema, OBJECT, "receives")?.1,
        })
    }
}

impl MobilityFields {
    fn declare(schema: &mut DocumentSchema) -> Self {
        let component = component(schema, MOBILITY);
        Self {
            component,
            mobility: field(
                schema,
                component,
                "mobility",
                ValueKind::Text,
                "static, stationary, or movable: what the lightmap bake keeps of the light.",
            ),
        }
    }

    fn find(schema: &DocumentSchema) -> Option<Self> {
        let (component, mobility) = found_field(schema, MOBILITY, "mobility")?;
        Some(Self {
            component,
            mobility,
        })
    }
}

// --- The read model ------------------------------------------------------------------------------

/// One placed irradiance volume.
#[derive(Clone, PartialEq, Debug)]
pub struct Volume {
    /// Its node.
    pub id: NodeId,
    /// Its name.
    pub name: String,
    /// Where probe (0, 0, 0) is: the node's translation.
    pub origin: [f32; 3],
    /// Metres between probes.
    pub spacing: f32,
    /// Probes along x, y and z.
    pub counts: [u32; 3],
    /// Capture rays per probe.
    pub rays: u32,
}

impl Volume {
    /// Every probe's position, x fastest, then y, then z — the engine's order.
    #[must_use]
    pub fn probe_positions(&self) -> Vec<[f32; 3]> {
        let mut out = Vec::new();
        for z in 0..self.counts[2] {
            for y in 0..self.counts[1] {
                for x in 0..self.counts[0] {
                    #[allow(clippy::cast_precision_loss, reason = "at most 4096 probes")]
                    let step = |index: u32| index as f32 * self.spacing;
                    out.push([
                        self.origin[0] + step(x),
                        self.origin[1] + step(y),
                        self.origin[2] + step(z),
                    ]);
                }
            }
        }
        out
    }
}

/// One light and the mobility its bake uses.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Light {
    /// Its node.
    pub id: NodeId,
    /// Its name.
    pub name: String,
    /// Stationary unless the author set otherwise: the engine's default.
    pub mobility: Mobility,
}

/// One object with authored lightmap settings.
#[derive(Clone, PartialEq, Debug)]
pub struct LightmappedObject {
    /// Its node.
    pub id: NodeId,
    /// Its name.
    pub name: String,
    /// Lightmap resolution over the level's density.
    pub resolution_scale: f32,
    /// Whether it owns a lightmap.
    pub receives: bool,
}

/// What a world holds for lighting, read from its document. Presentation, never a second store.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct LightingScene {
    /// Every irradiance volume.
    pub volumes: Vec<Volume>,
    /// Every light.
    pub lights: Vec<Light>,
    /// Every object whose lightmap settings were authored.
    pub objects: Vec<LightmappedObject>,
}

impl LightingScene {
    /// Read the document.
    #[must_use]
    pub fn read(document: &Document) -> Self {
        let content = document.content();
        let schema = document.schema();
        let name = |node: NodeId| {
            content
                .node(node)
                .map(|state| state.name.clone())
                .unwrap_or_default()
        };
        let mut scene = Self::default();
        let transform = TransformBinding::of_schema(schema);
        if let Some(volume) = VolumeFields::find(schema) {
            for node in content.nodes() {
                if !content.has_component(node, volume.component) {
                    continue;
                }
                let int = |field: FieldId| match content.field(node, volume.component, field) {
                    Some(Value::Int(value)) => u32::try_from(*value).unwrap_or(0),
                    _ => 0,
                };
                let origin = transform
                    .and_then(|binding| {
                        content
                            .field(node, binding.component, binding.translation)
                            .and_then(Value::as_vec3)
                    })
                    .unwrap_or([0.0; 3]);
                scene.volumes.push(Volume {
                    id: node,
                    name: name(node),
                    origin,
                    spacing: content
                        .field(node, volume.component, volume.spacing)
                        .and_then(Value::as_float)
                        .unwrap_or(1.0),
                    counts: [
                        int(volume.count_x),
                        int(volume.count_y),
                        int(volume.count_z),
                    ],
                    rays: int(volume.rays),
                });
            }
        }
        if let Some(light) = schema.type_named(LIGHT).map(|definition| definition.id) {
            let mobility = MobilityFields::find(schema);
            for node in content.nodes() {
                if !content.has_component(node, light) {
                    continue;
                }
                let mobility = mobility
                    .and_then(|fields| content.field(node, fields.component, fields.mobility))
                    .and_then(Value::as_text)
                    .and_then(|keyword| Mobility::parse(keyword).ok())
                    .unwrap_or_default();
                scene.lights.push(Light {
                    id: node,
                    name: name(node),
                    mobility,
                });
            }
        }
        if let Some(object) = ObjectFields::find(schema) {
            for node in content.nodes() {
                if !content.has_component(node, object.component) {
                    continue;
                }
                scene.objects.push(LightmappedObject {
                    id: node,
                    name: name(node),
                    resolution_scale: content
                        .field(node, object.component, object.scale)
                        .and_then(Value::as_float)
                        .unwrap_or(1.0),
                    receives: !matches!(
                        content.field(node, object.component, object.receives),
                        Some(Value::Bool(false))
                    ),
                });
            }
        }
        scene
    }

    /// The mobility a light's bake uses: stationary unless authored.
    #[must_use]
    pub fn mobility_of(&self, node: NodeId) -> Option<Mobility> {
        self.lights
            .iter()
            .find(|light| light.id == node)
            .map(|light| light.mobility)
    }

    /// The resolution scale an object's bake uses: one unless authored.
    #[must_use]
    pub fn resolution_of(&self, node: NodeId) -> f32 {
        self.objects
            .iter()
            .find(|object| object.id == node)
            .map_or(1.0, |object| object.resolution_scale)
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

fn int_argument(arguments: &Arguments, name: &str) -> i64 {
    arguments
        .get(name)
        .and_then(Value::as_int)
        .unwrap_or_default()
}

fn float_argument(arguments: &Arguments, name: &str) -> f32 {
    arguments
        .get(name)
        .and_then(Value::as_float)
        .unwrap_or_default()
}

struct Grid {
    spacing: f32,
    counts: [i64; 3],
    rays: i64,
}

impl Grid {
    fn from(arguments: &Arguments, action: &str) -> Result<Self> {
        let grid = Self {
            spacing: float_argument(arguments, "spacing"),
            counts: ["count_x", "count_y", "count_z"].map(|name| int_argument(arguments, name)),
            rays: int_argument(arguments, "rays"),
        };
        if !(grid.spacing > 0.0 && grid.spacing.is_finite()) {
            return Err(refuse(
                action,
                "the probe spacing must be a positive number of metres",
            ));
        }
        let probes = grid.counts.iter().try_fold(1_i64, |total, count| {
            (*count >= 1).then(|| total.saturating_mul(*count))
        });
        if probes.is_none_or(|probes| probes > MAX_PROBES) {
            return Err(refuse(
                action,
                format!(
                    "a volume needs at least one probe per axis and at most {MAX_PROBES} in all"
                ),
            ));
        }
        if !(1..=MAX_RAYS).contains(&grid.rays) {
            return Err(refuse(
                action,
                format!("capture rays per probe must be 1 to {MAX_RAYS}"),
            ));
        }
        Ok(grid)
    }

    fn values(&self, fields: VolumeFields) -> Vec<(FieldId, Value)> {
        vec![
            (fields.spacing, Value::Float(self.spacing)),
            (fields.count_x, Value::Int(self.counts[0])),
            (fields.count_y, Value::Int(self.counts[1])),
            (fields.count_z, Value::Int(self.counts[2])),
            (fields.rays, Value::Int(self.rays)),
        ]
    }
}

fn grid_parameters(metadata: Metadata) -> Metadata {
    let count = |axis: &'static str, name: &'static str| {
        ParameterSpec::optional(
            name,
            ValueKind::Int,
            match axis {
                "x" => "Probes along x, at least one.",
                "y" => "Probes along y, at least one.",
                _ => "Probes along z, at least one.",
            },
            Value::Int(4),
        )
    };
    metadata
        .with(ParameterSpec::optional(
            "spacing",
            ValueKind::Float,
            "Metres between neighbouring probes; positive.",
            Value::Float(1.0),
        ))
        .with(count("x", "count_x"))
        .with(count("y", "count_y"))
        .with(count("z", "count_z"))
        .with(ParameterSpec::optional(
            "rays",
            ValueKind::Int,
            "Capture rays per probe, 1 to 1024.",
            Value::Int(64),
        ))
}

// --- Authoring commands --------------------------------------------------------------------------

fn create_volume() -> Command {
    let metadata = Metadata::new(
        "lighting.volume.create",
        "Create Irradiance Volume",
        "Lighting",
        "Places an irradiance volume — a grid of light probes the next lightmap bake captures — at \
         a world position, as one undoable transaction, and selects it. Move it with the transform \
         gizmo; its translation is probe (0, 0, 0).",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::optional(
        "at",
        ValueKind::Vec3,
        "World position of the first probe in metres; the origin when omitted.",
        Value::Vec3([0.0, 0.0, 0.0]),
    ));
    Command::new(grid_parameters(metadata), |context, arguments| {
        const ACTION: &str = "create an irradiance volume";
        let grid = Grid::from(arguments, ACTION)?;
        let at = arguments
            .get("at")
            .and_then(Value::as_vec3)
            .unwrap_or([0.0; 3]);
        let id = active(context, ACTION)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let fields = VolumeFields::declare(document.schema_mut());
        let node = document.with_transaction("Create Irradiance Volume", actor, |document| {
            let node = document.create_node(None)?;
            document.set_name(node, "Irradiance Volume")?;
            crate::scene_actors::place(document, node, at, [0.0, 0.0, 0.0, 1.0])?;
            document.add_component(node, fields.component, grid.values(fields))?;
            Ok(node)
        })?;
        let mut selection = Selection::new();
        selection.add_node(node);
        context.set_selection(selection);
        Ok(Outcome::new("Created irradiance volume").with("volume", Value::Text(node.to_string())))
    })
}

fn set_volume() -> Command {
    let metadata = Metadata::new(
        "lighting.volume.set",
        "Edit Irradiance Volume",
        "Lighting",
        "Replaces an irradiance volume's probe grid — spacing, probes per axis and capture rays — \
         as one undoable transaction. Its position is its transform.",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "volume",
        ValueKind::Text,
        "Stable identity of the irradiance volume's node.",
    ));
    Command::new(grid_parameters(metadata), |context, arguments| {
        const ACTION: &str = "edit an irradiance volume";
        let volume = node_argument(arguments, "volume", ACTION)?;
        let grid = Grid::from(arguments, ACTION)?;
        let id = active(context, ACTION)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let fields = VolumeFields::find(document.schema())
            .filter(|fields| document.content().has_component(volume, fields.component))
            .ok_or_else(|| refuse(ACTION, "the node is not an irradiance volume"))?;
        document.with_transaction("Edit Irradiance Volume", actor, |document| {
            for (field, value) in grid.values(fields) {
                document.set_field(volume, fields.component, field, value)?;
            }
            Ok(())
        })?;
        Ok(Outcome::new("Edited irradiance volume"))
    })
}

fn set_resolution() -> Command {
    Command::new(
        Metadata::new(
            "lighting.object.set-resolution",
            "Set Lightmap Resolution",
            "Lighting",
            "Sets a placed object's lightmap resolution — a scale over the level's texel density — \
             and whether it owns a lightmap at all, as one undoable transaction. The next bake \
             packs it at that density; the Lightmap Density view shows what it was given.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "entity",
            ValueKind::Text,
            "Stable identity of the object's node.",
        ))
        .with(ParameterSpec::required(
            "scale",
            ValueKind::Float,
            "Resolution over the level's texel density, above 0 and at most 64.",
        ))
        .with(ParameterSpec::optional(
            "receives",
            ValueKind::Bool,
            "False: the object still occludes and bounces light but owns no lightmap.",
            Value::Bool(true),
        )),
        |context, arguments| {
            const ACTION: &str = "set a lightmap resolution";
            let node = node_argument(arguments, "entity", ACTION)?;
            let scale = float_argument(arguments, "scale");
            if !(scale > 0.0 && scale <= MAX_RESOLUTION_SCALE) {
                return Err(refuse(
                    ACTION,
                    format!("the scale must be above 0 and at most {MAX_RESOLUTION_SCALE}"),
                ));
            }
            let receives = !matches!(arguments.get("receives"), Some(Value::Bool(false)));
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
            let fields = ObjectFields::declare(document.schema_mut());
            let values = vec![
                (fields.scale, Value::Float(scale)),
                (fields.receives, Value::Bool(receives)),
            ];
            document.with_transaction("Set Lightmap Resolution", actor, |document| {
                if document.content().has_component(node, fields.component) {
                    for (field, value) in values {
                        document.set_field(node, fields.component, field, value)?;
                    }
                    Ok(())
                } else {
                    document.add_component(node, fields.component, values)
                }
            })?;
            Ok(Outcome::new(format!("Set lightmap resolution to {scale}x"))
                .with("scale", Value::Float(scale)))
        },
    )
}

fn set_mobility() -> Command {
    Command::new(
        Metadata::new(
            "lighting.light.set-mobility",
            "Set Light Mobility",
            "Lighting",
            "Sets what the lightmap bake keeps of a light, as one undoable transaction: static \
             bakes its direct light and bounce, stationary its bounce and a shadow mask, movable \
             nothing.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "entity",
            ValueKind::Text,
            "Stable identity of the light's node.",
        ))
        .with(ParameterSpec::required(
            "mobility",
            ValueKind::Text,
            "static, stationary, or movable.",
        )),
        |context, arguments| {
            const ACTION: &str = "set a light's mobility";
            let node = node_argument(arguments, "entity", ACTION)?;
            let mobility = Mobility::parse(arguments.text("mobility").unwrap_or_default())?;
            let id = active(context, ACTION)?;
            let actor = context.actor();
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the world"))?;
            let is_light = document
                .schema()
                .type_named(LIGHT)
                .is_some_and(|light| document.content().has_component(node, light.id));
            if !is_light {
                return Err(refuse(ACTION, "the node is not a light")
                    .with_remedy("select a light created with scene.create-light"));
            }
            let fields = MobilityFields::declare(document.schema_mut());
            let value = Value::Text(mobility.keyword().to_string());
            document.with_transaction(
                format!("Set Light Mobility to {}", mobility.label()),
                actor,
                |document| {
                    if document.content().has_component(node, fields.component) {
                        document.set_field(node, fields.component, fields.mobility, value)
                    } else {
                        document.add_component(
                            node,
                            fields.component,
                            vec![(fields.mobility, value)],
                        )
                    }
                },
            )?;
            Ok(
                Outcome::new(format!("Set light mobility to {}", mobility.keyword()))
                    .with("mobility", Value::Text(mobility.keyword().into())),
            )
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
        editor.open_document("worlds/lighting.cyworld").unwrap();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        (editor, registry)
    }

    fn invoke(
        editor: &mut Editor,
        registry: &Registry,
        id: &str,
        arguments: &Arguments,
    ) -> Outcome {
        editor
            .invoke(registry, id, &Scope::unrestricted(), arguments)
            .unwrap_or_else(|problem| panic!("{id}: {problem}"))
    }

    fn scene(editor: &Editor) -> LightingScene {
        LightingScene::read(
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

    fn light(editor: &mut Editor, registry: &Registry) -> String {
        invoke(
            editor,
            registry,
            "scene.create-light",
            &Arguments::new().with("kind", Value::Text("point".into())),
        )
        .values["entity"]
            .as_text()
            .unwrap()
            .to_string()
    }

    #[test]
    fn a_volume_is_placed_and_edited_in_one_transaction_each_and_undoes() {
        let (mut editor, registry) = setup();
        let before = history(&editor);
        let volume = invoke(
            &mut editor,
            &registry,
            "lighting.volume.create",
            &Arguments::new()
                .with("at", Value::Vec3([1.0, 2.0, 3.0]))
                .with("count_x", Value::Int(3))
                .with("count_y", Value::Int(2))
                .with("count_z", Value::Int(2)),
        )
        .values["volume"]
            .as_text()
            .unwrap()
            .to_string();
        assert_eq!(history(&editor), before + 1);
        let placed = scene(&editor).volumes;
        assert_eq!(placed.len(), 1);
        assert_eq!(placed[0].origin, [1.0, 2.0, 3.0]);
        assert_eq!(placed[0].counts, [3, 2, 2]);
        assert_eq!(placed[0].probe_positions().len(), 12);
        assert_eq!(placed[0].probe_positions()[1], [2.0, 2.0, 3.0]);
        assert_eq!(editor.selection.get().nodes().next(), Some(placed[0].id));

        invoke(
            &mut editor,
            &registry,
            "lighting.volume.set",
            &Arguments::new()
                .with("volume", Value::Text(volume))
                .with("spacing", Value::Float(0.5))
                .with("count_x", Value::Int(4))
                .with("count_y", Value::Int(4))
                .with("count_z", Value::Int(4))
                .with("rays", Value::Int(128)),
        );
        assert_eq!(history(&editor), before + 2);
        let edited = &scene(&editor).volumes[0];
        assert_eq!(
            (edited.spacing, edited.counts, edited.rays),
            (0.5, [4, 4, 4], 128)
        );

        invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
        let undone = &scene(&editor).volumes[0];
        assert_eq!(
            (undone.spacing, undone.counts, undone.rays),
            (1.0, [3, 2, 2], 64)
        );
        invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
        assert!(scene(&editor).volumes.is_empty());
        invoke(&mut editor, &registry, "edit.redo", &Arguments::new());
        assert_eq!(scene(&editor).volumes.len(), 1);
    }

    #[test]
    fn a_grid_the_engine_would_refuse_is_refused_before_it_reaches_the_document() {
        let (mut editor, registry) = setup();
        let before = history(&editor);
        for (name, value) in [
            ("spacing", Value::Float(0.0)),
            ("count_x", Value::Int(0)),
            ("count_y", Value::Int(4097)),
            ("rays", Value::Int(0)),
        ] {
            let problem = editor
                .invoke(
                    &registry,
                    "lighting.volume.create",
                    &Scope::unrestricted(),
                    &Arguments::new().with(name, value),
                )
                .expect_err(name);
            assert!(!problem.to_string().is_empty());
        }
        assert_eq!(history(&editor), before);
    }

    #[test]
    fn mobility_is_set_on_a_light_only_and_undo_restores_the_default() {
        let (mut editor, registry) = setup();
        let lamp = light(&mut editor, &registry);
        let node = NodeId::from_u128(u128::from_str_radix(&lamp, 16).unwrap());
        assert_eq!(scene(&editor).mobility_of(node), Some(Mobility::Stationary));
        for mobility in ["movable", "static"] {
            invoke(
                &mut editor,
                &registry,
                "lighting.light.set-mobility",
                &Arguments::new()
                    .with("entity", Value::Text(lamp.clone()))
                    .with("mobility", Value::Text(mobility.into())),
            );
        }
        assert_eq!(scene(&editor).mobility_of(node), Some(Mobility::Static));
        invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
        assert_eq!(scene(&editor).mobility_of(node), Some(Mobility::Movable));
        invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
        assert_eq!(scene(&editor).mobility_of(node), Some(Mobility::Stationary));

        let volume = invoke(
            &mut editor,
            &registry,
            "lighting.volume.create",
            &Arguments::new(),
        )
        .values["volume"]
            .as_text()
            .unwrap()
            .to_string();
        let problem = editor
            .invoke(
                &registry,
                "lighting.light.set-mobility",
                &Scope::unrestricted(),
                &Arguments::new()
                    .with("entity", Value::Text(volume))
                    .with("mobility", Value::Text("static".into())),
            )
            .unwrap_err();
        assert!(problem.to_string().contains("not a light"), "{problem}");
        let problem = editor
            .invoke(
                &registry,
                "lighting.light.set-mobility",
                &Scope::unrestricted(),
                &Arguments::new()
                    .with("entity", Value::Text(lamp))
                    .with("mobility", Value::Text("fixed".into())),
            )
            .unwrap_err();
        assert!(problem.to_string().contains("mobility"), "{problem}");
    }

    #[test]
    fn resolution_is_authored_per_object_and_undoes() {
        let (mut editor, registry) = setup();
        let lamp = light(&mut editor, &registry);
        // A light is a placed object too, which is enough for the resolution command.
        invoke(
            &mut editor,
            &registry,
            "lighting.object.set-resolution",
            &Arguments::new()
                .with("entity", Value::Text(lamp.clone()))
                .with("scale", Value::Float(2.0)),
        );
        invoke(
            &mut editor,
            &registry,
            "lighting.object.set-resolution",
            &Arguments::new()
                .with("entity", Value::Text(lamp.clone()))
                .with("scale", Value::Float(4.0))
                .with("receives", Value::Bool(false)),
        );
        let node = NodeId::from_u128(u128::from_str_radix(&lamp, 16).unwrap());
        let authored = scene(&editor);
        assert!((authored.resolution_of(node) - 4.0).abs() < f32::EPSILON);
        assert_eq!(authored.objects.len(), 1);
        assert!(!authored.objects[0].receives);

        invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
        assert!((scene(&editor).resolution_of(node) - 2.0).abs() < f32::EPSILON);
        invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
        assert!(scene(&editor).objects.is_empty());

        for scale in [0.0, 65.0] {
            assert!(
                editor
                    .invoke(
                        &registry,
                        "lighting.object.set-resolution",
                        &Scope::unrestricted(),
                        &Arguments::new()
                            .with("entity", Value::Text(lamp.clone()))
                            .with("scale", Value::Float(scale)),
                    )
                    .is_err()
            );
        }
    }

    #[test]
    fn lighting_authoring_survives_a_world_save_and_reload() {
        let (mut editor, registry) = setup();
        let lamp = light(&mut editor, &registry);
        invoke(
            &mut editor,
            &registry,
            "lighting.light.set-mobility",
            &Arguments::new()
                .with("entity", Value::Text(lamp))
                .with("mobility", Value::Text("static".into())),
        );
        invoke(
            &mut editor,
            &registry,
            "lighting.volume.create",
            &Arguments::new()
                .with("at", Value::Vec3([0.5, 1.0, -2.0]))
                .with("rays", Value::Int(32)),
        );
        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        let text = crate::worldfile::write_world(document);
        let mut reopened = Document::new("worlds/lighting.cyworld");
        crate::worldfile::load(&text, &mut reopened, cy_editor_core::Actor::human("test")).unwrap();
        let reloaded = LightingScene::read(&reopened);
        assert_eq!(reloaded.lights[0].mobility, Mobility::Static);
        assert_eq!(reloaded.volumes[0].origin, [0.5, 1.0, -2.0]);
        assert_eq!(reloaded.volumes[0].rays, 32);
        assert!(
            reopened
                .schema()
                .type_named(VOLUME)
                .is_some_and(|definition| definition.authoring_only),
            "the runtime world never receives the authoring components"
        );
    }
}
