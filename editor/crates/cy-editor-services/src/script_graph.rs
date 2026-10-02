// SPDX-License-Identifier: MIT
//! Gameplay graphs: the `.cyscript` source the editor writes, and the wire to the engine's
//! `script.*` operations. Issue #29, visual scripting.
//!
//! **The source is the engine's canonical CyberGraph text** (`cygraph 1`, `src/graph/include/cy/
//! graph/text.h`), written here in exactly the order and spelling `cy::graph::write_graph` uses:
//! nodes by key, a node's properties by name, wires by `(to, to pin, from, from pin)`, layout last
//! and apart from meaning, and every float as C's `%.9g`. So the file the editor saves is the file
//! the engine would write, a text diff of it is a semantic diff, and the engine's three-way merge
//! reads it unchanged. `src/editor_backend/tests/data/script_unit_command_v1.cyscript` pins that
//! from both sides: this crate's tests and the MCP suite must write it byte for byte, and the
//! engine's suite must read it back to the same bytes.
//!
//! The editor computes nothing about what a graph means. The engine declares the node vocabulary
//! (`script.catalogue.get`), compiles (`script.compile`) and runs it in Play
//! (`script.event.raise`, `script.state.get`); the payloads are specified in
//! `src/editor_backend/include/cy/editor/script_service.h` and decoded below.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

/// The undo record kind for a saved graph: this prefix and the project-relative reference.
pub const DOMAIN_PREFIX: &str = "script_graph:";
/// The scene component that attaches a graph to an entity, and its one field.
pub const COMPONENT: &str = "ScriptGraph";
/// The field of [`COMPONENT`] naming the project-relative `.cyscript`.
pub const FIELD: &str = "graph";
/// What a gameplay graph file is called.
pub const EXTENSION: &str = "cyscript";
/// The event a new graph answers, and the one Play's command raises.
pub const DEFAULT_EVENT: &str = "unit.command";

/// The engine's `script.*` operations.
pub const CATALOGUE: &str = "script.catalogue.get";
/// Compile a source and answer its program or what refused it.
pub const COMPILE: &str = "script.compile";
/// Raise an event on an entity during Play.
pub const RAISE: &str = "script.event.raise";
/// Read Play's graph instances and the cues they played.
pub const STATE: &str = "script.state.get";

/// The wire format every `script.*` payload begins with.
pub(crate) const WIRE_FORMAT: u32 = 1;

/// Every capability, in the order `cy::graph::write_graph` writes them.
pub const CAPABILITIES: [&str; 11] = [
    "read_world",
    "write_world",
    "spawn_entity",
    "destroy_entity",
    "physics",
    "audio",
    "network",
    "file_system",
    "randomness",
    "wall_clock",
    "native_call",
];

/// What a new gameplay graph is granted: `cy::game_backend::kGameplayGraphCapabilities`.
pub const GAMEPLAY_CAPABILITIES: [&str; 3] = ["read_world", "write_world", "audio"];

/// Refuse a reference that is not a project-relative `.cyscript` inside the project.
///
/// # Errors
///
/// An empty, absolute or escaping path, or another extension.
pub fn validate_reference(reference: &str) -> Result<()> {
    let path = std::path::Path::new(reference);
    let stem = path
        .file_stem()
        .and_then(|stem| stem.to_str())
        .unwrap_or("");
    // Checked on the text as well as the components: `Path` reads `/a` as relative on Windows and
    // `C:/a` as relative on Unix, and the reference is the same string on every host.
    let fine = !reference.is_empty()
        && !reference.starts_with('/')
        && !reference.contains(':')
        && path
            .components()
            .all(|component| matches!(component, std::path::Component::Normal(_)))
        && !reference.contains('\\')
        && path.extension().and_then(|extension| extension.to_str()) == Some(EXTENSION)
        && !stem.is_empty()
        && stem
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-');
    if fine {
        Ok(())
    } else {
        Err(Problem::new(
            format!("use {reference:?} as a gameplay graph"),
            "a gameplay graph is a project-relative .cyscript whose name is letters, digits, \
             underscores or hyphens",
        )
        .with_remedy("for example game/scripts/unit_command.cyscript"))
    }
}

/// The graph's name: the file's stem, which the engine reports instances by.
#[must_use]
pub fn graph_name(reference: &str) -> String {
    std::path::Path::new(reference)
        .file_stem()
        .and_then(|stem| stem.to_str())
        .unwrap_or_default()
        .to_owned()
}

/// A float as C's `printf("%.9g")` writes it, which round-trips an `f32` exactly and is the
/// spelling the engine's canonical text uses.
#[must_use]
pub fn format_float(value: f32) -> String {
    if value == 0.0 {
        return if value.is_sign_negative() { "-0" } else { "0" }.into();
    }
    if !value.is_finite() {
        return if value.is_nan() {
            "nan"
        } else if value > 0.0 {
            "inf"
        } else {
            "-inf"
        }
        .into();
    }
    let wide = f64::from(value);
    let scientific = format!("{wide:.8e}");
    let (mantissa, exponent) = scientific
        .split_once('e')
        .expect("Rust's exponent format always has an exponent");
    let exponent: i32 = exponent.parse().expect("an exponent is a number");
    if !(-4..9).contains(&exponent) {
        let sign = if exponent < 0 { '-' } else { '+' };
        return format!(
            "{}e{sign}{:02}",
            trim_fraction(mantissa),
            exponent.unsigned_abs()
        );
    }
    let decimals = usize::try_from(8 - exponent).unwrap_or(0);
    trim_fraction(&format!("{wide:.decimals$}"))
}

fn trim_fraction(text: &str) -> String {
    if !text.contains('.') {
        return text.to_owned();
    }
    text.trim_end_matches('0').trim_end_matches('.').to_owned()
}

/// A property's value, as the text form holds it: numbers as `(x, y, z, w, mask)` or a name.
#[derive(Clone, PartialEq, Debug)]
pub enum Literal {
    /// Four lanes and the integer mask. An `int` or `bool` lives in the mask.
    Number {
        /// `x`, `y`, `z`, `w`.
        lanes: [f32; 4],
        /// The integer lane.
        mask: u32,
    },
    /// An identifier, a tag, a path.
    Text(String),
}

impl Literal {
    /// The literal for `value` written at the property's literal type: `float`, `int`, `bool`
    /// or anything else, which is a name.
    ///
    /// # Errors
    ///
    /// A number that does not read, or a whole number below zero.
    pub fn parse(literal_type: &str, value: &str) -> Result<Self> {
        let refuse = |why: &str| {
            Problem::new(
                format!("write {value:?} as a {literal_type}"),
                why.to_owned(),
            )
        };
        match literal_type {
            "float" => {
                let number: f32 = value
                    .trim()
                    .parse()
                    .map_err(|_| refuse("it is not a number"))?;
                if !number.is_finite() {
                    return Err(refuse("a graph's number is finite"));
                }
                Ok(Self::Number {
                    lanes: [number, 0.0, 0.0, 0.0],
                    mask: 0,
                })
            }
            "int" => {
                let number: u32 = value
                    .trim()
                    .parse()
                    .map_err(|_| refuse("it is not a whole number of zero or more"))?;
                Ok(Self::Number {
                    lanes: [0.0; 4],
                    mask: number,
                })
            }
            "bool" => match value.trim() {
                "true" | "1" => Ok(Self::Number {
                    lanes: [0.0; 4],
                    mask: 1,
                }),
                "false" | "0" => Ok(Self::Number {
                    lanes: [0.0; 4],
                    mask: 0,
                }),
                _ => Err(refuse("it is true or false")),
            },
            _ => Ok(Self::Text(value.to_owned())),
        }
    }

    /// The value as the canvas shows it, for a property of `literal_type`.
    #[must_use]
    pub fn display(&self, literal_type: &str) -> String {
        match (self, literal_type) {
            (Self::Number { lanes, .. }, "float") => format_float(lanes[0]),
            (Self::Number { mask, .. }, "int") => mask.to_string(),
            (Self::Number { mask, .. }, "bool") => (*mask != 0).to_string(),
            (Self::Text(text), _) => text.clone(),
            // A name the engine wrote while empty reads back as the zero tuple.
            (Self::Number { .. }, _) => String::new(),
        }
    }

    fn write(&self, out: &mut String) {
        match self {
            // The engine writes an empty name as the zero tuple; so does this, or the bytes differ.
            Self::Text(text) if !text.is_empty() => out.push_str(&quoted(text)),
            Self::Text(_) => out.push_str("(0, 0, 0, 0, 0)"),
            Self::Number { lanes, mask } => {
                let _ = write!(
                    out,
                    "({}, {}, {}, {}, {mask})",
                    format_float(lanes[0]),
                    format_float(lanes[1]),
                    format_float(lanes[2]),
                    format_float(lanes[3])
                );
            }
        }
    }
}

/// One property of one node.
#[derive(Clone, PartialEq, Debug)]
pub struct Property {
    /// Its name.
    pub name: String,
    /// The literal type it is written at: `float`, `int`, `bool`, `name`.
    pub literal_type: String,
    /// Its value.
    pub literal: Literal,
}

/// One node of a gameplay graph.
#[derive(Clone, PartialEq, Debug)]
pub struct ScriptNode {
    /// Its stable identity.
    pub key: u64,
    /// Its type: `script.call`, `script.on_event`.
    pub type_name: String,
    /// The type version it was authored at.
    pub version: u32,
    /// Muted nodes stay in the graph and compile to nothing.
    pub muted: bool,
    /// Its properties. Written in name order.
    pub properties: Vec<Property>,
    /// A body this editor could not read, kept verbatim, as the engine keeps an unknown node's.
    pub opaque: Option<String>,
}

impl ScriptNode {
    /// The property named `name`.
    #[must_use]
    pub fn property(&self, name: &str) -> Option<&Property> {
        self.properties
            .iter()
            .find(|property| property.name == name)
    }
}

/// A wire. Ordered `(to, to pin, from, from pin)`, the engine's order.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct ScriptLink {
    /// The node the wire enters.
    pub to: u64,
    /// Its input pin.
    pub to_pin: String,
    /// The node the wire leaves.
    pub from: u64,
    /// Its output pin.
    pub from_pin: String,
}

/// A gameplay graph, as its `.cyscript` holds it.
#[derive(Clone, PartialEq, Debug)]
pub struct ScriptGraph {
    /// The graph's name: its file's stem.
    pub name: String,
    /// The graph format version.
    pub version: u32,
    /// What it was granted, in the engine's order.
    pub capabilities: Vec<String>,
    /// Whether its author claims it is deterministic; the engine audits the claim.
    pub deterministic: bool,
    /// `interface` lines, kept verbatim: the editor does not author subgraph interfaces yet.
    pub interface: Vec<String>,
    /// The nodes, by key.
    pub nodes: BTreeMap<u64, ScriptNode>,
    /// The wires.
    pub links: BTreeSet<ScriptLink>,
    /// Where each node sits, and any tint or comment after it, kept verbatim. Not meaning.
    pub layout: BTreeMap<u64, (f32, f32, String)>,
}

impl ScriptGraph {
    /// An empty gameplay graph, granted what gameplay graphs are granted.
    #[must_use]
    pub fn new(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            version: 1,
            capabilities: GAMEPLAY_CAPABILITIES.map(String::from).to_vec(),
            deterministic: true,
            interface: Vec::new(),
            nodes: BTreeMap::new(),
            links: BTreeSet::new(),
            layout: BTreeMap::new(),
        }
    }

    /// The canonical text: the bytes `cy::graph::write_graph` writes for this graph.
    #[must_use]
    pub fn encode(&self) -> String {
        let mut out = String::new();
        let _ = writeln!(out, "cygraph 1");
        let _ = writeln!(out, "graph {} version {}", quoted(&self.name), self.version);
        out.push_str("capability");
        for capability in CAPABILITIES {
            if self
                .capabilities
                .iter()
                .any(|granted| granted == capability)
            {
                out.push(' ');
                out.push_str(capability);
            }
        }
        let _ = writeln!(out, "\ndeterministic {}", self.deterministic);
        for line in &self.interface {
            let _ = writeln!(out, "{line}");
        }
        for node in self.nodes.values() {
            write_node(&mut out, node);
        }
        for link in &self.links {
            let _ = writeln!(
                out,
                "link {} {} -> {} {}",
                link.from,
                quoted(&link.from_pin),
                link.to,
                quoted(&link.to_pin)
            );
        }
        for (key, (x, y, rest)) in &self.layout {
            let _ = writeln!(
                out,
                "layout {key} at {} {}{rest}",
                format_float(*x),
                format_float(*y)
            );
        }
        out
    }

    /// Read a `.cyscript`.
    ///
    /// # Errors
    ///
    /// Anything the engine's reader would refuse: the header, a malformed line, a duplicate key.
    pub fn decode(text: &str) -> Result<Self> {
        Decoder::new(text).graph()
    }

    /// The next free node key: one past the largest in use.
    #[must_use]
    pub fn next_key(&self) -> u64 {
        self.nodes.keys().next_back().map_or(1, |key| key + 1)
    }
}

fn quoted(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for character in text.chars() {
        if character == '"' || character == '\\' {
            out.push('\\');
        }
        out.push(character);
    }
    out.push('"');
    out
}

fn write_node(out: &mut String, node: &ScriptNode) {
    let _ = write!(
        out,
        "node {} {} v{}",
        node.key,
        quoted(&node.type_name),
        node.version
    );
    if node.muted {
        out.push_str(" muted");
    }
    out.push_str(" {\n");
    if let Some(body) = &node.opaque {
        out.push_str(body);
        out.push_str("}\n");
        return;
    }
    let mut properties: Vec<&Property> = node.properties.iter().collect();
    properties.sort_by(|a, b| a.name.cmp(&b.name));
    for property in properties {
        let _ = write!(
            out,
            "    prop {} : {} = ",
            quoted(&property.name),
            quoted(&property.literal_type)
        );
        property.literal.write(out);
        out.push('\n');
    }
    out.push_str("}\n");
}

/// The reader, line by line like the engine's.
struct Decoder<'a> {
    lines: std::iter::Peekable<std::str::Lines<'a>>,
}

impl<'a> Decoder<'a> {
    fn new(text: &'a str) -> Self {
        Self {
            lines: text.lines().peekable(),
        }
    }

    fn refuse(why: impl Into<String>) -> Problem {
        Problem::new("read a gameplay graph", why.into())
    }

    /// The next meaningful line: trimmed, comments and blanks skipped.
    fn next(&mut self) -> Option<&'a str> {
        for line in self.lines.by_ref() {
            let trimmed = line.trim_matches(|c| c == ' ' || c == '\t' || c == '\r');
            if !trimmed.is_empty() && !trimmed.starts_with('#') {
                return Some(trimmed);
            }
        }
        None
    }

    fn graph(mut self) -> Result<ScriptGraph> {
        if self.next() != Some("cygraph 1") {
            return Err(Self::refuse("it does not begin with `cygraph 1`"));
        }
        let header = self
            .next()
            .ok_or_else(|| Self::refuse("a graph names itself on its second line"))?;
        let mut tokens = Tokens::new(header);
        if tokens.word() != Some("graph") {
            return Err(Self::refuse("the second line names the graph"));
        }
        let name = tokens
            .quoted()
            .ok_or_else(|| Self::refuse("a graph needs a quoted name"))?;
        let mut graph = ScriptGraph::new(name);
        graph.capabilities.clear();
        if tokens.word() == Some("version") {
            graph.version = tokens.word().and_then(|v| v.parse().ok()).unwrap_or(1);
        }
        while let Some(line) = self.next() {
            self.line(&mut graph, line)?;
        }
        Ok(graph)
    }

    fn line(&mut self, graph: &mut ScriptGraph, line: &'a str) -> Result<()> {
        let mut tokens = Tokens::new(line);
        match tokens.word() {
            Some("node") => self.node(graph, line),
            Some("link") => link(graph, &mut tokens),
            Some("layout") => layout(graph, &mut tokens),
            Some("interface") => {
                graph.interface.push(line.to_owned());
                Ok(())
            }
            Some("capability") => {
                while let Some(capability) = tokens.word() {
                    if !CAPABILITIES.contains(&capability) {
                        return Err(Self::refuse(format!(
                            "no capability is called {capability}"
                        )));
                    }
                    graph.capabilities.push(capability.to_owned());
                }
                Ok(())
            }
            Some("deterministic") => {
                graph.deterministic = tokens.word() != Some("false");
                Ok(())
            }
            _ => Err(Self::refuse(format!(
                "a line begins with a word the format does not have: {line:?}"
            ))),
        }
    }

    fn node(&mut self, graph: &mut ScriptGraph, header: &str) -> Result<()> {
        let mut tokens = Tokens::new(header);
        let _ = tokens.word();
        let key: u64 = tokens
            .word()
            .and_then(|key| key.parse().ok())
            .ok_or_else(|| Self::refuse("a node needs a key"))?;
        let type_name = tokens
            .quoted()
            .ok_or_else(|| Self::refuse("a node needs a quoted type"))?;
        let mut node = ScriptNode {
            key,
            type_name,
            version: 1,
            muted: false,
            properties: Vec::new(),
            opaque: None,
        };
        while let Some(word) = tokens.word() {
            match word {
                "{" => break,
                "muted" => node.muted = true,
                version if version.starts_with('v') => {
                    node.version = version[1..].parse().unwrap_or(1);
                }
                _ => {}
            }
        }
        let mut raw = String::new();
        let mut unreadable = false;
        for line in self.lines.by_ref() {
            if line.trim() == "}" {
                break;
            }
            raw.push_str(line);
            raw.push('\n');
            match property(line.trim()) {
                Some(property) => node.properties.push(property),
                None => unreadable = !line.trim().is_empty(),
            }
        }
        if unreadable {
            node.properties.clear();
            node.opaque = Some(raw);
        }
        if graph.nodes.insert(key, node).is_some() {
            return Err(Self::refuse(format!("two nodes have the key {key}")));
        }
        Ok(())
    }
}

fn property(line: &str) -> Option<Property> {
    let mut tokens = Tokens::new(line);
    if tokens.word()? != "prop" {
        return None;
    }
    let name = tokens.quoted()?;
    if tokens.word()? != ":" {
        return None;
    }
    let literal_type = tokens.quoted()?;
    if tokens.word()? != "=" {
        return None;
    }
    let rest = tokens.rest();
    let literal = if rest.starts_with('"') {
        Literal::Text(Tokens::new(rest).quoted()?)
    } else {
        tuple(rest)?
    };
    Some(Property {
        name,
        literal_type,
        literal,
    })
}

fn tuple(text: &str) -> Option<Literal> {
    let inner = text.strip_prefix('(')?.strip_suffix(')')?;
    let parts: Vec<&str> = inner.split(',').map(str::trim).collect();
    if parts.len() != 5 {
        return None;
    }
    let mut lanes = [0.0_f32; 4];
    for (lane, part) in lanes.iter_mut().zip(&parts) {
        *lane = part.parse().ok()?;
    }
    Some(Literal::Number {
        lanes,
        mask: parts[4].parse().ok()?,
    })
}

fn link(graph: &mut ScriptGraph, tokens: &mut Tokens<'_>) -> Result<()> {
    let refuse = || Decoder::refuse("a wire is written `link <from> \"pin\" -> <to> \"pin\"`");
    let from = tokens
        .word()
        .and_then(|w| w.parse().ok())
        .ok_or_else(refuse)?;
    let from_pin = tokens.quoted().ok_or_else(refuse)?;
    if tokens.word() != Some("->") {
        return Err(refuse());
    }
    let to = tokens
        .word()
        .and_then(|w| w.parse().ok())
        .ok_or_else(refuse)?;
    let to_pin = tokens.quoted().ok_or_else(refuse)?;
    graph.links.insert(ScriptLink {
        to,
        to_pin,
        from,
        from_pin,
    });
    Ok(())
}

fn layout(graph: &mut ScriptGraph, tokens: &mut Tokens<'_>) -> Result<()> {
    let refuse = || Decoder::refuse("a layout entry is written `layout <key> at <x> <y>`");
    let key: u64 = tokens
        .word()
        .and_then(|w| w.parse().ok())
        .ok_or_else(refuse)?;
    if tokens.word() != Some("at") {
        return Err(refuse());
    }
    let x: f32 = tokens
        .word()
        .and_then(|w| w.parse().ok())
        .ok_or_else(refuse)?;
    let y: f32 = tokens
        .word()
        .and_then(|w| w.parse().ok())
        .ok_or_else(refuse)?;
    let rest = tokens.rest();
    let rest = if rest.is_empty() {
        String::new()
    } else {
        format!(" {rest}")
    };
    graph.layout.insert(key, (x, y, rest));
    Ok(())
}

/// A cursor over one line's tokens, as the engine's `Tokens`.
struct Tokens<'a> {
    line: &'a str,
}

impl<'a> Tokens<'a> {
    fn new(line: &'a str) -> Self {
        Self { line }
    }

    fn word(&mut self) -> Option<&'a str> {
        let trimmed = self.line.trim_start_matches(' ');
        if trimmed.is_empty() {
            self.line = trimmed;
            return None;
        }
        let end = trimmed.find(' ').unwrap_or(trimmed.len());
        self.line = &trimmed[end..];
        Some(&trimmed[..end])
    }

    fn quoted(&mut self) -> Option<String> {
        let trimmed = self.line.trim_start_matches(' ');
        let mut characters = trimmed.char_indices();
        if characters.next()?.1 != '"' {
            return None;
        }
        let mut out = String::new();
        let mut escaped = false;
        for (index, character) in characters {
            if escaped {
                out.push(character);
                escaped = false;
            } else if character == '\\' {
                escaped = true;
            } else if character == '"' {
                self.line = &trimmed[index + 1..];
                return Some(out);
            } else {
                out.push(character);
            }
        }
        None
    }

    fn rest(&mut self) -> &'a str {
        let rest = self.line.trim_start_matches(' ');
        self.line = "";
        rest
    }
}

// --- The wire -------------------------------------------------------------------------------------

/// The `script.compile` request for `source`.
#[must_use]
pub fn compile_payload(source: &str) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.text(source);
    out.finish()
}

/// The `script.event.raise` request: an authored node's engine identity, an event and up to three
/// arguments.
#[must_use]
pub fn raise_payload(node: u64, event: &str, arguments: &[f32]) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.u64(node);
    out.text(event);
    let count = arguments.len().min(3);
    out.u32(u32::try_from(count).unwrap_or(3));
    for argument in &arguments[..count] {
        out.f32(*argument);
    }
    out.finish()
}

/// How serious an engine diagnostic is.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Severity {
    /// Worth knowing.
    Info,
    /// The graph compiles; something in it probably is not what was meant.
    Warning,
    /// The graph does not compile.
    Error,
}

/// One diagnostic from the engine's compiler, on the node it is about.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct CompileDiagnostic {
    /// How serious.
    pub severity: Severity,
    /// Stable machine-readable identity, such as `script.external.unknown`.
    pub code: String,
    /// The node's key, or zero for the graph as a whole.
    pub node: u64,
    /// The pin, or empty.
    pub pin: String,
    /// What is wrong.
    pub message: String,
    /// The name it is about: the unknown function, `expected float, received exec`.
    pub detail: String,
    /// The other end, for a wire or a duplicate; zero otherwise.
    pub related: u64,
}

impl CompileDiagnostic {
    /// One line a person reads: the message, then what it is about.
    #[must_use]
    pub fn describe(&self) -> String {
        if self.detail.is_empty() {
            self.message.clone()
        } else {
            format!("{} ({})", self.message, self.detail)
        }
    }
}

/// One event the program answers.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Handler {
    /// The event.
    pub event: String,
    /// The `script.on_event` node.
    pub node: u64,
    /// The block it starts at.
    pub block: u32,
}

/// The engine's answer to a `script.compile`.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct CompileReport {
    /// Whether a program was built.
    pub compiled: bool,
    /// The source's semantic digest: what layout does not move.
    pub semantic_digest: u64,
    /// The program's digest: its cook key.
    pub program_digest: u64,
    /// Instructions in the program.
    pub instructions: u32,
    /// Basic blocks.
    pub blocks: u32,
    /// Registers per instance.
    pub registers: u32,
    /// Registers kept across a wait: the compact state.
    pub state_slots: u32,
    /// The events it answers.
    pub handlers: Vec<Handler>,
    /// What it calls, reads, emits or waits for, with the kind the engine declared each as.
    pub externals: Vec<(String, u8)>,
    /// What it reads and writes, for the scheduler: `(resource, 0 read / 1 write)`.
    pub accesses: Vec<(String, u8)>,
    /// What the compiler said, node by node.
    pub diagnostics: Vec<CompileDiagnostic>,
    /// The program, instruction by instruction, each with its node.
    pub listing: String,
}

impl CompileReport {
    /// Decode a `script.compile` reply.
    ///
    /// # Errors
    ///
    /// Another format, a truncated reply, or bytes left over.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(payload);
        expect_format(&mut reader, "read the engine's compile")?;
        let mut report = Self {
            compiled: reader.u8()? != 0,
            semantic_digest: reader.u64()?,
            program_digest: reader.u64()?,
            instructions: reader.u32()?,
            blocks: reader.u32()?,
            registers: reader.u32()?,
            state_slots: reader.u32()?,
            ..Self::default()
        };
        for _ in 0..reader.u32()? {
            report.handlers.push(Handler {
                event: reader.text()?,
                node: reader.u64()?,
                block: reader.u32()?,
            });
        }
        for _ in 0..reader.u32()? {
            report.externals.push((reader.text()?, reader.u8()?));
        }
        for _ in 0..reader.u32()? {
            report.accesses.push((reader.text()?, reader.u8()?));
        }
        report.diagnostics = read_diagnostics(&mut reader)?;
        report.listing = reader.text()?;
        finished(&reader, "read the engine's compile")?;
        Ok(report)
    }

    /// The errors, the ones that stop the graph compiling.
    pub fn errors(&self) -> impl Iterator<Item = &CompileDiagnostic> {
        self.diagnostics
            .iter()
            .filter(|diagnostic| diagnostic.severity == Severity::Error)
    }
}

/// One graph instance during Play, as the engine reports it.
#[derive(Clone, PartialEq, Debug)]
pub struct PlayInstance {
    /// The authored node's engine identity.
    pub node: u64,
    /// The graph it runs: its file's stem.
    pub graph: String,
    /// `idle`, `waiting` or `failed`.
    pub status: &'static str,
    /// The wait's reason while waiting.
    pub waiting: String,
    /// Where the unit is.
    pub position: [f32; 3],
    /// Whether it has an order it has not finished.
    pub moving: bool,
    /// Handlers started.
    pub runs: u32,
    /// Why the last handler failed.
    pub problem: String,
}

/// One cue a graph played.
#[derive(Clone, PartialEq, Debug)]
pub struct PlayCue {
    /// The node that played it.
    pub node: u64,
    /// The cue.
    pub cue: String,
    /// The tick it played on.
    pub tick: u64,
    /// Where.
    pub position: [f32; 3],
}

/// Play's graphs, as the engine reports them.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct PlayState {
    /// Whether Play is running graphs.
    pub playing: bool,
    /// The graph system's tick.
    pub tick: u64,
    /// Every instance.
    pub instances: Vec<PlayInstance>,
    /// Every cue played since Play started.
    pub cues: Vec<PlayCue>,
}

impl PlayState {
    /// Decode a `script.state.get` reply.
    ///
    /// # Errors
    ///
    /// Another format, or a truncated reply.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(payload);
        let state = Self::read(&mut reader)?;
        finished(&reader, "read Play's graphs")?;
        Ok(state)
    }

    /// Decode a `script.event.raise` reply: the handlers started, then the state.
    ///
    /// # Errors
    ///
    /// As [`Self::decode`].
    pub fn decode_raise(payload: &[u8]) -> Result<(u32, Self)> {
        let mut reader = Reader::new(payload);
        let started = reader.u32()?;
        let state = Self::read(&mut reader)?;
        finished(&reader, "read Play's graphs")?;
        Ok((started, state))
    }

    fn read(reader: &mut Reader<'_>) -> Result<Self> {
        expect_format(reader, "read Play's graphs")?;
        let mut state = Self {
            playing: reader.u8()? != 0,
            tick: reader.u64()?,
            ..Self::default()
        };
        for _ in 0..reader.u32()? {
            state.instances.push(PlayInstance {
                node: reader.u64()?,
                graph: reader.text()?,
                status: match reader.u8()? {
                    0 => "idle",
                    1 => "waiting",
                    _ => "failed",
                },
                waiting: reader.text()?,
                position: [reader.f32()?, reader.f32()?, reader.f32()?],
                moving: reader.u8()? != 0,
                runs: reader.u32()?,
                problem: reader.text()?,
            });
        }
        for _ in 0..reader.u32()? {
            state.cues.push(PlayCue {
                node: reader.u64()?,
                cue: reader.text()?,
                tick: reader.u64()?,
                position: [reader.f32()?, reader.f32()?, reader.f32()?],
            });
        }
        Ok(state)
    }
}

/// The diagnostics list a compile and a reload reply both end with.
pub(crate) fn read_diagnostics(reader: &mut Reader<'_>) -> Result<Vec<CompileDiagnostic>> {
    let mut diagnostics = Vec::new();
    for _ in 0..reader.u32()? {
        diagnostics.push(CompileDiagnostic {
            severity: match reader.u8()? {
                0 => Severity::Info,
                1 => Severity::Warning,
                _ => Severity::Error,
            },
            code: reader.text()?,
            node: reader.u64()?,
            pin: reader.text()?,
            message: reader.text()?,
            detail: reader.text()?,
            related: reader.u64()?,
        });
    }
    Ok(diagnostics)
}

pub(crate) fn expect_format(reader: &mut Reader<'_>, action: &str) -> Result<()> {
    let format = reader.u32()?;
    if format == WIRE_FORMAT {
        Ok(())
    } else {
        Err(Problem::new(
            action,
            format!("the engine answered in script format {format}; this editor reads 1"),
        ))
    }
}

pub(crate) fn finished(reader: &Reader<'_>, action: &str) -> Result<()> {
    if reader.is_empty() {
        Ok(())
    } else {
        Err(Problem::new(action, "bytes remain after the reply"))
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub(crate) fn engine_fixture(name: &str) -> Vec<u8> {
        let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../src/editor_backend/tests/data")
            .join(name);
        std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
    }

    #[test]
    fn floats_are_written_as_c_writes_them_with_nine_significant_digits() {
        let cases: [(f32, &str); 14] = [
            (0.0, "0"),
            (-0.0, "-0"),
            (40.0, "40"),
            (1.5, "1.5"),
            (-2.25, "-2.25"),
            (0.1, "0.100000001"),
            (1.0 / 3.0, "0.333333343"),
            (123_456_789.0, "123456792"),
            (1.0e9, "1e+09"),
            (1.5e-5, "1.49999996e-05"),
            (0.0001, "9.99999975e-05"),
            (0.001, "0.00100000005"),
            (3.0e38, "3.00000001e+38"),
            (820.0, "820"),
        ];
        for (value, expected) in cases {
            assert_eq!(format_float(value), expected, "{value:e}");
        }
    }

    #[test]
    fn the_engines_canonical_graph_reads_and_writes_back_byte_for_byte() {
        let engine = String::from_utf8(engine_fixture("script_unit_command_v1.cyscript")).unwrap();
        let graph = ScriptGraph::decode(&engine).unwrap();
        assert_eq!(graph.nodes.len(), 6);
        assert_eq!(graph.links.len(), 5);
        assert_eq!(graph.capabilities, GAMEPLAY_CAPABILITIES);
        assert_eq!(graph.encode(), engine);
    }

    #[test]
    fn a_graph_built_in_any_order_writes_the_engines_order() {
        let mut graph = ScriptGraph::new("probe");
        for key in [3_u64, 1, 2] {
            graph.nodes.insert(
                key,
                ScriptNode {
                    key,
                    type_name: "script.const_float".into(),
                    version: 1,
                    muted: false,
                    properties: vec![
                        Property {
                            name: "value".into(),
                            literal_type: "float".into(),
                            literal: Literal::parse("float", "2.5").unwrap(),
                        },
                        Property {
                            name: "comment".into(),
                            literal_type: "name".into(),
                            literal: Literal::Text(String::new()),
                        },
                    ],
                    opaque: None,
                },
            );
        }
        let text = graph.encode();
        let first = text.find("node 1 ").unwrap();
        assert!(first < text.find("node 2 ").unwrap());
        assert!(text.find("node 2 ").unwrap() < text.find("node 3 ").unwrap());
        // By name, and an empty name is the zero tuple, as the engine writes it.
        assert!(text.contains(
            "    prop \"comment\" : \"name\" = (0, 0, 0, 0, 0)\n    prop \"value\" : \"float\" = (2.5, 0, 0, 0, 0)\n"
        ));
        assert_eq!(ScriptGraph::decode(&text).unwrap().encode(), text);
    }

    #[test]
    fn a_body_this_editor_cannot_read_is_kept_verbatim() {
        let text = "cygraph 1\ngraph \"g\" version 1\ncapability\ndeterministic true\nnode 1 \"plugin.thing\" v3 {\n    whatever the plugin wrote\n}\n";
        let graph = ScriptGraph::decode(text).unwrap();
        assert!(graph.nodes[&1].opaque.is_some());
        assert_eq!(graph.encode(), text);
    }

    #[test]
    fn literals_are_written_at_their_declared_types() {
        assert_eq!(
            Literal::parse("int", "7").unwrap(),
            Literal::Number {
                lanes: [0.0; 4],
                mask: 7
            }
        );
        assert!(Literal::parse("int", "-1").is_err());
        assert!(Literal::parse("float", "nan").is_err());
        assert_eq!(
            Literal::parse("bool", "true").unwrap().display("bool"),
            "true"
        );
        assert_eq!(
            Literal::parse("name", "unit.move_to").unwrap(),
            Literal::Text("unit.move_to".into())
        );
    }

    #[test]
    fn a_reference_is_a_project_relative_cyscript() {
        validate_reference("game/scripts/unit_command.cyscript").unwrap();
        for bad in [
            "",
            "/abs/a.cyscript",
            "../a.cyscript",
            "game/../a.cyscript",
            "./a.cyscript",
            "C:/a.cyscript",
            "C:a.cyscript",
            "game/a.cygraph",
            "game/a b.cyscript",
        ] {
            assert!(validate_reference(bad).is_err(), "{bad}");
        }
        assert_eq!(
            graph_name("game/scripts/unit_command.cyscript"),
            "unit_command"
        );
    }

    #[test]
    fn the_raise_is_the_bytes_the_engines_suite_submits() {
        assert_eq!(
            raise_payload(0x0123_4567_89AB_CDEF, "unit.command", &[6.0, 0.0, 8.0]),
            engine_fixture("script_raise_request_v1.wire")
        );
    }

    #[test]
    fn the_engines_replies_decode() {
        let compiled = CompileReport::decode(&engine_fixture("script_compile_v1.wire")).unwrap();
        assert!(compiled.compiled);
        assert!(compiled.diagnostics.is_empty());
        assert_eq!(compiled.handlers[0].event, "unit.command");
        assert!(compiled.listing.contains("suspend"));

        let refused =
            CompileReport::decode(&engine_fixture("script_compile_error_v1.wire")).unwrap();
        assert!(!refused.compiled);
        let error = refused.errors().next().unwrap();
        assert_eq!(error.code, "script.external.unknown");
        assert_eq!(error.node, 4);
        assert_eq!(error.detail, "unit.mvoe_to");

        let state = PlayState::decode(&engine_fixture("script_state_play_v1.wire")).unwrap();
        assert!(state.playing);
        assert_eq!(state.instances.len(), 1);
        assert_eq!(
            state.instances[0].position.map(f32::to_bits),
            [6.0_f32, 0.0, 8.0].map(f32::to_bits)
        );
        assert_eq!(state.cues[0].cue, "unit.arrived");
    }
}
