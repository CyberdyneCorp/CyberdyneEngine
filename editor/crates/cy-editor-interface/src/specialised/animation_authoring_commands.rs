// SPDX-License-Identifier: MIT
//! Animation graph and clip event edits through the shared command registry: the Animation panel,
//! the palette, scripts and MCP all reach these. Issue #29, animation.
//!
//! Each edit reads the `.cyanimgraph`, opens it on a canvas holding the ENGINE's pose catalogue,
//! makes one change through the canvas's checked operations, captures it and saves it as one
//! undoable transaction in the open world's history — so a gesture on the canvas or the timeline
//! and a tool call from an agent are refused for the same reasons (a pose wired into a state input,
//! a clip the character does not have, a node type the engine lacks) and undo the same way.
//!
//! A clip's events are its node's `events` property. `animation.event.*` changes that one property:
//! the timeline's add, drag and delete of an event key are these three commands.

use cy_editor_commands::{
    Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, ProjectHost,
    Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_services::animation_commands::writable;
use cy_editor_services::animation_graph::{
    CLIP_NODE, ClipEvent, EVENTS, format_events, new_graph, parse_events, validate_event_name,
};
use cy_editor_services::script_graph::ScriptGraph;

use super::animation::catalogue;
use super::graph::{GraphCanvas, Layout, NodeKey};
use super::script::{canvas_for, capture};

const CATEGORY: &str = "Animation";

/// How close a stated time must be to an event's for a move or a removal to mean that event: a
/// millisecond, well inside a frame and far outside the float noise of a written time.
const EVENT_TOLERANCE: f32 = 0.001;

/// Install the animation graph and clip event edits.
///
/// # Errors
///
/// When the registry refuses a command's metadata.
pub fn register(registry: &mut Registry) -> Result<()> {
    for command in [
        create_graph(),
        add_node(),
        move_node(),
        connect_nodes(),
        disconnect_nodes(),
        remove_node(),
        set_property(),
        add_event(),
        move_event(),
        remove_event(),
    ] {
        registry.register(command)?;
    }
    Ok(())
}

fn metadata(id: &str, label: &str, description: &str) -> Metadata {
    Metadata::new(
        id,
        label,
        CATEGORY,
        description,
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "reference",
        ValueKind::Text,
        "Project-relative .cyanimgraph animation graph, for example \
         game/animation/locomotion.cyanimgraph.",
    ))
}

fn text<'a>(arguments: &'a Arguments, name: &str) -> &'a str {
    arguments.text(name).unwrap_or_default()
}

fn node_parameter(name: &'static str, what: &str) -> ParameterSpec {
    ParameterSpec::required(name, ValueKind::Int, format!("Stable key of {what}."))
}

fn node_key(arguments: &Arguments, name: &str) -> Result<NodeKey> {
    let value = arguments
        .get(name)
        .and_then(Value::as_int)
        .unwrap_or_default();
    NodeKey::new(u64::try_from(value).unwrap_or_default())
}

fn float(arguments: &Arguments, name: &str) -> f32 {
    arguments
        .get(name)
        .and_then(Value::as_float)
        .unwrap_or(f32::NAN)
}

fn position(arguments: &Arguments) -> Result<Layout> {
    let (x, y) = (float(arguments, "x"), float(arguments, "y"));
    if !x.is_finite() || !y.is_finite() {
        return Err(Problem::new(
            "place an animation graph node",
            "a canvas position is finite",
        ));
    }
    Ok(Layout { x, y })
}

fn position_parameters(metadata: Metadata) -> Metadata {
    metadata
        .with(ParameterSpec::required(
            "x",
            ValueKind::Float,
            "Horizontal position on the shared canvas.",
        ))
        .with(ParameterSpec::required(
            "y",
            ValueKind::Float,
            "Vertical position on the shared canvas.",
        ))
}

fn host(context: &mut dyn CommandContext) -> Result<&mut dyn ProjectHost> {
    context
        .project()
        .ok_or_else(|| Problem::new("edit an animation graph", "no project is open"))
}

fn catalogue_of(project: &mut dyn ProjectHost) -> Result<super::graph::Catalogue> {
    let payload = project.animation_catalogue().ok_or_else(|| {
        Problem::new(
            "edit an animation graph",
            "the engine's pose catalogue has not arrived; it has been requested",
        )
        .with_remedy("attach a runtime, then try again once it has answered")
    })?;
    catalogue(&payload)
}

/// Read, open on the engine's catalogue, change, capture, save: one undoable transaction.
fn edit_graph(
    context: &mut dyn CommandContext,
    reference: &str,
    edit: impl FnOnce(&mut GraphCanvas) -> Result<Outcome>,
) -> Result<Outcome> {
    writable(context, reference)?;
    let project = host(context)?;
    let catalogue = catalogue_of(project)?;
    if !project.source_exists(reference) {
        return Err(Problem::new(
            format!("edit the animation graph {reference}"),
            "there is no such file",
        )
        .with_remedy("create it with animation.graph.create"));
    }
    let graph = ScriptGraph::decode(&project.read_source(reference)?)?;
    let mut canvas = canvas_for(catalogue, &graph)?;
    let outcome = edit(&mut canvas)?;
    let source = capture(&graph, &canvas)?.encode();
    project.animation_graph_save(reference, &source)?;
    Ok(outcome.with("source", Value::Text(source)))
}

/// Place a node of `node_type` with every property at the engine's default.
fn add_with_defaults(canvas: &mut GraphCanvas, node_type: &str, at: Layout) -> Result<NodeKey> {
    let node = canvas.add(node_type, at)?;
    let defaults: Vec<(u32, String)> = canvas
        .catalogue()
        .get(node_type)
        .map(|declared| {
            declared
                .properties
                .iter()
                .map(|property| (property.identity, property.default.clone()))
                .collect()
        })
        .unwrap_or_default();
    for (identity, default) in defaults {
        canvas.set_property_by_identity(node, identity, default)?;
    }
    Ok(node)
}

fn set_named_property(
    canvas: &mut GraphCanvas,
    node: NodeKey,
    property: &str,
    value: String,
) -> Result<()> {
    let identity = canvas
        .node(node)
        .and_then(|authored| canvas.catalogue().get(&authored.type_name))
        .and_then(|declared| {
            declared
                .properties
                .iter()
                .find(|candidate| candidate.name == property)
        })
        .map(|declared| declared.identity)
        .ok_or_else(|| {
            Problem::new(
                "set an animation graph property",
                format!(
                    "node {} has no engine-declared property {property}",
                    node.ordinal()
                ),
            )
        })?;
    canvas.set_property_by_identity(node, identity, value)
}

fn ordinal(key: NodeKey) -> Value {
    Value::Int(i64::try_from(key.ordinal()).unwrap_or(0))
}

fn create_graph() -> Command {
    Command::new(
        metadata(
            "animation.graph.create",
            "Create Animation Graph",
            "Creates an animation graph with one state playing the preview character's first \
             clip, as one undoable transaction.",
        ),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            writable(context, &reference)?;
            let project = host(context)?;
            if project.source_exists(&reference) {
                return Err(Problem::new(
                    "create an animation graph",
                    format!("{reference} already exists"),
                ));
            }
            let catalogue = catalogue_of(project)?;
            let graph = new_graph(&reference);
            let mut canvas = canvas_for(catalogue, &graph)?;
            let clip = add_with_defaults(&mut canvas, CLIP_NODE, Layout { x: 16.0, y: 16.0 })?;
            let state = add_with_defaults(&mut canvas, "pose.state", Layout { x: 230.0, y: 16.0 })?;
            let clip_name = canvas
                .resolved_properties(clip)
                .into_iter()
                .find(|(name, _)| name == "clip")
                .map(|(_, value)| value)
                .unwrap_or_default();
            if !clip_name.is_empty() {
                set_named_property(&mut canvas, state, "name", clip_name.clone())?;
            }
            canvas.connect(clip, "pose", state, "pose")?;
            let source = capture(&graph, &canvas)?.encode();
            project.animation_graph_save(&reference, &source)?;
            Ok(
                Outcome::new(format!("Created {reference}, playing {clip_name}"))
                    .with("clip", ordinal(clip))
                    .with("state", ordinal(state))
                    .with("source", Value::Text(source)),
            )
        },
    )
}

fn add_node() -> Command {
    Command::new(
        position_parameters(
            metadata(
                "animation.node.add",
                "Add Animation Graph Node",
                "Places a node from the engine's pose catalogue — a clip, a blend, a state, a \
                 transition — with every property at the engine's default, as one undoable \
                 transaction.",
            )
            .with(ParameterSpec::required(
                "node_type",
                ValueKind::Text,
                "Node type from the engine's catalogue, such as pose.clip or pose.transition.",
            )),
        ),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node_type = text(arguments, "node_type").to_owned();
            let at = position(arguments)?;
            edit_graph(context, &reference, |canvas| {
                let node = add_with_defaults(canvas, &node_type, at)?;
                Ok(Outcome::new(format!("Added {node_type}")).with("node", ordinal(node)))
            })
        },
    )
}

fn move_node() -> Command {
    Command::new(
        position_parameters(
            metadata(
                "animation.node.move",
                "Move Animation Graph Node",
                "Moves a node on the canvas. Layout is not meaning: the compiled program does not \
                 change.",
            )
            .with(node_parameter("node", "the node to move")),
        ),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            let at = position(arguments)?;
            edit_graph(context, &reference, |canvas| {
                canvas.move_to(node, at)?;
                Ok(Outcome::new(format!("Moved node {}", node.ordinal())))
            })
        },
    )
}

fn wire_parameters(metadata: Metadata) -> Metadata {
    metadata
        .with(node_parameter("from", "the node the wire leaves"))
        .with(ParameterSpec::required(
            "from_pin",
            ValueKind::Text,
            "The output pin the wire leaves by: a clip's or blend's pose, a state's state.",
        ))
        .with(node_parameter("to", "the node the wire enters"))
        .with(ParameterSpec::required(
            "to_pin",
            ValueKind::Text,
            "The input pin the wire enters by: a blend's a or b, a state's pose, a transition's \
             from or to.",
        ))
}

fn wire_ends(arguments: &Arguments) -> Result<(NodeKey, String, NodeKey, String)> {
    Ok((
        node_key(arguments, "from")?,
        text(arguments, "from_pin").to_owned(),
        node_key(arguments, "to")?,
        text(arguments, "to_pin").to_owned(),
    ))
}

fn connect_nodes() -> Command {
    Command::new(
        wire_parameters(metadata(
            "animation.node.connect",
            "Connect Animation Graph Nodes",
            "Wires an output pin to an input pin of the same engine type, as one undoable \
             transaction: a pose into a blend or a state, or a state into a transition's from or \
             to.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let (from, from_pin, to, to_pin) = wire_ends(arguments)?;
            edit_graph(context, &reference, |canvas| {
                canvas.connect(from, &from_pin, to, &to_pin)?;
                Ok(Outcome::new(format!(
                    "Wired node {} {from_pin} to node {} {to_pin}",
                    from.ordinal(),
                    to.ordinal()
                )))
            })
        },
    )
}

fn disconnect_nodes() -> Command {
    Command::new(
        wire_parameters(metadata(
            "animation.node.disconnect",
            "Disconnect Animation Graph Nodes",
            "Removes one wire, as one undoable transaction.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let (from, from_pin, to, to_pin) = wire_ends(arguments)?;
            edit_graph(context, &reference, |canvas| {
                canvas.disconnect(from, &from_pin, to, &to_pin)?;
                Ok(Outcome::new("Removed the wire"))
            })
        },
    )
}

fn remove_node() -> Command {
    Command::new(
        metadata(
            "animation.node.remove",
            "Remove Animation Graph Node",
            "Removes a node and every wire that touched it, as one undoable transaction.",
        )
        .with(node_parameter("node", "the node to remove")),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            edit_graph(context, &reference, |canvas| {
                canvas.remove(node)?;
                Ok(Outcome::new(format!("Removed node {}", node.ordinal())))
            })
        },
    )
}

fn set_property() -> Command {
    Command::new(
        metadata(
            "animation.node.property.set",
            "Set Animation Graph Property",
            "Sets one of a node's engine-declared properties — a clip, a state's name, a \
             transition's condition or blend length — as one undoable transaction. A clip the \
             preview character does not have is refused.",
        )
        .with(node_parameter("node", "the node whose property changes"))
        .with(ParameterSpec::required(
            "property",
            ValueKind::Text,
            "The property's name, as the engine declares it.",
        ))
        .with(ParameterSpec::required(
            "value",
            ValueKind::Text,
            "The value to write, as the property's engine-declared kind reads it.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            let property = text(arguments, "property").to_owned();
            let value = text(arguments, "value").to_owned();
            if property == EVENTS {
                parse_events(&value)?;
            }
            edit_graph(context, &reference, |canvas| {
                set_named_property(canvas, node, &property, value.clone())?;
                Ok(Outcome::new(format!(
                    "Set node {} {property} to {value}",
                    node.ordinal()
                )))
            })
        },
    )
}

// --- Events ---------------------------------------------------------------------------------------

/// The events of clip node `node` on `canvas`, refused when it is not a clip node.
fn events_of(canvas: &GraphCanvas, node: NodeKey) -> Result<Vec<ClipEvent>> {
    let is_clip = canvas
        .node(node)
        .is_some_and(|authored| authored.type_name == CLIP_NODE);
    if !is_clip {
        return Err(Problem::new(
            "place an animation event",
            format!(
                "node {} is not a pose.clip node: events belong to a clip",
                node.ordinal()
            ),
        ));
    }
    let text = canvas
        .resolved_properties(node)
        .into_iter()
        .find(|(name, _)| name == EVENTS)
        .map(|(_, value)| value)
        .unwrap_or_default();
    parse_events(&text)
}

fn write_events(canvas: &mut GraphCanvas, node: NodeKey, events: &[ClipEvent]) -> Result<()> {
    set_named_property(canvas, node, EVENTS, format_events(events))
}

fn event_time(arguments: &Arguments, name: &str) -> Result<f32> {
    let time = float(arguments, name);
    if time.is_finite() && time >= 0.0 {
        Ok(time)
    } else {
        Err(Problem::new(
            "place an animation event",
            "an event's time is a finite number of seconds, zero or more",
        ))
    }
}

/// The index of the event named `name` within [`EVENT_TOLERANCE`] of `time`.
fn find_event(events: &[ClipEvent], name: &str, time: f32) -> Result<usize> {
    events
        .iter()
        .enumerate()
        .filter(|(_, event)| event.name == name && (event.time - time).abs() <= EVENT_TOLERANCE)
        .min_by(|(_, a), (_, b)| (a.time - time).abs().total_cmp(&(b.time - time).abs()))
        .map(|(index, _)| index)
        .ok_or_else(|| {
            Problem::new(
                format!("find the event {name} at {time} s"),
                "the clip has no event of that name at that time",
            )
            .with_remedy("read the graph with animation.graph.read for its events")
        })
}

fn refuse_duplicate(events: &[ClipEvent], name: &str, time: f32) -> Result<()> {
    if events
        .iter()
        .any(|event| event.name == name && (event.time - time).abs() <= EVENT_TOLERANCE)
    {
        return Err(Problem::new(
            format!("place the event {name} at {time} s"),
            "the clip already has that event at that time, and a second would fire twice",
        ));
    }
    Ok(())
}

fn event_parameters(metadata: Metadata) -> Metadata {
    metadata
        .with(node_parameter(
            "node",
            "the pose.clip node whose clip the event is on",
        ))
        .with(ParameterSpec::required(
            "event",
            ValueKind::Text,
            "The event's name: footstep, hit.land.",
        ))
}

fn add_event() -> Command {
    Command::new(
        event_parameters(metadata(
            "animation.event.add",
            "Add Animation Event",
            "Places an event on a clip at a time, as one undoable transaction. The engine fires it \
             when the clip's playback crosses that time.",
        ))
        .with(ParameterSpec::required(
            "time",
            ValueKind::Float,
            "Seconds from the clip's start.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            let name = text(arguments, "event").to_owned();
            validate_event_name(&name)?;
            let time = event_time(arguments, "time")?;
            edit_graph(context, &reference, |canvas| {
                let mut events = events_of(canvas, node)?;
                refuse_duplicate(&events, &name, time)?;
                events.push(ClipEvent {
                    name: name.clone(),
                    time,
                });
                write_events(canvas, node, &events)?;
                Ok(Outcome::new(format!("Placed {name} at {time} s")))
            })
        },
    )
}

fn move_event() -> Command {
    Command::new(
        event_parameters(metadata(
            "animation.event.move",
            "Move Animation Event",
            "Moves one of a clip's events to another time, as one undoable transaction.",
        ))
        .with(ParameterSpec::required(
            "from",
            ValueKind::Float,
            "The time the event is at, in seconds.",
        ))
        .with(ParameterSpec::required(
            "to",
            ValueKind::Float,
            "The time to move it to, in seconds.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            let name = text(arguments, "event").to_owned();
            let (from, to) = (event_time(arguments, "from")?, event_time(arguments, "to")?);
            edit_graph(context, &reference, |canvas| {
                let mut events = events_of(canvas, node)?;
                let index = find_event(&events, &name, from)?;
                events.remove(index);
                refuse_duplicate(&events, &name, to)?;
                events.push(ClipEvent {
                    name: name.clone(),
                    time: to,
                });
                write_events(canvas, node, &events)?;
                Ok(Outcome::new(format!(
                    "Moved {name} from {from} s to {to} s"
                )))
            })
        },
    )
}

fn remove_event() -> Command {
    Command::new(
        event_parameters(metadata(
            "animation.event.remove",
            "Remove Animation Event",
            "Removes one of a clip's events, as one undoable transaction.",
        ))
        .with(ParameterSpec::required(
            "time",
            ValueKind::Float,
            "The time the event is at, in seconds.",
        )),
        |context, arguments| {
            let reference = text(arguments, "reference").to_owned();
            let node = node_key(arguments, "node")?;
            let name = text(arguments, "event").to_owned();
            let time = event_time(arguments, "time")?;
            edit_graph(context, &reference, |canvas| {
                let mut events = events_of(canvas, node)?;
                let index = find_event(&events, &name, time)?;
                events.remove(index);
                write_events(canvas, node, &events)?;
                Ok(Outcome::new(format!("Removed {name} at {time} s")))
            })
        },
    )
}
