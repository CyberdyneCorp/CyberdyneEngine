//! The world loader: what makes an opened document have a schema and content. M6 task 2.4.
//!
//! --- THE DEFECT THIS MODULE CLOSES ----------------------------------------------------------------
//!
//! M5.5's gate stated it plainly: *"`DocumentService::open` calls `Document::new` — a name and an
//! EMPTY SCHEMA — because there is no world loader."* Nothing is selectable, no `Transform` binds,
//! and a gizmo drag commits nothing. The editor is real and it has nothing to edit.
//!
//! --- TWO FILES, ONE GRAMMAR -----------------------------------------------------------------------
//!
//! | File | First line | What it holds |
//! |---|---|---|
//! | `types.cytypes` | `cyschema 1` | The **engine's** registered component types, as the editor must name them. Written by the engine (`cy::scene::serialization::write_authoring_schema`), read here. |
//! | `<world>.cyworld` | `cyworld 1` | A world: the schema it was written against, then its nodes. |
//!
//! The second is the first plus a `node` section, so there is one reader and one writer rather than
//! two that can disagree about how a field is spelled.
//!
//! ```text
//! cyworld 1
//! type 1 runtime "Transform"
//!   field 1 vec3 "translation" "Where it is, in world units."
//! node 0 - "default"
//!   component 1
//!     field 1 0 0 0
//! ```
//!
//! --- WHY THE SCHEMA IS IN THE FILE AND NOT DERIVED AT LOAD ------------------------------------------
//!
//! `serialization-and-prefabs` requires that tagged authoring data be readable by a build that does
//! not have every type registered — *"an editor without a plugin does not silently strip that
//! plugin's data from every file it touches"*. A world that carried only identifiers would be
//! unreadable without exactly the build that wrote it. Carrying the schema costs a few lines per
//! type and makes the file self-describing, which is what lets [`load`] answer with a document whose
//! inspector works with no runtime attached at all.
//!
//! Identifiers are still the contract. A field is addressed by its number everywhere in the `node`
//! section; the name appears once, in the `type` section, which is what makes a rename one changed
//! line rather than a rewrite of every world.
//!
//! --- WHAT IS DETERMINISTIC, AND WHY IT HAS TO BE -----------------------------------------------------
//!
//! *"The text form SHALL be deterministic: stable ordering of entities, components, and fields; no
//! volatile data such as timestamps or pointer values; and a stable float formatting that
//! round-trips exactly."* Every list below is written in ascending identifier order, floats are
//! written with Rust's shortest-round-trip formatting, and nothing here writes a path, a clock or a
//! hash of anything. [`tests::a_world_written_twice_is_the_same_bytes`] is that claim, run.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;

use cy_editor_core::Actor;
use cy_editor_core::ids::{FieldId, NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::schema::DocumentSchema;

/// The first word of a type manifest.
pub const SCHEMA_MAGIC: &str = "cyschema";
/// The first word of a world.
pub const WORLD_MAGIC: &str = "cyworld";
/// The version both carry. One number for both, because they are one grammar.
pub const FORMAT_VERSION: u32 = 1;
/// What the engine's type manifest is called inside a project.
pub const TYPE_MANIFEST: &str = "types.cytypes";

/// Two spaces per level, as `cy/core/serialize/text.h` fixes for the engine's own text form.
const INDENT: usize = 2;

// --- the lexical layer -----------------------------------------------------------------------

/// One significant line: how deep it is indented and the words on it.
struct Line {
    depth: usize,
    number: usize,
    words: Vec<String>,
}

impl Line {
    fn word(&self, index: usize) -> &str {
        self.words.get(index).map_or("", String::as_str)
    }

    fn number_at<T: std::str::FromStr>(&self, index: usize, what: &str) -> Result<T> {
        self.word(index).parse().map_err(|_| self.bad(what))
    }

    fn bad(&self, what: &str) -> Problem {
        Problem::new(
            format!("read line {}", self.number),
            format!("{what} was expected and {:?} is not one", self.word(0)),
        )
        .with_remedy("the file was written by a different build, or edited by hand")
    }
}

/// Split a line into words, honouring `"quoted text"` and stopping at a `#` comment.
///
/// One function rather than a loop in the reader because the quoting rule has to be the same for
/// every word on every line — a name that lost its spaces in one place and kept them in another
/// would be a file that round-trips for some documents and not for others.
fn words_of(text: &str) -> Result<Vec<String>> {
    let mut words = Vec::new();
    let mut characters = text.chars().peekable();
    while let Some(character) = characters.next() {
        match character {
            ' ' | '\t' => {}
            '#' => break,
            '"' => words.push(quoted(&mut characters)?),
            _ => {
                let mut word = String::from(character);
                while characters.peek().is_some_and(|next| !next.is_whitespace()) {
                    word.push(characters.next().unwrap_or_default());
                }
                words.push(word);
            }
        }
    }
    Ok(words)
}

/// The rest of a quoted word, with `\\`, `\"`, `\n` and `\t` unescaped.
fn quoted(characters: &mut std::iter::Peekable<std::str::Chars<'_>>) -> Result<String> {
    let mut word = String::new();
    loop {
        let Some(character) = characters.next() else {
            return Err(Problem::new(
                "read a quoted word",
                "the line ended before the closing quotation mark",
            ));
        };
        match character {
            '"' => return Ok(word),
            '\\' => word.push(match characters.next() {
                Some('n') => '\n',
                Some('t') => '\t',
                Some(other) => other,
                None => '\\',
            }),
            other => word.push(other),
        }
    }
}

/// Every significant line, blank lines and whole-line comments removed.
fn lines_of(text: &str) -> Result<Vec<Line>> {
    let mut lines = Vec::new();
    for (index, raw) in text.lines().enumerate() {
        let depth = raw.len() - raw.trim_start_matches(' ').len();
        let words = words_of(raw)?;
        if words.is_empty() {
            continue;
        }
        lines.push(Line {
            depth: depth / INDENT,
            number: index + 1,
            words,
        });
    }
    Ok(lines)
}

/// Quote and escape a word for writing.
fn quote(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for character in text.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\t' => out.push_str("\\t"),
            other => out.push(other),
        }
    }
    out.push('"');
    out
}

// --- values ------------------------------------------------------------------------------------

/// Write a value's words, without its kind: the field's declared kind is what says how to read it
/// back, so repeating it on every line would be a second source of truth per value.
fn write_value(value: &Value) -> String {
    match value {
        Value::Nil => "nil".to_string(),
        Value::Bool(flag) => flag.to_string(),
        Value::Int(number) => number.to_string(),
        Value::Float(number) => number.to_string(),
        Value::Double(number) => number.to_string(),
        Value::Vec2(numbers) => floats(numbers),
        Value::Vec3(numbers) => floats(numbers),
        Value::Vec4(numbers) | Value::Quat(numbers) => floats(numbers),
        Value::Text(text) => quote(text),
        Value::Bytes(bytes) if bytes.is_empty() => "-".to_string(),
        Value::Bytes(bytes) => {
            let mut hex = String::with_capacity(bytes.len() * 2);
            for byte in bytes {
                let _ = write!(hex, "{byte:02x}");
            }
            hex
        }
        Value::Entity(entity) => entity.to_string(),
    }
}

/// Rust's `Display` for a float is the shortest text that parses back to the same bits, which is
/// exactly the guarantee `serialization-and-prefabs` asks of the canonical form.
fn floats(numbers: &[f32]) -> String {
    let mut out = String::new();
    for (index, number) in numbers.iter().enumerate() {
        if index > 0 {
            out.push(' ');
        }
        let _ = write!(out, "{number}");
    }
    out
}

fn read_floats<const N: usize>(line: &Line, first: usize) -> Result<[f32; N]> {
    let mut out = [0.0_f32; N];
    for (index, slot) in out.iter_mut().enumerate() {
        *slot = line.number_at(first + index, "a float")?;
    }
    Ok(out)
}

/// Read a value of a declared kind from `first` onwards.
fn read_value(line: &Line, first: usize, kind: ValueKind) -> Result<Value> {
    Ok(match kind {
        ValueKind::Nil => Value::Nil,
        ValueKind::Bool => Value::Bool(line.word(first) == "true"),
        ValueKind::Int => Value::Int(line.number_at(first, "an integer")?),
        ValueKind::Float => Value::Float(line.number_at(first, "a float")?),
        ValueKind::Double => Value::Double(line.number_at(first, "a double")?),
        ValueKind::Vec2 => Value::Vec2(read_floats::<2>(line, first)?),
        ValueKind::Vec3 => Value::Vec3(read_floats::<3>(line, first)?),
        ValueKind::Vec4 => Value::Vec4(read_floats::<4>(line, first)?),
        ValueKind::Quat => Value::Quat(read_floats::<4>(line, first)?),
        ValueKind::Text => Value::Text(line.word(first).to_string()),
        ValueKind::Bytes => Value::Bytes(read_bytes(line.word(first))),
        ValueKind::Entity => Value::Entity(line.number_at(first, "an entity")?),
    })
}

fn read_bytes(text: &str) -> Vec<u8> {
    if text == "-" {
        return Vec::new();
    }
    text.as_bytes()
        .chunks(2)
        .filter_map(|pair| u8::from_str_radix(std::str::from_utf8(pair).ok()?, 16).ok())
        .collect()
}

fn kind_of(name: &str) -> Option<ValueKind> {
    const KINDS: [ValueKind; 12] = [
        ValueKind::Nil,
        ValueKind::Bool,
        ValueKind::Int,
        ValueKind::Float,
        ValueKind::Double,
        ValueKind::Vec2,
        ValueKind::Vec3,
        ValueKind::Vec4,
        ValueKind::Quat,
        ValueKind::Text,
        ValueKind::Bytes,
        ValueKind::Entity,
    ];
    KINDS.into_iter().find(|kind| kind.name() == name)
}

// --- the schema half ---------------------------------------------------------------------------

/// What the file called a type and a field, and what this document calls them.
///
/// The file's numbers are the numbers the *writer's* schema issued. A document that already has
/// types declared issues different ones, so a translation table is not optional — and getting it
/// wrong is a field written into the wrong component, which loads without a diagnostic.
#[derive(Default, Debug)]
pub struct Correspondence {
    types: BTreeMap<u64, TypeId>,
    fields: BTreeMap<(u64, u64), FieldId>,
    /// What the file said each field holds. Kept here rather than read back from the document,
    /// because a value must be read as the kind the FILE declared: a build whose registry has since
    /// changed a field's kind must not silently reinterpret bytes written under the old one.
    kinds: BTreeMap<(u64, u64), ValueKind>,
}

impl Correspondence {
    /// The document's identity for a type the file numbered `written`.
    #[must_use]
    pub fn type_of(&self, written: u64) -> Option<TypeId> {
        self.types.get(&written).copied()
    }

    /// The document's identity for one of its fields.
    #[must_use]
    pub fn field_of(&self, written_type: u64, written_field: u64) -> Option<FieldId> {
        self.fields.get(&(written_type, written_field)).copied()
    }

    /// What the file declared that field to hold.
    #[must_use]
    pub fn kind_of(&self, written_type: u64, written_field: u64) -> Option<ValueKind> {
        self.kinds.get(&(written_type, written_field)).copied()
    }
}

/// Declare everything a `type` section describes into `schema`.
///
/// Returns the correspondence, which the content half needs and a caller reading only a manifest
/// can throw away.
fn declare_types(lines: &[Line], schema: &mut DocumentSchema) -> Result<Correspondence> {
    let mut map = Correspondence::default();
    let mut current: Option<u64> = None;
    // DEPTH IS WHAT SEPARATES THE TWO SECTIONS. A schema field is written at depth 1 under its
    // type; a node's field is written at depth 2 under its component. Reading the schema by keyword
    // alone would swallow the content's field lines and report them as fields of nothing, which is
    // how this function failed the first time it was run.
    for line in lines {
        match (line.word(0), line.depth) {
            ("node", _) => break,
            ("type", 0) => current = Some(declare_type(line, schema, &mut map)?),
            ("field", 1) => {
                let owner = current.ok_or_else(|| line.bad("a field inside a type"))?;
                declare_field(line, owner, schema, &mut map)?;
            }
            _ => {}
        }
    }
    Ok(map)
}

fn declare_type(line: &Line, schema: &mut DocumentSchema, map: &mut Correspondence) -> Result<u64> {
    let written: u64 = line.number_at(1, "a type identifier")?;
    let authoring_only = match line.word(2) {
        "authoring" => true,
        "runtime" => false,
        _ => return Err(line.bad("'authoring' or 'runtime'")),
    };
    let id = schema.declare_type(line.word(3), authoring_only);
    map.types.insert(written, id);
    Ok(written)
}

fn declare_field(
    line: &Line,
    owner: u64,
    schema: &mut DocumentSchema,
    map: &mut Correspondence,
) -> Result<()> {
    let written: u64 = line.number_at(1, "a field identifier")?;
    let kind = kind_of(line.word(2)).ok_or_else(|| line.bad("a value kind"))?;
    let owner_id = map
        .types
        .get(&owner)
        .copied()
        .ok_or_else(|| line.bad("a field of a declared type"))?;
    let id = schema.declare_field(owner_id, line.word(3), kind, line.word(4))?;
    map.fields.insert((owner, written), id);
    map.kinds.insert((owner, written), kind);
    Ok(())
}

/// Read a `cyschema` manifest into `schema`.
///
/// This is how the **engine's** registered component types reach a document. The manifest is
/// written by the engine from its own reflection registry, so the names the gizmo binds by —
/// `Transform`, `translation`, `rotation`, `scale` — are the engine's names rather than the
/// editor's guess at them.
pub fn read_schema(text: &str, schema: &mut DocumentSchema) -> Result<Correspondence> {
    let lines = lines_of(text)?;
    let head = lines.first().ok_or_else(|| {
        Problem::new("read a type manifest", "the file is empty")
            .with_remedy("regenerate it from the engine")
    })?;
    expect_head(head, SCHEMA_MAGIC)?;
    declare_types(&lines[1..], schema)
}

fn expect_head(head: &Line, magic: &str) -> Result<()> {
    if head.word(0) != magic {
        return Err(head.bad(magic));
    }
    let version: u32 = head.number_at(1, "a format version")?;
    if version > FORMAT_VERSION {
        return Err(Problem::new(
            "read this file",
            format!("it is version {version} and this build understands {FORMAT_VERSION}"),
        )
        .with_remedy("use a newer editor, rather than reading it as though it were older"));
    }
    Ok(())
}

/// Write the `type` section of a schema. Shared by the manifest writer and the world writer.
fn write_types(schema: &DocumentSchema, out: &mut String) {
    for definition in schema.types() {
        let kind = if definition.authoring_only {
            "authoring"
        } else {
            "runtime"
        };
        let _ = writeln!(
            out,
            "type {} {kind} {}",
            definition.id.as_u64(),
            quote(&definition.name)
        );
        for field in &definition.fields {
            let _ = writeln!(
                out,
                "  field {} {} {} {}",
                field.id.as_u64(),
                field.kind.name(),
                quote(&field.name),
                quote(&field.description)
            );
        }
    }
}

/// A `cyschema` manifest describing `schema`. The inverse of [`read_schema`].
#[must_use]
pub fn write_schema(schema: &DocumentSchema) -> String {
    let mut out = format!("{SCHEMA_MAGIC} {FORMAT_VERSION}\n");
    write_types(schema, &mut out);
    out
}

// --- the content half ---------------------------------------------------------------------------

/// A world as text: its schema, then its nodes. The inverse of [`load`].
#[must_use]
pub fn write_world(document: &Document) -> String {
    let mut out = format!("{WORLD_MAGIC} {FORMAT_VERSION}\n");
    write_types(document.schema(), &mut out);

    // Positional identity. A `NodeId` is a hash of the document's identity and an ordinal, so the
    // number in the file is the node's INDEX in this enumeration and a parent is written as the
    // index of its parent. That is what lets the same file load into a document with a different
    // identity — a "Save As", a copy of a project — without every reference breaking.
    let nodes = authored_order(document.content());
    let index: BTreeMap<NodeId, usize> = nodes
        .iter()
        .enumerate()
        .map(|(position, node)| (*node, position))
        .collect();
    for (position, node) in nodes.iter().enumerate() {
        let Some(state) = document.content().node(*node) else {
            continue;
        };
        let parent = state
            .parent
            .and_then(|parent| index.get(&parent))
            .map_or_else(|| "-".to_string(), ToString::to_string);
        let _ = writeln!(out, "node {position} {parent} {}", quote(&state.layer));
        write_components(state, &mut out);
    }
    out
}

/// Every node in **authored order**: roots as they were created, each followed by its children.
///
/// NOT `DocumentContent::nodes`, and the difference is a defect this file's own round-trip test
/// caught. `nodes()` iterates by `NodeId`, which is a hash of the document's identity and the node's
/// ordinal — a stable order, but an order with no relation to the file's. Loading assigns ordinals in
/// FILE order, so writing in hash order and reading in file order permutes the file on every save:
/// a world saved, opened and saved again produced the same world with every node in a different
/// place, and every save would have been a whole-file diff. `serialization-and-prefabs` asks for
/// "stable ordering of entities" and this is what makes it true across a round trip.
///
/// A node not reachable from any root cannot exist through the operations, but the fallback is here
/// anyway: silently dropping content because a structure was unexpected is not a trade this format
/// may make.
fn authored_order(content: &cy_editor_documents::content::DocumentContent) -> Vec<NodeId> {
    let mut ordered = Vec::with_capacity(content.node_count());
    let mut seen = std::collections::BTreeSet::new();
    let mut stack: Vec<NodeId> = content.roots().iter().rev().copied().collect();
    while let Some(node) = stack.pop() {
        if !seen.insert(node) {
            continue;
        }
        ordered.push(node);
        if let Some(state) = content.node(node) {
            stack.extend(state.children.iter().rev().copied());
        }
    }
    for node in content.nodes() {
        if seen.insert(node) {
            ordered.push(node);
        }
    }
    ordered
}

fn write_components(state: &cy_editor_documents::content::NodeState, out: &mut String) {
    for (component, fields) in &state.components {
        let _ = writeln!(out, "  component {}", component.as_u64());
        for (field, value) in fields {
            let _ = writeln!(out, "    field {} {}", field.as_u64(), write_value(value));
        }
    }
}

/// What a load produced, so that a caller can say what happened rather than only that it worked.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct LoadReport {
    /// Component types declared from the file's schema.
    pub types: usize,
    /// Nodes created.
    pub nodes: usize,
    /// Components restored onto them.
    pub components: usize,
}

/// Load a world's text into `document`.
///
/// The document is left **clean and with an empty history**: a load is not something a user did, so
/// it is not something undo may take back. That is achieved by construction rather than by tidying
/// up afterwards — the content is applied inside one transaction which is then committed and the
/// document marked saved, and [`tests::a_freshly_loaded_world_has_nothing_to_undo`] holds it.
pub fn load(text: &str, document: &mut Document, actor: Actor) -> Result<LoadReport> {
    let lines = lines_of(text)?;
    let head = lines.first().ok_or_else(|| {
        Problem::new("read a world", "the file is empty")
            .with_remedy("delete it, or restore it from source control")
    })?;
    expect_head(head, WORLD_MAGIC)?;

    let map = declare_types(&lines[1..], document.schema_mut())?;
    let mut report = LoadReport {
        types: document.schema().types().count(),
        ..LoadReport::default()
    };
    let content = Content::read(&lines[1..], &map)?;
    report.nodes = content.nodes.len();
    report.components = content.nodes.iter().map(|node| node.components.len()).sum();

    document.with_transaction("Open world", actor, |document| content.apply(document))?;
    // A LOAD IS NOT AN EDIT, AND UNDO MAY NOT TAKE A WORLD AWAY. The content had to arrive through a
    // transaction, because a transaction is the only write path there is; `fork` is then what drops
    // the entry the load left behind — "the same schema and content, an empty history, and no
    // journal" is exactly the state a just-opened document should be in.
    *document = document.fork();
    Ok(report)
}

/// One node as the file describes it, before any of it exists.
struct ParsedNode {
    parent: Option<usize>,
    layer: String,
    components: Vec<(TypeId, Vec<(FieldId, Value)>)>,
}

/// The `node` section, parsed and translated but not yet applied.
///
/// Parsing and applying are separate so that a malformed file leaves the document **untouched**
/// rather than half-populated: nothing is written until every line has been understood.
struct Content {
    nodes: Vec<ParsedNode>,
}

impl Content {
    fn read(lines: &[Line], map: &Correspondence) -> Result<Self> {
        let mut nodes: Vec<ParsedNode> = Vec::new();
        let mut component: Option<u64> = None;
        for line in lines {
            match line.word(0) {
                "node" => {
                    nodes.push(read_node(line)?);
                    component = None;
                }
                "component" => component = Some(open_component(line, map, &mut nodes)?),
                "field" if line.depth >= 2 => {
                    let owner = component.ok_or_else(|| line.bad("a field inside a component"))?;
                    read_field(line, owner, map, &mut nodes)?;
                }
                _ => {}
            }
        }
        Ok(Self { nodes })
    }

    fn apply(&self, document: &mut Document) -> Result<()> {
        let mut created: Vec<NodeId> = Vec::with_capacity(self.nodes.len());
        for node in &self.nodes {
            let parent = node.parent.and_then(|index| created.get(index).copied());
            let id = document.create_node(parent)?;
            for (component, fields) in &node.components {
                document.add_component(id, *component, fields.clone())?;
            }
            if !node.layer.is_empty() {
                document.record(cy_editor_documents::Operation::SetLayer {
                    node: id,
                    before: String::new(),
                    after: node.layer.clone(),
                })?;
            }
            created.push(id);
        }
        Ok(())
    }
}

fn read_node(line: &Line) -> Result<ParsedNode> {
    let parent = match line.word(2) {
        "-" => None,
        _ => Some(line.number_at::<usize>(2, "a parent index")?),
    };
    Ok(ParsedNode {
        parent,
        layer: line.word(3).to_string(),
        components: Vec::new(),
    })
}

/// Begin a component on the node most recently read, and answer which type the file called it.
fn open_component(line: &Line, map: &Correspondence, nodes: &mut [ParsedNode]) -> Result<u64> {
    let written: u64 = line.number_at(1, "a type identifier")?;
    let component = map
        .type_of(written)
        .ok_or_else(|| line.bad("a component of a declared type"))?;
    let node = nodes
        .last_mut()
        .ok_or_else(|| line.bad("a component inside a node"))?;
    node.components.push((component, Vec::new()));
    Ok(written)
}

fn read_field(
    line: &Line,
    owner: u64,
    map: &Correspondence,
    nodes: &mut [ParsedNode],
) -> Result<()> {
    let written: u64 = line.number_at(1, "a field identifier")?;
    let field = map
        .field_of(owner, written)
        .ok_or_else(|| line.bad("a field of a declared type"))?;
    let node = nodes
        .last_mut()
        .ok_or_else(|| line.bad("a field inside a node"))?;
    let component = node
        .components
        .last_mut()
        .ok_or_else(|| line.bad("a field inside a component"))?;
    // The kind is the file's own `type` section, so a value is read as what the file said it is
    // rather than as what this build's registry happens to think it is now.
    let kind = map
        .kind_of(owner, written)
        .ok_or_else(|| line.bad("a field of a declared kind"))?;
    component.1.push((field, read_value(line, 2, kind)?));
    Ok(())
}

// --- the project's files -------------------------------------------------------------------------

/// Where a project keeps the engine's type manifest.
#[must_use]
pub fn manifest_path(root: &Path) -> std::path::PathBuf {
    root.join(TYPE_MANIFEST)
}

/// Read the project's type manifest into `schema`, answering how many types it declared.
///
/// A project with no manifest answers `Ok(0)` rather than failing: an editor opened on a directory
/// that is not a Cyberdyne project is an ordinary thing to do, and it should open.
pub fn declare_project_types(root: &Path, schema: &mut DocumentSchema) -> Result<usize> {
    let path = manifest_path(root);
    let Ok(text) = std::fs::read_to_string(&path) else {
        return Ok(0);
    };
    read_schema(&text, schema)?;
    Ok(schema.types().count())
}

/// Write a world to `path`, creating its directory.
pub fn write_to(document: &Document, path: &Path) -> Result<()> {
    if let Some(directory) = path.parent() {
        std::fs::create_dir_all(directory).map_err(|error| {
            Problem::new(format!("create {}", directory.display()), error.to_string())
        })?;
    }
    std::fs::write(path, write_world(document))
        .map_err(|error| Problem::new(format!("write {}", path.display()), error.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::ids::DocumentId;
    use cy_editor_viewport::gizmo::TransformBinding;

    /// The manifest the engine emits for its own components, in miniature. The committed one lives
    /// beside the sample project; this is the shape, not the content.
    const MANIFEST: &str = concat!(
        "cyschema 1\n",
        "type 1 runtime \"Transform\"\n",
        "  field 1 vec3 \"translation\" \"Where it is.\"\n",
        "  field 2 quat \"rotation\" \"Which way it faces.\"\n",
        "  field 3 vec3 \"scale\" \"How big it is.\"\n",
        "type 2 authoring \"EditorNote\"\n",
        "  field 4 text \"text\" \"A designer's note.\"\n",
    );

    fn loaded(text: &str) -> Document {
        let mut document = Document::new("worlds/city.cyworld");
        load(text, &mut document, Actor::human("designer")).unwrap();
        document
    }

    #[test]
    fn a_manifest_declares_the_component_the_gizmo_binds_to() {
        // The whole point of task 2.4, as one assertion: the schema an opened document has is one
        // in which `TransformBinding::of_schema` finds a Transform.
        let mut schema = DocumentSchema::new();
        read_schema(MANIFEST, &mut schema).unwrap();
        let binding = TransformBinding::of_schema(&schema)
            .expect("a Transform with translation, rotation and scale");
        assert_eq!(schema.type_of(binding.component).unwrap().name, "Transform");
        assert_eq!(
            schema
                .field(binding.component, binding.translation)
                .unwrap()
                .kind,
            ValueKind::Vec3
        );
    }

    #[test]
    fn a_manifest_round_trips_through_the_writer() {
        let mut schema = DocumentSchema::new();
        read_schema(MANIFEST, &mut schema).unwrap();
        assert_eq!(write_schema(&schema), MANIFEST);
    }

    #[test]
    fn an_authoring_only_type_stays_authoring_only() {
        let mut schema = DocumentSchema::new();
        read_schema(MANIFEST, &mut schema).unwrap();
        let note = schema.type_named("EditorNote").unwrap();
        assert!(note.authoring_only, "the manifest said so and it survived");
    }

    #[test]
    fn a_world_loads_with_content() {
        let world = concat!(
            "cyworld 1\n",
            "type 1 runtime \"Transform\"\n",
            "  field 1 vec3 \"translation\" \"Where it is.\"\n",
            "  field 2 quat \"rotation\" \"Which way it faces.\"\n",
            "  field 3 vec3 \"scale\" \"How big it is.\"\n",
            "node 0 - \"default\"\n",
            "  component 1\n",
            "    field 1 3 4 5\n",
            "    field 2 0 0 0 1\n",
            "    field 3 1 1 1\n",
            "node 1 0 \"default\"\n",
            "  component 1\n",
            "    field 1 -1.5 0 0\n",
            "    field 2 0 0 0 1\n",
            "    field 3 1 1 1\n",
        );
        let document = loaded(world);
        assert_eq!(document.content().node_count(), 2);
        let binding = TransformBinding::of_schema(document.schema()).unwrap();
        let root = document.content().roots()[0];
        assert_eq!(
            document
                .content()
                .field(root, binding.component, binding.translation),
            Some(&Value::Vec3([3.0, 4.0, 5.0])),
            "the values are the file's, not defaults"
        );
        let child = document
            .content()
            .nodes()
            .find(|node| {
                document
                    .content()
                    .node(*node)
                    .is_some_and(|state| state.parent.is_some())
            })
            .expect("the second node is parented to the first");
        assert_eq!(document.content().node(child).unwrap().parent, Some(root));
    }

    #[test]
    fn a_world_written_twice_is_the_same_bytes() {
        let document = loaded(&SAMPLE);
        let first = write_world(&document);
        let second = write_world(&document);
        assert_eq!(first, second, "the canonical form is deterministic");
        // And it round-trips: reading what was written gives a document that writes the same bytes.
        let again = loaded(&first);
        assert_eq!(write_world(&again), first);
    }

    #[test]
    fn a_freshly_loaded_world_has_nothing_to_undo() {
        let document = loaded(&SAMPLE);
        assert!(
            !document.is_dirty(),
            "opening a world is not an unsaved change"
        );
        assert!(
            document.history().peek_undo().is_none(),
            "and undo may not take a world away"
        );
    }

    #[test]
    fn a_file_that_is_not_a_world_is_refused_by_name() {
        let mut document = Document::new("worlds/city.cyworld");
        let problem = load("cyschema 1\n", &mut document, Actor::human("designer"))
            .expect_err("a manifest is not a world");
        assert!(problem.because.contains("cyworld"), "{problem:?}");
    }

    #[test]
    fn a_world_from_a_newer_build_is_refused_rather_than_misread() {
        let mut document = Document::new("worlds/city.cyworld");
        let problem = load("cyworld 99\n", &mut document, Actor::human("designer"))
            .expect_err("a newer version cannot be guessed at");
        assert!(problem.because.contains("99"), "{problem:?}");
    }

    #[test]
    fn a_malformed_world_leaves_the_document_untouched() {
        // Parsing is separate from applying for exactly this: a file that goes wrong on its last
        // line must not leave half a world behind for the user to discover later.
        let broken = concat!(
            "cyworld 1\n",
            "type 1 runtime \"Transform\"\n",
            "  field 1 vec3 \"translation\" \"Where it is.\"\n",
            "node 0 - \"default\"\n",
            "  component 1\n",
            "    field 1 3 4 5\n",
            "  component 7\n",
        );
        let mut document = Document::new("worlds/city.cyworld");
        assert!(load(broken, &mut document, Actor::human("designer")).is_err());
        assert_eq!(document.content().node_count(), 0);
    }

    /// **THE SEAM, PINNED FROM THIS SIDE.** M8.a task 1.1.
    ///
    /// `cy::scene::serialization::read_world` derives a node's identity rather than being told it:
    /// the document identity is an FNV-1a-128 of the asset path, a node's is the same hash of that
    /// and its ordinal, and a world's node at file position `p` is the ordinal `p + 1` because this
    /// module's [`load`] creates them in file order. The engine's `test_worldfile.cpp` asserts the
    /// same four numbers, so a change to either derivation breaks a test on both sides rather than
    /// silently giving the runtime a world whose objects the editor cannot name.
    #[test]
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the narrowing is what the protocol carries; see `engine_identity`"
    )]
    fn the_engine_derives_the_same_identities() {
        let document = DocumentId::of_asset("worlds/city.cyworld");
        assert_eq!(
            document.as_u128(),
            0xab40_8f05_37b6_c99a_90fd_e4ab_baa2_06e8
        );
        let low = |ordinal| NodeId::in_document(document, ordinal).as_u128() as u64;
        assert_eq!(low(1), 0x539e_e13c_1a82_e51f);
        assert_eq!(low(2), 0x8fda_e6b9_cff7_097c);
        assert_eq!(low(3), 0xd11c_3a3a_937a_fd5d);
        assert_eq!(low(4), 0x0d58_3fb8_48ef_21ba);
    }

    /// And the correspondence itself: the node written at position `p` is the ordinal `p + 1`.
    ///
    /// Not a convention this test states — a property of [`load`], which allocates ordinals in file
    /// order from a counter that starts at one. If that ever stops being true, the runtime's world
    /// silently stops agreeing with the editor's, so it is asserted rather than commented.
    #[test]
    fn a_nodes_position_in_the_file_is_its_ordinal_less_one() {
        let document = loaded(&SAMPLE);
        let nodes: Vec<NodeId> = document.content().roots().to_vec();
        assert_eq!(nodes.len(), 1);
        assert_eq!(nodes[0], NodeId::in_document(document.id(), 1));

        let three = concat!(
            "cyworld 1\n",
            "type 1 runtime \"Transform\"\n",
            "  field 1 vec3 \"translation\" \"Where it is.\"\n",
            "node 0 - \"default\"\n",
            "node 1 - \"default\"\n",
            "node 2 1 \"default\"\n",
        );
        let document = loaded(three);
        let id = |ordinal| NodeId::in_document(document.id(), ordinal);
        assert_eq!(document.content().roots(), &[id(1), id(2)]);
        assert_eq!(
            document.content().node(id(2)).unwrap().children,
            vec![id(3)],
            "the third node named the second as its parent, by position"
        );
    }

    /// A world with one node, used by several tests above.
    static SAMPLE: std::sync::LazyLock<String> = std::sync::LazyLock::new(|| {
        concat!(
            "cyworld 1\n",
            "type 1 runtime \"Transform\"\n",
            "  field 1 vec3 \"translation\" \"Where it is.\"\n",
            "  field 2 quat \"rotation\" \"Which way it faces.\"\n",
            "  field 3 vec3 \"scale\" \"How big it is.\"\n",
            "node 0 - \"default\"\n",
            "  component 1\n",
            "    field 1 0 0 0\n",
            "    field 2 0 0 0 1\n",
            "    field 3 1 1 1\n",
        )
        .to_string()
    });
}
