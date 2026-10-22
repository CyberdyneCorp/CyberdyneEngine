// SPDX-License-Identifier: MIT
//! Camera and light creation through the same command path as the scene UI and MCP.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::document::Document;
use cy_editor_documents::schema::DocumentSchema;
use cy_editor_documents::selection::Selection;
use cy_editor_viewport::gizmo::TransformBinding;

/// Register the scene camera and light creation commands.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(create_light())?;
    registry.register(create_camera())?;
    Ok(())
}

fn active(context: &dyn CommandContext) -> Result<cy_editor_core::ids::DocumentId> {
    context.active_document().ok_or_else(|| {
        Problem::new("create a scene object", "no world is open").with_remedy("open a world first")
    })
}

fn field(schema: &mut DocumentSchema, component: TypeId, name: &str, kind: ValueKind) -> FieldId {
    if let Some(id) = schema
        .type_of(component)
        .and_then(|definition| definition.field_named(name))
        .map(|definition| definition.id)
    {
        return id;
    }
    schema
        .declare_field(component, name, kind, format!("The authored {name} value."))
        .expect("the component is declared")
}

fn component(schema: &mut DocumentSchema, name: &str) -> TypeId {
    if let Some(definition) = schema.type_named(name) {
        return definition.id;
    }
    schema.declare_type(name, false)
}

pub(crate) fn place(
    document: &mut Document,
    node: NodeId,
    at: [f32; 3],
    rotation: [f32; 4],
) -> Result<()> {
    let binding = if let Some(binding) = TransformBinding::of_schema(document.schema()) {
        binding
    } else {
        let schema = document.schema_mut();
        let component = component(schema, TransformBinding::COMPONENT);
        for (name, kind) in [
            ("translation", ValueKind::Vec3),
            ("rotation", ValueKind::Quat),
            ("scale", ValueKind::Vec3),
        ] {
            field(schema, component, name, kind);
        }
        TransformBinding::of_schema(schema).expect("the transform fields were declared")
    };
    document.add_component(
        node,
        binding.component,
        vec![
            (binding.translation, Value::Vec3(at)),
            (binding.rotation, Value::Quat(rotation)),
            (binding.scale, Value::Vec3([1.0, 1.0, 1.0])),
        ],
    )
}

fn at(arguments: &Arguments) -> [f32; 3] {
    match arguments.get("at") {
        Some(Value::Vec3(lanes)) => *lanes,
        _ => [0.0, 0.0, 0.0],
    }
}

fn position_argument(metadata: Metadata) -> Metadata {
    metadata.with(ParameterSpec::optional(
        "at",
        ValueKind::Vec3,
        "Initial world position in metres; the origin when omitted.",
        Value::Vec3([0.0, 0.0, 0.0]),
    ))
}

fn add_fields(
    document: &mut Document,
    node: NodeId,
    name: &str,
    values: &[(&str, Value)],
) -> Result<()> {
    let component = component(document.schema_mut(), name);
    let fields = values
        .iter()
        .map(|(name, value)| {
            let id = field(document.schema_mut(), component, name, value.kind());
            (id, value.clone())
        })
        .collect();
    document.add_component(node, component, fields)
}

/// What `scene.create-light` makes of a kind keyword: the engine's `render::LightKind` number, the
/// node's name, its intensity (lux, candela, or nits for an area light) and range, and an area
/// light's size in metres.
struct LightPreset {
    kind: i64,
    label: &'static str,
    intensity: f32,
    range: f32,
    size: Option<[f32; 2]>,
}

/// The kinds the engine draws, in `render::LightKind` order. An area light's range is zero: the
/// engine derives its reach from its brightness and size.
const LIGHT_PRESETS: [(&str, LightPreset); 7] = [
    (
        "directional",
        LightPreset {
            kind: 0,
            label: "Directional Light",
            intensity: 100_000.0,
            range: 0.0,
            size: None,
        },
    ),
    (
        "point",
        LightPreset {
            kind: 1,
            label: "Point Light",
            intensity: 1000.0,
            range: 10.0,
            size: None,
        },
    ),
    (
        "spot",
        LightPreset {
            kind: 2,
            label: "Spot Light",
            intensity: 1000.0,
            range: 10.0,
            size: None,
        },
    ),
    (
        "rect",
        LightPreset {
            kind: 3,
            label: "Rect Light",
            intensity: 2000.0,
            range: 0.0,
            size: Some([1.0, 0.5]),
        },
    ),
    (
        "disc",
        LightPreset {
            kind: 4,
            label: "Disc Light",
            intensity: 2000.0,
            range: 0.0,
            size: Some([0.5, 0.0]),
        },
    ),
    (
        "sphere",
        LightPreset {
            kind: 5,
            label: "Sphere Light",
            intensity: 20_000.0,
            range: 0.0,
            size: Some([0.1, 0.0]),
        },
    ),
    (
        "tube",
        LightPreset {
            kind: 6,
            label: "Tube Light",
            intensity: 5000.0,
            range: 0.0,
            size: Some([1.2, 0.04]),
        },
    ),
];

fn light_preset(keyword: &str) -> Result<&'static LightPreset> {
    LIGHT_PRESETS
        .iter()
        .find(|(name, _)| *name == keyword)
        .map(|(_, preset)| preset)
        .ok_or_else(|| {
            Problem::new("create a light", format!("unknown type {keyword:?}"))
                .with_remedy("use directional, point, spot, rect, disc, sphere, or tube")
        })
}

fn light_fields(preset: &LightPreset) -> Vec<(&'static str, Value)> {
    let mut fields = vec![
        ("kind", Value::Int(preset.kind)),
        ("color.r", Value::Float(1.0)),
        ("color.g", Value::Float(1.0)),
        ("color.b", Value::Float(1.0)),
        ("intensity", Value::Float(preset.intensity)),
        ("range", Value::Float(preset.range)),
        ("inner_cone", Value::Float(0.0)),
        ("outer_cone", Value::Float(std::f32::consts::FRAC_PI_4)),
        ("casts_shadow", Value::Bool(true)),
        ("enabled", Value::Bool(true)),
    ];
    // Only an area light carries a size, so a punctual light's fields are what they always were.
    if let Some([width, height]) = preset.size {
        fields.extend([
            ("source_width", Value::Float(width)),
            ("source_height", Value::Float(height)),
            ("two_sided", Value::Bool(false)),
        ]);
    }
    fields
}

fn create_light() -> Command {
    let metadata = Metadata::new(
        "scene.create-light",
        "Create Light",
        "Scene",
        "Creates a directional, point, spot, or area (rect, disc, sphere, tube) light with an \
         editable transform and lighting fields in the active world. An area light is sized in \
         metres and its intensity is in nits. The creation is one undoable transaction and selects \
         the light.",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "kind",
        ValueKind::Text,
        "The light type: directional, point, spot, rect, disc, sphere, or tube.",
    ));
    Command::new(position_argument(metadata), |context, arguments| {
        let preset = light_preset(arguments.text("kind").unwrap_or_default())?;
        let label = preset.label;
        let id = active(context)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let node = document.with_transaction(format!("Create {label}"), actor, |document| {
            let node = document.create_node(None)?;
            document.set_name(node, label)?;
            place(document, node, at(arguments), [0.0, 0.0, 0.0, 1.0])?;
            add_fields(document, node, "LightSource", &light_fields(preset))?;
            Ok(node)
        })?;
        let mut selection = Selection::new();
        selection.add_node(node);
        context.set_selection(selection);
        Ok(Outcome::new(format!("Created {label}")).with("entity", Value::Text(node.to_string())))
    })
}

fn create_camera() -> Command {
    let metadata = Metadata::new(
        "scene.create-camera",
        "Create Camera",
        "Scene",
        "Creates an enabled scene camera in the active world. The camera is selected and can be \
         moved with the ordinary transform gizmo; saving the world stores its pose and projection.",
        EffectClass::ReversibleMutation,
    );
    Command::new(
        position_argument(metadata).with(ParameterSpec::optional(
            "rotation",
            ValueKind::Quat,
            "Initial camera orientation as a quaternion; identity when omitted.",
            Value::Quat([0.0, 0.0, 0.0, 1.0]),
        )),
        |context, arguments| {
            let id = active(context)?;
            let actor = context.actor();
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the world"))?;
            let node = document.with_transaction("Create Camera", actor, |document| {
                let node = document.create_node(None)?;
                document.set_name(node, "Camera")?;
                let rotation = match arguments.get("rotation") {
                    Some(Value::Quat(lanes)) => *lanes,
                    _ => [0.0, 0.0, 0.0, 1.0],
                };
                place(document, node, at(arguments), rotation)?;
                add_fields(
                    document,
                    node,
                    "Camera",
                    &[
                        ("projection.kind", Value::Int(0)),
                        ("projection.fov_y", Value::Float(0.9)),
                        ("projection.near", Value::Float(0.1)),
                        ("projection.far", Value::Float(10000.0)),
                        ("enabled", Value::Bool(true)),
                    ],
                )?;
                Ok(node)
            })?;
            let mut selection = Selection::new();
            selection.add_node(node);
            context.set_selection(selection);
            Ok(Outcome::new("Created Camera").with("entity", Value::Text(node.to_string())))
        },
    )
}

#[cfg(test)]
mod tests {
    use cy_editor_commands::Scope;

    use super::*;
    use crate::editor::Editor;

    #[test]
    fn lights_and_camera_are_selected_undoable_scene_components() {
        let mut editor = Editor::default();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        editor.open_document("worlds/actors.cyworld").unwrap();
        let scope = Scope::unrestricted();
        for (kind, expected) in [("directional", 0), ("point", 1), ("spot", 2)] {
            editor
                .invoke(
                    &registry,
                    "scene.create-light",
                    &scope,
                    &Arguments::new().with("kind", Value::Text(kind.into())),
                )
                .unwrap();
            let node = editor.selection.get().nodes().next().unwrap();
            let document = editor
                .documents
                .get(editor.workspace.active().unwrap())
                .unwrap();
            let light = document.schema().type_named("LightSource").unwrap();
            let field = light.field_named("kind").unwrap();
            assert_eq!(
                document.content().field(node, light.id, field.id),
                Some(&Value::Int(expected))
            );
        }
        editor
            .invoke(&registry, "scene.create-camera", &scope, &Arguments::new())
            .unwrap();
        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        assert_eq!(document.content().node_count(), 4);
        assert!(document.schema().type_named("Camera").is_some());
        editor
            .invoke(&registry, "edit.undo", &scope, &Arguments::new())
            .unwrap();
        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        assert_eq!(document.content().node_count(), 3);
    }

    #[test]
    fn light_and_camera_fields_survive_world_save_and_reload() {
        let mut editor = Editor::default();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        editor.open_document("worlds/actors.cyworld").unwrap();
        let scope = Scope::unrestricted();
        editor
            .invoke(
                &registry,
                "scene.create-light",
                &scope,
                &Arguments::new()
                    .with("kind", Value::Text("spot".into()))
                    .with("at", Value::Vec3([1.0, 2.0, 3.0])),
            )
            .unwrap();
        editor
            .invoke(&registry, "scene.create-camera", &scope, &Arguments::new())
            .unwrap();
        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        let text = crate::worldfile::write_world(document);
        let mut reopened = Document::new("worlds/actors.cyworld");
        crate::worldfile::load(&text, &mut reopened, cy_editor_core::Actor::human("test")).unwrap();
        let light = reopened.schema().type_named("LightSource").unwrap();
        let kind = light.field_named("kind").unwrap().id;
        let transform = TransformBinding::of_schema(reopened.schema()).unwrap();
        let light_node = reopened
            .content()
            .nodes()
            .find(|node| reopened.content().has_component(*node, light.id))
            .unwrap();
        assert_eq!(
            reopened.content().field(light_node, light.id, kind),
            Some(&Value::Int(2))
        );
        assert_eq!(
            reopened
                .content()
                .field(light_node, transform.component, transform.translation),
            Some(&Value::Vec3([1.0, 2.0, 3.0]))
        );
        assert!(reopened.schema().type_named("Camera").is_some());
    }

    fn light_field(document: &Document, node: NodeId, name: &str) -> Option<Value> {
        let light = document.schema().type_named("LightSource")?;
        let field = light.field_named(name)?;
        document.content().field(node, light.id, field.id).cloned()
    }

    #[test]
    fn area_lights_carry_their_kind_and_size_through_undo_and_a_world_save() {
        let mut editor = Editor::default();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        editor.open_document("worlds/area.cyworld").unwrap();
        let scope = Scope::unrestricted();
        let expected = [
            ("rect", 3, 1.0, 0.5),
            ("disc", 4, 0.5, 0.0),
            ("sphere", 5, 0.1, 0.0),
            ("tube", 6, 1.2, 0.04),
        ];
        for (kind, number, width, height) in expected {
            editor
                .invoke(
                    &registry,
                    "scene.create-light",
                    &scope,
                    &Arguments::new().with("kind", Value::Text(kind.into())),
                )
                .unwrap();
            let node = editor.selection.get().nodes().next().unwrap();
            let document = editor
                .documents
                .get(editor.workspace.active().unwrap())
                .unwrap();
            assert_eq!(
                light_field(document, node, "kind"),
                Some(Value::Int(number))
            );
            assert_eq!(
                light_field(document, node, "source_width"),
                Some(Value::Float(width))
            );
            assert_eq!(
                light_field(document, node, "source_height"),
                Some(Value::Float(height))
            );
            assert_eq!(
                light_field(document, node, "two_sided"),
                Some(Value::Bool(false))
            );
            // An area light's reach is derived from its brightness, not authored.
            assert_eq!(
                light_field(document, node, "range"),
                Some(Value::Float(0.0))
            );
        }
        // A punctual light gains no area field.
        editor
            .invoke(
                &registry,
                "scene.create-light",
                &scope,
                &Arguments::new().with("kind", Value::Text("point".into())),
            )
            .unwrap();
        let point = editor.selection.get().nodes().next().unwrap();
        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        assert_eq!(light_field(document, point, "source_width"), None);

        // Saved and reloaded, every area light is the light it was.
        let text = crate::worldfile::write_world(document);
        let mut reopened = Document::new("worlds/area.cyworld");
        crate::worldfile::load(&text, &mut reopened, cy_editor_core::Actor::human("test")).unwrap();
        let mut kinds: Vec<(i64, f32, f32)> = reopened
            .content()
            .nodes()
            .filter_map(|node| {
                let kind = light_field(&reopened, node, "kind")?.as_int()?;
                let width = light_field(&reopened, node, "source_width")
                    .and_then(|value| value.as_float())
                    .unwrap_or(0.0);
                let height = light_field(&reopened, node, "source_height")
                    .and_then(|value| value.as_float())
                    .unwrap_or(0.0);
                Some((kind, width, height))
            })
            .collect();
        kinds.sort_by_key(|(kind, _, _)| *kind);
        assert_eq!(
            kinds,
            vec![
                (1, 0.0, 0.0),
                (3, 1.0, 0.5),
                (4, 0.5, 0.0),
                (5, 0.1, 0.0),
                (6, 1.2, 0.04)
            ]
        );

        // Undo takes the lights back one by one.
        editor
            .invoke(&registry, "edit.undo", &scope, &Arguments::new())
            .unwrap();
        let document = editor
            .documents
            .get(editor.workspace.active().unwrap())
            .unwrap();
        assert_eq!(document.content().node_count(), 4);
    }

    #[test]
    fn an_unknown_light_kind_is_refused_naming_the_seven() {
        let mut editor = Editor::default();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        editor.open_document("worlds/area.cyworld").unwrap();
        let refused = editor
            .invoke(
                &registry,
                "scene.create-light",
                &Scope::unrestricted(),
                &Arguments::new().with("kind", Value::Text("quad".into())),
            )
            .unwrap_err();
        assert!(format!("{refused:?}").contains("rect, disc, sphere, or tube"));
    }
}
