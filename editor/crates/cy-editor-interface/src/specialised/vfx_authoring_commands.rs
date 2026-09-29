// SPDX-License-Identifier: MIT
//! VFX hierarchy and canvas edits exposed through the editor's shared command registry.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, ProjectHost,
    Registry,
};
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::document::Document;
use cy_editor_documents::schema::DocumentSchema;
use cy_editor_documents::selection::Selection;
use cy_editor_services::authoring::within_scope;
use cy_editor_services::primitives::add_transform;

use super::graph::{Catalogue, GraphCanvas, Layout, NodeKey};
use super::material::catalogue_from_service;
use super::vfx::{
    Attribute, Emitter, EmitterParameter, EventChannel, Parameter, SimulationPath, Stage,
    VfxDocument, validate_parameter,
};
use super::vfx_module::{ModuleInput, VfxModule};

/// Install the same VFX actions for the command palette, scripts, and MCP projection.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(create_document())?;
    registry.register(add_emitter())?;
    registry.register(remove_emitter())?;
    registry.register(configure_emitter())?;
    registry.register(bind_interface())?;
    registry.register(unbind_interface())?;
    registry.register(add_node())?;
    registry.register(move_node())?;
    registry.register(connect_nodes())?;
    registry.register(disconnect_nodes())?;
    registry.register(remove_node())?;
    registry.register(set_node_property())?;
    registry.register(set_parameter())?;
    registry.register(remove_parameter())?;
    registry.register(set_emitter_parameter())?;
    registry.register(remove_emitter_parameter())?;
    registry.register(create_scene_effect())?;
    registry.register(set_scene_effect_parameter())?;
    registry.register(set_emitter_capacity())?;
    registry.register(set_attribute())?;
    registry.register(remove_attribute())?;
    registry.register(set_channel())?;
    registry.register(remove_channel())?;
    registry.register(create_module())?;
    registry.register(set_module_stage())?;
    registry.register(add_module_input())?;
    registry.register(remove_module_input())?;
    registry.register(add_module_dependency())?;
    registry.register(remove_module_dependency())?;
    registry.register(add_module_node())?;
    registry.register(move_module_node())?;
    registry.register(connect_module_nodes())?;
    registry.register(disconnect_module_nodes())?;
    registry.register(remove_module_node())?;
    registry.register(set_module_node_property())?;
    registry.register(attach_module())?;
    Ok(())
}

fn create_document() -> Command {
    Command::new(
        metadata(
            "vfx.document.create",
            "Create VFX System",
            "Creates a saved VFX system through the scene's undo history.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "System name stored in the VFX authoring document.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            let document = VfxDocument::new(name)?;
            within_scope(context, reference)?;
            let project = host(context)?;
            if project.vfx_document_exists(reference) {
                return Err(Problem::new(
                    "create a VFX system",
                    format!("{reference} already contains a system"),
                ));
            }
            project.vfx_document_save(reference, &document.encode_text()?)?;
            Ok(Outcome::new(format!("Created VFX system {name}")))
        },
    )
}

fn metadata(id: &str, label: &str, description: &str) -> Metadata {
    Metadata::new(
        id,
        label,
        "VFX Graph",
        description,
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyvfxdoc authoring document path.",
    ))
}

fn module_metadata(id: &str, label: &str, description: &str) -> Metadata {
    Metadata::new(
        id,
        label,
        "VFX Graph",
        description,
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyvfxmodule authoring source path.",
    ))
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("edit a VFX graph", "no project is open"))
}

fn edit_document(
    context: &mut dyn CommandContext,
    reference: &str,
    edit: impl FnOnce(&mut VfxDocument, &mut dyn ProjectHost) -> Result<Outcome>,
) -> Result<Outcome> {
    within_scope(context, reference)?;
    let project = host(context)?;
    let mut document = VfxDocument::decode_text(&project.vfx_document_read(reference)?)?;
    let outcome = edit(&mut document, project)?;
    project.vfx_document_save(reference, &document.encode_text()?)?;
    Ok(outcome)
}

fn edit_module(
    context: &mut dyn CommandContext,
    reference: &str,
    edit: impl FnOnce(&mut VfxModule) -> Result<Outcome>,
) -> Result<Outcome> {
    within_scope(context, reference)?;
    let project = host(context)?;
    let mut module = VfxModule::decode_text(&project.vfx_module_read(reference)?)?;
    let outcome = edit(&mut module)?;
    project.vfx_module_save(reference, &module.encode_text()?)?;
    Ok(outcome)
}

fn edit_module_canvas(
    context: &mut dyn CommandContext,
    reference: &str,
    edit: impl FnOnce(&mut GraphCanvas) -> Result<Outcome>,
) -> Result<Outcome> {
    within_scope(context, reference)?;
    let project = host(context)?;
    let mut module = VfxModule::decode_text(&project.vfx_module_read(reference)?)?;
    let mut canvas = module_canvas(&catalogue(project)?, &module)?;
    let outcome = edit(&mut canvas)?;
    module.capture(&canvas)?;
    project.vfx_module_save(reference, &module.encode_text()?)?;
    Ok(outcome)
}

fn module_canvas(payload: &[u8], module: &VfxModule) -> Result<GraphCanvas> {
    let nodes = catalogue_from_service(payload)?;
    if nodes.is_empty() || nodes.iter().any(|node| !node.name.starts_with("vfx.")) {
        return Err(Problem::new(
            "edit a VFX module graph",
            "the engine supplied no usable VFX node catalogue",
        ));
    }
    let mut canvas = GraphCanvas::new(1);
    canvas.load(Catalogue::new(nodes)?);
    module.open(&mut canvas)?;
    Ok(canvas)
}

fn emitter_index(document: &VfxDocument, name: &str) -> Result<usize> {
    document
        .emitters
        .iter()
        .position(|emitter| emitter.name == name)
        .ok_or_else(|| Problem::new("edit a VFX stage", format!("emitter {name} does not exist")))
}

fn stage(arguments: &Arguments) -> Result<Stage> {
    match text(arguments, "stage").to_ascii_lowercase().as_str() {
        "spawn" => Ok(Stage::Spawn),
        "initialise" => Ok(Stage::Initialise),
        "update" => Ok(Stage::Update),
        "event" => Ok(Stage::Event),
        "render" => Ok(Stage::Render),
        "compute" => Ok(Stage::Compute),
        other => Err(Problem::new(
            "edit a VFX stage",
            format!(
                "stage {other} is not one of spawn, initialise, update, event, render, compute"
            ),
        )),
    }
}

fn node_key(arguments: &Arguments, name: &str) -> Result<NodeKey> {
    let value = arguments
        .get(name)
        .and_then(Value::as_int)
        .unwrap_or_default();
    NodeKey::new(u64::try_from(value).unwrap_or_default())
}

fn node_layout(arguments: &Arguments) -> Result<Layout> {
    let x = arguments
        .get("x")
        .and_then(Value::as_float)
        .unwrap_or_default();
    let y = arguments
        .get("y")
        .and_then(Value::as_float)
        .unwrap_or_default();
    if !x.is_finite() || !y.is_finite() {
        return Err(Problem::new(
            "move a VFX node",
            "canvas position must be finite",
        ));
    }
    Ok(Layout { x, y })
}

fn positive_u32(arguments: &Arguments, name: &str) -> Result<u32> {
    let value = arguments
        .get(name)
        .and_then(Value::as_int)
        .unwrap_or_default();
    u32::try_from(value)
        .ok()
        .filter(|value| *value > 0)
        .ok_or_else(|| Problem::new("edit VFX metadata", format!("{name} must be positive")))
}

fn stage_canvas(
    payload: &[u8],
    document: &VfxDocument,
    emitter: usize,
    stage: Stage,
) -> Result<GraphCanvas> {
    let nodes = catalogue_from_service(payload)?;
    if nodes.is_empty() || nodes.iter().any(|node| !node.name.starts_with("vfx.")) {
        return Err(Problem::new(
            "edit a VFX stage",
            "the engine supplied no usable VFX node catalogue",
        ));
    }
    let mut canvas = GraphCanvas::new(1);
    canvas.load(Catalogue::new(nodes)?);
    document.open_stage(emitter, stage, &mut canvas)?;
    Ok(canvas)
}

fn edit_canvas(
    document: &mut VfxDocument,
    payload: &[u8],
    emitter_name: &str,
    stage: Stage,
    edit: impl FnOnce(&mut GraphCanvas) -> Result<Outcome>,
) -> Result<Outcome> {
    let emitter = emitter_index(document, emitter_name)?;
    let mut canvas = stage_canvas(payload, document, emitter, stage)?;
    let outcome = edit(&mut canvas)?;
    document.capture_stage(emitter, stage, &canvas)?;
    Ok(outcome)
}

fn set_declared_property(
    canvas: &mut GraphCanvas,
    key: NodeKey,
    property: &str,
    value: &str,
) -> Result<()> {
    let identity = canvas
        .node(key)
        .and_then(|node| canvas.catalogue().get(&node.type_name))
        .and_then(|kind| kind.properties.iter().find(|item| item.name == property))
        .map(|item| item.identity)
        .ok_or_else(|| {
            Problem::new(
                "set a VFX node property",
                format!(
                    "node {} has no engine-declared property {property}",
                    key.ordinal()
                ),
            )
        })?;
    canvas.set_property_by_identity(key, identity, value)
}

fn catalogue(project: &dyn ProjectHost) -> Result<Vec<u8>> {
    project.vfx_catalogue().ok_or_else(|| {
        Problem::new(
            "edit a VFX stage",
            "the engine VFX node catalogue is unavailable",
        )
    })
}

fn add_emitter() -> Command {
    Command::new(
        metadata(
            "vfx.emitter.add",
            "Add VFX Emitter",
            "Adds an emitter to the saved system in one undoable document edit.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Unique name used to identify this emitter within the system.",
        ))
        .with(ParameterSpec::required(
            "target",
            ValueKind::Text,
            "Simulation target: cpu or gpu.",
        ))
        .with(ParameterSpec::required(
            "renderer",
            ValueKind::Text,
            "Engine renderer kind, such as Sprite or Mesh.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            let path = match text(arguments, "target") {
                "cpu" => SimulationPath::CpuRequired,
                "gpu" => SimulationPath::GpuPreferred,
                other => {
                    return Err(Problem::new(
                        "add a VFX emitter",
                        format!("target {other} must be cpu or gpu"),
                    ));
                }
            };
            let renderer = text(arguments, "renderer");
            edit_document(context, reference, |document, _| {
                document.emitters.push(Emitter {
                    name: name.into(),
                    path,
                    renderer: renderer.into(),
                    stages: Vec::new(),
                    modules: Vec::new(),
                    interfaces: Vec::new(),
                    capacity: 1024,
                    attributes: Vec::new(),
                });
                Ok(Outcome::new(format!("Added VFX emitter {name}")))
            })
        },
    )
}

fn simulation_path(value: &str) -> Result<SimulationPath> {
    match value {
        "cpu" => Ok(SimulationPath::CpuRequired),
        "gpu" => Ok(SimulationPath::GpuPreferred),
        other => Err(Problem::new(
            "configure a VFX emitter",
            format!("target {other} must be cpu or gpu"),
        )),
    }
}

fn remove_emitter() -> Command {
    Command::new(
        metadata(
            "vfx.emitter.remove",
            "Remove VFX Emitter",
            "Removes one named emitter and its stages in an undoable document edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter to remove from the system.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "emitter");
            edit_document(context, reference, |document, _| {
                let index = emitter_index(document, name)?;
                document.emitters.remove(index);
                document
                    .emitter_parameters
                    .retain(|entry| entry.emitter != name);
                Ok(Outcome::new(format!("Removed VFX emitter {name}")))
            })
        },
    )
}

fn configure_emitter() -> Command {
    Command::new(
        metadata(
            "vfx.emitter.configure",
            "Configure VFX Emitter",
            "Changes an emitter's renderer and CPU/GPU target in one undoable document edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter to configure.",
        ))
        .with(ParameterSpec::required(
            "target",
            ValueKind::Text,
            "Simulation target: cpu or gpu.",
        ))
        .with(ParameterSpec::required(
            "renderer",
            ValueKind::Text,
            "Engine renderer kind, such as Sprite or Mesh.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "emitter");
            let path = simulation_path(text(arguments, "target"))?;
            let renderer = text(arguments, "renderer");
            edit_document(context, reference, |document, _| {
                let index = emitter_index(document, name)?;
                let emitter = &mut document.emitters[index];
                emitter.path = path;
                emitter.renderer = renderer.into();
                Ok(Outcome::new(format!("Configured VFX emitter {name}")))
            })
        },
    )
}

fn bind_interface() -> Command {
    Command::new(
        metadata(
            "vfx.interface.bind",
            "Bind VFX Data Interface",
            "Binds a named engine data interface to an emitter in one undoable edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter receiving the interface.",
        ))
        .with(ParameterSpec::required(
            "interface",
            ValueKind::Text,
            "Engine data-interface name.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter_name = text(arguments, "emitter");
            let interface = text(arguments, "interface");
            edit_document(context, reference, |document, _| {
                let index = emitter_index(document, emitter_name)?;
                document.emitters[index].interfaces.push(interface.into());
                Ok(Outcome::new(format!(
                    "Bound {interface} to VFX emitter {emitter_name}"
                )))
            })
        },
    )
}

fn unbind_interface() -> Command {
    Command::new(
        metadata(
            "vfx.interface.unbind",
            "Unbind VFX Data Interface",
            "Removes an emitter's data-interface binding in one undoable edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter losing the interface.",
        ))
        .with(ParameterSpec::required(
            "interface",
            ValueKind::Text,
            "Bound engine data-interface name.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter_name = text(arguments, "emitter");
            let interface = text(arguments, "interface");
            edit_document(context, reference, |document, _| {
                let index = emitter_index(document, emitter_name)?;
                let bindings = &mut document.emitters[index].interfaces;
                let position = bindings
                    .iter()
                    .position(|name| name == interface)
                    .ok_or_else(|| {
                        Problem::new(
                            "unbind a VFX data interface",
                            format!("{interface} is not bound to {emitter_name}"),
                        )
                    })?;
                bindings.remove(position);
                Ok(Outcome::new(format!(
                    "Unbound {interface} from VFX emitter {emitter_name}"
                )))
            })
        },
    )
}

fn add_node() -> Command {
    Command::new(
        metadata(
            "vfx.node.add",
            "Add VFX Node",
            "Places an engine-catalogue node on one saved emitter stage.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter whose stage graph will receive the node.",
        ))
        .with(ParameterSpec::required(
            "stage",
            ValueKind::Text,
            "Stage of the named emitter that will receive the edit.",
        ))
        .with(ParameterSpec::required(
            "node_type",
            ValueKind::Text,
            "Node type from the engine VFX catalogue.",
        ))
        .with(ParameterSpec::required(
            "x",
            ValueKind::Float,
            "Horizontal position of the node on the shared canvas.",
        ))
        .with(ParameterSpec::required(
            "y",
            ValueKind::Float,
            "Vertical position of the node on the shared canvas.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter_name = text(arguments, "emitter");
            let stage = stage(arguments)?;
            let node_type = text(arguments, "node_type");
            let x = arguments
                .get("x")
                .and_then(Value::as_float)
                .unwrap_or_default();
            let y = arguments
                .get("y")
                .and_then(Value::as_float)
                .unwrap_or_default();
            if !x.is_finite() || !y.is_finite() {
                return Err(Problem::new(
                    "place a VFX node",
                    "canvas position must be finite",
                ));
            }
            edit_document(context, reference, |document, project| {
                edit_canvas(
                    document,
                    &catalogue(project)?,
                    emitter_name,
                    stage,
                    |canvas| {
                        let node = canvas.add(node_type, Layout { x, y })?;
                        Ok(Outcome::new(format!(
                            "Added {node_type} to {emitter_name} {}",
                            stage.label()
                        ))
                        .with(
                            "node",
                            Value::Int(i64::try_from(node.ordinal()).unwrap_or(i64::MAX)),
                        ))
                    },
                )
            })
        },
    )
}

fn move_node() -> Command {
    Command::new(
        metadata(
            "vfx.node.move",
            "Move VFX Node",
            "Moves a node on one saved emitter stage without changing its connections.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter whose stage contains the node.",
        ))
        .with(ParameterSpec::required(
            "stage",
            ValueKind::Text,
            "Stage containing the node to move.",
        ))
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the node to move.",
        ))
        .with(ParameterSpec::required(
            "x",
            ValueKind::Float,
            "Horizontal canvas position.",
        ))
        .with(ParameterSpec::required(
            "y",
            ValueKind::Float,
            "Vertical canvas position.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter_name = text(arguments, "emitter");
            let stage = stage(arguments)?;
            let node = node_key(arguments, "node")?;
            let at = node_layout(arguments)?;
            edit_document(context, reference, |document, project| {
                edit_canvas(
                    document,
                    &catalogue(project)?,
                    emitter_name,
                    stage,
                    |canvas| {
                        canvas.move_to(node, at)?;
                        Ok(Outcome::new(format!("Moved VFX node {}", node.ordinal())))
                    },
                )
            })
        },
    )
}

fn connect_nodes() -> Command {
    Command::new(
        metadata(
            "vfx.node.connect",
            "Connect VFX Nodes",
            "Connects two engine-typed pins on one saved emitter stage.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter whose stage graph owns both nodes.",
        ))
        .with(ParameterSpec::required(
            "stage",
            ValueKind::Text,
            "Stage of the named emitter that owns both nodes.",
        ))
        .with(ParameterSpec::required(
            "from",
            ValueKind::Int,
            "Stable key of the node providing the output value.",
        ))
        .with(ParameterSpec::required(
            "from_pin",
            ValueKind::Text,
            "Name of the source node's engine-declared output pin.",
        ))
        .with(ParameterSpec::required(
            "to",
            ValueKind::Int,
            "Stable key of the node receiving the input value.",
        ))
        .with(ParameterSpec::required(
            "to_pin",
            ValueKind::Text,
            "Name of the destination node's engine-declared input pin.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter_name = text(arguments, "emitter");
            let stage = stage(arguments)?;
            let from = node_key(arguments, "from")?;
            let to = node_key(arguments, "to")?;
            let from_pin = text(arguments, "from_pin");
            let to_pin = text(arguments, "to_pin");
            edit_document(context, reference, |document, project| {
                edit_canvas(
                    document,
                    &catalogue(project)?,
                    emitter_name,
                    stage,
                    |canvas| {
                        canvas.connect(from, from_pin, to, to_pin)?;
                        Ok(Outcome::new(format!(
                            "Connected VFX nodes {} and {}",
                            from.ordinal(),
                            to.ordinal()
                        )))
                    },
                )
            })
        },
    )
}

fn disconnect_nodes() -> Command {
    Command::new(
        metadata(
            "vfx.node.disconnect",
            "Disconnect VFX Nodes",
            "Removes one wire from a saved emitter stage in an undoable edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter whose stage contains this wire.",
        ))
        .with(ParameterSpec::required(
            "stage",
            ValueKind::Text,
            "Stage of that emitter containing this wire.",
        ))
        .with(ParameterSpec::required(
            "from",
            ValueKind::Int,
            "Stable key of the node providing the wire's value.",
        ))
        .with(ParameterSpec::required(
            "from_pin",
            ValueKind::Text,
            "Engine-declared output pin connected by the wire.",
        ))
        .with(ParameterSpec::required(
            "to",
            ValueKind::Int,
            "Stable key of the node receiving the wire's value.",
        ))
        .with(ParameterSpec::required(
            "to_pin",
            ValueKind::Text,
            "Engine-declared input pin connected by the wire.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter_name = text(arguments, "emitter");
            let stage = stage(arguments)?;
            let from = node_key(arguments, "from")?;
            let to = node_key(arguments, "to")?;
            let from_pin = text(arguments, "from_pin");
            let to_pin = text(arguments, "to_pin");
            edit_document(context, reference, |document, project| {
                edit_canvas(
                    document,
                    &catalogue(project)?,
                    emitter_name,
                    stage,
                    |canvas| {
                        canvas.disconnect(from, from_pin, to, to_pin)?;
                        Ok(Outcome::new(format!(
                            "Disconnected VFX nodes {} and {}",
                            from.ordinal(),
                            to.ordinal()
                        )))
                    },
                )
            })
        },
    )
}

fn remove_node() -> Command {
    Command::new(
        metadata(
            "vfx.node.remove",
            "Remove VFX Node",
            "Removes one node and its wires from a saved emitter stage in an undoable edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter whose stage contains this node.",
        ))
        .with(ParameterSpec::required(
            "stage",
            ValueKind::Text,
            "Stage of that emitter containing this node.",
        ))
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the node and attached wires to remove.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter_name = text(arguments, "emitter");
            let stage = stage(arguments)?;
            let node = node_key(arguments, "node")?;
            edit_document(context, reference, |document, project| {
                edit_canvas(
                    document,
                    &catalogue(project)?,
                    emitter_name,
                    stage,
                    |canvas| {
                        canvas.remove(node)?;
                        Ok(Outcome::new(format!("Removed VFX node {}", node.ordinal())))
                    },
                )
            })
        },
    )
}

fn set_node_property() -> Command {
    Command::new(
        metadata(
            "vfx.node.property.set",
            "Set VFX Node Property",
            "Sets an engine-declared node property on one saved emitter stage.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter whose stage owns the node.",
        ))
        .with(ParameterSpec::required(
            "stage",
            ValueKind::Text,
            "Stage containing the node.",
        ))
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the node to edit.",
        ))
        .with(ParameterSpec::required(
            "property",
            ValueKind::Text,
            "Engine-declared property name.",
        ))
        .with(ParameterSpec::required(
            "value",
            ValueKind::Text,
            "Property value in the engine-declared textual form.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter_name = text(arguments, "emitter");
            let stage = stage(arguments)?;
            let key = node_key(arguments, "node")?;
            let property = text(arguments, "property");
            let value = text(arguments, "value");
            edit_document(context, reference, |document, project| {
                edit_canvas(
                    document,
                    &catalogue(project)?,
                    emitter_name,
                    stage,
                    |canvas| {
                        set_declared_property(canvas, key, property, value)?;
                        Ok(Outcome::new(format!(
                            "Set {property} on VFX node {}",
                            key.ordinal()
                        )))
                    },
                )
            })
        },
    )
}

fn set_parameter() -> Command {
    Command::new(
        metadata(
            "vfx.parameter.set",
            "Set VFX Parameter",
            "Creates or updates a typed system parameter in one undoable document edit.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Name by which this system parameter is read and updated.",
        ))
        .with(ParameterSpec::required(
            "kind",
            ValueKind::Text,
            "Numeric type the engine uses to validate the parameter value.",
        ))
        .with(ParameterSpec::required(
            "values",
            ValueKind::Vec4,
            "Parameter components in engine order, with unused lanes zeroed.",
        ))
        .with(ParameterSpec::required(
            "exposed",
            ValueKind::Bool,
            "Whether a running effect may change this value without recompilation.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            let kind = text(arguments, "kind");
            let Some(Value::Vec4(value)) = arguments.get("values") else {
                return Err(Problem::new(
                    "set a VFX parameter",
                    "four numeric lanes are required",
                ));
            };
            let exposed = matches!(arguments.get("exposed"), Some(Value::Bool(true)));
            edit_document(context, reference, |document, _| {
                let parameter = Parameter {
                    name: name.into(),
                    kind: kind.into(),
                    value: *value,
                    exposed,
                };
                if let Some(existing) = document
                    .parameters
                    .iter_mut()
                    .find(|entry| entry.name == name)
                {
                    *existing = parameter;
                } else {
                    document.parameters.push(parameter);
                }
                Ok(Outcome::new(format!("Set VFX parameter {name}")))
            })
        },
    )
}

fn remove_parameter() -> Command {
    Command::new(
        metadata(
            "vfx.parameter.remove",
            "Remove VFX Parameter",
            "Removes one typed system parameter in an undoable document edit.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Identifier of the system parameter to remove.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            edit_document(context, reference, |document, _| {
                let position = document
                    .parameters
                    .iter()
                    .position(|entry| entry.name == name)
                    .ok_or_else(|| {
                        Problem::new("remove a VFX parameter", format!("{name} does not exist"))
                    })?;
                document.parameters.remove(position);
                Ok(Outcome::new(format!("Removed VFX parameter {name}")))
            })
        },
    )
}

fn set_emitter_parameter() -> Command {
    Command::new(
        metadata(
            "vfx.emitter.parameter.set",
            "Set Emitter Parameter",
            "Creates or updates one emitter-local typed default in an undoable edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the owning emitter.",
        ))
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Parameter name visible in that emitter's graphs.",
        ))
        .with(ParameterSpec::required(
            "kind",
            ValueKind::Text,
            "Numeric parameter type.",
        ))
        .with(ParameterSpec::required(
            "values",
            ValueKind::Vec4,
            "Default value, with unused lanes zeroed.",
        ))
        .with(ParameterSpec::required(
            "exposed",
            ValueKind::Bool,
            "Whether a playing effect may override this value.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter = text(arguments, "emitter");
            let name = text(arguments, "name");
            let kind = text(arguments, "kind");
            let Some(Value::Vec4(value)) = arguments.get("values") else {
                return Err(Problem::new(
                    "set an emitter parameter",
                    "four numeric lanes are required",
                ));
            };
            let exposed = matches!(arguments.get("exposed"), Some(Value::Bool(true)));
            edit_document(context, reference, |document, _| {
                emitter_index(document, emitter)?;
                let entry = EmitterParameter {
                    emitter: emitter.into(),
                    parameter: Parameter {
                        name: name.into(),
                        kind: kind.into(),
                        value: *value,
                        exposed,
                    },
                };
                if let Some(existing) = document
                    .emitter_parameters
                    .iter_mut()
                    .find(|existing| existing.emitter == emitter && existing.parameter.name == name)
                {
                    *existing = entry;
                } else {
                    document.emitter_parameters.push(entry);
                }
                Ok(Outcome::new(format!("Set {emitter} parameter {name}")))
            })
        },
    )
}

fn remove_emitter_parameter() -> Command {
    Command::new(
        metadata(
            "vfx.emitter.parameter.remove",
            "Remove Emitter Parameter",
            "Removes one emitter-local parameter in an undoable edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter that owns this parameter.",
        ))
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Name of the local parameter to remove from that emitter.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter = text(arguments, "emitter");
            let name = text(arguments, "name");
            edit_document(context, reference, |document, _| {
                let index = document
                    .emitter_parameters
                    .iter()
                    .position(|entry| entry.emitter == emitter && entry.parameter.name == name)
                    .ok_or_else(|| {
                        Problem::new("remove an emitter parameter", "parameter does not exist")
                    })?;
                document.emitter_parameters.remove(index);
                Ok(Outcome::new(format!("Removed {emitter} parameter {name}")))
            })
        },
    )
}

const EFFECT_COMPONENT: &str = "cy::vfx::Effect";

fn scene_field(
    schema: &mut DocumentSchema,
    component: TypeId,
    name: &str,
    kind: ValueKind,
) -> Result<FieldId> {
    if let Some(field) = schema
        .type_of(component)
        .and_then(|definition| definition.field_named(name))
    {
        if field.kind != kind {
            return Err(Problem::new(
                "author a scene effect",
                format!("field {name} has another value type"),
            ));
        }
        return Ok(field.id);
    }
    schema.declare_field(component, name, kind, format!("VFX {name} override"))
}

fn scene_component(schema: &mut DocumentSchema, name: &str) -> TypeId {
    if let Some(definition) = schema.type_named(name) {
        definition.id
    } else {
        schema.declare_type(name, false)
    }
}

#[allow(clippy::cast_possible_truncation)] // validate_parameter bounds integral f32 values to i32.
fn scene_parameter_value(parameter: &Parameter) -> Result<(ValueKind, Value)> {
    validate_parameter(parameter)?;
    Ok(match parameter.kind.as_str() {
        "vec2" => (
            ValueKind::Vec2,
            Value::Vec2([parameter.value[0], parameter.value[1]]),
        ),
        "vec3" => (
            ValueKind::Vec3,
            Value::Vec3([parameter.value[0], parameter.value[1], parameter.value[2]]),
        ),
        "vec4" => (ValueKind::Vec4, Value::Vec4(parameter.value)),
        "int" => (ValueKind::Int, Value::Int(parameter.value[0] as i64)),
        "bool" => (ValueKind::Bool, Value::Bool(parameter.value[0] != 0.0)),
        _ => (ValueKind::Float, Value::Float(parameter.value[0])),
    })
}

fn scene_parameter_name(emitter: Option<&str>, parameter: &Parameter) -> String {
    match emitter {
        Some(name) => format!("emitter.{name}.{}.{}", parameter.name, parameter.kind),
        None => format!("system.{}.{}", parameter.name, parameter.kind),
    }
}

fn add_scene_effect(
    document: &mut Document,
    node: NodeId,
    asset: &str,
    vfx: &VfxDocument,
) -> Result<()> {
    let component = scene_component(document.schema_mut(), EFFECT_COMPONENT);
    let asset_field = scene_field(document.schema_mut(), component, "asset", ValueKind::Text)?;
    let enabled_field = scene_field(document.schema_mut(), component, "enabled", ValueKind::Bool)?;
    let mut fields = vec![
        (asset_field, Value::Text(asset.into())),
        (enabled_field, Value::Bool(true)),
    ];
    for (emitter, parameter) in vfx
        .parameters
        .iter()
        .map(|parameter| (None, parameter))
        .chain(
            vfx.emitter_parameters
                .iter()
                .map(|entry| (Some(entry.emitter.as_str()), &entry.parameter)),
        )
    {
        if !parameter.exposed {
            continue;
        }
        let name = scene_parameter_name(emitter, parameter);
        let (kind, value) = scene_parameter_value(parameter)?;
        let field = scene_field(document.schema_mut(), component, &name, kind)?;
        fields.push((field, value));
    }
    document.add_component(node, component, fields)
}

fn create_scene_effect() -> Command {
    Command::new(
        Metadata::new(
            "scene.vfx-effect.create",
            "Create VFX Effect",
            "Scene",
            "Creates a scene effect instance with Inspector fields for exposed parameters.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "asset",
            ValueKind::Text,
            "Project-relative .cyvfxdoc system to play on this scene entity.",
        ))
        .with(ParameterSpec::optional(
            "at",
            ValueKind::Vec3,
            "Initial world position in metres; the origin when omitted.",
            Value::Vec3([0.0, 0.0, 0.0]),
        )),
        |context, arguments| {
            let asset = text(arguments, "asset");
            cy_editor_services::vfx_document::validate_reference(asset)?;
            let vfx = VfxDocument::decode_text(&host(context)?.vfx_document_read(asset)?)?;
            let document_id = context.active_document().ok_or_else(|| {
                Problem::new("create a scene effect", "no scene document is open")
            })?;
            let actor = context.actor();
            let at = match arguments.get("at") {
                Some(Value::Vec3(position)) => *position,
                _ => [0.0, 0.0, 0.0],
            };
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the open scene"))?;
            let node = document.with_transaction("Create VFX Effect", actor, |document| {
                let node = document.create_node(None)?;
                document.set_name(node, &vfx.name)?;
                add_transform(document, node, at)?;
                add_scene_effect(document, node, asset, &vfx)?;
                Ok(node)
            })?;
            let mut selection = Selection::new();
            selection.add_node(node);
            context.set_selection(selection);
            Ok(Outcome::new(format!("Created VFX effect {}", vfx.name))
                .with("entity", Value::Text(node.to_string())))
        },
    )
}

fn set_scene_effect_parameter() -> Command {
    Command::new(
        Metadata::new(
            "scene.vfx-effect.parameter.set",
            "Set Scene VFX Parameter",
            "Scene",
            "Overrides one exposed parameter on one saved VFX effect entity.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "entity",
            ValueKind::Text,
            "Hexadecimal identity of the scene effect entity to change.",
        ))
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Declared system or emitter-local parameter name.",
        ))
        .with(ParameterSpec::optional(
            "emitter",
            ValueKind::Text,
            "Owning emitter name; empty for a shared system parameter.",
            Value::Text(String::new()),
        ))
        .with(ParameterSpec::required(
            "values",
            ValueKind::Vec4,
            "Override components in engine order, with unused lanes zeroed.",
        )),
        apply_scene_effect_parameter,
    )
}

fn scene_effect_asset(document: &Document, entity: NodeId) -> Result<String> {
    let component = document
        .schema()
        .type_named(EFFECT_COMPONENT)
        .ok_or_else(|| Problem::new("set a scene VFX parameter", "scene has no VFX effects"))?;
    let field = component
        .field_named("asset")
        .ok_or_else(|| Problem::new("set a scene VFX parameter", "effect has no asset field"))?;
    match document.content().field(entity, component.id, field.id) {
        Some(Value::Text(asset)) => Ok(asset.clone()),
        _ => Err(Problem::new(
            "set a scene VFX parameter",
            "entity is not a VFX effect",
        )),
    }
}

fn scene_effect_declaration<'a>(
    vfx: &'a VfxDocument,
    emitter: &str,
    name: &str,
) -> Result<&'a Parameter> {
    let found = if emitter.is_empty() {
        vfx.parameters
            .iter()
            .find(|parameter| parameter.name == name)
    } else {
        vfx.emitter_parameters
            .iter()
            .find(|entry| entry.emitter == emitter && entry.parameter.name == name)
            .map(|entry| &entry.parameter)
    };
    let declaration = found
        .ok_or_else(|| Problem::new("set a scene VFX parameter", "parameter is not declared"))?;
    if !declaration.exposed {
        return Err(Problem::new(
            "set a scene VFX parameter",
            "parameter is folded at cook time",
        ));
    }
    Ok(declaration)
}

fn apply_scene_effect_parameter(
    context: &mut dyn CommandContext,
    arguments: &Arguments,
) -> Result<Outcome> {
    let entity = u128::from_str_radix(text(arguments, "entity"), 16)
        .map(NodeId::from_u128)
        .map_err(|_| Problem::new("set a scene VFX parameter", "invalid entity identity"))?;
    let emitter = text(arguments, "emitter");
    let name = text(arguments, "name");
    let Some(Value::Vec4(values)) = arguments.get("values") else {
        return Err(Problem::new(
            "set a scene VFX parameter",
            "four components are required",
        ));
    };
    let document_id = context
        .active_document()
        .ok_or_else(|| Problem::new("set a scene VFX parameter", "no scene document is open"))?;
    let document = context
        .document(document_id)
        .ok_or_else(|| Problem::not_found("the open scene"))?;
    let asset = scene_effect_asset(document, entity)?;
    let vfx = VfxDocument::decode_text(&host(context)?.vfx_document_read(&asset)?)?;
    let declaration = scene_effect_declaration(&vfx, emitter, name)?;
    let mut override_value = declaration.clone();
    override_value.value = *values;
    let (kind, value) = scene_parameter_value(&override_value)?;
    let field_name = scene_parameter_name((!emitter.is_empty()).then_some(emitter), declaration);
    let actor = context.actor();
    let document = context
        .document_mut(document_id)
        .ok_or_else(|| Problem::not_found("the open scene"))?;
    let component = document
        .schema()
        .type_named(EFFECT_COMPONENT)
        .ok_or_else(|| Problem::new("set a scene VFX parameter", "scene has no VFX effects"))?;
    let field = component.field_named(&field_name).ok_or_else(|| {
        Problem::new(
            "set a scene VFX parameter",
            "effect field is not in the scene schema",
        )
    })?;
    if field.kind != kind
        || document
            .content()
            .field(entity, component.id, field.id)
            .is_none()
    {
        return Err(Problem::new(
            "set a scene VFX parameter",
            "effect field is not present on this entity",
        ));
    }
    let component_id = component.id;
    let field_id = field.id;
    document.with_transaction("Set Scene VFX Parameter", actor, |document| {
        document.set_field(entity, component_id, field_id, value)
    })?;
    Ok(Outcome::new(format!("Set scene VFX parameter {name}")))
}

fn set_emitter_capacity() -> Command {
    Command::new(
        metadata(
            "vfx.emitter.capacity.set",
            "Set VFX Emitter Capacity",
            "Sets the maximum live particle count of one saved emitter.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter whose capacity will change.",
        ))
        .with(ParameterSpec::required(
            "capacity",
            ValueKind::Int,
            "Positive maximum number of live particles at full quality.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter = text(arguments, "emitter");
            let capacity = positive_u32(arguments, "capacity")?;
            edit_document(context, reference, |document, _| {
                let index = emitter_index(document, emitter)?;
                document.emitters[index].capacity = capacity;
                Ok(Outcome::new(format!(
                    "Set {emitter} capacity to {capacity}"
                )))
            })
        },
    )
}

fn set_attribute() -> Command {
    Command::new(
        metadata(
            "vfx.attribute.set",
            "Set VFX Attribute",
            "Creates or updates a typed particle attribute on one saved emitter.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter storing the particle attribute.",
        ))
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Attribute identifier used by the emitter's graph nodes.",
        ))
        .with(ParameterSpec::required(
            "kind",
            ValueKind::Text,
            "Engine numeric type of the particle attribute.",
        ))
        .with(ParameterSpec::required(
            "minimum",
            ValueKind::Float,
            "Lower bound of the authored attribute range.",
        ))
        .with(ParameterSpec::required(
            "maximum",
            ValueKind::Float,
            "Upper bound of the authored attribute range.",
        ))
        .with(ParameterSpec::required(
            "tolerance",
            ValueKind::Float,
            "Maximum acceptable storage error for this attribute.",
        ))
        .with(ParameterSpec::required(
            "precision",
            ValueKind::Text,
            "Storage policy: Auto, Float32, Float16, Unorm8, or Snorm16.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter = text(arguments, "emitter");
            let name = text(arguments, "name");
            let attribute = Attribute {
                name: name.into(),
                kind: text(arguments, "kind").into(),
                minimum: arguments
                    .get("minimum")
                    .and_then(Value::as_float)
                    .unwrap_or_default(),
                maximum: arguments
                    .get("maximum")
                    .and_then(Value::as_float)
                    .unwrap_or_default(),
                tolerance: arguments
                    .get("tolerance")
                    .and_then(Value::as_float)
                    .unwrap_or_default(),
                precision: text(arguments, "precision").into(),
            };
            edit_document(context, reference, |document, _| {
                let index = emitter_index(document, emitter)?;
                let attributes = &mut document.emitters[index].attributes;
                if let Some(existing) = attributes.iter_mut().find(|entry| entry.name == name) {
                    *existing = attribute;
                } else {
                    attributes.push(attribute);
                }
                Ok(Outcome::new(format!("Set {name} on VFX emitter {emitter}")))
            })
        },
    )
}

fn remove_attribute() -> Command {
    Command::new(
        metadata(
            "vfx.attribute.remove",
            "Remove VFX Attribute",
            "Removes one particle attribute from a saved emitter.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Name of the emitter storing the particle attribute.",
        ))
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Identifier of the particle attribute to remove.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter = text(arguments, "emitter");
            let name = text(arguments, "name");
            edit_document(context, reference, |document, _| {
                let index = emitter_index(document, emitter)?;
                let attributes = &mut document.emitters[index].attributes;
                let position = attributes
                    .iter()
                    .position(|entry| entry.name == name)
                    .ok_or_else(|| {
                        Problem::new(
                            "remove a VFX attribute",
                            format!("{name} does not exist on {emitter}"),
                        )
                    })?;
                attributes.remove(position);
                Ok(Outcome::new(format!(
                    "Removed {name} from VFX emitter {emitter}"
                )))
            })
        },
    )
}

fn set_channel() -> Command {
    Command::new(
        metadata(
            "vfx.channel.set",
            "Set VFX Event Channel",
            "Creates or updates a bounded system event channel.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Event channel identifier used by graph nodes.",
        ))
        .with(ParameterSpec::required(
            "max_events_per_frame",
            ValueKind::Int,
            "Positive maximum number of events emitted in one frame.",
        ))
        .with(ParameterSpec::required(
            "max_chain_depth",
            ValueKind::Int,
            "Positive maximum event propagation depth.",
        ))
        .with(ParameterSpec::required(
            "readback",
            ValueKind::Bool,
            "Whether the channel may be read back on the CPU.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            let channel = EventChannel {
                name: name.into(),
                max_events_per_frame: positive_u32(arguments, "max_events_per_frame")?,
                max_chain_depth: positive_u32(arguments, "max_chain_depth")?,
                readback: matches!(arguments.get("readback"), Some(Value::Bool(true))),
            };
            edit_document(context, reference, |document, _| {
                if let Some(existing) = document
                    .channels
                    .iter_mut()
                    .find(|entry| entry.name == name)
                {
                    *existing = channel;
                } else {
                    document.channels.push(channel);
                }
                Ok(Outcome::new(format!("Set VFX event channel {name}")))
            })
        },
    )
}

fn remove_channel() -> Command {
    Command::new(
        metadata(
            "vfx.channel.remove",
            "Remove VFX Event Channel",
            "Removes one bounded event channel from a saved system.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Identifier of the event channel to remove.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            edit_document(context, reference, |document, _| {
                let position = document
                    .channels
                    .iter()
                    .position(|entry| entry.name == name)
                    .ok_or_else(|| {
                        Problem::new(
                            "remove a VFX event channel",
                            format!("{name} does not exist"),
                        )
                    })?;
                document.channels.remove(position);
                Ok(Outcome::new(format!("Removed VFX event channel {name}")))
            })
        },
    )
}

fn create_module() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.create",
            "Create VFX Module",
            "Creates a separately saved reusable stage module with undo history.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Unique module identifier used by systems and other modules.",
        ))
        .with(ParameterSpec::required(
            "stage",
            ValueKind::Text,
            "Engine stage in which the module may run.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            let module = VfxModule::new(name, stage(arguments)?)?;
            within_scope(context, reference)?;
            let project = host(context)?;
            if project.vfx_module_exists(reference) {
                return Err(Problem::new(
                    "create a VFX module",
                    format!("{reference} already contains a module"),
                ));
            }
            project.vfx_module_save(reference, &module.encode_text()?)?;
            Ok(Outcome::new(format!("Created VFX module {name}")))
        },
    )
}

fn set_module_stage() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.stage.set",
            "Set VFX Module Stage",
            "Changes the compatible stage of a saved reusable module with undo history.",
        )
        .with(ParameterSpec::required(
            "stage",
            ValueKind::Text,
            "Engine stage in which the module may run.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let selected = stage(arguments)?;
            edit_module(context, reference, |module| {
                module.stage = selected;
                Ok(Outcome::new(format!(
                    "Set VFX module {} stage to {}",
                    module.name,
                    selected.label()
                )))
            })
        },
    )
}

fn add_module_input() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.input.add",
            "Add VFX Module Input",
            "Declares a typed emitter attribute read by a reusable module.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Attribute identifier expected from the emitter.",
        ))
        .with(ParameterSpec::required(
            "kind",
            ValueKind::Text,
            "Engine numeric type of the attribute.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            let kind = text(arguments, "kind");
            edit_module(context, reference, |module| {
                module.inputs.push(ModuleInput {
                    name: name.into(),
                    kind: kind.into(),
                });
                Ok(Outcome::new(format!("Added module input {name}")))
            })
        },
    )
}

fn remove_module_input() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.input.remove",
            "Remove VFX Module Input",
            "Removes one declared host attribute from a saved reusable module.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Identifier of the module host input to remove.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            edit_module(context, reference, |module| {
                let position = module
                    .inputs
                    .iter()
                    .position(|entry| entry.name == name)
                    .ok_or_else(|| {
                        Problem::new(
                            "remove a VFX module input",
                            format!("{name} does not exist"),
                        )
                    })?;
                module.inputs.remove(position);
                Ok(Outcome::new(format!("Removed VFX module input {name}")))
            })
        },
    )
}

fn add_module_dependency() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.dependency.add",
            "Add VFX Module Dependency",
            "Declares another reusable module this module needs.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Identifier of the required module.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            edit_module(context, reference, |module| {
                module.dependencies.push(name.into());
                Ok(Outcome::new(format!("Added module dependency {name}")))
            })
        },
    )
}

fn remove_module_dependency() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.dependency.remove",
            "Remove VFX Module Dependency",
            "Removes one named dependency from a saved reusable module.",
        )
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "Identifier of the module dependency to remove.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let name = text(arguments, "name");
            edit_module(context, reference, |module| {
                let position = module
                    .dependencies
                    .iter()
                    .position(|entry| entry == name)
                    .ok_or_else(|| {
                        Problem::new(
                            "remove a VFX module dependency",
                            format!("{name} does not exist"),
                        )
                    })?;
                module.dependencies.remove(position);
                Ok(Outcome::new(format!(
                    "Removed VFX module dependency {name}"
                )))
            })
        },
    )
}

fn add_module_node() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.node.add",
            "Add VFX Module Node",
            "Places an engine-catalogue node on a saved reusable module graph.",
        )
        .with(ParameterSpec::required(
            "node_type",
            ValueKind::Text,
            "Node type from the engine VFX catalogue.",
        ))
        .with(ParameterSpec::required(
            "x",
            ValueKind::Float,
            "Horizontal position on the shared canvas.",
        ))
        .with(ParameterSpec::required(
            "y",
            ValueKind::Float,
            "Vertical position on the shared canvas.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let node_type = text(arguments, "node_type");
            let x = arguments
                .get("x")
                .and_then(Value::as_float)
                .unwrap_or_default();
            let y = arguments
                .get("y")
                .and_then(Value::as_float)
                .unwrap_or_default();
            if !x.is_finite() || !y.is_finite() {
                return Err(Problem::new(
                    "place a VFX module node",
                    "canvas position must be finite",
                ));
            }
            edit_module_canvas(context, reference, |canvas| {
                let node = canvas.add(node_type, Layout { x, y })?;
                Ok(
                    Outcome::new(format!("Added {node_type} to VFX module")).with(
                        "node",
                        Value::Int(i64::try_from(node.ordinal()).unwrap_or(i64::MAX)),
                    ),
                )
            })
        },
    )
}

fn move_module_node() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.node.move",
            "Move VFX Module Node",
            "Moves a node on a saved reusable module graph without changing its connections.",
        )
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the module node to move.",
        ))
        .with(ParameterSpec::required(
            "x",
            ValueKind::Float,
            "Horizontal canvas position.",
        ))
        .with(ParameterSpec::required(
            "y",
            ValueKind::Float,
            "Vertical canvas position.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let node = node_key(arguments, "node")?;
            let at = node_layout(arguments)?;
            edit_module_canvas(context, reference, |canvas| {
                canvas.move_to(node, at)?;
                Ok(Outcome::new(format!(
                    "Moved VFX module node {}",
                    node.ordinal()
                )))
            })
        },
    )
}

fn connect_module_nodes() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.node.connect",
            "Connect VFX Module Nodes",
            "Connects engine-typed pins on one saved reusable module graph.",
        )
        .with(ParameterSpec::required(
            "from",
            ValueKind::Int,
            "Stable key of the node providing the output value.",
        ))
        .with(ParameterSpec::required(
            "from_pin",
            ValueKind::Text,
            "Engine-declared source output pin.",
        ))
        .with(ParameterSpec::required(
            "to",
            ValueKind::Int,
            "Stable key of the node receiving the input value.",
        ))
        .with(ParameterSpec::required(
            "to_pin",
            ValueKind::Text,
            "Engine-declared destination input pin.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let from = node_key(arguments, "from")?;
            let to = node_key(arguments, "to")?;
            let from_pin = text(arguments, "from_pin");
            let to_pin = text(arguments, "to_pin");
            edit_module_canvas(context, reference, |canvas| {
                canvas.connect(from, from_pin, to, to_pin)?;
                Ok(Outcome::new(format!(
                    "Connected VFX module nodes {} and {}",
                    from.ordinal(),
                    to.ordinal()
                )))
            })
        },
    )
}

fn disconnect_module_nodes() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.node.disconnect",
            "Disconnect VFX Module Nodes",
            "Removes one wire from a saved reusable module graph.",
        )
        .with(ParameterSpec::required(
            "from",
            ValueKind::Int,
            "Stable key of the node providing the wire's value.",
        ))
        .with(ParameterSpec::required(
            "from_pin",
            ValueKind::Text,
            "Engine-declared source output pin.",
        ))
        .with(ParameterSpec::required(
            "to",
            ValueKind::Int,
            "Stable key of the node receiving the wire's value.",
        ))
        .with(ParameterSpec::required(
            "to_pin",
            ValueKind::Text,
            "Engine-declared destination input pin.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let from = node_key(arguments, "from")?;
            let to = node_key(arguments, "to")?;
            let from_pin = text(arguments, "from_pin");
            let to_pin = text(arguments, "to_pin");
            edit_module_canvas(context, reference, |canvas| {
                canvas.disconnect(from, from_pin, to, to_pin)?;
                Ok(Outcome::new(format!(
                    "Disconnected VFX module nodes {} and {}",
                    from.ordinal(),
                    to.ordinal()
                )))
            })
        },
    )
}

fn remove_module_node() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.node.remove",
            "Remove VFX Module Node",
            "Removes a node and its wires from a saved reusable module graph.",
        )
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the node to remove.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let node = node_key(arguments, "node")?;
            edit_module_canvas(context, reference, |canvas| {
                canvas.remove(node)?;
                Ok(Outcome::new(format!(
                    "Removed VFX module node {}",
                    node.ordinal()
                )))
            })
        },
    )
}

fn set_module_node_property() -> Command {
    Command::new(
        module_metadata(
            "vfx.module.node.property.set",
            "Set VFX Module Node Property",
            "Sets an engine-declared property on a saved reusable module node.",
        )
        .with(ParameterSpec::required(
            "node",
            ValueKind::Int,
            "Stable key of the module node whose property will change.",
        ))
        .with(ParameterSpec::required(
            "property",
            ValueKind::Text,
            "Engine-declared property name.",
        ))
        .with(ParameterSpec::required(
            "value",
            ValueKind::Text,
            "Property value in the engine-declared textual form.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let node = node_key(arguments, "node")?;
            let property = text(arguments, "property");
            let value = text(arguments, "value");
            edit_module_canvas(context, reference, |canvas| {
                set_declared_property(canvas, node, property, value)?;
                Ok(Outcome::new(format!(
                    "Set {property} on VFX module node {}",
                    node.ordinal()
                )))
            })
        },
    )
}

fn attach_module() -> Command {
    Command::new(
        metadata(
            "vfx.module.attach",
            "Attach VFX Module",
            "Attaches a saved module asset to an emitter in one undoable system edit.",
        )
        .with(ParameterSpec::required(
            "emitter",
            ValueKind::Text,
            "Emitter to which the module is attached.",
        ))
        .with(ParameterSpec::required(
            "module_reference",
            ValueKind::Text,
            "Project-relative .cyvfxmodule path for the saved module.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference");
            let emitter = text(arguments, "emitter");
            let module_reference = text(arguments, "module_reference");
            edit_document(context, reference, |document, project| {
                let module = VfxModule::decode_text(&project.vfx_module_read(module_reference)?)?;
                let index = emitter_index(document, emitter)?;
                document.attach_module(index, module.name.clone(), module_reference.into())?;
                Ok(Outcome::new(format!(
                    "Attached VFX module {} to {emitter}",
                    module.name
                )))
            })
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::codec::Writer;

    fn engine_catalogue() -> Vec<u8> {
        let mut out = Writer::new();
        out.u32(1); // schema
        out.u32(1); // engine catalogue version
        out.u32(2);
        for (identity, name, pin, direction) in [
            (1, "vfx.constant", "out", 1),
            (2, "vfx.spawn_count", "value", 0),
        ] {
            out.u32(identity);
            out.u32(1); // node schema
            out.text(name);
            out.u32(1); // one pin
            out.u32(1); // pin identity
            out.u8(direction);
            out.text(pin);
            out.text("value");
            if identity == 1 {
                out.u32(1); // properties
                out.u32(1); // property identity
                out.u8(2); // scalar
                out.text("value");
                out.text("0");
                out.text(""); // legacy constraint
                out.text("Constant value");
            } else {
                out.u32(0);
            }
        }
        out.finish()
    }

    fn document() -> VfxDocument {
        let mut document = VfxDocument::new("sparks").unwrap();
        document.emitters.push(Emitter {
            name: "embers".into(),
            path: SimulationPath::CpuRequired,
            renderer: "Sprite".into(),
            stages: Vec::new(),
            modules: Vec::new(),
            interfaces: Vec::new(),
            capacity: 1024,
            attributes: Vec::new(),
        });
        document
    }

    #[test]
    fn catalogue_nodes_and_typed_links_survive_separate_stage_edits_and_reopen() {
        let catalogue = engine_catalogue();
        let mut document = document();
        edit_canvas(
            &mut document,
            &catalogue,
            "embers",
            Stage::Spawn,
            |canvas| {
                canvas.add("vfx.constant", Layout { x: 10.0, y: 20.0 })?;
                Ok(Outcome::new("constant placed"))
            },
        )
        .unwrap();
        edit_canvas(
            &mut document,
            &catalogue,
            "embers",
            Stage::Spawn,
            |canvas| {
                canvas.add("vfx.spawn_count", Layout { x: 80.0, y: 20.0 })?;
                Ok(Outcome::new("spawn count placed"))
            },
        )
        .unwrap();
        edit_canvas(
            &mut document,
            &catalogue,
            "embers",
            Stage::Spawn,
            |canvas| {
                canvas.connect(NodeKey::new(1)?, "out", NodeKey::new(2)?, "value")?;
                Ok(Outcome::new("nodes connected"))
            },
        )
        .unwrap();

        let reopened = VfxDocument::decode_text(&document.encode_text().unwrap()).unwrap();
        let canvas = stage_canvas(&catalogue, &reopened, 0, Stage::Spawn).unwrap();
        assert_eq!(canvas.nodes().count(), 2);
        assert_eq!(canvas.links().count(), 1);
        assert_eq!(
            canvas
                .layout_of(NodeKey::new(1).unwrap())
                .unwrap()
                .x
                .to_bits(),
            10.0_f32.to_bits()
        );
    }

    #[test]
    fn stage_wire_and_node_removal_preserve_other_saved_nodes() {
        let catalogue = engine_catalogue();
        let mut document = document();
        edit_canvas(
            &mut document,
            &catalogue,
            "embers",
            Stage::Spawn,
            |canvas| {
                let from = canvas.add("vfx.constant", Layout::default())?;
                let to = canvas.add("vfx.spawn_count", Layout::default())?;
                canvas.connect(from, "out", to, "value")?;
                Ok(Outcome::new("connected"))
            },
        )
        .unwrap();
        let from = NodeKey::new(1).unwrap();
        let to = NodeKey::new(2).unwrap();
        edit_canvas(
            &mut document,
            &catalogue,
            "embers",
            Stage::Spawn,
            |canvas| {
                canvas.disconnect(from, "out", to, "value")?;
                assert!(canvas.disconnect(from, "out", to, "value").is_err());
                Ok(Outcome::new("disconnected"))
            },
        )
        .unwrap();
        let reopened = VfxDocument::decode_text(&document.encode_text().unwrap()).unwrap();
        let canvas = stage_canvas(&catalogue, &reopened, 0, Stage::Spawn).unwrap();
        assert_eq!(canvas.nodes().count(), 2);
        assert_eq!(canvas.links().count(), 0);

        edit_canvas(
            &mut document,
            &catalogue,
            "embers",
            Stage::Spawn,
            |canvas| {
                canvas.connect(from, "out", to, "value")?;
                canvas.remove(from)?;
                Ok(Outcome::new("removed"))
            },
        )
        .unwrap();
        let reopened = VfxDocument::decode_text(&document.encode_text().unwrap()).unwrap();
        let canvas = stage_canvas(&catalogue, &reopened, 0, Stage::Spawn).unwrap();
        assert_eq!(canvas.nodes().count(), 1);
        assert_eq!(canvas.links().count(), 0);
        assert!(canvas.node(to).is_some());
    }

    #[test]
    fn stage_edits_refuse_nodes_and_pins_absent_from_the_engine_catalogue() {
        let catalogue = engine_catalogue();
        let mut document = document();
        assert!(
            edit_canvas(
                &mut document,
                &catalogue,
                "embers",
                Stage::Spawn,
                |canvas| {
                    canvas.add("vfx.unknown", Layout::default())?;
                    Ok(Outcome::new("unknown node"))
                }
            )
            .is_err()
        );
        assert!(document.emitters[0].stages.is_empty());
        let mut canvas = stage_canvas(&catalogue, &document, 0, Stage::Spawn).unwrap();
        let from = canvas.add("vfx.constant", Layout::default()).unwrap();
        let to = canvas.add("vfx.spawn_count", Layout::default()).unwrap();
        assert!(canvas.connect(from, "wrong", to, "value").is_err());
    }

    #[test]
    fn stage_property_edit_uses_the_engine_property_schema() {
        let catalogue = engine_catalogue();
        let mut document = document();
        edit_canvas(
            &mut document,
            &catalogue,
            "embers",
            Stage::Spawn,
            |canvas| {
                let node = canvas.add("vfx.constant", Layout::default())?;
                set_declared_property(canvas, node, "value", "2.5")?;
                Ok(Outcome::new("constant edited"))
            },
        )
        .unwrap();
        let reopened = VfxDocument::decode_text(&document.encode_text().unwrap()).unwrap();
        let canvas = stage_canvas(&catalogue, &reopened, 0, Stage::Spawn).unwrap();
        let key = NodeKey::new(1).unwrap();
        assert_eq!(
            canvas.resolved_properties(key).get("value"),
            Some(&"2.5".into())
        );
        assert!(
            edit_canvas(
                &mut document,
                &catalogue,
                "embers",
                Stage::Spawn,
                |canvas| {
                    set_declared_property(canvas, key, "unknown", "3")?;
                    Ok(Outcome::new("unknown property"))
                }
            )
            .is_err()
        );
    }

    #[test]
    fn module_canvas_edits_preserve_graph_and_reject_unknown_engine_pins() {
        let catalogue = engine_catalogue();
        let mut module = VfxModule::new("shared_drag", Stage::Update).unwrap();
        let mut canvas = module_canvas(&catalogue, &module).unwrap();
        let from = canvas
            .add("vfx.constant", Layout { x: 10.0, y: 20.0 })
            .unwrap();
        let to = canvas
            .add("vfx.spawn_count", Layout { x: 80.0, y: 20.0 })
            .unwrap();
        canvas.connect(from, "out", to, "value").unwrap();
        set_declared_property(&mut canvas, from, "value", "2.5").unwrap();
        module.capture(&canvas).unwrap();

        let reopened = VfxModule::decode_text(&module.encode_text().unwrap()).unwrap();
        let mut canvas = module_canvas(&catalogue, &reopened).unwrap();
        assert_eq!(canvas.nodes().count(), 2);
        assert_eq!(canvas.links().count(), 1);
        assert_eq!(
            canvas.resolved_properties(from).get("value"),
            Some(&"2.5".into())
        );
        assert!(canvas.connect(from, "unknown", to, "value").is_err());
        canvas.disconnect(from, "out", to, "value").unwrap();
        canvas.remove(from).unwrap();
        module.capture(&canvas).unwrap();
        let reopened = VfxModule::decode_text(&module.encode_text().unwrap()).unwrap();
        let canvas = module_canvas(&catalogue, &reopened).unwrap();
        assert_eq!(canvas.nodes().count(), 1);
        assert_eq!(canvas.links().count(), 0);
        assert!(canvas.node(to).is_some());
    }
}
