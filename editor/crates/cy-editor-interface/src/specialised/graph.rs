//! THE node-graph canvas. One, not six. Task 3.4, `editor-architecture` (Specialised editors).
//!
//! > **All node-graph editors SHALL be built on the shared graph infrastructure defined in
//! > `visual-scripting`**: one canvas, one identity model, one serialization and diff format, one
//! > debugging model — while each domain keeps its own lowering. A sixth bespoke graph editor SHALL
//! > NOT be created.
//!
//! The prohibition is the requirement. So this module holds the whole editing model for every graph
//! the editor will ever show — script, ability, animation, material, VFX — and a domain contributes
//! exactly one thing to it: a [`Catalogue`] of node types. There is no extension point through which
//! a domain could bring its own canvas, because that is the thing being forbidden.
//!
//! --- IDENTITY IS THE ENGINE'S, NOT A SECOND ONE ----------------------------------------------------
//!
//! `cy::graph::Graph` already fixes what a node is: a stable `NodeKey` that survives reordering, a
//! type name, properties by name, and wires ordered by `(target, target pin, source, source pin)`.
//! A canvas that invented its own identity would be a second model to migrate, so [`NodeKey`] here
//! is the same u64 the engine's authored graph carries and [`Link`] is ordered the same way. The
//! layout — where a node sits, what colour the author gave it — is a SIDE TABLE outside the
//! semantic model, for the reason `cybergraph.h` gives: "did the meaning change?" and "did the
//! canvas change?" are different questions and the first must not depend on the second.
//!
//! --- WHAT IS DELIBERATELY NOT HERE, AND WHICH RUNG OWES IT -----------------------------------------
//!
//! **The on-disk text form.** `src/graph/include/cy/graph/text.h` owns it and it is canonical —
//! nodes in key order, properties in name order, links in tuple order, floats at `%.9g`. This crate
//! does not re-implement it and must not: a second writer of a canonical format is a second format
//! the day the two disagree about a float. The editor reaches the authored graph through the
//! engine, and the canvas edits what it is handed. Writing `.cygraph` from Rust is M11.e's, beside
//! the semantic-diff work `editor-documents-and-transactions` still owes.
//!
//! So [`GraphCanvas::diff`] is a diff over THIS model — what an undo stack and a review panel need —
//! and it is not the file diff. The distinction is stated here rather than discovered later.

use std::collections::{BTreeMap, BTreeSet};

use cy_editor_core::problem::{Problem, Result};

/// A node's stable authoring identity, as `cy::graph` fixes it. Zero is "no node".
///
/// The same integer the engine's authored graph carries, so a canvas selection names the node the
/// engine names and neither side has to translate.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct NodeKey(u64);

impl NodeKey {
    /// The key for `ordinal`. Zero is refused: `cy::graph` reserves it for "no node".
    pub fn new(ordinal: u64) -> Result<Self> {
        if ordinal == 0 {
            return Err(Problem::new(
                "name a graph node 0",
                "`cy::graph` reserves the key 0 for \"no node\", so a canvas that handed it out \
                 would produce a selection the engine reads as empty",
            )
            .with_remedy("number authored nodes from 1"));
        }
        Ok(Self(ordinal))
    }

    /// The integer the engine's authored graph carries.
    pub fn ordinal(self) -> u64 {
        self.0
    }
}

/// Which side of a node a pin is on.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum PinDirection {
    /// A value or execution flow entering the node.
    Input,
    /// A value or execution flow leaving the node.
    Output,
}

/// One pin of a node type: its name, its side, and the type that flows along it.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct Pin {
    /// The pin's name, unique within its node type and direction.
    pub name: String,
    /// Which side of the node it is on.
    pub direction: PinDirection,
    /// The domain's own type name — `float`, `exec`, `pose`, `vec3`. Not a universal pin type:
    /// `cybergraph.h` decision 2 keeps that the domain's business.
    pub data_type: String,
}

impl Pin {
    /// A pin, by its three parts.
    pub fn new(
        name: impl Into<String>,
        direction: PinDirection,
        data_type: impl Into<String>,
    ) -> Self {
        Self {
            name: name.into(),
            direction,
            data_type: data_type.into(),
        }
    }
}

/// A node type as a domain registers it: what it is called, and what it connects by.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct NodeType {
    /// The engine's own spelling — `script.add_float`, `pose.blend`, `ai.selector`. This is the
    /// name `cy::graph::NodeRegistry` interns, and the contract gate compares this list against the
    /// engine's lowering tables so that a node type added to one side and not the other is red.
    pub name: String,
    /// Its pins, in declaration order.
    pub pins: Vec<Pin>,
}

impl NodeType {
    /// A node type and its pins.
    pub fn new(name: impl Into<String>, pins: Vec<Pin>) -> Self {
        Self {
            name: name.into(),
            pins,
        }
    }

    /// The pin of this name and direction, if the type has one.
    pub fn pin(&self, name: &str, direction: PinDirection) -> Option<&Pin> {
        self.pins
            .iter()
            .find(|pin| pin.name == name && pin.direction == direction)
    }
}

/// What a domain contributes to the canvas: its node types, and nothing else.
///
/// A domain brings a vocabulary. It does not bring a canvas, a selection model, an undo model or a
/// diff — which is the whole of "a sixth bespoke graph editor SHALL NOT be created", expressed as
/// the only thing the type system lets a domain hand over.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Catalogue {
    types: BTreeMap<String, NodeType>,
}

impl Catalogue {
    /// A catalogue of these node types. A duplicate name is refused rather than shadowed.
    pub fn new(types: Vec<NodeType>) -> Result<Self> {
        let mut catalogue = Self::default();
        for node_type in types {
            catalogue.declare(node_type)?;
        }
        Ok(catalogue)
    }

    /// Add a node type. Refuses a name already present.
    pub fn declare(&mut self, node_type: NodeType) -> Result<()> {
        if self.types.contains_key(&node_type.name) {
            return Err(Problem::new(
                format!("declare the node type {} twice", node_type.name),
                "two declarations of one type name would make which pins a node has depend on \
                 which plugin loaded last",
            )
            .with_remedy("give the second type its own name, prefixed by its domain"));
        }
        self.types.insert(node_type.name.clone(), node_type);
        Ok(())
    }

    /// Every type name, sorted — the palette's own order.
    pub fn type_names(&self) -> Vec<&str> {
        self.types.keys().map(String::as_str).collect()
    }

    /// The node type of this name.
    pub fn get(&self, name: &str) -> Option<&NodeType> {
        self.types.get(name)
    }

    /// How many node types the domain declared.
    pub fn len(&self) -> usize {
        self.types.len()
    }

    /// Whether the domain declared no node types at all.
    pub fn is_empty(&self) -> bool {
        self.types.is_empty()
    }
}

/// One node on the canvas.
#[derive(Clone, PartialEq, Debug)]
pub struct Node {
    /// Its stable identity.
    pub key: NodeKey,
    /// Which node type it is an instance of.
    pub type_name: String,
    /// What the author typed into it, by property name.
    pub properties: BTreeMap<String, String>,
}

/// Where a node sits and what colour it was given. A SIDE TABLE, outside the semantic model.
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub struct Layout {
    /// Canvas position.
    pub x: f32,
    /// Canvas position.
    pub y: f32,
}

/// A wire. Ordered by `(to, to_pin, from, from_pin)`, which is the order `cy::graph` fixes.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct Link {
    /// The node the wire enters.
    pub to: NodeKey,
    /// The input pin it enters by.
    pub to_pin: String,
    /// The node the wire leaves.
    pub from: NodeKey,
    /// The output pin it leaves by.
    pub from_pin: String,
}

/// How bad an authoring diagnostic is.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Severity {
    /// Worth saying.
    Info,
    /// The graph compiles and something is probably wrong.
    Warning,
    /// The graph does not compile.
    Error,
}

/// A diagnostic, NODE- AND PIN-PRECISE.
///
/// `visual-scripting`: "a diagnostic that names a graph and not a pin sends an author hunting
/// through a canvas." So the site is a node and, where there is one, a pin — never just a message.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Diagnostic {
    /// How bad it is.
    pub severity: Severity,
    /// The node it is about.
    pub node: NodeKey,
    /// The pin it is about, where it is about one.
    pub pin: Option<String>,
    /// What is wrong, in the engine's own words.
    pub message: String,
}

/// One difference between two states of a canvas.
///
/// The editing diff, not the file diff: see the module header. It is what an undo stack, a review
/// panel and a live-edit announcement read.
#[derive(Clone, PartialEq, Debug)]
pub enum Change {
    /// A node that is in the later state and not the earlier one.
    NodeAdded(NodeKey),
    /// A node that is in the earlier state and not the later one.
    NodeRemoved(NodeKey),
    /// A node whose properties differ.
    NodeChanged(NodeKey),
    /// A wire that is in the later state and not the earlier one.
    LinkAdded(Link),
    /// A wire that is in the earlier state and not the later one.
    LinkRemoved(Link),
}

/// Which canvas this is. There is exactly one per host, and this is how a test says so.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct CanvasId(u64);

/// THE node-graph canvas.
///
/// Every graph editor in the editor is this type with a different [`Catalogue`] loaded. Nothing in
/// the crate constructs a second one outside a test, and [`crate::specialised::SpecialisedEditors`]
/// holds exactly one.
#[derive(Clone, PartialEq, Debug)]
pub struct GraphCanvas {
    id: CanvasId,
    catalogue: Catalogue,
    nodes: BTreeMap<NodeKey, Node>,
    layout: BTreeMap<NodeKey, Layout>,
    links: BTreeSet<Link>,
    selection: BTreeSet<NodeKey>,
    next_ordinal: u64,
}

impl GraphCanvas {
    /// An empty canvas with no vocabulary. A domain loads one with [`GraphCanvas::load`].
    pub fn new(id: u64) -> Self {
        Self {
            id: CanvasId(id),
            catalogue: Catalogue::default(),
            nodes: BTreeMap::new(),
            layout: BTreeMap::new(),
            links: BTreeSet::new(),
            selection: BTreeSet::new(),
            next_ordinal: 1,
        }
    }

    /// Which canvas this is.
    ///
    /// The check that every graph domain shares one canvas is this value being equal across them,
    /// which a registry of names cannot fake.
    pub fn id(&self) -> CanvasId {
        self.id
    }

    /// Point the canvas at a domain's vocabulary, discarding whatever was being edited.
    ///
    /// Switching domains is switching catalogue, and that is the ONLY thing that changes: the
    /// selection model, the connection rules, the diagnostics and the diff are the same code.
    pub fn load(&mut self, catalogue: Catalogue) {
        self.catalogue = catalogue;
        self.nodes.clear();
        self.layout.clear();
        self.links.clear();
        self.selection.clear();
        self.next_ordinal = 1;
    }

    /// The vocabulary currently loaded.
    pub fn catalogue(&self) -> &Catalogue {
        &self.catalogue
    }

    /// Place a node of this type. Refuses a type the loaded catalogue does not declare.
    pub fn add(&mut self, type_name: &str, at: Layout) -> Result<NodeKey> {
        if self.catalogue.get(type_name).is_none() {
            return Err(Problem::new(
                format!("place a {type_name} node"),
                format!(
                    "the loaded catalogue declares {} node type(s) and none of them is {type_name}",
                    self.catalogue.len()
                ),
            )
            .with_remedy("open the domain whose catalogue declares it"));
        }
        let key = NodeKey::new(self.next_ordinal)?;
        self.next_ordinal += 1;
        self.nodes.insert(
            key,
            Node {
                key,
                type_name: type_name.to_owned(),
                properties: BTreeMap::new(),
            },
        );
        self.layout.insert(key, at);
        Ok(key)
    }

    /// Every node, in key order.
    pub fn nodes(&self) -> impl Iterator<Item = &Node> {
        self.nodes.values()
    }

    /// The node of this key.
    pub fn node(&self, key: NodeKey) -> Option<&Node> {
        self.nodes.get(&key)
    }

    /// Where a node sits.
    pub fn layout_of(&self, key: NodeKey) -> Option<Layout> {
        self.layout.get(&key).copied()
    }

    /// Move a node. A layout change is outside the semantic model, so it produces no [`Change`].
    pub fn move_to(&mut self, key: NodeKey, at: Layout) -> Result<()> {
        if !self.nodes.contains_key(&key) {
            return Err(Self::no_such_node(key));
        }
        self.layout.insert(key, at);
        Ok(())
    }

    /// Set what the author typed into a node.
    pub fn set_property(
        &mut self,
        key: NodeKey,
        name: impl Into<String>,
        value: impl Into<String>,
    ) -> Result<()> {
        let node = self
            .nodes
            .get_mut(&key)
            .ok_or_else(|| Self::no_such_node(key))?;
        node.properties.insert(name.into(), value.into());
        Ok(())
    }

    /// Wire an output pin to an input pin.
    ///
    /// Refuses, by name and with the reason: a node that is not there, a pin the type does not
    /// declare, a pin on the wrong side, two pin types that do not match, and a wire that would
    /// close a cycle among pure data pins.
    pub fn connect(
        &mut self,
        from: NodeKey,
        from_pin: &str,
        to: NodeKey,
        to_pin: &str,
    ) -> Result<()> {
        let source = self.pin_of(from, from_pin, PinDirection::Output)?;
        let target = self.pin_of(to, to_pin, PinDirection::Input)?;
        if source.data_type != target.data_type {
            return Err(Problem::new(
                format!("wire {from_pin} to {to_pin}"),
                format!(
                    "{from_pin} carries {} and {to_pin} takes {}",
                    source.data_type, target.data_type
                ),
            )
            .with_remedy("insert a conversion node, or wire a pin of the same type"));
        }
        let link = Link {
            to,
            to_pin: to_pin.to_owned(),
            from,
            from_pin: from_pin.to_owned(),
        };
        if self.reaches(to, from) {
            return Err(Problem::new(
                format!("wire node {} to node {}", from.ordinal(), to.ordinal()),
                format!(
                    "node {} already reaches node {}, so this wire closes a cycle",
                    to.ordinal(),
                    from.ordinal()
                ),
            )
            .with_remedy("break the existing path first, or use the domain's own loop node"));
        }
        self.links.insert(link);
        Ok(())
    }

    /// Every wire, in `(to, to_pin, from, from_pin)` order.
    pub fn links(&self) -> impl Iterator<Item = &Link> {
        self.links.iter()
    }

    /// Remove a node and every wire that touched it.
    pub fn remove(&mut self, key: NodeKey) -> Result<()> {
        if self.nodes.remove(&key).is_none() {
            return Err(Self::no_such_node(key));
        }
        self.layout.remove(&key);
        self.selection.remove(&key);
        self.links.retain(|link| link.to != key && link.from != key);
        Ok(())
    }

    /// Replace the selection.
    pub fn select(&mut self, keys: impl IntoIterator<Item = NodeKey>) -> Result<()> {
        let wanted: BTreeSet<NodeKey> = keys.into_iter().collect();
        if let Some(missing) = wanted.iter().find(|key| !self.nodes.contains_key(key)) {
            return Err(Self::no_such_node(*missing));
        }
        self.selection = wanted;
        Ok(())
    }

    /// What is selected, in key order.
    pub fn selection(&self) -> Vec<NodeKey> {
        self.selection.iter().copied().collect()
    }

    /// Check the canvas, node- and pin-precisely.
    ///
    /// Reports an input pin the domain marked required that has neither a wire nor a property, and
    /// a node whose type the loaded catalogue no longer declares — which is what a catalogue
    /// switched underneath authored content looks like.
    pub fn diagnostics(&self) -> Vec<Diagnostic> {
        let mut found = Vec::new();
        for node in self.nodes.values() {
            let Some(node_type) = self.catalogue.get(&node.type_name) else {
                found.push(Diagnostic {
                    severity: Severity::Error,
                    node: node.key,
                    pin: None,
                    message: format!(
                        "the loaded catalogue does not declare {}; the node is preserved and \
                         cannot be compiled",
                        node.type_name
                    ),
                });
                continue;
            };
            for pin in &node_type.pins {
                if pin.direction != PinDirection::Input {
                    continue;
                }
                let wired = self
                    .links
                    .iter()
                    .any(|link| link.to == node.key && link.to_pin == pin.name);
                if !wired && !node.properties.contains_key(&pin.name) {
                    found.push(Diagnostic {
                        severity: Severity::Warning,
                        node: node.key,
                        pin: Some(pin.name.clone()),
                        message: format!(
                            "{} has neither a wire nor a value on {}",
                            node.type_name, pin.name
                        ),
                    });
                }
            }
        }
        found
    }

    /// What changed between two states of a canvas, in a stable order.
    ///
    /// Layout is excluded on purpose: moving a node is not a change to the graph's meaning, and a
    /// diff that said otherwise would make every review a review of where boxes sit.
    pub fn diff(before: &Self, after: &Self) -> Vec<Change> {
        let mut changes = Vec::new();
        for (key, node) in &after.nodes {
            match before.nodes.get(key) {
                None => changes.push(Change::NodeAdded(*key)),
                Some(was)
                    if was.type_name != node.type_name || was.properties != node.properties =>
                {
                    changes.push(Change::NodeChanged(*key));
                }
                Some(_) => {}
            }
        }
        for key in before.nodes.keys() {
            if !after.nodes.contains_key(key) {
                changes.push(Change::NodeRemoved(*key));
            }
        }
        for link in after.links.difference(&before.links) {
            changes.push(Change::LinkAdded(link.clone()));
        }
        for link in before.links.difference(&after.links) {
            changes.push(Change::LinkRemoved(link.clone()));
        }
        changes
    }

    /// Whether `from` reaches `to` along existing wires.
    fn reaches(&self, from: NodeKey, to: NodeKey) -> bool {
        let mut seen = BTreeSet::new();
        let mut pending = vec![from];
        while let Some(at) = pending.pop() {
            if at == to {
                return true;
            }
            if !seen.insert(at) {
                continue;
            }
            pending.extend(
                self.links
                    .iter()
                    .filter(|link| link.from == at)
                    .map(|link| link.to),
            );
        }
        false
    }

    /// The pin of this node, this name and this direction, or why there is none.
    fn pin_of(&self, key: NodeKey, name: &str, direction: PinDirection) -> Result<&Pin> {
        let node = self
            .nodes
            .get(&key)
            .ok_or_else(|| Self::no_such_node(key))?;
        let node_type = self.catalogue.get(&node.type_name).ok_or_else(|| {
            Problem::new(
                format!("wire the {} node {}", node.type_name, key.ordinal()),
                "the loaded catalogue does not declare its type, so its pins are unknown",
            )
            .with_remedy("open the domain whose catalogue declares it")
        })?;
        node_type.pin(name, direction).ok_or_else(|| {
            Problem::new(
                format!("wire {name} on a {} node", node.type_name),
                format!(
                    "{} declares no {direction:?} pin called {name}",
                    node.type_name
                ),
            )
            .with_remedy("name one of its pins")
        })
    }

    fn no_such_node(key: NodeKey) -> Problem {
        Problem::new(
            format!("act on graph node {}", key.ordinal()),
            "the canvas holds no node with that key",
        )
        .with_remedy("place the node first, or name one that is there")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A domain-neutral vocabulary. The canvas is generic, so its own behaviour is tested on its
    /// own terms rather than through one domain's palette — which is the claim being made about it.
    fn catalogue() -> Catalogue {
        Catalogue::new(vec![
            NodeType::new(
                "test.source",
                vec![Pin::new("out", PinDirection::Output, "float")],
            ),
            NodeType::new(
                "test.sink",
                vec![Pin::new("in", PinDirection::Input, "float")],
            ),
            NodeType::new(
                "test.relay",
                vec![
                    Pin::new("in", PinDirection::Input, "float"),
                    Pin::new("out", PinDirection::Output, "float"),
                ],
            ),
            NodeType::new(
                "test.flag",
                vec![Pin::new("out", PinDirection::Output, "bool")],
            ),
        ])
        .expect("four distinct node types")
    }

    fn canvas() -> GraphCanvas {
        let mut canvas = GraphCanvas::new(7);
        canvas.load(catalogue());
        canvas
    }

    #[test]
    fn a_wire_between_pins_of_different_types_is_refused_naming_both() {
        let mut canvas = canvas();
        let flag = canvas
            .add("test.flag", Layout::default())
            .expect("a flag node");
        let sink = canvas
            .add("test.sink", Layout::default())
            .expect("a sink node");
        let refused = canvas
            .connect(flag, "out", sink, "in")
            .expect_err("bool does not flow into float");
        assert!(
            refused.because.contains("bool") && refused.because.contains("float"),
            "the refusal names neither type: {refused:?}"
        );
        assert_eq!(canvas.links().count(), 0, "a refused wire was made anyway");
    }

    #[test]
    fn a_wire_that_would_close_a_cycle_is_refused_naming_the_path() {
        let mut canvas = canvas();
        let first = canvas
            .add("test.relay", Layout::default())
            .expect("a relay");
        let second = canvas
            .add("test.relay", Layout::default())
            .expect("a relay");
        canvas
            .connect(first, "out", second, "in")
            .expect("a forward wire");
        let refused = canvas
            .connect(second, "out", first, "in")
            .expect_err("the second wire closes a cycle");
        assert!(
            refused.because.contains("cycle"),
            "the refusal does not say what is wrong: {refused:?}"
        );
        assert_eq!(canvas.links().count(), 1, "the cycle was wired anyway");
    }

    #[test]
    fn a_diagnostic_names_the_node_and_the_pin() {
        let mut canvas = canvas();
        let sink = canvas
            .add("test.sink", Layout::default())
            .expect("a sink node");
        let reported = canvas.diagnostics();
        assert_eq!(reported.len(), 1, "expected one unwired required input");
        assert_eq!(reported[0].node, sink);
        assert_eq!(
            reported[0].pin.as_deref(),
            Some("in"),
            "a diagnostic that names a graph and not a pin sends an author hunting"
        );

        canvas
            .set_property(sink, "in", "0.5")
            .expect("a literal on the pin");
        assert!(
            canvas.diagnostics().is_empty(),
            "a valued pin still reports"
        );
    }

    #[test]
    fn the_diff_reports_meaning_and_ignores_where_the_boxes_sit() {
        let mut before = canvas();
        let node = before
            .add("test.relay", Layout::default())
            .expect("a relay");
        let mut after = before.clone();
        after
            .move_to(node, Layout { x: 500.0, y: 500.0 })
            .expect("the node moves");
        assert!(
            GraphCanvas::diff(&before, &after).is_empty(),
            "moving a node changed the graph's meaning"
        );

        after.set_property(node, "gain", "2").expect("a property");
        assert_eq!(
            GraphCanvas::diff(&before, &after),
            vec![Change::NodeChanged(node)]
        );

        let extra = after
            .add("test.source", Layout::default())
            .expect("a source");
        after.connect(extra, "out", node, "in").expect("a wire");
        let changes = GraphCanvas::diff(&before, &after);
        assert!(changes.contains(&Change::NodeAdded(extra)));
        assert_eq!(
            changes
                .iter()
                .filter(|change| matches!(change, Change::LinkAdded(_)))
                .count(),
            1
        );
    }

    #[test]
    fn a_node_type_the_catalogue_does_not_declare_cannot_be_placed() {
        let mut canvas = canvas();
        let refused = canvas
            .add("material.multiply", Layout::default())
            .expect_err("no such type in this vocabulary");
        assert!(refused.because.contains("material.multiply"), "{refused:?}");
        assert_eq!(canvas.nodes().count(), 0);
    }

    #[test]
    fn removing_a_node_takes_its_wires_and_its_selection_with_it() {
        let mut canvas = canvas();
        let source = canvas
            .add("test.source", Layout::default())
            .expect("a source");
        let sink = canvas.add("test.sink", Layout::default()).expect("a sink");
        canvas.connect(source, "out", sink, "in").expect("a wire");
        canvas.select([source, sink]).expect("both selected");
        canvas.remove(source).expect("the source goes");
        assert_eq!(canvas.links().count(), 0, "a wire outlived its node");
        assert_eq!(canvas.selection(), vec![sink]);
    }
}
