//! Project graph parameters become generated, undoable scene Inspector fields.

use std::collections::BTreeMap;
use std::path::Path;

use cy_editor_core::Actor;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::operation::Operation;
use cy_editor_services::Editor;
use cy_editor_services::primitives::material_of;

struct Parameter {
    name: String,
    kind: ValueKind,
    default: Value,
}

fn parameters(source: &str) -> Result<Vec<Parameter>, String> {
    let mut nodes: BTreeMap<u64, BTreeMap<&str, &str>> = BTreeMap::new();
    for line in source.lines() {
        if let Some(rest) = line.strip_prefix("node ") {
            let (id, kind) = rest.split_once(' ').ok_or("invalid graph node")?;
            if kind == "material.parameter" {
                nodes.insert(id.parse().map_err(|_| "invalid node id")?, BTreeMap::new());
            }
        } else if let Some(rest) = line.strip_prefix("prop ") {
            let mut words = rest.splitn(3, ' ');
            let id: u64 = words
                .next()
                .ok_or("missing property node")?
                .parse()
                .map_err(|_| "invalid property node")?;
            let name = words.next().ok_or("missing property name")?;
            let value = words.next().ok_or("missing property value")?;
            if let Some(node) = nodes.get_mut(&id) {
                node.insert(name, value);
            }
        }
    }
    let parsed: Vec<Parameter> = nodes
        .into_values()
        .map(|properties| {
            let name = properties
                .get("symbol")
                .ok_or("parameter has no symbol")?
                .to_string();
            if !name
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
            {
                return Err("parameter symbol must be an identifier".into());
            }
            let kind = match properties.get("type").copied().unwrap_or("float") {
                "float" => ValueKind::Float,
                "float2" => ValueKind::Vec2,
                "float3" => ValueKind::Vec3,
                "float4" => ValueKind::Vec4,
                _ => return Err(format!("unsupported parameter type for {name}")),
            };
            let mut lanes = [0.0_f32; 4];
            for (index, word) in properties
                .get("default")
                .copied()
                .unwrap_or("0")
                .split_whitespace()
                .take(4)
                .enumerate()
            {
                lanes[index] = word
                    .parse()
                    .map_err(|_| format!("invalid default for {name}"))?;
            }
            let default = match kind {
                ValueKind::Float => Value::Float(lanes[0]),
                ValueKind::Vec2 => Value::Vec2([lanes[0], lanes[1]]),
                ValueKind::Vec3 => Value::Vec3([lanes[0], lanes[1], lanes[2]]),
                ValueKind::Vec4 => Value::Vec4(lanes),
                _ => unreachable!(),
            };
            Ok(Parameter {
                name,
                kind,
                default,
            })
        })
        .collect::<Result<_, _>>()?;
    let mut unique = BTreeMap::new();
    for parameter in parsed {
        if let Some(existing) = unique.get(&parameter.name) {
            let existing: &Parameter = existing;
            if existing.kind != parameter.kind || existing.default != parameter.default {
                return Err(format!(
                    "parameter {} has conflicting declarations",
                    parameter.name
                ));
            }
        } else {
            unique.insert(parameter.name.clone(), parameter);
        }
    }
    Ok(unique.into_values().collect())
}

/// Install fields for every object using this graph. Existing object overrides survive a graph save.
pub(super) fn sync(
    editor: &mut Editor,
    reference: &str,
    prior_source: Option<&str>,
) -> Result<usize, String> {
    let path = Path::new(reference);
    if path
        .extension()
        .is_none_or(|extension| extension != "cygraph")
        || !path
            .components()
            .all(|part| matches!(part, std::path::Component::Normal(_)))
    {
        return Err("material path must stay inside the project".into());
    }
    let source = std::fs::read_to_string(
        editor
            .project
            .root()
            .join(path.with_extension("cymatcanvas")),
    )
    .map_err(|error| error.to_string())?;
    let declared = parameters(&source)?;
    let prior: BTreeMap<_, _> = prior_source
        .map(parameters)
        .transpose()?
        .unwrap_or_default()
        .into_iter()
        .map(|parameter| (parameter.name, parameter.default))
        .collect();
    if declared.is_empty() {
        return Ok(0);
    }
    let Some(active) = editor.workspace.active() else {
        return Ok(0);
    };
    let Some(document) = editor.documents.get_mut(active) else {
        return Ok(0);
    };
    let nodes: Vec<_> = document
        .content()
        .nodes()
        .filter(|node| material_of(document, *node).as_deref() == Some(reference))
        .collect();
    if nodes.is_empty() {
        return Ok(0);
    }
    let title = format!(
        "Material: {}",
        path.file_stem().unwrap_or_default().to_string_lossy()
    );
    let component = document
        .schema()
        .type_named(&title)
        .map(|kind| kind.id)
        .unwrap_or_else(|| document.schema_mut().declare_type(&title, false));
    let mut fields = Vec::new();
    for parameter in declared {
        let field = match document
            .schema()
            .type_named(&title)
            .and_then(|kind| kind.field_named(&parameter.name))
        {
            Some(field) if field.kind == parameter.kind => field.id,
            Some(_) => {
                return Err(format!(
                    "parameter {} changed type; rename it first",
                    parameter.name
                ));
            }
            None => document
                .schema_mut()
                .declare_field(
                    component,
                    &parameter.name,
                    parameter.kind,
                    "Graph parameter override for this object",
                )
                .map_err(|problem| problem.to_string())?,
        };
        fields.push((
            field,
            parameter.default,
            prior.get(&parameter.name).cloned(),
        ));
    }
    let mut changed = 0;
    for node in nodes {
        let old = document
            .content()
            .node(node)
            .and_then(|state| state.components.get(&component))
            .cloned();
        let values: Vec<_> = fields
            .iter()
            .map(|(field, default, prior_default)| {
                let existing = old.as_ref().and_then(|values| values.get(field));
                let value = match existing {
                    Some(value) if prior_default.as_ref() != Some(value) => value.clone(),
                    _ => default.clone(),
                };
                (*field, value)
            })
            .collect();
        if old.as_ref().is_some_and(|current| {
            current
                .iter()
                .eq(values.iter().map(|(field, value)| (field, value)))
        }) {
            continue;
        }
        document.begin("Sync material properties", Actor::human("Editor"));
        if let Some(old) = old {
            document
                .record(Operation::RemoveComponent {
                    node,
                    component,
                    before: old.into_iter().collect(),
                })
                .map_err(|problem| problem.to_string())?;
        }
        document
            .add_component(node, component, values)
            .map_err(|problem| problem.to_string())?;
        document.commit().map_err(|problem| problem.to_string())?;
        changed += 1;
    }
    Ok(changed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_services::project::ProjectService;

    #[test]
    fn graph_parameter_defaults_keep_their_declared_types() {
        let source = "cymatcanvas 1\nmaterial paint\nnode 1 material.parameter\nprop 1 symbol tint\nprop 1 type float3\nprop 1 default 0.7 0.2 0.1 0\nnode 2 material.parameter\nprop 2 symbol roughness\nprop 2 type float\nprop 2 default 0.6 0 0 0\n";
        let values = parameters(source).expect("valid graph parameters");
        assert_eq!(values.len(), 2);
        let tint = values.iter().find(|value| value.name == "tint").unwrap();
        let roughness = values
            .iter()
            .find(|value| value.name == "roughness")
            .unwrap();
        assert_eq!(tint.default, Value::Vec3([0.7, 0.2, 0.1]));
        assert_eq!(roughness.default, Value::Float(0.6));
    }

    #[test]
    fn graph_default_change_updates_an_untouched_object_and_can_be_undone() {
        let repository = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
        let sample = repository.join("samples/05b-editor-window/project");
        let root =
            std::env::temp_dir().join(format!("cy-material-parameters-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("worlds")).unwrap();
        std::fs::create_dir_all(root.join("materials")).unwrap();
        for relative in [
            "project.json",
            "worlds/material-graph.cyworld",
            "materials/copper_clay.cymatcanvas",
        ] {
            std::fs::copy(sample.join(relative), root.join(relative)).unwrap();
        }
        let mut editor = Editor::default().with_project(ProjectService::new(&root));
        let document_id = editor
            .open_document("worlds/material-graph.cyworld")
            .unwrap();
        let reference = "materials/copper_clay.cygraph";
        let source_path = root.join("materials/copper_clay.cymatcanvas");
        let previous = std::fs::read_to_string(&source_path).unwrap();
        std::fs::write(
            &source_path,
            previous.replace("default 0.72", "default 0.25"),
        )
        .unwrap();
        assert_eq!(sync(&mut editor, reference, Some(&previous)).unwrap(), 1);
        let document = editor.documents.get_mut(document_id).unwrap();
        let component = document
            .schema()
            .type_named("Material: copper_clay")
            .unwrap();
        let component_id = component.id;
        let field = component.field_named("albedo").unwrap().id;
        let node = document
            .content()
            .nodes()
            .find(|node| {
                document
                    .content()
                    .node(*node)
                    .is_some_and(|state| state.name == "Graph Cube")
            })
            .unwrap();
        assert_eq!(
            document.content().field(node, component_id, field),
            Some(&Value::Vec3([0.25, 0.2, 0.1]))
        );
        document.undo().unwrap();
        assert_eq!(
            document.content().field(node, component_id, field),
            Some(&Value::Vec3([0.72, 0.2, 0.1]))
        );
        document
            .with_transaction("Override albedo", Actor::human("designer"), |document| {
                document.set_field(node, component_id, field, Value::Vec3([0.9, 0.2, 0.1]))
            })
            .unwrap();
        let last = std::fs::read_to_string(&source_path).unwrap();
        std::fs::write(&source_path, last.replace("default 0.25", "default 0.3")).unwrap();
        assert_eq!(sync(&mut editor, reference, Some(&last)).unwrap(), 0);
        assert_eq!(
            editor
                .documents
                .get(document_id)
                .unwrap()
                .content()
                .field(node, component_id, field),
            Some(&Value::Vec3([0.9, 0.2, 0.1]))
        );
        drop(editor);
        std::fs::remove_dir_all(root).unwrap();
    }
}
