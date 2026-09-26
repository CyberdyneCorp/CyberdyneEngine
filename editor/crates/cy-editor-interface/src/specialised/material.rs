//! The material editor's own half: the palette's pins, and what a finished canvas hands the engine.
//! M11.c task 6.1a.
//!
//! --- WHAT THIS CLOSES ------------------------------------------------------------------------------
//!
//! M11.c's spike ran the rung's whole authoring path and junction 1 — AUTHOR — came back **REFUSED**:
//!
//! > `SpecialisedEditors::open(Domain::Materials)` returns *"this build declares no authoring
//! > vocabulary for materials — `material-compiler` owes it"*
//!
//! The engine now declares one (`src/graph/material/src/lower_material.cpp`), and
//! [`Domain::node_types`](super::Domain::node_types) offers it. That is enough to OPEN the editor and
//! not enough to AUTHOR in it: [`GraphCanvas::connect`](super::graph::GraphCanvas::connect) refuses a
//! pin the node type does not declare, and the catalogues this crate built carried
//! `NodeType::new(name, Vec::new())` — no pins at all. A material editor whose nodes cannot be wired
//! is the same kind of thing as one that will not open.
//!
//! The desktop editor no longer uses that copied table: it starts without a material vocabulary and
//! installs `material.catalogue.get` from the backend. The table below remains only for the
//! explicitly named legacy `cy-author-material` content generator, so the committed M11.c beauty
//! inputs remain reproducible until that offline tool also becomes a service client.
//!
//! --- WHY THE EDITOR DOES NOT WRITE `.cygraph` ------------------------------------------------------
//!
//! [`super::graph`]'s own header assigns that to M11.e, and the reason is not scheduling:
//!
//! > **The on-disk text form.** `src/graph/include/cy/graph/text.h` owns it and it is canonical […]
//! > This crate does not re-implement it and must not: a second writer of a canonical format is a
//! > second format the day the two disagree about a float.
//!
//! That stands. What this module writes is an **interchange** — a line-per-fact listing of the canvas
//! that the engine reads and immediately canonicalises with its own writer. `cy_material author`
//! turns it into the `.cygraph` that is committed, so there is still exactly one writer of the
//! canonical form and it is the engine's. The same arrangement M8.a chose for `.cyprim`: the editor
//! writes a source, the engine owns what it becomes.
//!
//! A project may retain this interchange beside the canonical graph as a canvas source for the
//! editor. The engine still owns the canonical `.cygraph` writer and compilation semantics.

use std::fmt::Write as _;

use cy_editor_core::codec::Reader;
use cy_editor_core::problem::{Problem, Result};

use super::graph::{
    Catalogue, GraphCanvas, Layout, NodeKey, NodeType, Pin, PinDirection, Property, PropertyKind,
};

/// The interchange's own version, written on its first line.
pub const INTERCHANGE_VERSION: u32 = 1;
/// Engine material catalogue bit for surface graph nodes.
pub const SURFACE_STAGE: u8 = 1;
/// Engine material catalogue bit for vertex graph nodes.
pub const VERTEX_STAGE: u8 = 2;

/// The pin type a numeric material pin carries.
///
/// Two pin types and not a lattice, which is [`lower_material.cpp`'s](
/// https://example.invalid) own decision and is stated there: the WIDTH of a number is derived by the
/// material IR from what is wired into it, so a pin that called itself `float` would be asserting
/// what the compiler is what decides.
pub const VALUE_PIN: &str = "value";
/// The pin type a closure carries. A closure is not a number, and that is the one distinction this
/// domain's pin types make.
pub const CLOSURE_PIN: &str = "closure";

/// One palette entry's input pins, in PORT ORDER, and whether its output is a closure.
///
/// Port order is the point: `MaterialGraph::connect` takes a port INDEX and `graph.h` fixes what each
/// one means — "input 0 is the colour … and THE LAST INPUT IS ALWAYS THE WEIGHT". The engine's
/// `kPalette` is the same table and [`tests::the_palette_pins_are_the_engines_own`] reads it.
const MATERIAL_PINS: &[(&str, &[&str], bool)] = &[
    ("material.add", &["a", "b"], false),
    ("material.add_closures", &["a", "b"], true),
    ("material.attribute", &[], false),
    ("material.coat", &["roughness", "weight"], true),
    ("material.combine", &["a", "b"], false),
    ("material.constant", &[], false),
    ("material.custom", &["a", "b"], false),
    ("material.diffuse", &["colour", "weight"], true),
    ("material.divide", &["a", "b"], false),
    ("material.emission", &["colour", "weight"], true),
    ("material.field", &[], false),
    ("material.layer_closures", &["top", "base"], true),
    ("material.lerp", &["a", "b", "t"], false),
    ("material.multiply", &["a", "b"], false),
    ("material.noise", &["position"], false),
    ("material.normal", &[], false),
    ("material.one_minus", &["value"], false),
    ("material.object_position", &[], false),
    ("material.output", &["surface", "opacity"], false),
    ("material.parameter", &[], false),
    ("material.saturate", &["value"], false),
    ("material.sin", &["value"], false),
    ("material.sheen", &["colour", "weight"], true),
    (
        "material.specular",
        &["colour", "roughness", "weight"],
        true,
    ),
    ("material.subsurface", &["colour", "weight"], true),
    ("material.subtract", &["a", "b"], false),
    ("material.swizzle", &["value"], false),
    ("material.time", &[], false),
    ("material.texture_sample", &["uv"], false),
    ("material.transmission", &["colour", "weight"], true),
    ("material.uv0", &[], false),
    ("material.vertex_output", &["offset"], false),
    ("material.vertex_color", &[], false),
    ("material.world_position", &[], false),
    ("material.wind", &["position", "time"], false),
];

/// Whether a closure node's INPUTS are closures too, which only the two combiners' are.
fn takes_closures(type_name: &str) -> bool {
    type_name == "material.add_closures" || type_name == "material.layer_closures"
}

/// The catalogue the material editor opens with: the engine's node types, with their pins.
///
/// Surface and vertex output nodes represent the graph's typed roots and are not `GraphOp` values.
#[must_use]
pub fn material_catalogue() -> Vec<NodeType> {
    MATERIAL_PINS
        .iter()
        .map(|(name, inputs, closure)| {
            let mut pins: Vec<Pin> = inputs
                .iter()
                .map(|pin| {
                    let data_type = if *name == "material.output" {
                        if *pin == "surface" {
                            CLOSURE_PIN
                        } else {
                            VALUE_PIN
                        }
                    } else if takes_closures(name) {
                        CLOSURE_PIN
                    } else {
                        VALUE_PIN
                    };
                    Pin::new(*pin, PinDirection::Input, data_type)
                })
                .collect();
            if *name != "material.output" && *name != "material.vertex_output" {
                pins.push(Pin::new(
                    "out",
                    PinDirection::Output,
                    if *closure { CLOSURE_PIN } else { VALUE_PIN },
                ));
            }
            let node = NodeType::new(*name, pins);
            if *name == "material.vertex_output" {
                node.with_stage_mask(VERTEX_STAGE)
            } else {
                node
            }
        })
        .collect()
}

fn decode_pin(reader: &mut Reader<'_>) -> Result<Pin> {
    let identity = reader.u32()?;
    let direction = match reader.u8()? {
        0 => PinDirection::Input,
        1 => PinDirection::Output,
        value => {
            return Err(Problem::new(
                "read the material catalogue",
                format!("pin direction {value} is not supported"),
            ));
        }
    };
    let mut pin = Pin::new(reader.text()?, direction, reader.text()?);
    pin.identity = identity;
    Ok(pin)
}

fn decode_property_kind(value: u8) -> Result<PropertyKind> {
    match value {
        0 => Ok(PropertyKind::Text),
        1 => Ok(PropertyKind::Bool),
        2 => Ok(PropertyKind::Scalar),
        3 => Ok(PropertyKind::Vector),
        4 => Ok(PropertyKind::Enumeration),
        5 => Ok(PropertyKind::Asset),
        value => Err(Problem::new(
            "read the material catalogue",
            format!("property kind {value} is not supported"),
        )),
    }
}

struct PropertyMetadata {
    constraint: String,
    tooltip: String,
    minimum: Option<f64>,
    maximum: Option<f64>,
    step: Option<f64>,
    choices: Vec<String>,
    asset_kind: String,
    semantic: String,
    stage: String,
    domain: String,
    required_capabilities: u64,
    vector_lanes: u8,
}

fn legacy_property_metadata(
    reader: &mut Reader<'_>,
    kind: PropertyKind,
) -> Result<PropertyMetadata> {
    let constraint = reader.text()?;
    let tooltip = reader.text()?;
    Ok(PropertyMetadata {
        choices: if kind == PropertyKind::Enumeration {
            constraint
                .split('|')
                .filter(|choice| !choice.is_empty())
                .map(ToOwned::to_owned)
                .collect()
        } else {
            Vec::new()
        },
        asset_kind: if kind == PropertyKind::Asset {
            constraint.clone()
        } else {
            String::new()
        },
        semantic: if constraint == "identifier" {
            constraint.clone()
        } else {
            String::new()
        },
        constraint,
        tooltip,
        minimum: None,
        maximum: None,
        step: None,
        stage: String::new(),
        domain: "material".into(),
        required_capabilities: 0,
        vector_lanes: 0,
    })
}

fn property_metadata(reader: &mut Reader<'_>) -> Result<PropertyMetadata> {
    let tooltip = reader.text()?;
    let semantic = reader.text()?;
    let asset_kind = reader.text()?;
    let choice_count = reader.u32()?;
    let choices = (0..choice_count)
        .map(|_| reader.text())
        .collect::<Result<Vec<_>>>()?;
    let stage = reader.text()?;
    let domain = reader.text()?;
    let required_capabilities = reader.u64()?;
    let vector_lanes = reader.u8()?;
    let flags = reader.u8()?;
    let encoded_minimum = reader.f64()?;
    let encoded_maximum = reader.f64()?;
    let encoded_step = reader.f64()?;
    Ok(PropertyMetadata {
        constraint: String::new(),
        tooltip,
        minimum: (flags & 1 != 0).then_some(encoded_minimum),
        maximum: (flags & 2 != 0).then_some(encoded_maximum),
        step: (flags & 4 != 0).then_some(encoded_step),
        choices,
        asset_kind,
        semantic,
        stage,
        domain,
        required_capabilities,
        vector_lanes,
    })
}

fn decode_property(reader: &mut Reader<'_>, schema: u32) -> Result<Property> {
    let identity = reader.u32()?;
    let kind = decode_property_kind(reader.u8()?)?;
    let name = reader.text()?;
    let default = reader.text()?;
    let metadata = if schema == 1 {
        legacy_property_metadata(reader, kind)?
    } else {
        property_metadata(reader)?
    };
    Ok(Property {
        identity,
        name,
        kind,
        default,
        constraint: metadata.constraint,
        tooltip: metadata.tooltip,
        minimum: metadata.minimum,
        maximum: metadata.maximum,
        step: metadata.step,
        choices: metadata.choices,
        asset_kind: metadata.asset_kind,
        semantic: metadata.semantic,
        stage: metadata.stage,
        domain: metadata.domain,
        required_capabilities: metadata.required_capabilities,
        vector_lanes: metadata.vector_lanes,
    })
}

/// Decode the versioned catalogue returned by `material.catalogue.get`.
pub fn catalogue_from_service(bytes: &[u8]) -> Result<Vec<NodeType>> {
    let mut reader = Reader::new(bytes);
    let schema = reader.u32()?;
    if !matches!(schema, 1..=3) {
        return Err(Problem::new(
            "read the material catalogue",
            format!("schema {schema} is not supported; this editor supports schemas 1 to 3"),
        ));
    }
    let _catalogue_version = reader.u32()?;
    let count = reader.u32()?;
    let mut nodes = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let identity = reader.u32()?;
        let node_schema = reader.u32()?;
        let name = reader.text()?;
        let stage_mask = if schema >= 3 { reader.u8()? } else { 0 };
        if schema >= 3
            && name.starts_with("material.")
            && (stage_mask == 0 || stage_mask & !(SURFACE_STAGE | VERTEX_STAGE) != 0)
        {
            return Err(Problem::new(
                "read the material catalogue",
                format!("node {name} has an invalid stage mask {stage_mask}"),
            ));
        }
        let pin_count = reader.u32()?;
        let pins = (0..pin_count)
            .map(|_| decode_pin(&mut reader))
            .collect::<Result<Vec<_>>>()?;
        let property_count = reader.u32()?;
        let properties = (0..property_count)
            .map(|_| decode_property(&mut reader, schema))
            .collect::<Result<Vec<_>>>()?;
        nodes.push(
            NodeType::identified(identity, node_schema, name, pins)
                .with_properties(properties)
                .with_stage_mask(stage_mask),
        );
    }
    if !reader.is_empty() {
        return Err(Problem::new(
            "read the material catalogue",
            "bytes remain after the declared node table",
        ));
    }
    Ok(nodes)
}

/// A material being authored on the shared canvas.
///
/// Thin on purpose: every edit below is a call on [`GraphCanvas`], which is THE canvas — the one
/// `editor-architecture` forbids a sixth bespoke graph editor beside. This type adds a name, a
/// left-to-right layout so the canvas reads like a graph rather than a pile, and the interchange
/// writer. It stores no node of its own.
pub struct MaterialAuthoring<'a> {
    name: String,
    canvas: &'a mut GraphCanvas,
    column: f32,
}

impl<'a> MaterialAuthoring<'a> {
    /// Begin a material on an opened canvas.
    pub fn begin(name: impl Into<String>, canvas: &'a mut GraphCanvas) -> Result<Self> {
        let name = name.into();
        if name.is_empty() || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
            return Err(Problem::new(
                "name a material",
                "a material's name becomes a Slang identifier in the generated program, so it is \
                 ASCII letters, digits and underscores",
            )
            .with_remedy("rename it, for example `weathered_stone`"));
        }
        Ok(Self {
            name,
            canvas,
            column: 0.0,
        })
    }

    /// Place a node, laid out a column further right than the last.
    pub fn node(&mut self, type_name: &str) -> Result<NodeKey> {
        let at = Layout {
            x: self.column,
            y: 0.0,
        };
        self.column += 180.0;
        self.canvas.add(type_name, at)
    }

    /// Type a value into a node.
    pub fn set(&mut self, key: NodeKey, property: &str, value: impl Into<String>) -> Result<()> {
        self.canvas.set_property(key, property, value)
    }

    /// Wire one node's output into another's input pin.
    pub fn wire(&mut self, from: NodeKey, to: NodeKey, pin: &str) -> Result<()> {
        self.canvas.connect(from, "out", to, pin)
    }

    /// The canvas this material is on.
    #[must_use]
    pub fn canvas(&self) -> &GraphCanvas {
        self.canvas
    }

    /// The interchange the engine canonicalises.
    ///
    /// Deterministic: nodes in key order, properties in name order, links in the canvas's own
    /// `(to, to_pin, from, from_pin)` order. Not because anything diffs it — nothing does — but
    /// because `cy_material author` turns it into a `.cygraph` that IS committed and IS diffed, and
    /// a non-deterministic producer would make that file churn.
    #[must_use]
    pub fn interchange(&self) -> String {
        canvas_interchange(&self.name, self.canvas)
            .expect("MaterialAuthoring::begin already validated the material name")
    }
}

/// Encode an existing visible canvas for the engine-owned material service.
///
/// This is the same interchange used by [`MaterialAuthoring`]; it is not a canonical CyberGraph
/// and contains no compiler implementation.
pub fn canvas_interchange(name: &str, canvas: &GraphCanvas) -> Result<String> {
    graph_canvas_interchange(name, canvas, "cymatcanvas", "material")
}

/// Serialize any domain's shared canvas as editable interchange facts.
pub(crate) fn graph_canvas_interchange(
    name: &str,
    canvas: &GraphCanvas,
    format: &str,
    subject: &str,
) -> Result<String> {
    if name.is_empty() || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
        return Err(Problem::new(
            "name a material",
            "the service material name must contain only ASCII letters, digits and underscores",
        ));
    }
    let mut out = String::new();
    let _ = writeln!(out, "{format} {INTERCHANGE_VERSION}");
    let _ = writeln!(out, "{subject} {name}");
    for node in canvas.nodes() {
        let _ = writeln!(out, "node {} {}", node.key.ordinal(), node.type_name);
        if let Some(at) = canvas.layout_of(node.key) {
            let _ = writeln!(out, "# layout {} {} {}", node.key.ordinal(), at.x, at.y);
        }
        for (property, value) in canvas.resolved_properties(node.key) {
            let encoded = if canvas
                .catalogue()
                .get(&node.type_name)
                .and_then(|kind| kind.properties.iter().find(|entry| entry.name == property))
                .is_some_and(|entry| entry.kind == PropertyKind::Vector)
            {
                value.replace(',', " ")
            } else {
                value
            };
            let _ = writeln!(out, "prop {} {} {}", node.key.ordinal(), property, encoded);
        }
    }
    for link in canvas.links() {
        let _ = writeln!(
            out,
            "link {} {} {} {}",
            link.from.ordinal(),
            link.from_pin,
            link.to.ordinal(),
            link.to_pin
        );
    }
    Ok(out)
}

fn load_canvas_nodes(
    facts: &[&str],
    canvas: &mut GraphCanvas,
) -> Result<std::collections::BTreeMap<u64, NodeKey>> {
    let mut keys = std::collections::BTreeMap::new();
    for line in facts {
        let Some(rest) = line.strip_prefix("node ") else {
            continue;
        };
        let (id, kind) = rest
            .split_once(' ')
            .ok_or_else(|| Problem::new("open a material graph", "invalid node"))?;
        let id = id
            .parse::<u64>()
            .map_err(|_| Problem::new("open a material graph", "invalid node key"))?;
        // Layout is a visual fallback; the interchange may replace it with exact saved positions.
        #[allow(
            clippy::cast_precision_loss,
            reason = "fallback layout coordinates are approximate"
        )]
        let at = Layout {
            x: 28.0 + (keys.len() % 3) as f32 * 225.0,
            y: 34.0 + (keys.len() / 3) as f32 * 170.0,
        };
        let key = NodeKey::new(id)?;
        canvas.add_with_key(key, kind, at)?;
        if keys.insert(id, key).is_some() {
            return Err(Problem::new("open a material graph", "duplicate node key"));
        }
    }
    Ok(keys)
}

/// Reopen an engine material canvas source using the active engine catalogue.
/// The canonical `.cygraph` is produced by the engine's material authoring service.
pub fn load_canvas_interchange(source: &str, canvas: &mut GraphCanvas) -> Result<String> {
    load_graph_canvas_interchange(source, canvas, "cymatcanvas", "material")
}

/// Restore any domain's shared canvas through its active backend catalogue.
pub(crate) fn load_graph_canvas_interchange(
    source: &str,
    canvas: &mut GraphCanvas,
    format: &str,
    subject: &str,
) -> Result<String> {
    let mut lines = source.lines();
    if lines.next() != Some(format!("{format} {INTERCHANGE_VERSION}").as_str()) {
        return Err(Problem::new(
            "open a material graph",
            "unsupported canvas version",
        ));
    }
    let name = lines
        .next()
        .and_then(|line| line.strip_prefix(&format!("{subject} ")))
        .ok_or_else(|| Problem::new("open a material graph", "missing material name"))?
        .to_owned();
    if name.is_empty()
        || !name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        return Err(Problem::new(
            "open a material graph",
            "invalid material name",
        ));
    }
    let mut loaded = canvas.clone();
    loaded.load(canvas.catalogue().clone());
    let facts: Vec<&str> = lines.collect();
    let keys = load_canvas_nodes(&facts, &mut loaded)?;
    for line in &facts {
        if let Some(rest) = line.strip_prefix("# layout ") {
            let words: Vec<_> = rest.split_whitespace().collect();
            if words.len() != 3 {
                return Err(Problem::new("open a material graph", "invalid node layout"));
            }
            let id = words[0].parse::<u64>().ok();
            let x = words[1]
                .parse::<f32>()
                .ok()
                .filter(|value| value.is_finite());
            let y = words[2]
                .parse::<f32>()
                .ok()
                .filter(|value| value.is_finite());
            let (Some(key), Some(x), Some(y)) = (id.and_then(|id| keys.get(&id)), x, y) else {
                return Err(Problem::new("open a material graph", "invalid node layout"));
            };
            loaded.move_to(*key, Layout { x, y })?;
        }
        if let Some(rest) = line.strip_prefix("prop ") {
            let mut words = rest.splitn(3, ' ');
            let id = words.next().and_then(|word| word.parse::<u64>().ok());
            let property = words.next();
            let value = words.next();
            let (Some(id), Some(property), Some(value)) = (id, property, value) else {
                return Err(Problem::new("open a material graph", "invalid property"));
            };
            let key = keys
                .get(&id)
                .ok_or_else(|| Problem::new("open a material graph", "unknown property node"))?;
            let value = if loaded
                .catalogue()
                .get(&loaded.node(*key).expect("loaded node").type_name)
                .and_then(|kind| kind.properties.iter().find(|entry| entry.name == property))
                .is_some_and(|entry| entry.kind == PropertyKind::Vector)
            {
                value.split_whitespace().collect::<Vec<_>>().join(",")
            } else {
                value.to_owned()
            };
            loaded.set_property(*key, property, value)?;
        }
        if let Some(rest) = line.strip_prefix("link ") {
            let words: Vec<_> = rest.split_whitespace().collect();
            if words.len() != 4 {
                return Err(Problem::new("open a material graph", "invalid link"));
            }
            let from = words[0].parse::<u64>().ok().and_then(|id| keys.get(&id));
            let to = words[2].parse::<u64>().ok().and_then(|id| keys.get(&id));
            let (Some(from), Some(to)) = (from, to) else {
                return Err(Problem::new("open a material graph", "unknown link node"));
            };
            loaded.connect(*from, words[1], *to, words[3])?;
        }
        if !line.is_empty()
            && !line.starts_with("node ")
            && !line.starts_with("prop ")
            && !line.starts_with("link ")
            && !line.starts_with('#')
        {
            return Err(Problem::new("open a material graph", "unknown canvas fact"));
        }
    }
    *canvas = loaded;
    Ok(name)
}

/// Build a `Catalogue` out of [`material_catalogue`]. Separate so the failure is one call's.
pub(crate) fn catalogue() -> Result<Catalogue> {
    Catalogue::new(material_catalogue())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canvas_round_trip_preserves_layout_and_properties() {
        let mut editors = SpecialisedEditors::with_legacy_material_catalogue().unwrap();
        let canvas = editors.open(Domain::Materials).unwrap().graph.unwrap();
        let key = canvas
            .add("material.parameter", Layout { x: 321.0, y: 145.0 })
            .unwrap();
        canvas.set_property(key, "symbol", "tint").unwrap();
        let source = canvas_interchange("layout_probe", canvas)
            .unwrap()
            .replace("node 1 ", "node 7 ")
            .replace("# layout 1 ", "# layout 7 ")
            .replace("prop 1 ", "prop 7 ");
        let mut loaded = canvas.clone();
        assert_eq!(
            load_canvas_interchange(&source, &mut loaded).unwrap(),
            "layout_probe"
        );
        let restored = loaded.nodes().next().unwrap().key;
        assert_eq!(restored.ordinal(), 7);
        assert_eq!(
            loaded.layout_of(restored),
            Some(Layout { x: 321.0, y: 145.0 })
        );
        assert_eq!(
            loaded
                .resolved_properties(restored)
                .get("symbol")
                .map(String::as_str),
            Some("tint")
        );
        assert_eq!(
            loaded
                .add("material.output", Layout::default())
                .unwrap()
                .ordinal(),
            8
        );
    }
    use crate::specialised::{Domain, SpecialisedEditors};
    use cy_editor_core::codec::Writer;

    fn repository() -> std::path::PathBuf {
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../..")
            .canonicalize()
            .expect("the workspace is inside the repository")
    }

    #[test]
    fn a_backend_catalogue_supplies_stable_node_and_pin_identities() {
        let mut bytes = Writer::new();
        bytes.u32(1);
        bytes.u32(7);
        bytes.u32(1);
        bytes.u32(42);
        bytes.u32(3);
        bytes.text("material.future");
        bytes.u32(1);
        bytes.u32(9);
        bytes.u8(1);
        bytes.text("out");
        bytes.text("value");
        bytes.u32(0);
        let decoded = catalogue_from_service(&bytes.finish()).expect("schema 1 decodes");
        assert_eq!(decoded.len(), 1);
        assert_eq!(decoded[0].identity, 42);
        assert_eq!(decoded[0].schema_version, 3);
        assert_eq!(decoded[0].pins[0].identity, 9);

        let mut editors = SpecialisedEditors::new().expect("non-material catalogues");
        let mut bytes = Writer::new();
        bytes.u32(1);
        bytes.u32(7);
        bytes.u32(1);
        bytes.u32(42);
        bytes.u32(3);
        bytes.text("material.future");
        bytes.u32(1);
        bytes.u32(9);
        bytes.u8(1);
        bytes.text("out");
        bytes.text("value");
        bytes.u32(0);
        editors
            .install_material_catalogue(&bytes.finish())
            .expect("backend catalogue installs");
        let session = editors.open(Domain::Materials).expect("materials opens");
        assert!(
            session
                .graph
                .expect("graph")
                .catalogue()
                .get("material.future")
                .is_some()
        );
    }

    #[test]
    fn backend_property_descriptors_remain_typed_and_constrained() {
        let mut bytes = Writer::new();
        bytes.u32(1);
        bytes.u32(2);
        bytes.u32(1);
        bytes.u32(5);
        bytes.u32(1);
        bytes.text("material.texture_sample");
        bytes.u32(0);
        bytes.u32(1);
        bytes.u32(2);
        bytes.u8(5);
        bytes.text("texture");
        bytes.text("");
        bytes.text("texture");
        bytes.text("Project texture asset");

        let decoded = catalogue_from_service(&bytes.finish()).expect("property catalogue decodes");
        let property = &decoded[0].properties[0];
        assert_eq!(property.identity, 2);
        assert_eq!(property.kind, PropertyKind::Asset);
        assert_eq!(property.constraint, "texture");
    }

    #[test]
    fn schema_two_keeps_authoring_metadata_and_stable_property_identity() {
        let mut bytes = Writer::new();
        bytes.u32(2);
        bytes.u32(3);
        bytes.u32(1);
        bytes.u32(24);
        bytes.u32(1);
        bytes.text("material.texture_sample");
        bytes.u32(0);
        bytes.u32(1);
        bytes.u32(2);
        bytes.u8(5);
        bytes.text("texture");
        bytes.text("");
        bytes.text("Project texture asset");
        bytes.text("texture");
        bytes.text("texture");
        bytes.u32(0);
        bytes.text("fragment");
        bytes.text("material");
        bytes.u64(1);
        bytes.u8(0);
        bytes.u8(0);
        bytes.f64(0.0);
        bytes.f64(0.0);
        bytes.f64(0.0);

        let decoded = catalogue_from_service(&bytes.finish()).expect("schema 2 decodes");
        let property = &decoded[0].properties[0];
        assert_eq!(property.identity, 2);
        assert_eq!(property.asset_kind, "texture");
        assert_eq!(property.semantic, "texture");
        assert_eq!(property.stage, "fragment");
        assert_eq!(property.domain, "material");
        assert_eq!(property.required_capabilities, 1);
        assert_eq!(decoded[0].stage_mask, 0);
    }

    #[test]
    fn schema_three_preserves_engine_node_stage_compatibility() {
        let mut bytes = Writer::new();
        bytes.u32(3);
        bytes.u32(5);
        bytes.u32(2);
        for (identity, name, stages) in [
            (25, "material.output", SURFACE_STAGE),
            (26, "material.sin", SURFACE_STAGE | VERTEX_STAGE),
        ] {
            bytes.u32(identity);
            bytes.u32(1);
            bytes.text(name);
            bytes.u8(stages);
            bytes.u32(0);
            bytes.u32(0);
        }
        let decoded = catalogue_from_service(&bytes.finish()).expect("schema 3 decodes");
        assert!(decoded[0].supports_stage(SURFACE_STAGE));
        assert!(!decoded[0].supports_stage(VERTEX_STAGE));
        assert!(decoded[1].supports_stage(SURFACE_STAGE));
        assert!(decoded[1].supports_stage(VERTEX_STAGE));

        let mut invalid = Writer::new();
        invalid.u32(3);
        invalid.u32(5);
        invalid.u32(1);
        invalid.u32(26);
        invalid.u32(1);
        invalid.text("material.sin");
        invalid.u8(0);
        invalid.u32(0);
        invalid.u32(0);
        assert!(catalogue_from_service(&invalid.finish()).is_err());
    }

    /// THE CROSS-LANGUAGE JOIN, AND IT IS READ RATHER THAN COPIED.
    ///
    /// `lower_material.cpp`'s `kPalette` rows are `{"material.x", GraphOp::X, {"pin", ...}, n, c}`.
    /// This parses them and requires this module's table to be the same table — same type names, same
    /// pin names, SAME ORDER, because the order is the port index the compiler wires by. A pin
    /// renamed on one side and not the other is red here rather than at cook time.
    #[test]
    fn the_palette_pins_are_the_engines_own() {
        let source =
            std::fs::read_to_string(repository().join("src/graph/material/src/lower_material.cpp"))
                .expect("the engine's material lowering is in the tree");
        let start = source
            .find("constexpr NodeSpec kPalette[] = {")
            .expect("lower_material.cpp still declares kPalette");
        let body = &source[start..source[start..].find("\n};").expect("kPalette closes") + start];

        let mut engine: Vec<(String, Vec<String>)> = Vec::new();
        for line in body.lines().skip(1) {
            let line = line.trim();
            if !line.contains("\"material.") {
                continue;
            }
            let quoted: Vec<&str> = line.split('"').skip(1).step_by(2).collect();
            let (name, pins) = quoted.split_first().expect("a row names its type");
            engine.push((
                (*name).to_owned(),
                pins.iter().map(|pin| (*pin).to_owned()).collect(),
            ));
        }
        assert!(
            engine.len() >= 20,
            "kPalette parsed to {} rows; this is not the table the test reads",
            engine.len()
        );

        for (name, pins) in &engine {
            let ours = MATERIAL_PINS
                .iter()
                .find(|(entry, _, _)| entry == name)
                .unwrap_or_else(|| panic!("the editor's palette has no {name}"));
            assert_eq!(
                ours.1, pins,
                "{name}: the editor's pins and the engine's ports disagree"
            );
        }
        // The two output roots are not `GraphOp` values and so are not in `kPalette`.
        assert_eq!(
            MATERIAL_PINS.len(),
            engine.len() + 2,
            "the editor offers a node type the engine cannot lower, or is missing one it can"
        );
    }

    #[test]
    fn the_material_editor_opens_and_its_nodes_can_be_wired() {
        // The refusal M11.c's spike measured, performed in reverse: this exact call returned
        // "this build declares no authoring vocabulary for materials" before the engine declared one.
        let mut editors = SpecialisedEditors::with_legacy_material_catalogue()
            .expect("the legacy build-tool catalogues are well formed");
        assert!(editors.can_open(Domain::Materials));
        let session = editors.open(Domain::Materials).expect("materials opens");
        let canvas = session
            .graph
            .expect("the material editor is a graph editor");

        let mut material = MaterialAuthoring::begin("probe", canvas).expect("a legal name");
        let uv = material
            .node("material.attribute")
            .expect("an attribute node");
        material.set(uv, "symbol", "uv0").expect("a symbol");
        let sample = material
            .node("material.texture_sample")
            .expect("a sample node");
        material
            .set(sample, "symbol", "albedo_map")
            .expect("a name");
        let diffuse = material.node("material.diffuse").expect("a diffuse node");
        let output = material.node("material.output").expect("the root");

        // THE EDIT THAT WAS IMPOSSIBLE BEFORE THIS MODULE: the catalogue carried no pins, so every
        // wire was refused with "the type declares no pin of that name".
        material.wire(uv, sample, "uv").expect("uv wires");
        material
            .wire(sample, diffuse, "colour")
            .expect("colour wires");
        material
            .wire(diffuse, output, "surface")
            .expect("the closure reaches the root");

        // And the type check is real: a closure may not be wired into a numeric pin.
        assert!(material.wire(diffuse, sample, "uv").is_err());

        let text = material.interchange();
        assert!(text.starts_with("cymatcanvas 1\nmaterial probe\n"));
        assert!(text.contains("link 1 out 2 uv"));
        assert!(text.contains("prop 1 symbol uv0"));
    }

    /// REGRESSION, and the defect it is against shipped green and went red without a line of this
    /// module changing. `SpecialisedEditors::open` stopped clearing the canvas when the domain
    /// asked for is the one ALREADY active — deliberately, so that a repeated or mis-clicked open
    /// cannot empty an author's region, which
    /// `reopening_the_active_material_editor_preserves_authored_nodes` asserts from the other side.
    /// `cy-author-material` authors the beauty shot's three materials in ONE process and had been
    /// relying on each `open` to start it an empty canvas, so it began writing 20 nodes, then 40,
    /// then 60, and `m11c:shot-authored-through-the-editor` went red on the first committed canvas
    /// it compared. The close is what separates two materials, and this is the check that it does.
    #[test]
    fn authoring_two_materials_in_one_session_keeps_their_canvases_apart() {
        let mut editors = SpecialisedEditors::with_legacy_material_catalogue()
            .expect("legacy build-tool catalogues");
        let mut interchanges = Vec::new();
        for name in ["first_material", "second_material"] {
            // What the generator does between two materials, and what a person does: close the
            // editor, open it again. Without this line the second material carries the first.
            editors.close();
            let session = editors.open(Domain::Materials).expect("materials opens");
            let canvas = session.graph.expect("a graph editor");
            let mut material =
                MaterialAuthoring::begin(name, canvas).expect("the name is an identifier");
            material
                .node("material.attribute")
                .expect("the attribute node is in the catalogue");
            material
                .node("material.output")
                .expect("the output node is in the catalogue");
            interchanges.push(material.interchange());
        }

        for (name, text) in ["first_material", "second_material"]
            .iter()
            .zip(&interchanges)
        {
            let nodes = text
                .lines()
                .filter(|line| line.starts_with("node "))
                .count();
            assert_eq!(nodes, 2, "{name} carries only its own nodes:\n{text}");
            // The ordinals restart too, which is what makes the committed canvases byte-comparable
            // whichever order the generator writes them in.
            assert!(
                text.contains("node 1 material.attribute"),
                "{name}:\n{text}"
            );
            assert!(text.contains("node 2 material.output"), "{name}:\n{text}");
            assert!(text.starts_with(&format!(
                "cymatcanvas {INTERCHANGE_VERSION}\nmaterial {name}\n"
            )));
        }

        // AND THE OTHER HALF, SO THIS TEST CANNOT PASS BY ACCIDENT. If `open` went back to
        // clearing the canvas every time, the loop above would still pass and the defect it is
        // against would be unreachable — and an author's work would be lost on a mis-click again.
        // So the preserving behaviour is asserted here in the same test: authoring a third
        // material WITHOUT closing first carries the second one's nodes, which is exactly what
        // `cy-author-material` was doing.
        let session = editors.open(Domain::Materials).expect("materials reopens");
        let canvas = session.graph.expect("a graph editor");
        let mut third =
            MaterialAuthoring::begin("third_material", canvas).expect("the name is an identifier");
        third
            .node("material.output")
            .expect("the output node is in the catalogue");
        let carried = third.interchange();
        let nodes = carried
            .lines()
            .filter(|line| line.starts_with("node "))
            .count();
        assert_eq!(
            nodes, 3,
            "re-opening the ACTIVE material editor preserves what is on the canvas:\n{carried}"
        );
    }

    #[test]
    fn a_material_name_that_is_not_an_identifier_is_refused() {
        let mut editors = SpecialisedEditors::with_legacy_material_catalogue()
            .expect("legacy build-tool catalogues");
        let session = editors.open(Domain::Materials).expect("materials opens");
        let canvas = session.graph.expect("a graph editor");
        // The generated program declares `void cy_material_<name>_primary_high(...)`, so a name with
        // a hyphen in it is a Slang syntax error three tools downstream. Refusing here names it.
        assert!(MaterialAuthoring::begin("worn-metal", canvas).is_err());
    }
}
