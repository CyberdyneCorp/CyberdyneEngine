// SPDX-License-Identifier: MIT
//! VFX hierarchy and canvas edits exposed through the editor's shared command registry.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, ProjectHost,
    Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_services::authoring::within_scope;

use super::graph::{Catalogue, GraphCanvas, Layout, NodeKey};
use super::material::catalogue_from_service;
use super::vfx::{Emitter, Parameter, SimulationPath, Stage, VfxDocument};
use super::vfx_module::{ModuleInput, VfxModule};

/// Install the same VFX actions for the command palette, scripts, and MCP projection.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(add_emitter())?;
    registry.register(configure_emitter())?;
    registry.register(bind_interface())?;
    registry.register(unbind_interface())?;
    registry.register(add_node())?;
    registry.register(connect_nodes())?;
    registry.register(disconnect_nodes())?;
    registry.register(remove_node())?;
    registry.register(set_node_property())?;
    registry.register(set_parameter())?;
    registry.register(create_module())?;
    registry.register(add_module_input())?;
    registry.register(add_module_dependency())?;
    registry.register(attach_module())?;
    Ok(())
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

fn create_module() -> Command {
    Command::new(
        metadata(
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

fn add_module_input() -> Command {
    Command::new(
        metadata(
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

fn add_module_dependency() -> Command {
    Command::new(
        metadata(
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
}
