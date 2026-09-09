//! Creating a primitive: an asset the engine generates, and an ordinary mesh instance in the world.
//! M8.a tasks 2.1, 2.2 and 2.3.
//!
//! --- WHAT THIS FILE IS NOT ---------------------------------------------------------------------
//!
//! It is not a shape generator. `design.md` §2 refuses that arrangement in as many words:
//!
//! > A created box is a **mesh instance whose mesh the engine generated rather than imported** —
//! > the same components, the same asset handles, the same cooking path. […] Generation belongs
//! > beside the importers, not in the editor: the editor issues a command, the engine produces a
//! > mesh asset, and the result is indistinguishable from an import.
//!
//! So the editor's whole part in a primitive is **writing a source asset**. `scene.create-primitive`
//! writes a `.cyprim` — six lines of text naming a shape and its parameters — into the project, and
//! then creates an entity that references it. `tools/import/`'s `PrimitiveImporter` is what turns
//! those six lines into a cooked mesh, through the same registry, the same derivation key and the
//! same cache as a glTF. There is no geometry in this crate and nowhere here to put any.
//!
//! --- WHY CREATING AN ENTITY AND IMPORTING A MESH CALL ONE FUNCTION -----------------------------
//!
//! Task 2.3 asks for an assertion: "selection, the inspector, the gizmo, saving, cooking and
//! physics cannot tell it from an import". The way to make that hold rather than to test for it
//! afterwards is to leave one function that builds a mesh instance — [`create_mesh_instance`] — and
//! have both callers use it. An import that lands a mesh in the world (task 3.5) calls this; so
//! does a primitive. A difference between the two would have to be written on purpose, in a
//! function that has no parameter for it.
//!
//! --- THE MESH REFERENCE, AND THE ONE THING THAT IS PROVISIONAL HERE ----------------------------
//!
//! A document's schema is the editor's own and is related to the engine's registry BY NAME
//! (`cy_editor_services::worldfile`, and `cy::scene::serialization::resolve_against`, which carries
//! a name this build does not know rather than dropping it). The engine declares the component a
//! drawn mesh lives on — `cy::render::MeshRenderer`, in `src/scene/src/node_template.cpp`'s
//! catalogue — and has not registered it: that catalogue's own comment says "there is no renderer
//! until M3, no physics or audio until M4 and M8", and the mesh component is still a declared name
//! with no reflected type behind it.
//!
//! [`MeshBinding`] therefore does what [`TransformBinding`] does — finds the component by name, and
//! declares it in the document's schema when the world does not already carry it. The name is the
//! engine's own, so the day the renderer registers `cy::render::MeshRenderer` the field a primitive
//! wrote resolves to it with no migration and no change here.
//!
//! What that costs, stated plainly: until then the reference is authoring data that round-trips
//! through `.cyworld` and is read by no runtime system, and a world that had no `MeshRenderer` in
//! its type section acquires one. That is a type declaration, not content — a `.cyworld` already
//! declares seven types no node in it uses — and undo removes every node and every value the
//! transaction wrote, which the tests assert.

use cy_editor_commands::{
    Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::operation::Operation;
use cy_editor_documents::schema::DocumentSchema;
use cy_editor_documents::selection::Selection;
use cy_editor_viewport::gizmo::{Transform3, TransformBinding};
use cy_editor_viewport::math::Vec3;

/// Register `scene.create-primitive` and `asset.write-primitive`.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(create_primitive())?;
    registry.register(write_primitive())?;
    Ok(())
}

// --- The source format -------------------------------------------------------------------------
//
// THE OTHER HALF OF THIS FORMAT IS IN C++, and the two halves are pinned to each other by a golden
// string in each language's test suite. See `tools/import/include/cy/import/primitive.h`: the
// source bytes are hashed into the derivation key, so the editor writing `1.0` where the engine's
// own writer writes `1` would be two cooked meshes of one primitive.

/// The five shapes, and nothing else.
///
/// This enumeration decides which parameters a source file carries and stops there. It is not a
/// node type, it is not stored in a document, and nothing downstream of the written file branches
/// on it — which is design.md §2's "no shape enum the rest of the editor branches on".
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Shape {
    /// A rectangular box, sized by `extent`.
    Box,
    /// A sphere of `radius`, divided into `segments` and `rings`.
    Sphere,
    /// A capped cylinder of `radius` and `height`.
    Cylinder,
    /// A flat rectangle in the XZ plane, sized by `extent` and divided by `subdivisions`.
    Plane,
    /// A capsule: a cylinder of `height` capped by two hemispheres of `radius`.
    Capsule,
}

impl Shape {
    /// The keyword the source file carries.
    #[must_use]
    pub const fn keyword(self) -> &'static str {
        match self {
            Shape::Box => "box",
            Shape::Sphere => "sphere",
            Shape::Cylinder => "cylinder",
            Shape::Plane => "plane",
            Shape::Capsule => "capsule",
        }
    }

    /// The name a primitive of this shape takes when the caller supplies none.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Shape::Box => "Box",
            Shape::Sphere => "Sphere",
            Shape::Cylinder => "Cylinder",
            Shape::Plane => "Plane",
            Shape::Capsule => "Capsule",
        }
    }

    /// Every shape, for a listing and for a test that must cover all of them.
    pub const ALL: [Shape; 5] = [
        Shape::Box,
        Shape::Sphere,
        Shape::Cylinder,
        Shape::Plane,
        Shape::Capsule,
    ];

    /// The shape a keyword names.
    #[must_use]
    pub fn from_keyword(keyword: &str) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|shape| shape.keyword() == keyword)
    }

    /// Whether this shape's source file carries a given parameter.
    #[must_use]
    const fn takes(self, parameter: Parameter) -> bool {
        match parameter {
            Parameter::Extent => matches!(self, Shape::Box | Shape::Plane),
            Parameter::Radius => matches!(self, Shape::Sphere | Shape::Cylinder | Shape::Capsule),
            Parameter::Height => matches!(self, Shape::Cylinder | Shape::Capsule),
            Parameter::Segments => matches!(self, Shape::Sphere | Shape::Cylinder | Shape::Capsule),
            Parameter::Rings => matches!(self, Shape::Sphere | Shape::Capsule),
            Parameter::Subdivisions => matches!(self, Shape::Plane),
        }
    }
}

/// The parameters a shape may take, for the refusal below.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Parameter {
    Extent,
    Radius,
    Height,
    Segments,
    Rings,
    Subdivisions,
}

impl Parameter {
    const ALL: [Parameter; 6] = [
        Parameter::Extent,
        Parameter::Radius,
        Parameter::Height,
        Parameter::Segments,
        Parameter::Rings,
        Parameter::Subdivisions,
    ];

    const fn name(self) -> &'static str {
        match self {
            Parameter::Extent => "extent",
            Parameter::Radius => "radius",
            Parameter::Height => "height",
            Parameter::Segments => "segments",
            Parameter::Rings => "rings",
            Parameter::Subdivisions => "subdivisions",
        }
    }
}

/// Where the shape's origin sits relative to its geometry.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Origin {
    /// The centre of its bounds.
    Centre,
    /// The centre of its footprint, with its lowest point at y = 0.
    Base,
}

impl Origin {
    const fn keyword(self) -> &'static str {
        match self {
            Origin::Centre => "centre",
            Origin::Base => "base",
        }
    }

    fn from_keyword(keyword: &str) -> Option<Self> {
        match keyword {
            "centre" => Some(Origin::Centre),
            "base" => Some(Origin::Base),
            _ => None,
        }
    }
}

/// A whole primitive, as its source file states it.
#[derive(Clone, PartialEq, Debug)]
pub struct Primitive {
    /// Which shape.
    pub shape: Shape,
    /// Its name: the node's, and the stem of every sub-asset the cook produces.
    pub name: String,
    /// Where its origin sits.
    pub origin: Origin,
    /// The full size along each axis. Box: all three. Plane: x and z.
    pub extent: [f32; 3],
    /// Sphere, cylinder and capsule.
    pub radius: f32,
    /// Cylinder and capsule: the length of the cylindrical section.
    pub height: f32,
    /// Divisions around the axis of revolution.
    pub segments: i64,
    /// Divisions along it.
    pub rings: i64,
    /// Plane: quads along each axis.
    pub subdivisions: i64,
}

impl Default for Primitive {
    fn default() -> Self {
        Self {
            shape: Shape::Box,
            name: Shape::Box.label().to_string(),
            origin: Origin::Centre,
            extent: [1.0, 1.0, 1.0],
            radius: 0.5,
            height: 1.0,
            segments: 32,
            rings: 16,
            subdivisions: 1,
        }
    }
}

/// The bounds `tools/import/include/cy/import/primitive.h` states, mirrored so that a mistake is
/// refused where a person can see it rather than at cook time.
///
/// They are duplicated rather than shared because they are on the other side of a process boundary
/// and of a language boundary; the parser is the authority, and this is the courtesy. A value the
/// engine would refuse and this accepts produces a `malformed-primitive` diagnostic naming the
/// line, which is a worse message and not a wrong one.
const MIN_EXTENT: f32 = 1.0e-4;
const MAX_EXTENT: f32 = 1.0e5;
const MIN_SEGMENTS: i64 = 3;
const MAX_SEGMENTS: i64 = 512;
const MIN_RINGS: i64 = 2;
const MAX_RINGS: i64 = 512;
const MAX_SUBDIVISIONS: i64 = 512;
const MAX_NAME: usize = 96;

/// The directory a primitive's source lands in when the caller names no path.
pub const PRIMITIVE_DIRECTORY: &str = "assets/primitives";

/// The extension a primitive source carries.
pub const PRIMITIVE_EXTENSION: &str = ".cyprim";

impl Primitive {
    /// The canonical source text: the bytes `cy::import::parse_primitive_source` reads.
    ///
    /// Byte for byte the same text `cy::import::write_primitive_source` produces, which the suites
    /// on both sides pin with the same golden string.
    #[must_use]
    pub fn source_text(&self) -> String {
        let mut lines = vec![
            "cyprim 1".to_string(),
            format!("shape {}", self.shape.keyword()),
            format!("name {}", self.name),
            format!("origin {}", self.origin.keyword()),
        ];
        match self.shape {
            Shape::Box => lines.push(format!(
                "extent {} {} {}",
                number(self.extent[0]),
                number(self.extent[1]),
                number(self.extent[2])
            )),
            Shape::Plane => {
                lines.push(format!(
                    "extent {} {}",
                    number(self.extent[0]),
                    number(self.extent[2])
                ));
                let divisions = self.subdivisions;
                lines.push(format!("subdivisions {divisions} {divisions}"));
            }
            Shape::Sphere => {
                lines.push(format!("radius {}", number(self.radius)));
                lines.push(format!("segments {}", self.segments));
                lines.push(format!("rings {}", self.rings));
            }
            Shape::Cylinder => {
                lines.push(format!("radius {}", number(self.radius)));
                lines.push(format!("height {}", number(self.height)));
                lines.push(format!("segments {}", self.segments));
            }
            Shape::Capsule => {
                lines.push(format!("radius {}", number(self.radius)));
                lines.push(format!("height {}", number(self.height)));
                lines.push(format!("segments {}", self.segments));
                lines.push(format!("rings {}", self.rings));
            }
        }
        // A trailing newline, because every line of this format ends with one — including the last.
        lines.join("\n") + "\n"
    }

    /// Where this primitive's source belongs when the caller names no path.
    #[must_use]
    pub fn default_asset_path(&self) -> String {
        format!("{PRIMITIVE_DIRECTORY}/{}{PRIMITIVE_EXTENSION}", self.name)
    }

    /// Refuse a primitive the generator would refuse, and say which parameter and why.
    fn check(&self) -> Result<()> {
        let refuse = |what: &str, why: String| {
            Err(Problem::new(format!("write the {what}"), why)
                .with_remedy("pass a value inside the range the parameter's description states"))
        };
        if self.name.trim().is_empty() || self.name.len() > MAX_NAME {
            return refuse(
                "primitive",
                format!("a name is one to {MAX_NAME} characters and this one is not"),
            );
        }
        if self
            .name
            .chars()
            .any(|character| character.is_control() || character == '"' || character == '\n')
        {
            return refuse(
                "primitive",
                "a name carries no control characters and no quotes".to_string(),
            );
        }
        for lane in self.extent {
            if self.shape.takes(Parameter::Extent) && !(MIN_EXTENT..=MAX_EXTENT).contains(&lane) {
                return refuse(
                    "extent",
                    format!("{lane} is outside {MIN_EXTENT}..{MAX_EXTENT}"),
                );
            }
        }
        if self.shape.takes(Parameter::Radius) && !(MIN_EXTENT..=MAX_EXTENT).contains(&self.radius)
        {
            return refuse(
                "radius",
                format!("{} is outside {MIN_EXTENT}..{MAX_EXTENT}", self.radius),
            );
        }
        if self.shape.takes(Parameter::Height) && !(MIN_EXTENT..=MAX_EXTENT).contains(&self.height)
        {
            return refuse(
                "height",
                format!("{} is outside {MIN_EXTENT}..{MAX_EXTENT}", self.height),
            );
        }
        if self.shape.takes(Parameter::Segments)
            && !(MIN_SEGMENTS..=MAX_SEGMENTS).contains(&self.segments)
        {
            return refuse(
                "segments",
                format!(
                    "{} is outside {MIN_SEGMENTS}..{MAX_SEGMENTS}",
                    self.segments
                ),
            );
        }
        if self.shape.takes(Parameter::Rings) && !(MIN_RINGS..=MAX_RINGS).contains(&self.rings) {
            return refuse(
                "rings",
                format!("{} is outside {MIN_RINGS}..{MAX_RINGS}", self.rings),
            );
        }
        if self.shape.takes(Parameter::Subdivisions)
            && !(1..=MAX_SUBDIVISIONS).contains(&self.subdivisions)
        {
            return refuse(
                "subdivisions",
                format!("{} is outside 1..{MAX_SUBDIVISIONS}", self.subdivisions),
            );
        }
        Ok(())
    }
}

/// One number, as the canonical text spells it: up to six decimal places, trailing zeros and a
/// trailing point removed.
///
/// Not Rust's `{}`, and not C++'s `%g`. Both are shortest-round-trip formatters and they disagree
/// about the exponent form, so a parameter of 1e-7 would be written two ways by the two writers of
/// this format. Six fixed decimals then trimmed is a rule both languages implement identically.
fn number(value: f32) -> String {
    let text = format!("{:.6}", f64::from(value));
    let trimmed = if text.contains('.') {
        text.trim_end_matches('0').trim_end_matches('.')
    } else {
        text.as_str()
    };
    if trimmed.is_empty() || trimmed == "-0" {
        "0".to_string()
    } else {
        trimmed.to_string()
    }
}

// --- The mesh instance -------------------------------------------------------------------------

/// Where a mesh instance keeps the asset it draws.
///
/// The same shape as [`TransformBinding`], and for the same reason: a document's schema is the
/// editor's own and the engine is related to it by name. See the module note about
/// `cy::render::MeshRenderer`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct MeshBinding {
    /// The component that holds the reference.
    pub component: cy_editor_core::ids::TypeId,
    /// The field holding the asset the entity draws.
    pub mesh: cy_editor_core::ids::FieldId,
}

impl MeshBinding {
    /// The component's name. The engine's own, unqualified — `src/scene/src/node_template.cpp`
    /// names `cy::render::MeshRenderer`, and `authoring_name_of` is what drops the namespace.
    pub const COMPONENT: &'static str = "MeshRenderer";

    /// The field's name.
    pub const FIELD: &'static str = "mesh";

    /// Find the binding in a document's schema, or answer that there is none.
    #[must_use]
    pub fn of_schema(schema: &DocumentSchema) -> Option<Self> {
        let definition = schema.type_named(Self::COMPONENT)?;
        Some(Self {
            component: definition.id,
            mesh: definition.field_named(Self::FIELD)?.id,
        })
    }

    /// Find it, or declare it. Idempotent: a world that already carries the component keeps its own
    /// identifiers, because a second declaration would give the same name a second identity and
    /// every history entry addressing the first would stop applying.
    pub fn declare(schema: &mut DocumentSchema) -> Self {
        if let Some(found) = Self::of_schema(schema) {
            return found;
        }
        let component = schema.declare_type(Self::COMPONENT, false);
        let mesh = schema
            .declare_field(
                component,
                Self::FIELD,
                ValueKind::Text,
                "The mesh asset this entity draws.",
            )
            .expect("the type was declared on the line above");
        Self { component, mesh }
    }
}

/// Create one mesh instance — a transform and a mesh reference — inside the open transaction.
///
/// **The one function that builds a mesh instance, whatever produced the mesh.** A primitive calls
/// it with the `.cyprim` it just wrote; an import calls it with the file it just imported. Task 2.3
/// asks that nothing downstream be able to tell the two apart, and one constructor with no
/// parameter for the difference is how that is guaranteed rather than checked.
///
/// The reference is recorded as [`Operation::SetAssetReference`] rather than as a text field,
/// because "a reference is what a dependency tracker follows and what a rename has to rewrite".
///
/// # Errors
///
/// When the document refuses an operation — no transaction is open, or the node is unknown.
pub fn create_mesh_instance(
    document: &mut Document,
    parent: Option<NodeId>,
    asset: &str,
    placement: Transform3,
) -> Result<NodeId> {
    let transform = TransformBinding::of_schema(document.schema());
    let mesh = MeshBinding::declare(document.schema_mut());

    let node = document.create_node(parent)?;
    if let Some(binding) = transform {
        document.add_component(
            node,
            binding.component,
            vec![
                (
                    binding.translation,
                    Value::Vec3(placement.translation.to_array()),
                ),
                (binding.rotation, Value::Quat(placement.rotation.to_array())),
                (binding.scale, Value::Vec3(placement.scale.to_array())),
            ],
        )?;
    }
    document.add_component(
        node,
        mesh.component,
        vec![(mesh.mesh, Value::Text(String::new()))],
    )?;
    document.record(Operation::SetAssetReference {
        node,
        component: mesh.component,
        field: mesh.mesh,
        before: String::new(),
        after: asset.to_string(),
    })?;
    Ok(node)
}

/// The mesh asset an entity draws, or nothing when it draws none.
///
/// What an inspector, a dependency listing and a test all read, so that "which asset is this" has
/// one answer rather than one per caller.
#[must_use]
pub fn mesh_of(document: &Document, node: NodeId) -> Option<String> {
    let binding = MeshBinding::of_schema(document.schema())?;
    match document
        .content()
        .field(node, binding.component, binding.mesh)?
    {
        Value::Text(asset) if !asset.is_empty() => Some(asset.clone()),
        _ => None,
    }
}

// --- The commands ------------------------------------------------------------------------------

/// The document a command with no explicit target acts on.
fn active(context: &dyn CommandContext) -> Result<DocumentId> {
    context.active_document().ok_or_else(|| {
        Problem::new("create a primitive", "no document is open").with_remedy("open a world first")
    })
}

/// Read the primitive a set of arguments describes, refusing what the generator would refuse.
///
/// A parameter the shape does not take is refused when it differs from its default — a `radius` on
/// a box would otherwise be a value a person sets, saves and watches do nothing, which is exactly
/// what the source format refuses at its own parse.
fn requested(arguments: &cy_editor_commands::Arguments) -> Result<Primitive> {
    let keyword = arguments.text("shape").unwrap_or_default().trim();
    let shape = Shape::from_keyword(keyword).ok_or_else(|| {
        Problem::new(
            "create a primitive",
            format!("{keyword:?} is not a shape this editor generates"),
        )
        .with_remedy(format!(
            "name one of: {}",
            Shape::ALL.map(Shape::keyword).join(", ")
        ))
    })?;
    let origin_keyword = arguments.text("origin").unwrap_or_default().trim();
    let origin = Origin::from_keyword(origin_keyword).ok_or_else(|| {
        Problem::new(
            "create a primitive",
            format!("{origin_keyword:?} is not an origin"),
        )
        .with_remedy("pass 'centre' or 'base'")
    })?;

    let name = match arguments.text("name").unwrap_or_default().trim() {
        "" => shape.label().to_string(),
        given => given.to_string(),
    };
    let defaults = Primitive::default();
    let primitive = Primitive {
        shape,
        name,
        origin,
        extent: match arguments.get("extent") {
            Some(Value::Vec3(lanes)) => *lanes,
            _ => defaults.extent,
        },
        radius: float_of(arguments, "radius", defaults.radius),
        height: float_of(arguments, "height", defaults.height),
        segments: int_of(arguments, "segments", defaults.segments),
        rings: int_of(arguments, "rings", defaults.rings),
        subdivisions: int_of(arguments, "subdivisions", defaults.subdivisions),
    };
    refuse_parameters_the_shape_does_not_take(&primitive, &defaults)?;
    primitive.check()?;
    Ok(primitive)
}

#[allow(
    clippy::float_cmp,
    reason = "the question is whether the CALLER supplied a value, and the registry fills an \
              omitted parameter with its declared default — so the comparison is against that \
              exact bit pattern rather than against a computed quantity. An epsilon here would \
              accept a radius of 0.5000001 on a box, which is the mistake this refusal exists for."
)]
fn refuse_parameters_the_shape_does_not_take(
    primitive: &Primitive,
    defaults: &Primitive,
) -> Result<()> {
    for parameter in Parameter::ALL {
        if primitive.shape.takes(parameter) {
            continue;
        }
        let supplied = match parameter {
            Parameter::Extent => primitive.extent != defaults.extent,
            Parameter::Radius => primitive.radius != defaults.radius,
            Parameter::Height => primitive.height != defaults.height,
            Parameter::Segments => primitive.segments != defaults.segments,
            Parameter::Rings => primitive.rings != defaults.rings,
            Parameter::Subdivisions => primitive.subdivisions != defaults.subdivisions,
        };
        if supplied {
            return Err(Problem::new(
                format!("create a {}", primitive.shape.keyword()),
                format!(
                    "a {} does not take {}",
                    primitive.shape.keyword(),
                    parameter.name()
                ),
            )
            .with_remedy(
                "leave it out; the source file a primitive is generated from carries only the \
                 parameters its shape has, and one it does not carry would change the cache key \
                 and nothing else",
            ));
        }
    }
    Ok(())
}

fn float_of(arguments: &cy_editor_commands::Arguments, name: &str, fallback: f32) -> f32 {
    match arguments.get(name) {
        Some(Value::Float(value)) => *value,
        _ => fallback,
    }
}

fn int_of(arguments: &cy_editor_commands::Arguments, name: &str, fallback: i64) -> i64 {
    match arguments.get(name) {
        Some(Value::Int(value)) => *value,
        _ => fallback,
    }
}

/// Where the source goes: what the caller named, or the primitive's own default path.
fn asset_path(arguments: &cy_editor_commands::Arguments, primitive: &Primitive) -> String {
    match arguments.text("asset").unwrap_or_default().trim() {
        "" => primitive.default_asset_path(),
        given => given.to_string(),
    }
}

/// Write the source asset, or leave an identical one alone.
///
/// Refuses to overwrite a DIFFERENT primitive at the same path: every entity referencing it would
/// silently change shape, which is a thing a person should ask for by name.
fn write_source(
    context: &mut dyn CommandContext,
    path: &str,
    primitive: &Primitive,
    overwrite: bool,
) -> Result<bool> {
    crate::authoring::within_scope(context, path)?;
    let text = primitive.source_text();
    let project = context.project().ok_or_else(|| {
        Problem::new("reach the project", "this editor has no project open")
            .with_remedy("open a project first")
    })?;
    if project.source_exists(path) {
        let held = project.read_source(path)?;
        if held == text {
            return Ok(false);
        }
        if !overwrite {
            return Err(Problem::new(
                format!("write {path}"),
                "a different primitive is already stored there",
            )
            .with_remedy(
                "pass a different name or asset path, or use asset.write-primitive to change the \
                 one that is there — which changes every entity that draws it",
            ));
        }
    }
    project.put_source(path, Some(&text))?;
    Ok(true)
}

fn create_primitive() -> Command {
    Command::new(
        metadata(
            "scene.create-primitive",
            "Create Primitive",
            "Scene",
            "Creates a box, sphere, cylinder, plane or capsule in the active world, as one \
             undoable transaction. The shape is written into the project as a .cyprim source \
             asset, which the engine generates a mesh from through the same importer registry, \
             derivation key and cache as a glTF — so the entity this creates is an ordinary mesh \
             instance and nothing downstream can tell it from an imported one. Undo removes the \
             entity; the source asset stays, exactly as an imported file does.",
        )
        .with(ParameterSpec::optional(
            "at",
            ValueKind::Vec3,
            "Where to put it, in metres, in the world's own space. The origin when omitted.",
            Value::Vec3([0.0, 0.0, 0.0]),
        ))
        .with(ParameterSpec::optional(
            "parent",
            ValueKind::Text,
            "The identity of the entity to create it under; a root when omitted.",
            Value::Text(String::new()),
        ))
        .bound_to("Ctrl+Shift+B"),
        |context, arguments| {
            let primitive = requested(arguments)?;
            let path = asset_path(arguments, &primitive);
            let document_id = active(context)?;
            let parent = parse_node(arguments.text("parent").unwrap_or_default())?;
            let at = match arguments.get("at") {
                Some(Value::Vec3(lanes)) => *lanes,
                _ => [0.0, 0.0, 0.0],
            };

            // The asset first, and the entity second: an entity referencing a source file that was
            // never written would be a world that does not cook, and the failure would arrive a
            // milestone later. A failed write leaves no transaction behind.
            let written = write_source(context, &path, &primitive, false)?;

            let actor = context.actor();
            let document = context
                .document_mut(document_id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            let placement = Transform3 {
                translation: Vec3::new(at[0], at[1], at[2]),
                ..Transform3::default()
            };
            let node = document.with_transaction(
                format!("Create {}", primitive.shape.keyword()),
                actor,
                |document| create_mesh_instance(document, parent, &path, placement),
            )?;

            let mut selection = Selection::new();
            selection.add_node(node);
            context.set_selection(selection);

            Ok(Outcome::new(format!(
                "Created a {} named {}",
                primitive.shape.keyword(),
                primitive.name
            ))
            .with("entity", Value::Text(node.to_string()))
            .with("asset", Value::Text(path))
            .with("shape", Value::Text(primitive.shape.keyword().to_string()))
            .with("generated", Value::Bool(written)))
        },
    )
}

fn write_primitive() -> Command {
    Command::new(
        metadata(
            "asset.write-primitive",
            "Write Primitive Asset",
            "Asset",
            "Writes or rewrites a .cyprim source asset with the parameters given, without \
             creating anything in the world. This is how a primitive's parameters are edited: the \
             file changes, the engine re-generates its mesh under a new derivation key, and every \
             entity that draws it follows. Not undoable through the document history — it is a \
             file, like every other asset source.",
        )
        .with(ParameterSpec::optional(
            "asset",
            ValueKind::Text,
            "The project-relative path of the .cyprim to write; assets/primitives/<name>.cyprim \
             when omitted.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let primitive = requested(arguments)?;
            let path = asset_path(arguments, &primitive);
            let changed = write_source(context, &path, &primitive, true)?;
            Ok(Outcome::new(if changed {
                format!("Wrote {path}")
            } else {
                format!("{path} already held that")
            })
            .with("asset", Value::Text(path))
            .with("changed", Value::Bool(changed)))
        },
    )
}

/// The shape parameters both commands declare, in one table, so the two cannot drift.
///
/// A caller that has never seen the editor reads these descriptions and nothing else, which is what
/// `editor-agent-interface` requires of every parameter — and what `Registry::register` refuses a
/// command without.
fn metadata(id: &str, label: &str, category: &str, description: &str) -> Metadata {
    Metadata::new(
        id,
        label,
        category,
        description,
        EffectClass::ReversibleMutation,
    )
    .with(ParameterSpec::required(
        "shape",
        ValueKind::Text,
        "Which shape: box, sphere, cylinder, plane or capsule.",
    ))
    .with(ParameterSpec::optional(
        "name",
        ValueKind::Text,
        "What to call it. It names the source asset, the generated mesh sub-asset and the node. \
         The shape's own name when omitted. A name ending in the importers' collision suffix \
         (_collision) generates a collider that is not drawn, which is the same convention a model \
         import applies to a node's name.",
        Value::Text(String::new()),
    ))
    .with(ParameterSpec::optional(
        "origin",
        ValueKind::Text,
        "Where the shape's origin sits: 'centre' of its bounds, or 'base' — its footprint, with \
         its lowest point at y=0, which is what a thing standing on a floor wants. 'centre' when \
         omitted.",
        Value::Text(Origin::Centre.keyword().to_string()),
    ))
    .with(ParameterSpec::optional(
        "extent",
        ValueKind::Vec3,
        "Box: its full size along X, Y and Z, in metres. Plane: X and Z, with Y ignored. One \
         metre cubed when omitted. Refused for a shape that has no extent.",
        Value::Vec3([1.0, 1.0, 1.0]),
    ))
    .with(ParameterSpec::optional(
        "radius",
        ValueKind::Float,
        "Sphere, cylinder and capsule: the radius in metres. Half a metre when omitted.",
        Value::Float(0.5),
    ))
    .with(ParameterSpec::optional(
        "height",
        ValueKind::Float,
        "Cylinder and capsule: the height in metres. For a capsule this is the CYLINDRICAL \
         section, so its total height is height + 2 * radius — the convention every physics \
         engine uses for a capsule. One metre when omitted.",
        Value::Float(1.0),
    ))
    .with(ParameterSpec::optional(
        "segments",
        ValueKind::Int,
        "Sphere, cylinder and capsule: divisions around the axis of revolution, 3 to 512. \
         Thirty-two when omitted.",
        Value::Int(32),
    ))
    .with(ParameterSpec::optional(
        "rings",
        ValueKind::Int,
        "Sphere and capsule: divisions along the axis of revolution, 2 to 512. Sixteen when \
         omitted.",
        Value::Int(16),
    ))
    .with(ParameterSpec::optional(
        "subdivisions",
        ValueKind::Int,
        "Plane: how many quads along each axis, 1 to 512. One when omitted.",
        Value::Int(1),
    ))
}

/// An entity identity as text, or nothing when the text is empty.
///
/// The same spelling `crate::builtin` reads — 32 hex digits, which is what a command's result
/// prints — because an identity a person copies out of one result and into another argument must
/// not need reformatting on the way.
fn parse_node(text: &str) -> Result<Option<NodeId>> {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return Ok(None);
    }
    u128::from_str_radix(trimmed, 16)
        .map(NodeId::from_u128)
        .map(Some)
        .map_err(|_| {
            Problem::new(
                format!("read the entity {trimmed:?}"),
                "it is not an entity identity",
            )
            .with_remedy("use the identity a command's result printed, which is 32 hex digits")
        })
}
