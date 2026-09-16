//! `cy-author-material` — the beauty shot's materials, authored on the editor's own canvas.
//! M11.c tasks 6.1a and 7.1.
//!
//! --- WHY THIS PROGRAM EXISTS AND WHAT IT IS NOT ----------------------------------------------------
//!
//! M11.c's design says the thing this rung is FOR in one sentence:
//!
//! > A shot assembled by hand in C++ proves the renderer and nothing else. One authored *through* the
//! > editor proves the renderer and the editor at once.
//!
//! So the three materials in `docs/design/images/m11c-beauty-shot.png` are not typed into a C++
//! builder. They are placed and wired on `cy_editor_interface::specialised::graph::GraphCanvas` — THE
//! canvas, the one `editor-architecture` forbids a sixth bespoke graph editor beside — opened through
//! `SpecialisedEditors::open(Domain::Materials)`, which **refused to open at all** until this rung:
//! M11.c's spike measured that refusal and recorded it as junction 1 of six.
//!
//! **What this is NOT is a person at a window.** The editor's window, its command registry and its
//! agent surface are not driven here: this is the editor's authoring MODEL, exercised by a program.
//! `docs/design/beauty-shot.md` says so in the artefact's own provenance, and `m11c.toml` declares the
//! remaining half — `material.*` commands on the editor's registry, so the same three materials can
//! be authored over the control socket the way `samples/08a-authoring` drives a scene — as a gap with
//! the rung that owes it. A claim of "authored in the editor's window" would be one nobody could
//! check; this one is exactly as strong as what it did.
//!
//! --- WHAT IT WRITES --------------------------------------------------------------------------------
//!
//! One interchange file per material, which `cy_material author` canonicalises into the committed
//! `.cygraph`. The editor does not write the canonical form — `specialised/graph.rs` assigns that to
//! M11.e and gives the reason: a second writer of a canonical format is a second format the day the
//! two disagree about a float.
//!
//! ```text
//! cy-author-material content/beauty/materials
//! ```

use std::path::PathBuf;

use cy_editor_interface::specialised::graph::NodeKey;
use cy_editor_interface::specialised::material::MaterialAuthoring;
use cy_editor_interface::specialised::{Domain, SpecialisedEditors};

/// The parameter signature every material in the shot shares, in this order.
///
/// IT IS THE SAME IN ALL THREE ON PURPOSE. The generated prelude lays `CyMaterialParams` out in the
/// module's own declaration order, and the frame uploads that block from one C++ struct. Three
/// materials with three different parameter lists would be three structs, three uploads and three
/// chances for an offset to be wrong in a way only a picture shows. `cy_material author` writes the
/// signature it actually produced into a sidecar and `cy_sample_beauty` refuses one that does not
/// match, so this agreement is checked rather than assumed.
const PARAMETERS: [(&str, &str); 3] = [
    ("base_color", "float3"),
    ("roughness", "float"),
    ("metallic", "float"),
];

/// What one material's authored constants are.
struct Recipe {
    name: &'static str,
    /// The albedo tint, multiplied over the base colour texture.
    base_color: [f32; 3],
    /// A multiplier over the roughness channel of the data texture.
    roughness: f32,
    /// A multiplier over the metalness channel of the data texture.
    metallic: f32,
}

const RECIPES: [Recipe; 3] = [
    Recipe {
        name: "weathered_stone",
        base_color: [0.93, 0.90, 0.84],
        roughness: 1.0,
        metallic: 1.0,
    },
    Recipe {
        name: "oxidised_copper",
        base_color: [1.0, 0.98, 0.95],
        roughness: 0.92,
        metallic: 1.0,
    },
    Recipe {
        name: "courtyard_gravel",
        base_color: [0.98, 0.95, 0.90],
        roughness: 1.05,
        metallic: 1.0,
    },
];

fn triple(values: [f32; 3]) -> String {
    format!("{} {} {} 0", values[0], values[1], values[2])
}

/// Author one material on the canvas and return its interchange.
///
/// THE GRAPH IS SHAPED LIKE AN AUTHORED ONE and not like the IR it lowers to. Every closure node
/// carries an untouched weight port, the two closures are summed pairwise, the base colour reaches
/// both the diffuse and the specular branch through the same multiply, and the metalness split is
/// two nodes rather than a `lerp` — because that is what dragging boxes around produces, and
/// `graph.h` is explicit that the front end must produce it so the compiler has something to remove.
fn author(recipe: &Recipe, editors: &mut SpecialisedEditors) -> Result<String, String> {
    let session = editors
        .open(Domain::Materials)
        .map_err(|problem| problem.to_string())?;
    let canvas = session
        .graph
        .ok_or_else(|| "the material editor is a graph editor".to_owned())?;
    let mut material =
        MaterialAuthoring::begin(recipe.name, canvas).map_err(|problem| problem.to_string())?;

    let place = |material: &mut MaterialAuthoring<'_>,
                     kind: &str,
                     properties: &[(&str, String)]|
     -> Result<NodeKey, String> {
        let key = material.node(kind).map_err(|p| p.to_string())?;
        for (name, value) in properties {
            material
                .set(key, name, value.clone())
                .map_err(|p| p.to_string())?;
        }
        Ok(key)
    };

    // --- The two texture samples, and the attribute both read -------------------------------------
    let uv = place(
        &mut material,
        "material.attribute",
        &[
            ("symbol", "uv0".to_owned()),
            ("type", "float2".to_owned()),
        ],
    )?;
    let albedo_sample = place(
        &mut material,
        "material.texture_sample",
        &[
            ("symbol", "albedo_map".to_owned()),
            ("type", "float4".to_owned()),
            // The far-field program substitutes this for the sample. A mid grey rather than white:
            // a material that turned white at a kilometre is the artefact `material-compiler`'s own
            // "derivation changes albedo" diagnostic exists to catch.
            ("average", "0.5 0.48 0.44 1".to_owned()),
        ],
    )?;
    let data_sample = place(
        &mut material,
        "material.texture_sample",
        &[
            ("symbol", "data_map".to_owned()),
            ("type", "float4".to_owned()),
            ("average", "0.7 1 0 1".to_owned()),
        ],
    )?;

    // --- The base colour ---------------------------------------------------------------------------
    let albedo_rgb = place(
        &mut material,
        "material.swizzle",
        &[("swizzle", "xyz".to_owned())],
    )?;
    let base_color = place(
        &mut material,
        "material.parameter",
        &[
            ("symbol", "base_color".to_owned()),
            ("type", "float3".to_owned()),
            ("default", triple(recipe.base_color)),
        ],
    )?;
    let base = place(&mut material, "material.multiply", &[])?;

    // --- Roughness and metalness, out of the packed data map ---------------------------------------
    let rough_channel = place(
        &mut material,
        "material.swizzle",
        &[("swizzle", "x".to_owned())],
    )?;
    let roughness_param = place(
        &mut material,
        "material.parameter",
        &[
            ("symbol", "roughness".to_owned()),
            ("type", "float".to_owned()),
            ("default", format!("{} 0 0 0", recipe.roughness)),
        ],
    )?;
    let roughness = place(&mut material, "material.multiply", &[])?;

    let metal_channel = place(
        &mut material,
        "material.swizzle",
        &[("swizzle", "z".to_owned())],
    )?;
    let metallic_param = place(
        &mut material,
        "material.parameter",
        &[
            ("symbol", "metallic".to_owned()),
            ("type", "float".to_owned()),
            ("default", format!("{} 0 0 0", recipe.metallic)),
        ],
    )?;
    let metalness = place(&mut material, "material.multiply", &[])?;

    // --- The two closures --------------------------------------------------------------------------
    //
    // `cyResolveSurface` reconstructs the engine's `Surface` from the closure sums: the albedo is
    // diffuse + specular and the METALNESS is the specular term's luminance. So the split below is
    // not an aesthetic choice — it is the only way a compiled material can say "metal" at all, and
    // `docs/design/beauty-shot.md` publishes what it costs.
    let dielectric = place(&mut material, "material.one_minus", &[])?;
    let diffuse_colour = place(&mut material, "material.multiply", &[])?;
    let diffuse = place(&mut material, "material.diffuse", &[])?;
    let specular_colour = place(&mut material, "material.multiply", &[])?;
    let specular = place(&mut material, "material.specular", &[])?;
    let sum = place(&mut material, "material.add_closures", &[])?;

    let opacity = place(
        &mut material,
        "material.constant",
        &[("type", "float".to_owned()), ("value", "1 0 0 0".to_owned())],
    )?;
    let output = place(&mut material, "material.output", &[])?;

    // --- The wires ---------------------------------------------------------------------------------
    let wires: &[(NodeKey, NodeKey, &str)] = &[
        (uv, albedo_sample, "uv"),
        (uv, data_sample, "uv"),
        (albedo_sample, albedo_rgb, "value"),
        (albedo_rgb, base, "a"),
        (base_color, base, "b"),
        (data_sample, rough_channel, "value"),
        (rough_channel, roughness, "a"),
        (roughness_param, roughness, "b"),
        (data_sample, metal_channel, "value"),
        (metal_channel, metalness, "a"),
        (metallic_param, metalness, "b"),
        (metalness, dielectric, "value"),
        (base, diffuse_colour, "a"),
        (dielectric, diffuse_colour, "b"),
        (diffuse_colour, diffuse, "colour"),
        (base, specular_colour, "a"),
        (metalness, specular_colour, "b"),
        (specular_colour, specular, "colour"),
        (roughness, specular, "roughness"),
        (diffuse, sum, "a"),
        (specular, sum, "b"),
        (sum, output, "surface"),
        (opacity, output, "opacity"),
    ];
    for (from, to, pin) in wires {
        material
            .wire(*from, *to, pin)
            .map_err(|problem| problem.to_string())?;
    }
    Ok(material.interchange())
}

fn main() -> std::process::ExitCode {
    let mut arguments = std::env::args().skip(1);
    let Some(directory) = arguments.next().map(PathBuf::from) else {
        eprintln!("usage: cy-author-material <output directory>");
        return std::process::ExitCode::from(2);
    };
    if let Err(error) = std::fs::create_dir_all(&directory) {
        eprintln!("cy-author-material: {}: {error}", directory.display());
        return std::process::ExitCode::from(2);
    }

    let mut editors = match SpecialisedEditors::new() {
        Ok(editors) => editors,
        Err(problem) => {
            eprintln!("cy-author-material: {problem}");
            return std::process::ExitCode::from(2);
        }
    };
    // The refusal this rung removed, checked before anything is written: an editor that cannot open
    // the materials domain would otherwise fail one call later with a less useful message.
    if !editors.can_open(Domain::Materials) {
        eprintln!(
            "cy-author-material: this build declares no authoring vocabulary for materials, so \
             there is no canvas to author on"
        );
        return std::process::ExitCode::from(1);
    }

    println!("parameter signature: {PARAMETERS:?}");
    for recipe in &RECIPES {
        match author(recipe, &mut editors) {
            Ok(text) => {
                let path = directory.join(format!("{}.cymatcanvas", recipe.name));
                if let Err(error) = std::fs::write(&path, &text) {
                    eprintln!("cy-author-material: {}: {error}", path.display());
                    return std::process::ExitCode::from(2);
                }
                let nodes = text.lines().filter(|line| line.starts_with("node ")).count();
                let links = text.lines().filter(|line| line.starts_with("link ")).count();
                println!("{}  {nodes} nodes, {links} wires", path.display());
            }
            Err(problem) => {
                eprintln!("cy-author-material: {}: {problem}", recipe.name);
                return std::process::ExitCode::from(1);
            }
        }
    }
    std::process::ExitCode::SUCCESS
}
