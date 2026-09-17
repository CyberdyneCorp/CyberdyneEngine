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
//! So this module is the pins, and [`tests`] checks them against the engine's own table by reading
//! `lower_material.cpp` — the rule `tools/editor/selftest.py` states and the reason it gives: a
//! copied fixture goes stale, and a stale fixture agrees with a broken check.
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
//! The interchange is deliberately NOT a format anything reads twice — nothing loads it, nothing
//! diffs it, and it is not committed. It exists for the length of one pipe.

use std::collections::BTreeMap;
use std::fmt::Write as _;

use cy_editor_core::problem::{Problem, Result};

use super::graph::{Catalogue, GraphCanvas, Layout, NodeKey, NodeType, Pin, PinDirection};

/// The interchange's own version, written on its first line.
pub const INTERCHANGE_VERSION: u32 = 1;

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
    ("material.one_minus", &["value"], false),
    ("material.output", &["surface", "opacity"], false),
    ("material.parameter", &[], false),
    ("material.saturate", &["value"], false),
    ("material.sheen", &["colour", "weight"], true),
    (
        "material.specular",
        &["colour", "roughness", "weight"],
        true,
    ),
    ("material.subsurface", &["colour", "weight"], true),
    ("material.subtract", &["a", "b"], false),
    ("material.swizzle", &["value"], false),
    ("material.texture_sample", &["uv"], false),
    ("material.transmission", &["colour", "weight"], true),
];

/// Whether a closure node's INPUTS are closures too, which only the two combiners' are.
fn takes_closures(type_name: &str) -> bool {
    type_name == "material.add_closures" || type_name == "material.layer_closures"
}

/// The catalogue the material editor opens with: the engine's node types, with their pins.
///
/// `material.output` is the root and the one entry that is not a `GraphOp` — `MaterialGraph` has
/// `set_surface_output` and `set_opacity_output` rather than an output node, and an author needs
/// something to wire the final closure into.
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
            if *name != "material.output" {
                pins.push(Pin::new(
                    "out",
                    PinDirection::Output,
                    if *closure { CLOSURE_PIN } else { VALUE_PIN },
                ));
            }
            NodeType::new(*name, pins)
        })
        .collect()
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
        let mut out = String::new();
        let _ = writeln!(out, "cymatcanvas {INTERCHANGE_VERSION}");
        let _ = writeln!(out, "material {}", self.name);
        for node in self.canvas.nodes() {
            let _ = writeln!(out, "node {} {}", node.key.ordinal(), node.type_name);
            let ordered: BTreeMap<&String, &String> = node.properties.iter().collect();
            for (property, value) in ordered {
                let _ = writeln!(out, "prop {} {} {}", node.key.ordinal(), property, value);
            }
        }
        for link in self.canvas.links() {
            let _ = writeln!(
                out,
                "link {} {} {} {}",
                link.from.ordinal(),
                link.from_pin,
                link.to.ordinal(),
                link.to_pin
            );
        }
        out
    }
}

/// Build a `Catalogue` out of [`material_catalogue`]. Separate so the failure is one call's.
pub(crate) fn catalogue() -> Result<Catalogue> {
    Catalogue::new(material_catalogue())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::specialised::{Domain, SpecialisedEditors};

    fn repository() -> std::path::PathBuf {
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../..")
            .canonicalize()
            .expect("the workspace is inside the repository")
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
            if !line.starts_with("{\"material.") {
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
        // `material.output` is the one entry that is not a `GraphOp` and so is not in `kPalette`;
        // everything else must be on both sides.
        assert_eq!(
            MATERIAL_PINS.len(),
            engine.len() + 1,
            "the editor offers a node type the engine cannot lower, or is missing one it can"
        );
    }

    #[test]
    fn the_material_editor_opens_and_its_nodes_can_be_wired() {
        // The refusal M11.c's spike measured, performed in reverse: this exact call returned
        // "this build declares no authoring vocabulary for materials" before the engine declared one.
        let mut editors =
            SpecialisedEditors::new().expect("the built-in catalogues are well formed");
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

    #[test]
    fn a_material_name_that_is_not_an_identifier_is_refused() {
        let mut editors = SpecialisedEditors::new().expect("catalogues");
        let session = editors.open(Domain::Materials).expect("materials opens");
        let canvas = session.graph.expect("a graph editor");
        // The generated program declares `void cy_material_<name>_primary_high(...)`, so a name with
        // a hyphen in it is a Slang syntax error three tools downstream. Refusing here names it.
        assert!(MaterialAuthoring::begin("worn-metal", canvas).is_err());
    }
}
