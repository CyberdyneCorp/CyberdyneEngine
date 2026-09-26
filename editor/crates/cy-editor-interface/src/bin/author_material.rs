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
//! `docs/design/beauty-shot.md` says so in the artefact's own provenance. The registry has since
//! gained `material.graph.{read,preview,save,status}`, which carry a WHOLE canvas as text over the
//! control socket; none of them places a node or wires a pin, so the node-by-node authoring this
//! program does is still not something a socket client can do, and the provenance says that too. A
//! claim of "authored in the editor's window" would be one nobody could check; this one is exactly as
//! strong as what it did.
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
///
/// THESE ARE ART DIRECTION AND THEY ARE AUTHORED, which is the distinction the artefact's provenance
/// turns on. `base_color` multiplies the albedo texture: limestone at 0.62 and damp sand at 0.47 are
/// the reflectances those materials actually have, and the first draft's 0.93 and 0.98 were the
/// reason the floor clipped to white under a sky whose mean radiance the engine's own atmosphere put
/// at 2 788. Nothing in the renderer was changed to fix that; the material was.
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
        base_color: [0.62, 0.59, 0.53],
        roughness: 1.0,
        metallic: 1.0,
    },
    Recipe {
        name: "oxidised_copper",
        base_color: [0.95, 0.92, 0.88],
        roughness: 0.78,
        metallic: 1.0,
    },
    Recipe {
        name: "courtyard_gravel",
        base_color: [0.47, 0.43, 0.37],
        roughness: 1.05,
        metallic: 1.0,
    },
];

fn triple(values: [f32; 3]) -> String {
    format!("{} {} {} 0", values[0], values[1], values[2])
}

/// Place one node and set its properties, which is every line of `place_nodes` below.
fn place(
    material: &mut MaterialAuthoring<'_>,
    kind: &str,
    properties: &[(&str, String)],
) -> Result<NodeKey, String> {
    let key = material.node(kind).map_err(|p| p.to_string())?;
    for (name, value) in properties {
        material
            .set(key, name, value.clone())
            .map_err(|p| p.to_string())?;
    }
    Ok(key)
}

/// Every node of the authored graph, named so `wire_nodes` reads as the picture it draws.
struct Nodes {
    uv: NodeKey,
    albedo_sample: NodeKey,
    data_sample: NodeKey,
    albedo_rgb: NodeKey,
    base_color: NodeKey,
    base: NodeKey,
    rough_channel: NodeKey,
    roughness_param: NodeKey,
    roughness: NodeKey,
    metal_channel: NodeKey,
    metallic_param: NodeKey,
    metalness: NodeKey,
    dielectric: NodeKey,
    diffuse_colour: NodeKey,
    diffuse: NodeKey,
    specular_colour: NodeKey,
    specular: NodeKey,
    sum: NodeKey,
    opacity: NodeKey,
    output: NodeKey,
}

/// Drop every node on the canvas. Placement and wiring are separate because the wire list IS the
/// graph's shape and reads as one, and because together they are one function clippy refuses.
fn place_nodes(recipe: &Recipe, material: &mut MaterialAuthoring<'_>) -> Result<Nodes, String> {
    // --- The two texture samples, and the attribute both read -------------------------------------
    let uv = place(
        material,
        "material.attribute",
        &[("symbol", "uv0".to_owned()), ("type", "float2".to_owned())],
    )?;
    let albedo_sample = place(
        material,
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
        material,
        "material.texture_sample",
        &[
            ("symbol", "data_map".to_owned()),
            ("type", "float4".to_owned()),
            ("average", "0.7 1 0 1".to_owned()),
        ],
    )?;

    // --- The base colour ---------------------------------------------------------------------------
    let albedo_rgb = place(
        material,
        "material.swizzle",
        &[("swizzle", "xyz".to_owned())],
    )?;
    let base_color = place(
        material,
        "material.parameter",
        &[
            ("symbol", "base_color".to_owned()),
            ("type", "float3".to_owned()),
            ("default", triple(recipe.base_color)),
        ],
    )?;
    let base = place(material, "material.multiply", &[])?;

    // --- Roughness and metalness, out of the packed data map ---------------------------------------
    let rough_channel = place(material, "material.swizzle", &[("swizzle", "x".to_owned())])?;
    let roughness_param = place(
        material,
        "material.parameter",
        &[
            ("symbol", "roughness".to_owned()),
            ("type", "float".to_owned()),
            ("default", format!("{} 0 0 0", recipe.roughness)),
        ],
    )?;
    let roughness = place(material, "material.multiply", &[])?;

    let metal_channel = place(material, "material.swizzle", &[("swizzle", "z".to_owned())])?;
    let metallic_param = place(
        material,
        "material.parameter",
        &[
            ("symbol", "metallic".to_owned()),
            ("type", "float".to_owned()),
            ("default", format!("{} 0 0 0", recipe.metallic)),
        ],
    )?;
    let metalness = place(material, "material.multiply", &[])?;

    // --- The two closures --------------------------------------------------------------------------
    //
    // `cyResolveSurface` reconstructs the engine's `Surface` from the closure sums: the albedo is
    // diffuse + specular and the METALNESS is the specular term's luminance. So the split below is
    // not an aesthetic choice — it is the only way a compiled material can say "metal" at all, and
    // `docs/design/beauty-shot.md` publishes what it costs.
    let dielectric = place(material, "material.one_minus", &[])?;
    let diffuse_colour = place(material, "material.multiply", &[])?;
    let diffuse = place(material, "material.diffuse", &[])?;
    let specular_colour = place(material, "material.multiply", &[])?;
    let specular = place(material, "material.specular", &[])?;
    let sum = place(material, "material.add_closures", &[])?;

    let opacity = place(
        material,
        "material.constant",
        &[
            ("type", "float".to_owned()),
            ("value", "1 0 0 0".to_owned()),
        ],
    )?;
    let output = place(material, "material.output", &[])?;
    Ok(Nodes {
        uv,
        albedo_sample,
        data_sample,
        albedo_rgb,
        base_color,
        base,
        rough_channel,
        roughness_param,
        roughness,
        metal_channel,
        metallic_param,
        metalness,
        dielectric,
        diffuse_colour,
        diffuse,
        specular_colour,
        specular,
        sum,
        opacity,
        output,
    })
}

/// The wires, which are the authored graph's shape written out once.
fn wire_nodes(material: &mut MaterialAuthoring<'_>, n: &Nodes) -> Result<(), String> {
    let wires: &[(NodeKey, NodeKey, &str)] = &[
        (n.uv, n.albedo_sample, "uv"),
        (n.uv, n.data_sample, "uv"),
        (n.albedo_sample, n.albedo_rgb, "value"),
        (n.albedo_rgb, n.base, "a"),
        (n.base_color, n.base, "b"),
        (n.data_sample, n.rough_channel, "value"),
        (n.rough_channel, n.roughness, "a"),
        (n.roughness_param, n.roughness, "b"),
        (n.data_sample, n.metal_channel, "value"),
        (n.metal_channel, n.metalness, "a"),
        (n.metallic_param, n.metalness, "b"),
        (n.metalness, n.dielectric, "value"),
        (n.base, n.diffuse_colour, "a"),
        (n.dielectric, n.diffuse_colour, "b"),
        (n.diffuse_colour, n.diffuse, "colour"),
        (n.base, n.specular_colour, "a"),
        (n.metalness, n.specular_colour, "b"),
        (n.specular_colour, n.specular, "colour"),
        (n.roughness, n.specular, "roughness"),
        (n.diffuse, n.sum, "a"),
        (n.specular, n.sum, "b"),
        (n.sum, n.output, "surface"),
        (n.opacity, n.output, "opacity"),
    ];
    for (from, to, pin) in wires {
        material
            .wire(*from, *to, pin)
            .map_err(|problem| problem.to_string())?;
    }
    Ok(())
}

/// Author one material on the canvas and return its interchange.
///
/// THE GRAPH IS SHAPED LIKE AN AUTHORED ONE and not like the IR it lowers to. Every closure node
/// carries an untouched weight port, the two closures are summed pairwise, the base colour reaches
/// both the diffuse and the specular branch through the same multiply, and the metalness split is
/// two nodes rather than a `lerp` — because that is what dragging boxes around produces, and
/// `graph.h` is explicit that the front end must produce it so the compiler has something to remove.
fn author(recipe: &Recipe, editors: &mut SpecialisedEditors) -> Result<String, String> {
    // CLOSED FIRST, AND THE CLOSE IS THE WHOLE OF THIS LINE'S REASON. Re-opening the domain that is
    // ALREADY active deliberately preserves what is on the canvas — a failed or repeated open that
    // emptied the region would lose an author's work to a mis-click, and
    // `reopening_the_active_material_editor_preserves_authored_nodes` asserts it. This program
    // authors three materials in one process, so without the close the second material is authored
    // on top of the first: 20 nodes, then 40, then 60, and the committed canvases stop matching what
    // the editor produces. Closing between materials is what a person does between two materials,
    // and it is what makes each one's ordinals start at 1 the way the committed files do.
    editors.close();
    let session = editors
        .open(Domain::Materials)
        .map_err(|problem| problem.to_string())?;
    let canvas = session
        .graph
        .ok_or_else(|| "the material editor is a graph editor".to_owned())?;
    let mut material =
        MaterialAuthoring::begin(recipe.name, canvas).map_err(|problem| problem.to_string())?;

    let nodes = place_nodes(recipe, &mut material)?;
    wire_nodes(&mut material, &nodes)?;
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

    let mut editors = match SpecialisedEditors::with_legacy_material_catalogue() {
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
                let nodes = text
                    .lines()
                    .filter(|line| line.starts_with("node "))
                    .count();
                let links = text
                    .lines()
                    .filter(|line| line.starts_with("link "))
                    .count();
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

#[cfg(test)]
mod tests {
    use super::{RECIPES, SpecialisedEditors, author};

    /// The committed canvases are this program's output, byte for byte, checked in the editor's own
    /// suite. `m11c:shot-authored-through-the-editor` checks the same bytes, but only in the milestone
    /// job; `ec73658` added a `# layout` line per node to the interchange and the committed canvases
    /// went stale unnoticed until the next ledger. Here a change to what the canvas writes fails the
    /// pull request that makes it.
    #[test]
    fn the_committed_beauty_canvases_are_what_the_editor_writes() {
        let committed = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../content/beauty/materials");
        let mut editors = SpecialisedEditors::with_legacy_material_catalogue()
            .expect("the legacy material catalogue builds");
        for recipe in &RECIPES {
            let written = author(recipe, &mut editors).expect("the recipe authors on the canvas");
            let path = committed.join(format!("{}.cymatcanvas", recipe.name));
            let on_disk = std::fs::read_to_string(&path)
                .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
            assert!(
                written == on_disk,
                "{} is not what the editor's canvas writes; regenerate it with \
                 `cy-author-material content/beauty/materials`",
                path.display()
            );
        }
    }
}
