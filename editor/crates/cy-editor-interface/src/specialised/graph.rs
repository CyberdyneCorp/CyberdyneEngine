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
    /// Stable engine-assigned identity, or zero for a legacy catalogue.
    pub identity: u32,
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
            identity: 0,
            name: name.into(),
            direction,
            data_type: data_type.into(),
        }
    }
}

/// Generic property control kind supplied by a backend graph catalogue.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PropertyKind {
    /// Identifier or arbitrary text.
    Text,
    /// Boolean checkbox.
    Bool,
    /// One numeric scalar.
    Scalar,
    /// A comma-separated numeric vector.
    Vector,
    /// One value from a declared choice list.
    Enumeration,
    /// Stable project asset reference constrained by kind.
    Asset,
}

/// One catalogue-driven node property.
#[derive(Clone, PartialEq, Debug)]
pub struct Property {
    /// Stable identity within the node type.
    pub identity: u32,
    /// Serialized property name.
    pub name: String,
    /// Generic control/value kind.
    pub kind: PropertyKind,
    /// Typed default encoded in the graph's readable literal form.
    pub default: String,
    /// Legacy schema-1 range, choices, identifier rule, or required asset kind.
    pub constraint: String,
    /// Backend-owned authoring explanation.
    pub tooltip: String,
    /// Optional inclusive numeric minimum.
    pub minimum: Option<f64>,
    /// Optional inclusive numeric maximum.
    pub maximum: Option<f64>,
    /// Optional numeric editing increment.
    pub step: Option<f64>,
    /// Declared enumeration choices, in presentation order.
    pub choices: Vec<String>,
    /// Required project asset kind, empty for non-asset properties.
    pub asset_kind: String,
    /// Backend-owned semantic role such as `identifier` or `linear-colour`.
    pub semantic: String,
    /// Compiler/runtime stage that consumes this property.
    pub stage: String,
    /// Graph domain which owns the property.
    pub domain: String,
    /// Target feature bits required to author or compile this property.
    pub required_capabilities: u64,
    /// Required vector lane count, or zero when another property determines it.
    pub vector_lanes: u8,
}

impl Property {
    /// Validate the readable literal before it enters authored graph state.
    pub fn validate_literal(&self, value: &str) -> Result<()> {
        let invalid = |because: String| {
            Problem::new(format!("set the {} property", self.name), because)
                .with_remedy(format!("enter a value accepted by {}", self.tooltip))
        };
        match self.kind {
            PropertyKind::Text => {
                if self.semantic == "identifier"
                    && !value.is_empty()
                    && !value
                        .bytes()
                        .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
                {
                    return Err(invalid("the value is not an ASCII identifier".into()));
                }
            }
            PropertyKind::Bool => {
                if !matches!(value, "true" | "false") {
                    return Err(invalid("a boolean is `true` or `false`".into()));
                }
            }
            PropertyKind::Scalar => {
                let parsed = value
                    .parse::<f64>()
                    .map_err(|_| invalid("the value is not a number".into()))?;
                self.validate_number(parsed, &invalid)?;
            }
            PropertyKind::Vector => {
                let values = value
                    .split(',')
                    .map(str::trim)
                    .map(str::parse::<f64>)
                    .collect::<std::result::Result<Vec<_>, _>>()
                    .map_err(|_| invalid("every vector lane must be a number".into()))?;
                if values.is_empty()
                    || self.vector_lanes != 0 && values.len() != usize::from(self.vector_lanes)
                {
                    return Err(invalid(format!(
                        "the value needs {} numeric lane(s)",
                        self.vector_lanes
                    )));
                }
                for value in values {
                    self.validate_number(value, &invalid)?;
                }
            }
            PropertyKind::Enumeration => {
                if !self.choices.iter().any(|choice| choice == value) {
                    return Err(invalid(format!(
                        "{value:?} is not one of {}",
                        self.choices.join(", ")
                    )));
                }
            }
            PropertyKind::Asset => {}
        }
        Ok(())
    }

    fn validate_number(&self, value: f64, invalid: &impl Fn(String) -> Problem) -> Result<()> {
        if !value.is_finite() {
            return Err(invalid("the value must be finite".into()));
        }
        if self.minimum.is_some_and(|minimum| value < minimum)
            || self.maximum.is_some_and(|maximum| value > maximum)
        {
            return Err(invalid(format!(
                "{value} is outside {} through {}",
                self.minimum
                    .map_or_else(|| "−∞".into(), |minimum| minimum.to_string()),
                self.maximum
                    .map_or_else(|| "+∞".into(), |maximum| maximum.to_string())
            )));
        }
        Ok(())
    }
}

/// A node type as a domain registers it: what it is called, and what it connects by.
#[derive(Clone, PartialEq, Debug)]
pub struct NodeType {
    /// Stable engine-assigned identity, or zero for a legacy catalogue.
    pub identity: u32,
    /// Version of this node's serialized schema.
    pub schema_version: u32,
    /// The engine's own spelling — `script.add_float`, `pose.blend`, `ai.selector`. This is the
    /// name `cy::graph::NodeRegistry` interns, and the contract gate compares this list against the
    /// engine's lowering tables so that a node type added to one side and not the other is red.
    pub name: String,
    /// Its pins, in declaration order.
    pub pins: Vec<Pin>,
    /// Properties rendered generically by clients.
    pub properties: Vec<Property>,
}

impl NodeType {
    /// A node type and its pins.
    pub fn new(name: impl Into<String>, pins: Vec<Pin>) -> Self {
        Self {
            identity: 0,
            schema_version: 1,
            name: name.into(),
            pins,
            properties: Vec::new(),
        }
    }

    /// A node type described by the engine-owned catalogue.
    pub fn identified(identity: u32, schema_version: u32, name: String, pins: Vec<Pin>) -> Self {
        Self {
            identity,
            schema_version,
            name,
            pins,
            properties: Vec::new(),
        }
    }

    /// Attach backend-owned property descriptors.
    #[must_use]
    pub fn with_properties(mut self, properties: Vec<Property>) -> Self {
        self.properties = properties;
        self
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
#[derive(Clone, PartialEq, Debug, Default)]
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
    /// The same authored values addressed by stable catalogue property identity.
    pub property_identities: BTreeMap<u32, String>,
    /// Last readable name seen for each stable property identity, used only for migration cleanup.
    pub property_names: BTreeMap<u32, String>,
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
    /// Stable identity of the target pin. The name remains readable migration metadata.
    pub to_pin_identity: u32,
    /// Stable identity of the source pin. The name remains readable migration metadata.
    pub from_pin_identity: u32,
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

    /// Replace the domain vocabulary without discarding authored graph state.
    ///
    /// A backend reconnect may return a newer compatible catalogue while the author is editing.
    /// Existing nodes remain present even when their definition disappeared; [`Self::diagnostics`]
    /// then reports the missing type instead of turning a service refresh into data loss.
    pub fn replace_catalogue(&mut self, catalogue: Catalogue) {
        self.catalogue = catalogue;
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
                property_identities: BTreeMap::new(),
                property_names: BTreeMap::new(),
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
        let name = name.into();
        let value = value.into();
        let descriptor = self
            .nodes
            .get(&key)
            .and_then(|node| self.catalogue.get(&node.type_name))
            .and_then(|node_type| {
                node_type
                    .properties
                    .iter()
                    .find(|property| property.name == name)
            })
            .cloned();
        if let Some(descriptor) = descriptor {
            return self.set_property_by_identity(key, descriptor.identity, value);
        }
        let node = self
            .nodes
            .get_mut(&key)
            .ok_or_else(|| Self::no_such_node(key))?;
        node.properties.insert(name, value);
        Ok(())
    }

    /// Set a catalogue property by stable identity, validating its typed constraints first.
    pub fn set_property_by_identity(
        &mut self,
        key: NodeKey,
        identity: u32,
        value: impl Into<String>,
    ) -> Result<()> {
        let value = value.into();
        let descriptor = self
            .nodes
            .get(&key)
            .ok_or_else(|| Self::no_such_node(key))
            .and_then(|node| {
                self.catalogue
                    .get(&node.type_name)
                    .and_then(|node_type| {
                        node_type
                            .properties
                            .iter()
                            .find(|property| property.identity == identity)
                    })
                    .cloned()
                    .ok_or_else(|| {
                        Problem::new(
                            format!("set property {identity} on {}", node.type_name),
                            "the current catalogue does not declare that property identity",
                        )
                    })
            })?;
        descriptor.validate_literal(&value)?;
        let node = self
            .nodes
            .get_mut(&key)
            .expect("the descriptor lookup found the node");
        node.properties
            .insert(descriptor.name.clone(), value.clone());
        node.property_identities.insert(identity, value);
        node.property_names.insert(identity, descriptor.name);
        Ok(())
    }

    /// Read an authored value by stable identity, falling back to readable legacy metadata.
    #[must_use]
    pub fn property_value(&self, key: NodeKey, property: &Property) -> Option<&str> {
        let node = self.nodes.get(&key)?;
        node.property_identities
            .get(&property.identity)
            .or_else(|| node.properties.get(&property.name))
            .map(String::as_str)
    }

    /// Authored values resolved through the current catalogue names, in deterministic order.
    #[must_use]
    pub fn resolved_properties(&self, key: NodeKey) -> BTreeMap<String, String> {
        let Some(node) = self.nodes.get(&key) else {
            return BTreeMap::new();
        };
        let mut resolved = node.properties.clone();
        if let Some(node_type) = self.catalogue.get(&node.type_name) {
            for property in &node_type.properties {
                if let Some(value) = node.property_identities.get(&property.identity) {
                    if let Some(previous_name) = node.property_names.get(&property.identity) {
                        resolved.remove(previous_name);
                    }
                    resolved.insert(property.name.clone(), value.clone());
                }
            }
        }
        resolved
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
            to_pin_identity: target.identity,
            from_pin_identity: source.identity,
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

    /// Wire pins by their persistent catalogue identities.
    ///
    /// Names remain readable metadata on [`Link`], but an editor gesture addresses the definitions
    /// by ID so a compatible catalogue refresh cannot silently move the gesture to a same-named
    /// replacement pin.
    pub fn connect_identified(
        &mut self,
        from: NodeKey,
        from_pin: u32,
        to: NodeKey,
        to_pin: u32,
    ) -> Result<()> {
        let source = self
            .pin_with_identity(from, from_pin, PinDirection::Output)?
            .name
            .clone();
        let target = self
            .pin_with_identity(to, to_pin, PinDirection::Input)?
            .name
            .clone();
        self.connect(from, &source, to, &target)
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
                    if was.type_name != node.type_name
                        || was.properties != node.properties
                        || was.property_identities != node.property_identities
                        || was.property_names != node.property_names =>
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

    fn pin_with_identity(
        &self,
        key: NodeKey,
        identity: u32,
        direction: PinDirection,
    ) -> Result<&Pin> {
        let node = self
            .nodes
            .get(&key)
            .ok_or_else(|| Self::no_such_node(key))?;
        let node_type = self.catalogue.get(&node.type_name).ok_or_else(|| {
            Problem::new(
                format!("wire the {} node {}", node.type_name, key.ordinal()),
                "the loaded catalogue does not declare its type, so its pins are unknown",
            )
            .with_remedy("restore the plugin or choose a declared node type")
        })?;
        node_type
            .pins
            .iter()
            .find(|pin| pin.identity == identity && pin.direction == direction)
            .ok_or_else(|| {
                Problem::new(
                    format!("wire pin identity {identity} on a {} node", node.type_name),
                    format!(
                        "{} declares no {direction:?} pin carrying identity {identity}",
                        node.type_name
                    ),
                )
                .with_remedy("refresh the catalogue and select one of its stable pin identities")
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

    fn scalar_property(name: &str) -> Property {
        Property {
            identity: 7,
            name: name.into(),
            kind: PropertyKind::Scalar,
            default: "0.5".into(),
            constraint: String::new(),
            tooltip: "A bounded scalar".into(),
            minimum: Some(0.0),
            maximum: Some(1.0),
            step: Some(0.1),
            choices: Vec::new(),
            asset_kind: String::new(),
            semantic: "unit-interval".into(),
            stage: "runtime".into(),
            domain: "test".into(),
            required_capabilities: 0,
            vector_lanes: 0,
        }
    }

    #[test]
    fn typed_properties_refuse_invalid_values_before_mutating_the_canvas() {
        let node_type = NodeType::identified(9, 1, "test.typed".into(), Vec::new())
            .with_properties(vec![scalar_property("roughness")]);
        let mut canvas = GraphCanvas::new(9);
        canvas.load(Catalogue::new(vec![node_type]).unwrap());
        let node = canvas.add("test.typed", Layout::default()).unwrap();

        let refused = canvas
            .set_property_by_identity(node, 7, "1.5")
            .expect_err("the declared maximum is authoritative");
        assert!(refused.because.contains("outside"));
        assert!(
            canvas
                .property_value(node, &scalar_property("roughness"))
                .is_none()
        );
    }

    #[test]
    fn property_values_survive_a_catalogue_rename_by_stable_identity() {
        let original = NodeType::identified(9, 1, "test.typed".into(), Vec::new())
            .with_properties(vec![scalar_property("roughness")]);
        let mut canvas = GraphCanvas::new(9);
        canvas.load(Catalogue::new(vec![original]).unwrap());
        let node = canvas.add("test.typed", Layout::default()).unwrap();
        canvas
            .set_property_by_identity(node, 7, "0.8")
            .expect("valid typed value");

        let renamed = NodeType::identified(9, 2, "test.typed".into(), Vec::new())
            .with_properties(vec![scalar_property("surface_roughness")]);
        canvas.replace_catalogue(Catalogue::new(vec![renamed]).unwrap());
        let descriptor = &canvas.catalogue().get("test.typed").unwrap().properties[0];
        assert_eq!(canvas.property_value(node, descriptor), Some("0.8"));
        assert_eq!(
            canvas.resolved_properties(node).get("surface_roughness"),
            Some(&"0.8".to_string())
        );
        assert!(!canvas.resolved_properties(node).contains_key("roughness"));
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
    fn a_visible_connection_records_stable_pin_identities() {
        let mut source_pin = Pin::new("renamable_out", PinDirection::Output, "float");
        source_pin.identity = 41;
        let mut target_pin = Pin::new("renamable_in", PinDirection::Input, "float");
        target_pin.identity = 73;
        let catalogue = Catalogue::new(vec![
            NodeType::identified(10, 1, "test.identified_source".into(), vec![source_pin]),
            NodeType::identified(11, 1, "test.identified_sink".into(), vec![target_pin]),
        ])
        .expect("identified catalogue");
        let mut canvas = GraphCanvas::new(8);
        canvas.load(catalogue);
        let source = canvas
            .add("test.identified_source", Layout::default())
            .expect("source");
        let target = canvas
            .add("test.identified_sink", Layout::default())
            .expect("target");

        canvas
            .connect_identified(source, 41, target, 73)
            .expect("stable identities connect");
        let link = canvas.links().next().expect("the link");
        assert_eq!(link.from_pin_identity, 41);
        assert_eq!(link.to_pin_identity, 73);
        assert_eq!(link.from_pin, "renamable_out");
        assert_eq!(link.to_pin, "renamable_in");
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
