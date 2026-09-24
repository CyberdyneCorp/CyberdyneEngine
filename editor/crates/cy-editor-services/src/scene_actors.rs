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

fn place(document: &mut Document, node: NodeId, at: [f32; 3], rotation: [f32; 4]) -> Result<()> {
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

fn create_light() -> Command {
    let metadata = Metadata::new(
        "scene.create-light",
        "Create Light",
        "Scene",
        "Creates a directional, point, or spot light with an editable transform and lighting fields \
         in the active world. The creation is one undoable transaction and selects the light.",
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "kind",
        ValueKind::Text,
        "The light type: directional, point, or spot.",
    ));
    Command::new(position_argument(metadata), |context, arguments| {
        let (kind, label, intensity, range) = match arguments.text("kind").unwrap_or_default() {
            "directional" => (0, "Directional Light", 100_000.0, 0.0),
            "point" => (1, "Point Light", 1000.0, 10.0),
            "spot" => (2, "Spot Light", 1000.0, 10.0),
            value => {
                return Err(
                    Problem::new("create a light", format!("unknown type {value:?}"))
                        .with_remedy("use directional, point, or spot"),
                );
            }
        };
        let id = active(context)?;
        let actor = context.actor();
        let document = context
            .document_mut(id)
            .ok_or_else(|| Problem::not_found("the world"))?;
        let node = document.with_transaction(format!("Create {label}"), actor, |document| {
            let node = document.create_node(None)?;
            document.set_name(node, label)?;
            place(document, node, at(arguments), [0.0, 0.0, 0.0, 1.0])?;
            add_fields(
                document,
                node,
                "LightSource",
                &[
                    ("kind", Value::Int(kind)),
                    ("color.r", Value::Float(1.0)),
                    ("color.g", Value::Float(1.0)),
                    ("color.b", Value::Float(1.0)),
                    ("intensity", Value::Float(intensity)),
                    ("range", Value::Float(range)),
                    ("inner_cone", Value::Float(0.0)),
                    ("outer_cone", Value::Float(std::f32::consts::FRAC_PI_4)),
                    ("casts_shadow", Value::Bool(true)),
                    ("enabled", Value::Bool(true)),
                ],
            )?;
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
}
