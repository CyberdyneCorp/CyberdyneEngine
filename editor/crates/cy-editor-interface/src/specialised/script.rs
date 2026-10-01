// SPDX-License-Identifier: MIT
//! A gameplay graph on the one shared canvas. Issue #29, visual scripting.
//!
//! The `.cyscript` file is the document ([`ScriptGraph`]); the canvas is how it is drawn and
//! edited. [`open`] lays a graph onto the canvas with the engine's catalogue loaded, and [`capture`]
//! reads the canvas back into a graph — so a panel gesture and an MCP tool edit the same way: open,
//! change the canvas through its checked operations (a pin of the wrong type, a property outside
//! its choices, a node type the engine does not declare are all refused there), capture, save.
//!
//! A node whose type the loaded catalogue does not declare — a plugin's, absent — is not drawn and
//! is NOT dropped: [`capture`] keeps it, its wires and its layout exactly as the file had them, as
//! `visual-scripting` requires of a graph opened without the plugin that owns a node.

use std::collections::BTreeSet;

use cy_editor_core::problem::{Problem, Result};
use cy_editor_services::script_graph::{Literal, Property, ScriptGraph, ScriptLink, ScriptNode};

use super::graph::{Catalogue, GraphCanvas, Layout, NodeKey, NodeType};
use super::material::catalogue_from_service;

/// The prefix every gameplay graph node type carries.
pub const NODE_PREFIX: &str = "script.";

/// The engine's gameplay graph vocabulary from a `script.catalogue.get` reply.
///
/// # Errors
///
/// An unreadable catalogue, an empty one, or one with a node outside the script vocabulary.
pub fn catalogue(payload: &[u8]) -> Result<Catalogue> {
    let nodes = catalogue_from_service(payload)?;
    if nodes.is_empty() || nodes.iter().any(|node| !node.name.starts_with(NODE_PREFIX)) {
        return Err(Problem::new(
            "load the gameplay graph catalogue",
            "the engine supplied no gameplay graph nodes, or a node outside the script vocabulary",
        ));
    }
    Catalogue::new(nodes)
}

/// The literal type a catalogue property is written at in the graph's text.
#[must_use]
pub fn literal_type(property: &super::graph::Property) -> &str {
    property.semantic.strip_prefix("literal:").unwrap_or("name")
}

fn key_of(key: u64) -> Result<NodeKey> {
    NodeKey::new(key)
}

/// Lay `graph` onto `canvas`, which already holds the engine's catalogue. Replaces what the canvas
/// held.
///
/// # Errors
///
/// A property value or a wire the catalogue refuses: the file says something the engine's
/// vocabulary does not allow.
pub fn open(graph: &ScriptGraph, canvas: &mut GraphCanvas) -> Result<()> {
    canvas.load(canvas.catalogue().clone());
    for node in graph.nodes.values() {
        let Some(node_type) = canvas.catalogue().get(&node.type_name).cloned() else {
            continue;
        };
        let (x, y) = graph
            .layout
            .get(&node.key)
            .map_or((0.0, 0.0), |(x, y, _)| (*x, *y));
        let key = key_of(node.key)?;
        canvas.add_with_key(key, &node.type_name, Layout { x, y })?;
        for property in &node.properties {
            set_property(canvas, &node_type, key, property)?;
        }
    }
    for link in &graph.links {
        let (Ok(from), Ok(to)) = (key_of(link.from), key_of(link.to)) else {
            continue;
        };
        if canvas.node(from).is_some() && canvas.node(to).is_some() {
            canvas.connect(from, &link.from_pin, to, &link.to_pin)?;
        }
    }
    Ok(())
}

fn set_property(
    canvas: &mut GraphCanvas,
    node_type: &NodeType,
    key: NodeKey,
    property: &Property,
) -> Result<()> {
    match node_type
        .properties
        .iter()
        .find(|declared| declared.name == property.name)
    {
        Some(declared) => canvas.set_property_by_identity(
            key,
            declared.identity,
            property.literal.display(literal_type(declared)),
        ),
        None => canvas.set_property(
            key,
            &property.name,
            property.literal.display(&property.literal_type),
        ),
    }
}

/// Read `canvas` back into `graph`, keeping what the canvas could not show.
///
/// # Errors
///
/// A value the canvas holds that does not write at its literal type.
pub fn capture(graph: &ScriptGraph, canvas: &GraphCanvas) -> Result<ScriptGraph> {
    let catalogue = canvas.catalogue();
    let kept: BTreeSet<u64> = graph
        .nodes
        .values()
        .filter(|node| catalogue.get(&node.type_name).is_none())
        .map(|node| node.key)
        .collect();
    let mut next = graph.clone();
    next.nodes.retain(|key, _| kept.contains(key));
    next.layout.retain(|key, _| kept.contains(key));
    next.links
        .retain(|link| kept.contains(&link.from) || kept.contains(&link.to));
    for node in canvas.nodes() {
        let key = node.key.ordinal();
        let previous = graph.nodes.get(&key);
        let node_type = catalogue.get(&node.type_name);
        let mut properties = Vec::new();
        for (name, value) in canvas.resolved_properties(node.key) {
            let declared = node_type
                .and_then(|node_type| node_type.properties.iter().find(|p| p.name == name));
            let literal_type = declared.map_or_else(
                || {
                    previous
                        .and_then(|previous| previous.property(&name))
                        .map_or_else(|| "name".to_owned(), |p| p.literal_type.clone())
                },
                |declared| literal_type(declared).to_owned(),
            );
            let literal = Literal::parse(&literal_type, &value)?;
            properties.push(Property {
                name,
                literal_type,
                literal,
            });
        }
        next.nodes.insert(
            key,
            ScriptNode {
                key,
                type_name: node.type_name.clone(),
                version: previous.map_or_else(
                    || node_type.map_or(1, |node_type| node_type.schema_version),
                    |previous| previous.version,
                ),
                muted: previous.is_some_and(|previous| previous.muted),
                properties,
                opaque: None,
            },
        );
        let layout = canvas.layout_of(node.key).unwrap_or_default();
        let rest = graph
            .layout
            .get(&key)
            .map(|(_, _, rest)| rest.clone())
            .unwrap_or_default();
        next.layout.insert(key, (layout.x, layout.y, rest));
    }
    for link in canvas.links() {
        next.links.insert(ScriptLink {
            to: link.to.ordinal(),
            to_pin: link.to_pin.clone(),
            from: link.from.ordinal(),
            from_pin: link.from_pin.clone(),
        });
    }
    Ok(next)
}

/// A canvas holding `graph` under `catalogue`, for an edit that is not the panel's own canvas.
///
/// # Errors
///
/// As [`open`].
pub fn canvas_for(catalogue: Catalogue, graph: &ScriptGraph) -> Result<GraphCanvas> {
    let mut canvas = GraphCanvas::new(1);
    canvas.load(catalogue);
    open(graph, &mut canvas)?;
    Ok(canvas)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine_fixture(name: &str) -> Vec<u8> {
        let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../src/editor_backend/tests/data")
            .join(name);
        std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
    }

    #[test]
    fn the_engines_catalogue_opens_and_its_graph_round_trips_through_the_canvas() {
        let catalogue = catalogue(&engine_fixture("script_catalogue_v1.wire")).unwrap();
        assert!(catalogue.get("script.on_event").is_some());
        assert!(catalogue.get("script.entry").is_none());
        let source = String::from_utf8(engine_fixture("script_unit_command_v1.cyscript")).unwrap();
        let graph = ScriptGraph::decode(&source).unwrap();
        let canvas = canvas_for(catalogue, &graph).unwrap();
        assert_eq!(canvas.nodes().count(), 6);
        assert_eq!(canvas.links().count(), 5);
        assert_eq!(capture(&graph, &canvas).unwrap().encode(), source);
    }

    #[test]
    fn a_node_the_catalogue_lacks_is_kept_with_its_wires() {
        let catalogue = catalogue(&engine_fixture("script_catalogue_v1.wire")).unwrap();
        let source = String::from_utf8(engine_fixture("script_unit_command_v1.cyscript")).unwrap();
        let mut graph = ScriptGraph::decode(&source).unwrap();
        graph.nodes.get_mut(&6).unwrap().type_name = "plugin.fireworks".into();
        let canvas = canvas_for(catalogue, &graph).unwrap();
        assert_eq!(canvas.nodes().count(), 5);
        let captured = capture(&graph, &canvas).unwrap();
        assert_eq!(captured.nodes[&6].type_name, "plugin.fireworks");
        assert!(captured.links.iter().any(|link| link.to == 6));
        assert_eq!(captured.encode(), graph.encode());
    }

    #[test]
    fn a_catalogue_outside_the_script_vocabulary_is_refused() {
        assert!(catalogue(&[]).is_err());
    }
}
