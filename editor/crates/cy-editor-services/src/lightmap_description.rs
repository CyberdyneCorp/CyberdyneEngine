// SPDX-License-Identifier: MIT
//! The level's `.cylightmap` description, written from the open world.
//!
//! `lighting.bake-lightmaps` bakes a description with `cy_build lightmap` (see [`crate::lightmaps`]).
//! A description written by hand would bake whatever its author remembered to copy out of the
//! world; this writes it FROM the world, on every bake request, so the bake is always of what was
//! authored:
//!
//! | The world has | The description gets |
//! |---|---|
//! | a `LightSource` (enabled) | a `light` line: world position or direction, colour, intensity, range, its mobility word and `id` |
//! | a `MeshRenderer` with a mesh | an `instance` line: the cooked mesh (`.cy/cooked/<id>.cyasset`) or `.cyprim` source, its world transform, its `LightmapObject` resolution scale, `id`, and `occluder` when it receives no lightmap |
//! | the material it draws with | a `material` line: `cooked` from the drawn slot's cooked material with the renderer's tint, or the frame's default grey |
//! | an `IrradianceVolume` | a `volume` line: `id`, world origin, spacing, probe counts and rays |
//!
//! `id` is the object's engine identity ([`crate::mirror::engine_identity`]), the same number the
//! runtime world knows it by, so the cooked lightmap's shadow-mask channels and directly baked
//! lights, and each probe volume, name the scene objects they came from.
//!
//! THE TEXT IS A FUNCTION OF THE WORLD AND THE SETTINGS, NOTHING ELSE. Lines are ordered by
//! identity and numbers are written in Rust's shortest round-trip form, so an unchanged world
//! writes the same bytes — [`write_if_changed`] then leaves the file alone, and `cy_build
//! lightmap`'s content key (the build graph's derivation over the description and every file it
//! reads) says the level need not be baked again.
//!
//! Not written: spot cones (the bake's `gi::GiLight` has none, so a spot bakes as a point light),
//! material graphs (`.cygraph`: drawn by the frame, baked with the default grey), terrain, and
//! the sky, which the world does not author yet.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;

use cy_editor_commands::Arguments;
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_documents::Document;
use cy_editor_viewport::gizmo::TransformBinding;

use crate::assets::COOKED_DIRECTORY;
use crate::lighting::{LIGHT, LightingScene};
use crate::mirror::engine_identity;
use crate::primitives::{MaterialBinding, MaterialSlotsBinding, MeshBinding};

/// A description's extension.
pub const EXTENSION: &str = "cylightmap";

/// How the level is baked: the description's settings lines.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct LevelSettings {
    /// `irradiance`, `directional` or `sh-l1`.
    pub mode: &'static str,
    /// Path-traced samples per texel.
    pub samples: u32,
    /// Bounces per path.
    pub bounces: u32,
    /// Texels per metre.
    pub density: f32,
    /// Texels along an atlas page side.
    pub page: u32,
}

impl Default for LevelSettings {
    fn default() -> Self {
        Self {
            mode: "directional",
            samples: 64,
            bounces: 2,
            density: 4.0,
            page: 256,
        }
    }
}

/// The modes the engine bakes, as the description spells them.
pub const MODES: [&str; 3] = ["irradiance", "directional", "sh-l1"];

impl LevelSettings {
    /// The settings a command's arguments name, each defaulted when absent.
    ///
    /// # Errors
    ///
    /// A mode the engine does not have, or a number outside what it bakes.
    pub fn from_arguments(arguments: &Arguments) -> Result<Self> {
        let defaults = Self::default();
        let refuse = |why: String| Err(Problem::new("bake lightmaps", why));
        let mode = match arguments.text("mode") {
            None | Some("") => defaults.mode,
            Some(named) => match MODES.into_iter().find(|mode| *mode == named) {
                Some(mode) => mode,
                None => {
                    return refuse(format!("{named:?} is not irradiance, directional or sh-l1"));
                }
            },
        };
        let int = |name: &str, fallback: u32| {
            arguments
                .get(name)
                .and_then(Value::as_int)
                .map_or(Ok(fallback), u32::try_from)
        };
        let (Ok(samples), Ok(bounces), Ok(page)) = (
            int("samples", defaults.samples),
            int("bounces", defaults.bounces),
            int("page", defaults.page),
        ) else {
            return refuse("samples, bounces and page are counts".to_string());
        };
        let density = arguments
            .get("density")
            .and_then(Value::as_float)
            .unwrap_or(defaults.density);
        if !(1..=4096).contains(&samples) {
            return refuse(format!("{samples} samples per texel is not 1 to 4096"));
        }
        if bounces > 8 {
            return refuse(format!("{bounces} bounces is not 0 to 8"));
        }
        if !(density > 0.0 && density <= 64.0) {
            return refuse(format!(
                "{density} texels per metre is not above 0 and at most 64"
            ));
        }
        if !page.is_power_of_two() || !(128..=4096).contains(&page) {
            return refuse(format!(
                "{page} is not a page size: a power of two from 128 to 4096"
            ));
        }
        Ok(Self {
            mode,
            samples,
            bounces,
            density,
            page,
        })
    }
}

/// Where a world's description lives: beside it, with the `.cylightmap` extension.
#[must_use]
pub fn description_path(world: &str) -> String {
    Path::new(world)
        .with_extension(EXTENSION)
        .to_string_lossy()
        .replace('\\', "/")
}

/// Write `text` at `path` unless the file already holds exactly it. True when it was written.
///
/// # Errors
///
/// When the directory cannot be made or the file cannot be written.
pub fn write_if_changed(path: &Path, text: &str) -> Result<bool> {
    if std::fs::read(path).is_ok_and(|held| held == text.as_bytes()) {
        return Ok(false);
    }
    let failed = |error: std::io::Error| {
        Problem::new(format!("write {}", path.display()), error.to_string())
    };
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(failed)?;
    }
    // Through a sibling and a rename, so the tool never reads half a description.
    let staging = path.with_extension(format!("{EXTENSION}.writing"));
    std::fs::write(&staging, text).map_err(failed)?;
    std::fs::rename(&staging, path).map_err(failed)?;
    Ok(true)
}

// --- Transforms ----------------------------------------------------------------------------------

/// A 3x4 affine transform, row-major: the description's instance layout.
type Affine = [[f32; 4]; 3];

const IDENTITY: Affine = [
    [1.0, 0.0, 0.0, 0.0],
    [0.0, 1.0, 0.0, 0.0],
    [0.0, 0.0, 1.0, 0.0],
];

/// Translation, rotation (`x, y, z, w`) and scale, as one affine transform: `T * R * S`.
fn affine(translation: [f32; 3], rotation: [f32; 4], scale: [f32; 3]) -> Affine {
    let [x, y, z, w] = rotation;
    let rows = [
        [
            1.0 - 2.0 * (y * y + z * z),
            2.0 * (x * y - z * w),
            2.0 * (x * z + y * w),
        ],
        [
            2.0 * (x * y + z * w),
            1.0 - 2.0 * (x * x + z * z),
            2.0 * (y * z - x * w),
        ],
        [
            2.0 * (x * z - y * w),
            2.0 * (y * z + x * w),
            1.0 - 2.0 * (x * x + y * y),
        ],
    ];
    let mut out = IDENTITY;
    for (row, rotated) in out.iter_mut().zip(rows) {
        for column in 0..3 {
            row[column] = rotated[column] * scale[column];
        }
    }
    for (row, offset) in out.iter_mut().zip(translation) {
        row[3] = offset;
    }
    out
}

/// `parent * child`.
fn compose(parent: &Affine, child: &Affine) -> Affine {
    let mut out = IDENTITY;
    for row in 0..3 {
        for column in 0..4 {
            let mut sum = if column == 3 { parent[row][3] } else { 0.0 };
            for inner in 0..3 {
                sum += parent[row][inner] * child[inner][column];
            }
            out[row][column] = sum;
        }
    }
    out
}

/// A node's own transform: identity where it has none.
fn local(document: &Document, binding: Option<TransformBinding>, node: NodeId) -> Affine {
    let Some(binding) = binding else {
        return IDENTITY;
    };
    let content = document.content();
    let vec3 = |field, fallback| {
        content
            .field(node, binding.component, field)
            .and_then(Value::as_vec3)
            .unwrap_or(fallback)
    };
    let rotation = match content.field(node, binding.component, binding.rotation) {
        Some(Value::Quat(lanes)) => *lanes,
        _ => [0.0, 0.0, 0.0, 1.0],
    };
    affine(
        vec3(binding.translation, [0.0; 3]),
        rotation,
        vec3(binding.scale, [1.0; 3]),
    )
}

/// A node's world transform: its own under every ancestor's.
fn world(document: &Document, binding: Option<TransformBinding>, node: NodeId) -> Affine {
    let mut chain = vec![node];
    let mut at = node;
    // Bounded by the document's size, so a malformed cycle cannot spin.
    for _ in 0..document.content().nodes().count() {
        match document.content().node(at).and_then(|state| state.parent) {
            Some(parent) => {
                chain.push(parent);
                at = parent;
            }
            None => break,
        }
    }
    chain.iter().rev().fold(IDENTITY, |outer, node| {
        compose(&outer, &local(document, binding, *node))
    })
}

// --- The lines -----------------------------------------------------------------------------------

/// A number as the description writes it: Rust's shortest form that reads back exactly.
fn number(value: f32) -> String {
    format!("{value}")
}

fn numbers(values: &[f32]) -> String {
    values
        .iter()
        .map(|value| number(*value))
        .collect::<Vec<_>>()
        .join(" ")
}

fn quoted(text: &str) -> String {
    format!("\"{}\"", text.replace('\\', "\\\\").replace('"', "\\\""))
}

/// A field of a named component, when the node has it.
fn field<'document>(
    document: &'document Document,
    node: NodeId,
    component: &str,
    name: &str,
) -> Option<&'document Value> {
    let definition = document.schema().type_named(component)?;
    let field = definition.field_named(name)?;
    document.content().field(node, definition.id, field.id)
}

fn float_field(document: &Document, node: NodeId, component: &str, name: &str, or: f32) -> f32 {
    field(document, node, component, name)
        .and_then(Value::as_float)
        .unwrap_or(or)
}

/// Nodes carrying `component`, in engine-identity order.
fn with_component(document: &Document, component: &str) -> Vec<NodeId> {
    let Some(definition) = document.schema().type_named(component) else {
        return Vec::new();
    };
    let mut nodes: Vec<NodeId> = document
        .content()
        .nodes()
        .filter(|node| document.content().has_component(*node, definition.id))
        .collect();
    nodes.sort_by_key(|node| engine_identity(*node));
    nodes
}

fn light_line(
    document: &Document,
    binding: Option<TransformBinding>,
    scene: &LightingScene,
    node: NodeId,
) -> Option<String> {
    if matches!(
        field(document, node, LIGHT, "enabled"),
        Some(Value::Bool(false))
    ) {
        return None;
    }
    let kind = field(document, node, LIGHT, "kind").and_then(Value::as_int)?;
    let matrix = world(document, binding, node);
    let colour = numbers(&[
        float_field(document, node, LIGHT, "color.r", 1.0),
        float_field(document, node, LIGHT, "color.g", 1.0),
        float_field(document, node, LIGHT, "color.b", 1.0),
    ]);
    let intensity = number(float_field(document, node, LIGHT, "intensity", 1000.0));
    let mobility = scene.mobility_of(node).unwrap_or_default().keyword();
    let id = engine_identity(node);
    Some(if kind == 0 {
        // The light points down its local -z, as the frame shades it.
        let direction = [-matrix[0][2], -matrix[1][2], -matrix[2][2]];
        let length = direction.iter().map(|lane| lane * lane).sum::<f32>().sqrt();
        let direction = if length > 0.0 {
            direction.map(|lane| lane / length)
        } else {
            [0.0, 0.0, -1.0]
        };
        format!(
            "light directional {} {intensity} {colour} {mobility} id {id}",
            numbers(&direction)
        )
    } else {
        let range = number(float_field(document, node, LIGHT, "range", 10.0));
        let position = [matrix[0][3], matrix[1][3], matrix[2][3]];
        format!(
            "light point {} {intensity} {range} {colour} {mobility} id {id}",
            numbers(&position)
        )
    })
}

/// The file an instance's mesh reference names: a `.cyprim` source as it is, anything else the
/// cooked asset the importer wrote for it.
fn mesh_path(reference: &str) -> String {
    if Path::new(reference)
        .extension()
        .is_some_and(|extension| extension == "cyprim")
    {
        reference.to_string()
    } else {
        format!("{COOKED_DIRECTORY}/{reference}.cyasset")
    }
}

/// The material an object draws its first section with, as the frame resolves it: its imported
/// slot 0, else the renderer's material; and the renderer's tint.
fn drawn_material(document: &Document, node: NodeId) -> (String, [f32; 3]) {
    let slot = MaterialSlotsBinding::of_schema(document.schema())
        .and_then(|binding| {
            let first = *binding.slots.first()?;
            document.content().field(node, binding.component, first)
        })
        .and_then(Value::as_text)
        .filter(|reference| !reference.is_empty());
    let primary = MaterialBinding::of_schema(document.schema())
        .and_then(|binding| {
            document
                .content()
                .field(node, binding.component, binding.material)
        })
        .and_then(Value::as_text);
    let reference = slot.or(primary).unwrap_or_default().to_string();
    let tint = field(document, node, MeshBinding::COMPONENT, "tint")
        .and_then(Value::as_vec3)
        .unwrap_or([1.0; 3]);
    (reference, tint)
}

/// The `material` line a drawn material and tint bake as.
fn material_line(name: &str, reference: &str, tint: [f32; 3]) -> String {
    let graph = Path::new(reference)
        .extension()
        .is_some_and(|extension| extension == "cygraph");
    if reference.is_empty() || graph {
        // The frame's default surface: `apply_standard_defaults`' grey, under the tint.
        return format!(
            "material {} {}",
            quoted(name),
            numbers(&tint.map(|lane| 0.5 * lane))
        );
    }
    let mut line = format!(
        "material {} cooked {}",
        quoted(name),
        quoted(&format!("{COOKED_DIRECTORY}/{reference}.cyasset"))
    );
    // Compared by bits: a tint the author typed as exactly one is the untinted material.
    if tint.map(f32::to_bits) != [1.0_f32; 3].map(f32::to_bits) {
        let _ = write!(line, " tint {}", numbers(&tint));
    }
    line
}

struct Instances {
    materials: Vec<String>,
    lines: Vec<String>,
}

fn instance_lines(
    document: &Document,
    binding: Option<TransformBinding>,
    scene: &LightingScene,
) -> Instances {
    let mut names: BTreeMap<(String, [u32; 3]), String> = BTreeMap::new();
    let mut out = Instances {
        materials: Vec::new(),
        lines: Vec::new(),
    };
    let Some(mesh) = MeshBinding::of_schema(document.schema()) else {
        return out;
    };
    for node in with_component(document, MeshBinding::COMPONENT) {
        let Some(reference) = document
            .content()
            .field(node, mesh.component, mesh.mesh)
            .and_then(Value::as_text)
            .filter(|reference| !reference.is_empty())
        else {
            continue;
        };
        let (material, tint) = drawn_material(document, node);
        let key = (material.clone(), tint.map(f32::to_bits));
        let next = names.len();
        let name = names
            .entry(key)
            .or_insert_with(|| {
                let name = format!("m{next}");
                out.materials.push(material_line(&name, &material, tint));
                name
            })
            .clone();
        let matrix = world(document, binding, node);
        let rows: Vec<f32> = matrix.iter().flatten().copied().collect();
        let mut line = format!(
            "instance {} \"mesh\" {} {} {} id {}",
            quoted(&mesh_path(reference)),
            quoted(&name),
            number(scene.resolution_of(node)),
            numbers(&rows),
            engine_identity(node)
        );
        let receives = scene
            .objects
            .iter()
            .find(|object| object.id == node)
            .is_none_or(|object| object.receives);
        if !receives {
            line.push_str(" occluder");
        }
        out.lines.push(line);
    }
    out
}

fn volume_lines(
    document: &Document,
    binding: Option<TransformBinding>,
    scene: &LightingScene,
) -> Vec<String> {
    let mut volumes = scene.volumes.clone();
    volumes.sort_by_key(|volume| engine_identity(volume.id));
    volumes
        .iter()
        .map(|volume| {
            let matrix = world(document, binding, volume.id);
            format!(
                "volume {} {} {} {} {}",
                engine_identity(volume.id),
                numbers(&[matrix[0][3], matrix[1][3], matrix[2][3]]),
                number(volume.spacing),
                volume.counts.map(|count| count.to_string()).join(" "),
                volume.rays
            )
        })
        .collect()
}

/// The description of `document`, the world saved at `world` (project-relative), baked with
/// `settings`.
#[must_use]
pub fn write(document: &Document, world: &str, settings: &LevelSettings) -> String {
    let binding = TransformBinding::of_schema(document.schema());
    let scene = LightingScene::read(document);
    let mut text = String::from("cylightmap 1\n");
    let _ = writeln!(
        text,
        "# Written from {world} by the editor on every bake request: an edit here is replaced."
    );
    let _ = writeln!(text, "mode {}", settings.mode);
    let _ = writeln!(text, "bounces {}", settings.bounces);
    let _ = writeln!(text, "samples {}", settings.samples);
    let _ = writeln!(text, "density {}", number(settings.density));
    let _ = writeln!(text, "page {}", settings.page);
    let instances = instance_lines(document, binding, &scene);
    let lights = with_component(document, LIGHT)
        .into_iter()
        .filter_map(|node| light_line(document, binding, &scene, node));
    for line in instances
        .materials
        .iter()
        .cloned()
        .chain(lights)
        .chain(instances.lines)
        .chain(volume_lines(document, binding, &scene))
    {
        text.push_str(&line);
        text.push('\n');
    }
    text
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;

    use super::*;
    use crate::primitives::MaterialSlotsBinding;

    /// A placed node, with `components` of named fields, under `parent`.
    fn node(
        document: &mut Document,
        parent: Option<NodeId>,
        at: [f32; 3],
        rotation: [f32; 4],
        components: &[(&str, &[(&str, Value)])],
    ) -> NodeId {
        document
            .with_transaction("place", Actor::human("test"), |document| {
                let node = document.create_node(parent)?;
                crate::scene_actors::place(document, node, at, rotation)?;
                for (component, fields) in components {
                    let component = match document.schema().type_named(component) {
                        Some(found) => found.id,
                        None => document.schema_mut().declare_type(*component, false),
                    };
                    let mut values = Vec::new();
                    for (name, value) in *fields {
                        let schema = document.schema_mut();
                        let field = match schema
                            .type_of(component)
                            .and_then(|definition| definition.field_named(name))
                        {
                            Some(found) => found.id,
                            None => schema
                                .declare_field(component, *name, value.kind(), "a test field")
                                .unwrap(),
                        };
                        values.push((field, value.clone()));
                    }
                    document.add_component(node, component, values)?;
                }
                Ok(node)
            })
            .unwrap()
    }

    fn mesh(reference: &str, material: &str) -> [(&'static str, Value); 2] {
        [
            ("mesh", Value::Text(reference.into())),
            ("material", Value::Text(material.into())),
        ]
    }

    fn lines<'text>(text: &'text str, start: &str) -> Vec<&'text str> {
        text.lines()
            .filter(|line| line.starts_with(start))
            .collect()
    }

    #[test]
    fn a_child_is_written_where_its_parents_put_it() {
        let mut document = Document::new("levels/nested.cyworld");
        // A quarter turn about +y, then one metre up: the child's +x offset lands at -z.
        let turn = [
            0.0,
            std::f32::consts::FRAC_1_SQRT_2,
            0.0,
            std::f32::consts::FRAC_1_SQRT_2,
        ];
        let parent = node(&mut document, None, [0.0, 1.0, 0.0], turn, &[]);
        let mesh = mesh("meshes/box.cyprim", "");
        node(
            &mut document,
            Some(parent),
            [2.0, 0.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
            &[(MeshBinding::COMPONENT, &mesh)],
        );
        let text = write(
            &document,
            "levels/nested.cyworld",
            &LevelSettings::default(),
        );
        let instance = lines(&text, "instance ");
        assert_eq!(instance.len(), 1, "{text}");
        let numbers: Vec<f32> = instance[0]
            .split_whitespace()
            .skip(5)
            .take(12)
            .map(|word| word.parse().unwrap())
            .collect();
        let translation = [numbers[3], numbers[7], numbers[11]];
        let expected = [0.0, 1.0, -2.0];
        for (got, want) in translation.iter().zip(expected) {
            assert!(
                (got - want).abs() < 1.0e-5,
                "{translation:?} is not {expected:?}"
            );
        }
    }

    #[test]
    fn an_object_bakes_with_the_material_it_draws_and_its_tint() {
        let mut document = Document::new("levels/materials.cyworld");
        let plain = mesh("meshes/a.cyprim", "abc");
        node(
            &mut document,
            None,
            [0.0; 3],
            [0.0, 0.0, 0.0, 1.0],
            &[(MeshBinding::COMPONENT, &plain)],
        );
        let tinted = [
            ("mesh", Value::Text("meshes/b.cyprim".into())),
            ("material", Value::Text("abc".into())),
            ("tint", Value::Vec3([0.5, 1.0, 0.25])),
        ];
        node(
            &mut document,
            None,
            [1.0, 0.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
            &[(MeshBinding::COMPONENT, &tinted)],
        );
        // An imported slot wins over the renderer's material, as the frame draws it.
        let slotted = [("slot_0", Value::Text("def".into()))];
        let unslotted = mesh("meshes/c.cyprim", "abc");
        node(
            &mut document,
            None,
            [2.0, 0.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
            &[
                (MeshBinding::COMPONENT, &unslotted),
                (MaterialSlotsBinding::COMPONENT, &slotted),
            ],
        );
        let graph = mesh("meshes/d.cyprim", "materials/glow.cygraph");
        node(
            &mut document,
            None,
            [3.0, 0.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
            &[(MeshBinding::COMPONENT, &graph)],
        );
        let text = write(
            &document,
            "levels/materials.cyworld",
            &LevelSettings::default(),
        );
        let materials = lines(&text, "material ");
        assert!(
            materials
                .iter()
                .any(|line| line.ends_with("cooked \".cy/cooked/abc.cyasset\"")),
            "{text}"
        );
        assert!(
            materials
                .iter()
                .any(|line| line.ends_with("cooked \".cy/cooked/abc.cyasset\" tint 0.5 1 0.25")),
            "{text}"
        );
        assert!(
            materials
                .iter()
                .any(|line| line.ends_with("cooked \".cy/cooked/def.cyasset\"")),
            "{text}"
        );
        // A material graph bakes with the frame's default grey.
        assert!(
            materials.iter().any(|line| line.ends_with(" 0.5 0.5 0.5")),
            "{text}"
        );
        assert_eq!(materials.len(), 4, "{text}");
        // Every instance names a declared material.
        for instance in lines(&text, "instance ") {
            let name = instance.split_whitespace().nth(3).unwrap();
            assert!(
                materials
                    .iter()
                    .any(|line| line.contains(&format!(" {name} "))),
                "{instance} names an undeclared material"
            );
        }
    }

    #[test]
    fn a_disabled_light_is_not_baked_and_a_spot_bakes_as_a_point() {
        let mut document = Document::new("levels/lights.cyworld");
        let light = |kind: i64, enabled: bool| {
            [
                ("kind", Value::Int(kind)),
                ("intensity", Value::Float(50.0)),
                ("range", Value::Float(8.0)),
                ("enabled", Value::Bool(enabled)),
            ]
        };
        let off = light(1, false);
        let spot = light(2, true);
        node(
            &mut document,
            None,
            [0.0; 3],
            [0.0, 0.0, 0.0, 1.0],
            &[(LIGHT, &off)],
        );
        node(
            &mut document,
            None,
            [0.0, 3.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
            &[(LIGHT, &spot)],
        );
        let text = write(
            &document,
            "levels/lights.cyworld",
            &LevelSettings::default(),
        );
        let lights = lines(&text, "light ");
        assert_eq!(lights.len(), 1, "{text}");
        assert!(
            lights[0].starts_with("light point 0 3 0 50 8 1 1 1 stationary id "),
            "{}",
            lights[0]
        );
    }

    #[test]
    fn the_settings_are_refused_where_the_engine_would_refuse_them() {
        let refused = |name: &str, value: Value| {
            LevelSettings::from_arguments(&Arguments::new().with(name, value)).is_err()
        };
        assert!(refused("mode", Value::Text("lumens".into())));
        assert!(refused("samples", Value::Int(0)));
        assert!(refused("samples", Value::Int(4097)));
        assert!(refused("bounces", Value::Int(9)));
        assert!(refused("density", Value::Float(0.0)));
        assert!(refused("page", Value::Int(300)));
        assert!(refused("page", Value::Int(64)));
        let chosen = LevelSettings::from_arguments(
            &Arguments::new()
                .with("mode", Value::Text("sh-l1".into()))
                .with("page", Value::Int(1024)),
        )
        .unwrap();
        assert_eq!((chosen.mode, chosen.page), ("sh-l1", 1024));
    }

    #[test]
    fn a_description_lives_beside_its_world() {
        assert_eq!(
            description_path("game/worlds/lit.cyworld"),
            "game/worlds/lit.cylightmap"
        );
    }
}
