//! Terrain authoring through the shared painting surface and ordinary document transactions.
//!
//! A terrain, each material layer, and every sculpt or paint gesture has a stable [`NodeId`]. A
//! completed gesture creates one modifier child in one transaction; enable and order edits are also
//! transactions. The encoded stroke is deliberately retained as authoring data instead of baking a
//! heightmap, which keeps the stack non-destructive and gives Task 5.3 a versioned evaluation input.

use cy_editor_commands::{
    Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::brush::Stroke;
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::operation::Operation;
use cy_editor_documents::schema::DocumentSchema;
use cy_editor_documents::selection::Selection;

/// Component names are persisted and form the editor/compiler join keys.
const TERRAIN: &str = "TerrainAuthoring";
const LAYER: &str = "TerrainMaterialLayer";
const MODIFIER: &str = "TerrainModifier";

/// Register the terrain authoring commands used by UI, scripts, tests, and agents alike.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(create())?;
    registry.register(add_layer())?;
    registry.register(commit_stroke())?;
    registry.register(set_modifier_enabled())?;
    registry.register(move_modifier())?;
    Ok(())
}

#[derive(Clone, Copy)]
struct TerrainBinding {
    component: TypeId,
    source: FieldId,
}

#[derive(Clone, Copy)]
struct LayerBinding {
    component: TypeId,
    material: FieldId,
    order: FieldId,
}

#[derive(Clone, Copy)]
struct ModifierBinding {
    component: TypeId,
    kind: FieldId,
    order: FieldId,
    enabled: FieldId,
    layer: FieldId,
    brush: FieldId,
    stroke: FieldId,
}

#[derive(Clone, Copy)]
struct Bindings {
    terrain: TerrainBinding,
    layer: LayerBinding,
    modifier: ModifierBinding,
}

impl Bindings {
    fn declare(schema: &mut DocumentSchema) -> Self {
        let terrain = component(schema, TERRAIN, false);
        let source = field(
            schema,
            terrain,
            "source",
            ValueKind::Text,
            "Source heightmap asset.",
        );
        let layer = component(schema, LAYER, true);
        let material = field(
            schema,
            layer,
            "material",
            ValueKind::Text,
            "Material asset painted by this layer.",
        );
        let layer_order = field(
            schema,
            layer,
            "order",
            ValueKind::Int,
            "Stable layer evaluation order.",
        );
        let modifier = component(schema, MODIFIER, true);
        Self {
            terrain: TerrainBinding {
                component: terrain,
                source,
            },
            layer: LayerBinding {
                component: layer,
                material,
                order: layer_order,
            },
            modifier: ModifierBinding {
                component: modifier,
                kind: field(
                    schema,
                    modifier,
                    "kind",
                    ValueKind::Text,
                    "Sculpt or material-paint operation.",
                ),
                order: field(
                    schema,
                    modifier,
                    "order",
                    ValueKind::Int,
                    "Stable modifier evaluation order.",
                ),
                enabled: field(
                    schema,
                    modifier,
                    "enabled",
                    ValueKind::Bool,
                    "Whether evaluation includes this modifier.",
                ),
                layer: field(
                    schema,
                    modifier,
                    "layer",
                    ValueKind::Text,
                    "Stable material-layer identity, or empty for sculpting.",
                ),
                brush: field(
                    schema,
                    modifier,
                    "brush",
                    ValueKind::Vec4,
                    "Radius, strength, falloff, and spacing.",
                ),
                stroke: field(
                    schema,
                    modifier,
                    "stroke",
                    ValueKind::Bytes,
                    "Versioned ordered brush samples.",
                ),
            },
        }
    }

    fn find(schema: &DocumentSchema) -> Option<Self> {
        let terrain = schema.type_named(TERRAIN)?;
        let layer = schema.type_named(LAYER)?;
        let modifier = schema.type_named(MODIFIER)?;
        Some(Self {
            terrain: TerrainBinding {
                component: terrain.id,
                source: terrain.field_named("source")?.id,
            },
            layer: LayerBinding {
                component: layer.id,
                material: layer.field_named("material")?.id,
                order: layer.field_named("order")?.id,
            },
            modifier: ModifierBinding {
                component: modifier.id,
                kind: modifier.field_named("kind")?.id,
                order: modifier.field_named("order")?.id,
                enabled: modifier.field_named("enabled")?.id,
                layer: modifier.field_named("layer")?.id,
                brush: modifier.field_named("brush")?.id,
                stroke: modifier.field_named("stroke")?.id,
            },
        })
    }
}

fn component(schema: &mut DocumentSchema, name: &str, authoring_only: bool) -> TypeId {
    if let Some(found) = schema.type_named(name) {
        return found.id;
    }
    schema.declare_type(name, authoring_only)
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
        .expect("component was declared")
}

fn create() -> Command {
    Command::new(
        Metadata::new(
            "terrain.create",
            "Create Terrain",
            "Terrain",
            "Creates a terrain authoring root for an optional heightmap source as one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::optional("source", ValueKind::Text, "Project-relative heightmap source asset.", Value::Text(String::new()))),
        |context, arguments| {
            let document_id = active(context)?;
            let actor = context.actor();
            let source = arguments.text("source").unwrap_or_default().trim().to_string();
            let document = context.document_mut(document_id).ok_or_else(|| Problem::not_found("the active document"))?;
            let bindings = Bindings::declare(document.schema_mut());
            let node = document.with_transaction("Create terrain", actor, |document| {
                let node = document.create_node(None)?;
                document.set_name(node, "Terrain")?;
                document.add_component(
                    node,
                    bindings.terrain.component,
                    vec![(bindings.terrain.source, Value::Text(String::new()))],
                )?;
                if !source.is_empty() {
                    document.record(Operation::SetAssetReference {
                        node,
                        component: bindings.terrain.component,
                        field: bindings.terrain.source,
                        before: String::new(),
                        after: source.clone(),
                    })?;
                }
                Ok(node)
            })?;
            let mut selection = Selection::new();
            selection.add_node(node);
            context.set_selection(selection);
            Ok(Outcome::new("Created terrain").with("terrain", Value::Text(node.to_string())))
        },
    )
}

fn add_layer() -> Command {
    Command::new(
        Metadata::new(
            "terrain.layer.add",
            "Add Terrain Material Layer",
            "Terrain",
            "Adds an ordered material layer with stable identity in one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(required_text(
            "terrain",
            "Stable identity of the terrain root.",
        ))
        .with(required_text("name", "Author-facing layer name."))
        .with(required_text(
            "material",
            "Project-relative material asset assigned to the layer.",
        )),
        |context, arguments| {
            let document_id = active(context)?;
            let terrain = node_argument(arguments, "terrain")?;
            let name = non_empty(arguments.text("name"), "name a terrain layer")?;
            let material = non_empty(arguments.text("material"), "assign a terrain material")?;
            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            let bindings = Bindings::declare(document.schema_mut());
            require_terrain(document, terrain, bindings)?;
            let order = count_children_with(document, terrain, bindings.layer.component);
            let layer = document.with_transaction(
                format!("Add terrain layer {name}"),
                actor,
                |document| {
                    let layer = document.create_node(Some(terrain))?;
                    document.set_name(layer, name)?;
                    document.add_component(
                        layer,
                        bindings.layer.component,
                        vec![
                            (bindings.layer.material, Value::Text(String::new())),
                            (bindings.layer.order, Value::Int(index_value(order)?)),
                        ],
                    )?;
                    document.record(Operation::SetAssetReference {
                        node: layer,
                        component: bindings.layer.component,
                        field: bindings.layer.material,
                        before: String::new(),
                        after: material.to_string(),
                    })?;
                    Ok(layer)
                },
            )?;
            Ok(Outcome::new(format!("Added terrain layer {name}"))
                .with("layer", Value::Text(layer.to_string()))
                .with("order", Value::Int(index_value(order)?)))
        },
    )
}

fn commit_stroke() -> Command {
    Command::new(
        Metadata::new(
            "terrain.stroke.commit",
            "Commit Terrain Stroke",
            "Terrain",
            "Records one completed sculpt or material-paint gesture as one non-destructive modifier transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(required_text("terrain", "Stable identity of the terrain root."))
        .with(required_text("tool", "One of raise, lower, smooth, flatten, or paint."))
        .with(ParameterSpec::optional("layer", ValueKind::Text, "Stable material-layer identity required by paint.", Value::Text(String::new())))
        .with(ParameterSpec::required("stroke", ValueKind::Bytes, "Versioned payload produced by the shared painting surface.")),
        |context, arguments| {
            let document_id = active(context)?;
            let terrain = node_argument(arguments, "terrain")?;
            let tool = parse_tool(arguments.text("tool").unwrap_or_default())?;
            let layer_text = arguments.text("layer").unwrap_or_default();
            let bytes = match arguments.get("stroke") { Some(Value::Bytes(bytes)) => bytes.clone(), _ => Vec::new() };
            let stroke = Stroke::decode(&bytes)?;
            let actor = context.actor();
            let document = context.document_mut(document_id).ok_or_else(|| Problem::not_found("the active document"))?;
            let bindings = Bindings::declare(document.schema_mut());
            require_terrain(document, terrain, bindings)?;
            let layer = validate_layer(document, bindings, terrain, tool, layer_text)?;
            let order = count_children_with(document, terrain, bindings.modifier.component);
            let modifier = document.with_transaction(format!("Terrain {} stroke", tool_label(tool)), actor, |document| {
                let modifier = document.create_node(Some(terrain))?;
                document.set_name(modifier, format!("{} stroke", tool_label(tool)))?;
                document.add_component(modifier, bindings.modifier.component, vec![
                    (bindings.modifier.kind, Value::Text(tool.to_string())),
                    (bindings.modifier.order, Value::Int(index_value(order)?)),
                    (bindings.modifier.enabled, Value::Bool(true)),
                    (bindings.modifier.layer, Value::Text(layer.map_or_else(String::new, |id| id.to_string()))),
                    (bindings.modifier.brush, Value::Vec4([stroke.brush.radius, stroke.brush.strength, stroke.brush.falloff, stroke.brush.spacing])),
                    (bindings.modifier.stroke, Value::Bytes(bytes)),
                ])?;
                Ok(modifier)
            })?;
            Ok(Outcome::new(format!("Committed terrain {} stroke", tool_label(tool)))
                .with("modifier", Value::Text(modifier.to_string()))
                .with("samples", Value::Int(index_value(stroke.samples.len())?)))
        },
    )
}

fn set_modifier_enabled() -> Command {
    Command::new(
        Metadata::new(
            "terrain.modifier.set-enabled",
            "Enable Terrain Modifier",
            "Terrain",
            "Enables or disables a non-destructive terrain modifier without deleting its stroke.",
            EffectClass::ReversibleMutation,
        )
        .with(required_text(
            "modifier",
            "Stable identity of the terrain modifier.",
        ))
        .with(ParameterSpec::required(
            "enabled",
            ValueKind::Bool,
            "Whether terrain evaluation includes this modifier.",
        )),
        |context, arguments| {
            let document_id = active(context)?;
            let modifier = node_argument(arguments, "modifier")?;
            let enabled = matches!(arguments.get("enabled"), Some(Value::Bool(true)));
            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            let bindings = Bindings::find(document.schema())
                .ok_or_else(|| Problem::not_found("terrain authoring schema"))?;
            require_component(
                document,
                modifier,
                bindings.modifier.component,
                "terrain modifier",
            )?;
            document.with_transaction(
                if enabled {
                    "Enable terrain modifier"
                } else {
                    "Disable terrain modifier"
                },
                actor,
                |document| {
                    document.set_field(
                        modifier,
                        bindings.modifier.component,
                        bindings.modifier.enabled,
                        Value::Bool(enabled),
                    )
                },
            )?;
            Ok(Outcome::new(if enabled {
                "Enabled terrain modifier"
            } else {
                "Disabled terrain modifier"
            }))
        },
    )
}

fn move_modifier() -> Command {
    Command::new(
        Metadata::new(
            "terrain.modifier.move",
            "Move Terrain Modifier",
            "Terrain",
            "Moves a modifier in its terrain's deterministic evaluation order as one undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(required_text("modifier", "Stable identity of the terrain modifier."))
        .with(ParameterSpec::required("order", ValueKind::Int, "Zero-based destination in the modifier stack.")),
        |context, arguments| {
            let document_id = active(context)?;
            let modifier = node_argument(arguments, "modifier")?;
            let requested = match arguments.get("order") { Some(Value::Int(value)) => *value, _ => 0 };
            let actor = context.actor();
            let document = context.document_mut(document_id).ok_or_else(|| Problem::not_found("the active document"))?;
            let bindings = Bindings::find(document.schema()).ok_or_else(|| Problem::not_found("terrain authoring schema"))?;
            let parent = document.content().node(modifier).and_then(|node| node.parent).ok_or_else(|| Problem::not_found("the modifier's terrain"))?;
            let mut modifiers = modifier_ids(document, parent, bindings);
            let from = modifiers.iter().position(|candidate| *candidate == modifier).ok_or_else(|| Problem::not_found("that terrain modifier"))?;
            let last = modifiers.len().saturating_sub(1);
            let destination = usize::try_from(requested.max(0)).unwrap_or(last).min(last);
            let moved = modifiers.remove(from);
            modifiers.insert(destination, moved);
            document.with_transaction("Move terrain modifier", actor, |document| {
                for (order, node) in modifiers.iter().enumerate() {
                    document.set_field(*node, bindings.modifier.component, bindings.modifier.order, Value::Int(index_value(order)?))?;
                }
                Ok(())
            })?;
            Ok(Outcome::new("Moved terrain modifier").with("order", Value::Int(index_value(destination)?)))
        },
    )
}

fn required_text(name: &'static str, description: &'static str) -> ParameterSpec {
    ParameterSpec::required(name, ValueKind::Text, description)
}

fn active(context: &dyn CommandContext) -> Result<cy_editor_core::ids::DocumentId> {
    context
        .active_document()
        .ok_or_else(|| Problem::new("edit terrain", "no document is open"))
}

fn node_argument(arguments: &cy_editor_commands::Arguments, name: &str) -> Result<NodeId> {
    parse_node(arguments.text(name).unwrap_or_default())?
        .ok_or_else(|| Problem::new("edit terrain", format!("no {name} identity was given")))
}

fn parse_node(text: &str) -> Result<Option<NodeId>> {
    if text.is_empty() {
        return Ok(None);
    }
    u128::from_str_radix(text, 16)
        .map(NodeId::from_u128)
        .map(Some)
        .map_err(|_| {
            Problem::new(
                "read a terrain identity",
                "it is not a 32-digit hexadecimal node identity",
            )
        })
}

fn non_empty<'a>(value: Option<&'a str>, action: &str) -> Result<&'a str> {
    value
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .ok_or_else(|| Problem::new(action, "a non-empty value is required"))
}

fn parse_tool(tool: &str) -> Result<&str> {
    match tool {
        "raise" | "lower" | "smooth" | "flatten" | "paint" => Ok(tool),
        _ => Err(Problem::new(
            "commit a terrain stroke",
            format!("tool {tool:?} is unsupported"),
        )
        .with_remedy("use raise, lower, smooth, flatten, or paint")),
    }
}

fn tool_label(tool: &str) -> &str {
    match tool {
        "raise" => "Raise",
        "lower" => "Lower",
        "smooth" => "Smooth",
        "flatten" => "Flatten",
        "paint" => "Paint",
        _ => tool,
    }
}

fn require_terrain(document: &Document, node: NodeId, bindings: Bindings) -> Result<()> {
    require_component(document, node, bindings.terrain.component, "terrain root")
}

fn require_component(
    document: &Document,
    node: NodeId,
    component: TypeId,
    description: &str,
) -> Result<()> {
    if document.content().has_component(node, component) {
        Ok(())
    } else {
        Err(Problem::new(
            "edit terrain",
            format!("the node is not a {description}"),
        ))
    }
}

fn validate_layer(
    document: &Document,
    bindings: Bindings,
    terrain: NodeId,
    tool: &str,
    text: &str,
) -> Result<Option<NodeId>> {
    if tool != "paint" {
        if text.is_empty() {
            return Ok(None);
        }
        return Err(Problem::new(
            "commit a sculpt stroke",
            "sculpt tools do not take a material layer",
        ));
    }
    let layer = parse_node(text)?
        .ok_or_else(|| Problem::new("commit a paint stroke", "paint requires a material layer"))?;
    require_component(
        document,
        layer,
        bindings.layer.component,
        "terrain material layer",
    )?;
    if document
        .content()
        .node(layer)
        .and_then(|state| state.parent)
        != Some(terrain)
    {
        return Err(Problem::new(
            "commit a paint stroke",
            "the material layer belongs to another terrain",
        )
        .with_remedy("use a layer whose stable identity is a child of this terrain"));
    }
    Ok(Some(layer))
}

fn count_children_with(document: &Document, parent: NodeId, component: TypeId) -> usize {
    document.content().node(parent).map_or(0, |node| {
        node.children
            .iter()
            .filter(|child| document.content().has_component(**child, component))
            .count()
    })
}

fn modifier_ids(document: &Document, parent: NodeId, bindings: Bindings) -> Vec<NodeId> {
    let mut modifiers: Vec<(i64, NodeId)> = document
        .content()
        .node(parent)
        .into_iter()
        .flat_map(|node| node.children.iter().copied())
        .filter_map(|node| {
            let Value::Int(order) = document.content().field(
                node,
                bindings.modifier.component,
                bindings.modifier.order,
            )?
            else {
                return None;
            };
            Some((*order, node))
        })
        .collect();
    modifiers.sort_by_key(|(order, node)| (*order, *node));
    modifiers.into_iter().map(|(_, node)| node).collect()
}

fn index_value(value: usize) -> Result<i64> {
    i64::try_from(value).map_err(|_| {
        Problem::new(
            "record terrain authoring order",
            "the stack exceeds the supported size",
        )
    })
}

/// Read-only terrain state used by the panel without becoming a second source of truth.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct TerrainStack {
    /// Stable ordered material layers.
    pub layers: Vec<TerrainLayer>,
    /// Stable ordered non-destructive modifiers.
    pub modifiers: Vec<TerrainModifier>,
}

/// One persisted material layer.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct TerrainLayer {
    /// Stable node identity used by paint modifiers.
    pub id: NodeId,
    /// Author-facing layer name.
    pub name: String,
    /// Project-relative material asset.
    pub material: String,
}

/// One persisted sculpt or paint modifier.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct TerrainModifier {
    /// Stable node identity used by commands and history.
    pub id: NodeId,
    /// Author-facing modifier name.
    pub name: String,
    /// Stable tool keyword.
    pub kind: String,
    /// Whether evaluation includes this modifier.
    pub enabled: bool,
    /// Number of ordered points retained in the stroke payload.
    pub sample_count: usize,
}

impl TerrainStack {
    /// Project the document's stable authoring nodes into rows for presentation.
    #[must_use]
    pub fn read(document: &Document, terrain: NodeId) -> Option<Self> {
        let bindings = Bindings::find(document.schema())?;
        if !document
            .content()
            .has_component(terrain, bindings.terrain.component)
        {
            return None;
        }
        let children = &document.content().node(terrain)?.children;
        let mut layers = Vec::new();
        let mut modifiers = Vec::new();
        for child in children {
            let state = document.content().node(*child)?;
            if let (Some(Value::Text(material)), Some(Value::Int(order))) = (
                document
                    .content()
                    .field(*child, bindings.layer.component, bindings.layer.material),
                document
                    .content()
                    .field(*child, bindings.layer.component, bindings.layer.order),
            ) {
                layers.push((
                    *order,
                    TerrainLayer {
                        id: *child,
                        name: state.name.clone(),
                        material: material.clone(),
                    },
                ));
            }
            if let (
                Some(Value::Text(kind)),
                Some(Value::Int(order)),
                Some(Value::Bool(enabled)),
                Some(Value::Bytes(bytes)),
            ) = (
                document.content().field(
                    *child,
                    bindings.modifier.component,
                    bindings.modifier.kind,
                ),
                document.content().field(
                    *child,
                    bindings.modifier.component,
                    bindings.modifier.order,
                ),
                document.content().field(
                    *child,
                    bindings.modifier.component,
                    bindings.modifier.enabled,
                ),
                document.content().field(
                    *child,
                    bindings.modifier.component,
                    bindings.modifier.stroke,
                ),
            ) {
                let sample_count = Stroke::decode(bytes).map_or(0, |stroke| stroke.samples.len());
                modifiers.push((
                    *order,
                    TerrainModifier {
                        id: *child,
                        name: state.name.clone(),
                        kind: kind.clone(),
                        enabled: *enabled,
                        sample_count,
                    },
                ));
            }
        }
        layers.sort_by_key(|(order, layer)| (*order, layer.id));
        modifiers.sort_by_key(|(order, modifier)| (*order, modifier.id));
        Some(Self {
            layers: layers.into_iter().map(|(_, layer)| layer).collect(),
            modifiers: modifiers
                .into_iter()
                .map(|(_, modifier)| modifier)
                .collect(),
        })
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_commands::{Arguments, Scope};
    use cy_editor_core::Actor;
    use cy_editor_core::brush::{Brush, Sample, Stroke};

    use super::*;
    use crate::Editor;

    fn setup() -> (Editor, Registry) {
        let mut editor = Editor::new(Actor::human("designer"));
        editor.open_document("worlds/terrain.cyworld").unwrap();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        (editor, registry)
    }

    #[allow(
        clippy::needless_pass_by_value,
        reason = "test calls mirror the registry's owned invocation arguments"
    )]
    fn invoke(registry: &Registry, editor: &mut Editor, id: &str, arguments: Arguments) -> Outcome {
        registry
            .invoke(id, &Scope::unrestricted(), editor, &arguments)
            .unwrap()
    }

    #[test]
    fn one_completed_gesture_is_one_undoable_transaction_with_stable_identity() {
        let (mut editor, registry) = setup();
        let terrain =
            invoke(&registry, &mut editor, "terrain.create", Arguments::new()).values["terrain"]
                .as_text()
                .unwrap()
                .to_string();
        let document_id = editor.workspace.active().unwrap();
        let before = editor
            .documents
            .get(document_id)
            .unwrap()
            .history()
            .entries()
            .len();
        let stroke = Stroke {
            brush: Brush::default(),
            samples: vec![
                Sample::new(0.1, 0.2, 1.0).unwrap(),
                Sample::new(0.8, 0.7, 0.5).unwrap(),
            ],
        };
        let outcome = invoke(
            &registry,
            &mut editor,
            "terrain.stroke.commit",
            Arguments::new()
                .with("terrain", Value::Text(terrain.clone()))
                .with("tool", Value::Text("raise".into()))
                .with("stroke", Value::Bytes(stroke.encode())),
        );
        let modifier = outcome.values["modifier"].as_text().unwrap().to_string();
        let document = editor.documents.get(document_id).unwrap();
        assert_eq!(document.history().entries().len(), before + 1);
        assert!(
            document
                .content()
                .node(parse_node(&modifier).unwrap().unwrap())
                .is_some()
        );
        invoke(&registry, &mut editor, "edit.undo", Arguments::new());
        assert!(
            editor
                .documents
                .get(document_id)
                .unwrap()
                .content()
                .node(parse_node(&modifier).unwrap().unwrap())
                .is_none()
        );
        invoke(&registry, &mut editor, "edit.redo", Arguments::new());
        assert!(
            editor
                .documents
                .get(document_id)
                .unwrap()
                .content()
                .node(parse_node(&modifier).unwrap().unwrap())
                .is_some()
        );
    }

    #[test]
    fn paint_requires_a_stable_material_layer_and_disable_preserves_the_stroke() {
        let (mut editor, registry) = setup();
        let terrain =
            invoke(&registry, &mut editor, "terrain.create", Arguments::new()).values["terrain"]
                .as_text()
                .unwrap()
                .to_string();
        let layer = invoke(
            &registry,
            &mut editor,
            "terrain.layer.add",
            Arguments::new()
                .with("terrain", Value::Text(terrain.clone()))
                .with("name", Value::Text("Rock".into()))
                .with("material", Value::Text("materials/rock.cymat".into())),
        )
        .values["layer"]
            .as_text()
            .unwrap()
            .to_string();
        let stroke = Stroke {
            brush: Brush::default(),
            samples: vec![Sample::new(0.5, 0.5, 1.0).unwrap()],
        };
        let modifier = invoke(
            &registry,
            &mut editor,
            "terrain.stroke.commit",
            Arguments::new()
                .with("terrain", Value::Text(terrain.clone()))
                .with("tool", Value::Text("paint".into()))
                .with("layer", Value::Text(layer))
                .with("stroke", Value::Bytes(stroke.encode())),
        )
        .values["modifier"]
            .as_text()
            .unwrap()
            .to_string();
        invoke(
            &registry,
            &mut editor,
            "terrain.modifier.set-enabled",
            Arguments::new()
                .with("modifier", Value::Text(modifier))
                .with("enabled", Value::Bool(false)),
        );
        let terrain_id = parse_node(&terrain).unwrap().unwrap();
        let stack = TerrainStack::read(
            editor
                .documents
                .get(editor.workspace.active().unwrap())
                .unwrap(),
            terrain_id,
        )
        .unwrap();
        assert_eq!(stack.layers.len(), 1);
        assert_eq!(stack.modifiers.len(), 1);
        assert!(!stack.modifiers[0].enabled);
        assert_eq!(stack.modifiers[0].sample_count, 1);
    }

    #[test]
    fn terrain_layers_and_modifiers_survive_world_save_and_reload() {
        let (mut editor, registry) = setup();
        let terrain = invoke(
            &registry,
            &mut editor,
            "terrain.create",
            Arguments::new().with("source", Value::Text("terrain/island.png".into())),
        )
        .values["terrain"]
            .as_text()
            .unwrap()
            .to_string();
        let layer = invoke(
            &registry,
            &mut editor,
            "terrain.layer.add",
            Arguments::new()
                .with("terrain", Value::Text(terrain.clone()))
                .with("name", Value::Text("Grass".into()))
                .with("material", Value::Text("materials/grass.cymat".into())),
        )
        .values["layer"]
            .as_text()
            .unwrap()
            .to_string();
        let stroke = Stroke {
            brush: Brush::default(),
            samples: vec![Sample::new(0.25, 0.75, 1.0).unwrap()],
        };
        invoke(
            &registry,
            &mut editor,
            "terrain.stroke.commit",
            Arguments::new()
                .with("terrain", Value::Text(terrain.clone()))
                .with("tool", Value::Text("paint".into()))
                .with("layer", Value::Text(layer))
                .with("stroke", Value::Bytes(stroke.encode())),
        );

        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        let world = crate::worldfile::write_world(document);
        let mut reopened = Document::new("worlds/terrain.cyworld");
        crate::worldfile::load(&world, &mut reopened, Actor::system("reload")).unwrap();
        let terrain = reopened.content().roots()[0];
        let stack = TerrainStack::read(&reopened, terrain).expect("terrain authoring reloads");
        assert_eq!(stack.layers[0].material, "materials/grass.cymat");
        assert_eq!(stack.modifiers[0].kind, "paint");
        assert_eq!(stack.modifiers[0].sample_count, 1);
    }
}
