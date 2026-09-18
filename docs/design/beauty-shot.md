# The Colonnade — M11.c's beauty shot, and what is in it

> `just capture-beauty-shot --video` regenerates every file on this page from committed content.
> The recipe's five steps are the five junctions M11.c's spike measured: author, compile, shader,
> cook-and-bind, capture.

![A colonnade of weathered limestone pillars on plinths, lit by a low sun from the left. Their
shadows fall across a gravel courtyard. A copper monolith stands on a plinth at the far end and a
copper sphere sits in the foreground, both showing verdigris and a specular
highlight](images/m11c-beauty-shot.png)

**This is the first frame this engine has drawn in which a material decided a pixel.** Every
published capture before it shades from constants or from a debug channel, and M11.c's own ledger
opens with the reason: *"outside `docs/`, the tree holds six image files, four of them editor
identity marks and two of them golden references. There is no albedo, normal, roughness or mask
texture anywhere."* There are nine now, and this is what they are for.

---

## The provenance

Everything in this section is read off
[`m11c-beauty-shot.manifest`](images/m11c-beauty-shot.manifest), which the capture writes from the
frame's OWN report — `cy::rendering::assembly::capture_manifest` copies the stage list out of
`AssemblyReport` and re-derives nothing. A caption that listed a stage the frame did not run is what
that function exists to make impossible.

| | |
|---|---|
| **Rendered at** | 3840 x 2160, box-filtered to 1920 x 1080 |
| **Exposure** | −11.45 stops, committed in `content/beauty/shot.cyshot` |
| **Tone curve** | AgX, `rendering-post-processing` step 11 |
| **Post chain** | three stages: ExposureApply, Tonemap, OutputEncoding |
| **Anti-aliasing** | **none.** 2x supersampling, which is not the same thing — see below |
| **Passes declared** | 8, by `cy::rendering::assembly::FrameAssembly` |
| **Arbiter** | pinned; no arbiter runs in this program, so nothing could degrade the frame between assembling it and photographing it |
| **Validation errors** | 0 |
| **Cost** | 344 ms to build the scene on the processor (124 ms of it the sky), 12.5 ms of device submit |

### What a person authored

| | |
|---|---|
| **Three materials** | authored as node graphs on the editor's own canvas, `cy_editor_interface::specialised::graph::GraphCanvas` — twenty nodes and twenty-three wires each. `content/beauty/materials/*.cygraph` is the committed result |
| **Nine textures** | 256 x 256 each, generated from a seed by `tools/content/make_beauty_textures.py`. The project's own; see [the provenance record](../../content/beauty/textures/PROVENANCE.md) |
| **Six meshes** | `.cyprim` sources — a plane, a cylinder, three boxes and a sphere — built by the engine's own `build_primitive_mesh` |
| **The scene** | `content/beauty/shot.cyshot`: thirty-one instances, the camera, the sun's elevation and azimuth, the shadow extent and the exposure |

### What the renderer produced

| | |
|---|---|
| **The material programs** | `lower_material` → `lower_graph` → `compile_material` → the emitter's Slang → `slangc`. Cook keys `0xd10b8e4404ad56c5`, `0x08f3975a2338abc4`, `0x159982f6f14a706e` |
| **The cooked textures** | `cy::import::TextureImporter`: PNG in, **BC7 and BC5 blocks with a nine-level mip chain** out. 899 346 bytes of PNG became 786 852 bytes of blocks, and the blocks are what the device holds |
| **The sky** | `rendering::sky::compose_sky` over a 96 x 192 dome — the engine's atmosphere, its multiple-scattering table and its volumetric cloud march, 18 432 evaluations on the processor |
| **The light** | `compose_sky_lighting` answered 102 333 / 76 288 / 47 310 lux of sun and 4 422 / 3 715 / 3 385 of mean sky radiance for this sun elevation. **No colour was typed anywhere**; a colour in the shot file would have been a second sun |
| **The shadows** | one 2048 x 2048 directional shadow map, 3x3 percentage-closer filtered |
| **The frame** | `FrameAssembly`, and `cy::rendering-pipeline`'s tonemapping resolve — `cy/fullscreen.slang`'s own, the same one every other caller of the layer binds |

---

## What it is not

design.md §7: *"a beautiful picture with an unstated provenance is the most efficient way to make
this whole record dishonest."* So:

- **There is no anti-aliasing stage.** `FramePassKind::Temporal` is declared by the frame and nothing
  in this tree records it — there is no temporal resolve shader under `src/rendering/` at all. The
  frame is drawn at twice the published resolution and box-filtered down, which is supersampling.
  The manifest says `post-stages 3` and names all three; none of them is temporal.
- **There is no global illumination and no ambient occlusion pass.** `cy::rendering-gi` is not linked
  by this program. The ambient term is the sky's own mean radiance, weighted by how much of the sky
  the shading normal faces and by an occlusion channel the material's data texture carries. A pillar
  does not darken the ground it stands on except where the shadow map says so.
- **The normal map and the occlusion are sampled by the FRAME, not by the material.** `CySurface` is
  `{ CyClosure closures; float opacity; float3 preview; }` and `CyClosure` is five terms — diffuse,
  specular, emission, roughness, weight. **There is no normal term and no occlusion term in the
  compiled-material vocabulary at all**, so `cyResolveSurface` leaves `Surface::normal` at (0,0,1)
  and `Surface::occlusion` at 1 whatever a material does. `beauty.slang` samples both from the
  material's own table at the material's own slots and the manifest says so per material. `m11c:the-frame-samples-what-the-material-cannot` is the criterion that keeps this sentence
  and the code in step: the day `CyClosure` gains a normal term it goes red, which is the right
  moment to delete the frame's stand-in.
- **Metalness is reconstructed from the specular closure's luminance.** `cyResolveSurface` computes
  `metallic = saturate(luminance(specular))`, so a compiled material cannot say "this is copper"
  directly — it says it by putting the base colour into the specular term, and the metalness that
  comes back is the luminance of that. The copper in this shot is therefore a partial metal, which is
  what oxidised copper is, and would be the same partial metal if it were gold.
- **The geometry is procedural and there is no mesh asset.** Thirty-one instances of six primitives,
  4 334 triangles. Nothing here was modelled or scanned.
- **The air is particles, and the effect is authored in C++ rather than as content.** Three ember
  emitters drift up the courtyard: `samples/12-beauty/embers.cpp` places and wires the spawn,
  initialise and update graphs node by node, `cy::vfx`'s shipping compiler cooks them,
  `SimulationWorld` settles the field for six seconds at the effect's own Ambient rate, and
  `cy::rendering::particles::ParticleRenderer` draws the whole field in ONE draw inside the frame's
  own `Transparent` stage — so the motes are depth-tested against the colonnade and graded by the
  same exposure and the same tone curve as the stone. The manifest publishes the count the
  RENDERER reported.
  **What that does not prove is an authoring path.** Everything else in this shot is a file; the air
  is not, because **there is no on-disk VFX asset format in this tree** — `VfxSystemAsset` is built
  by a caller and cooked by `compile_system`, nothing reads one from a file and nothing writes one —
  and `src/vfx/README.md`'s fourth recorded absence, no VFX graph editor, is the same fact from the
  authoring side. The effect is authored exactly the way `src/vfx/tests/effects.cpp` authors the
  spark plume.
  **And the motes have no texture and no soft depth fade.** `ParticleRenderer`'s sprite is a radial
  falloff computed in the fragment shader; there is no atlas and no depth-fade, which that module
  records as its own absence.
  **This is the half of `vfx-system` a picture can show, and something asserts it.**
  `render.vfx`'s `particles are in the assembled frame` renders the same field through the same
  camera at 480x270, compares it against `tests/render/references/beauty_shot_air.png`, and asserts
  the difference between that frame and the identical frame with the ring uploaded empty — 4 035 of
  129 600 texels, 3.11% of the frame, worst channel 112. `integration.vfx`'s case of the same name
  projects every published record through the artefact's camera on a machine with no GPU at all.
- **`draws 0` in the manifest is correct and is worth reading carefully.** The frame's own draw list
  is empty: this program draws its geometry inside the frame through `FrameSinks::passes`, which is
  the seam `ForwardFrame` documents — *"ForwardFrame knows the frame STRUCTURE and the caller knows
  how to draw"* — and not through the pipeline layer's draw path. The thirty-one draws are the
  sample's.
- **The still is recompressed and the pixels are not touched.**
  `tools/docs/collect_beauty_shot.py` rewrites the PNG container with deflate because the golden
  suite's writer stores rather than compresses, and it re-reads the file afterwards and refuses to
  keep one whose pixels changed. The same two lines of PIL, and the same argument,
  `tools/docs/collect_world.py` has made since M10.

---

## Before and after

![The same frame's linear scene colour with the display transfer applied and nothing else: almost
entirely white, with only the deepest mortar joints and shadow cores holding any
value](images/m11c-beauty-shot-linear.png)

**That is the same frame with no post chain**, read out of `FrameResources::color` — the linear HDR
target the resolve reads — in the SAME submission as the picture above. Not a second run: one frame,
one set of draws, one sun, read twice, so the two images cannot differ in anything but the chain.

It is almost entirely white because the scene is in physical units and the sun is 102 000 lux. Every
surface in it is two to three orders of magnitude above 1.0, and the only pixels that survive are the
mortar joints and the shadow cores. **That is what the exposure stage is for**, and it is the part of
sections 1 and 3 of this rung that no number shows.

---

## The turntable

[`m11c-beauty-shot.mp4`](videos/m11c-beauty-shot.mp4) — 180 frames, one orbit, the same committed
scene and the same three authored materials from every angle. The sun does not move; the camera does,
which is what makes the shadows read as geometry rather than as texture.

The geometry is baked against the shot's own camera once and the orbiting eye is expressed as an
offset from it, so two hundred and forty frames share one vertex buffer. The sky dome is re-evaluated
per frame — 18 432 calls to `compose_sky` — which is where the 124 ms goes.

---

## The path, and why each step is where it is

```
cy-author-material   the editor's own canvas          →  *.cymatcanvas   (the interchange)
cy_material author   the engine's canonical writer    →  *.cygraph       (committed)
                     lower_material → lower_graph     →  the material IR
                     compile_material                 →  twelve programs, one cook key
                     assemble_translation_unit        →  *.slang         (prelude + emitter bytes)
slangc               the engine's shader toolchain    →  *.spv
cy_sample_beauty     TextureImporter                  →  BC7/BC5 + mips
                     upload + bind at set 0 binding 1 →  cyMaterialTextures[]
                     FrameAssembly + the resolve      →  the picture
```

**The editor does not write the `.cygraph`.** `specialised/graph.rs` assigns the canonical on-disk
form to M11.e and gives the reason — *"a second writer of a canonical format is a second format the
day the two disagree about a float"* — so the editor writes an interchange and the engine writes the
file, which `cy_material author` then parses back and compares by semantic digest before it is kept.
The same arrangement M8.a chose for `.cyprim`.

**What is honestly missing from "authored through the editor".** The materials are placed and wired
on the editor's authoring MODEL by a program, not by a person at a window: the editor's command
registry has no `material.*` commands, so this cannot yet be driven over the control socket the way
`samples/08a-authoring` drives a scene. `m11c:the-shot-does-not-overclaim-the-editor` checks that sentence against the tree: the day a
`material.*` command is registered it goes red and this caption is owed an update. What IS true, and was not true before this rung, is that
`SpecialisedEditors::open(Domain::Materials)` succeeds at all: M11.c's spike measured it refusing
with *"this build declares no authoring vocabulary for materials — `material-compiler` owes it"*, and
that was junction 1 of six.
