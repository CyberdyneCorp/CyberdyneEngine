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
| **The cooked textures** | `cy::import::TextureImporter`: PNG in, **BC7 and BC5 blocks with a nine-level mip chain** out. 899 346 bytes of PNG became 786 852 bytes of blocks, and the blocks are what the device holds — **and every level of the chain is read**, see [the mip chain](#the-mip-chain-and-why-this-picture-was-re-captured) |
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
- **There is no global illumination pass, and the published frame has no ambient occlusion pass.**
  `cy::rendering-gi` is not linked by this program. The ambient term is the sky's own mean radiance,
  weighted by how much of the sky the shading normal faces and by an occlusion channel the
  material's data texture carries. In `m11c-beauty-shot.png` a pillar does not darken the ground it
  stands on except where the shadow map says so.
- **Ambient occlusion is a setting of this program, off in the published frame.**
  `--ambient-occlusion on` adds the frame's depth and normal prepass and the post chain's ambient
  occlusion stage (`src/rendering/occlusion/`), and multiplies the SKY term — never the sun — by the
  term. `just capture-ambient-occlusion` photographs both and requires the setting off to be this
  file's picture pixel for pixel:

  ![Ambient occlusion on](images/ambient-occlusion-on.png)

  ![Off, on, and the difference amplified eight times](images/ambient-occlusion-detail.png)

  The detail is a 384x200 window at the foot of a plinth, doubled with nearest-neighbour sampling:
  off, on, and a third panel that is not a rendered image — the difference, off minus on, times
  eight. Measured on the capture: 8.3 % of pixels change, the mean luma falls by 0.26 of 255 and
  the largest fall is 25.8; no pixel gets brighter. The effect is small in this frame because a
  15.5-degree sun at about 100 000 lux carries most of the light and the term touches only the sky
  term; it is where the sun does not reach — the shadowed faces of the plinths and the ground at
  their feet — that it is visible. `images/ambient-occlusion-on.manifest` is the frame with the
  stage, built from its own report.
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
  the difference between that frame and the identical frame with the ring uploaded empty — 6 586 of
  129 600 texels, 5.08% of the frame, worst channel 255, trails included. `integration.vfx`'s case of
  the same name projects every published record through the artefact's camera on a machine with no
  GPU at all.
- **Every mote carries a TRAIL, and that is a second renderer kind in the frame.** M11.c task 6.3:
  the same particles are published a second time through `vfx-system`'s `Trail` renderer — a
  position every third simulation step for the last eight, so each trail is the last third of a
  second of its mote's flight — and `cy::rendering::particles::StripRenderer` draws every trail in
  ONE draw in the same `Transparent` stage, before the motes, so each bright core sits over its own
  wake. The manifest's `trails` line is that renderer's own count. `render.vfx` renders the air with
  and without the trails, motes present in both, and asserts what the trails alone add: 3 215 of
  129 600 texels at 480x270, mean |delta| 15.2/255 where they differ. `m11c:vfx-trails-in-the-shot`
  is the criterion. **Decals, particle lights and volumes are still not in this shot and not
  composited anywhere** — `src/vfx/README.md` records it.
- **Until task 6.3 the motes were drawn with the wrong billboard basis, and the still published
  before it was captured that way.** `ParticleRenderer`'s embedded SPIR-V was compiled against the per-view
  block before the temporal work inserted four rows into it, so it read the sprite's right and up
  vectors out of LAST FRAME'S clip matrix. A single still hid it; `render.vfx` rendering the same
  frame twice did not — 39 524 texels apart — and `unit.particle_modules` now compares every
  embedded module's own layout with the C++ block on every machine.
- **`draws 0` in the manifest is correct and is worth reading carefully, and `particles 996 in 1
  draw(s)` is a different number for the same reason.** The frame's own draw list
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

## The mip chain, and why this picture was re-captured

**Until M11.c's gate, the material programs in this shot sampled level 0 and nothing else**, and the
still published before this section existed was captured that way. The importer cooks a nine-level
chain for every texture and the frame uploaded all nine; the material never read eight of them.

The reason was one missing line. The prelude the engine generates in front of every compiled
material writes `cy_material_sample` twice, behind `CY_MATERIAL_PIXEL_STAGE`: the implicit level of
detail for a pixel stage, the explicit level 0 for everything else, because a material program is
compiled before anything knows which stage will call it. `samples/12-beauty/shaders/beauty.slang` IS
the pixel stage — its header says so — and never said so to the preprocessor. Disassembled, each of
the three compiled modules carried two `OpImageSampleImplicitLod` (the frame's own normal and
occlusion samples) and two `OpImageSampleExplicitLod ... Lod %float_0` (the material's albedo and
data maps). The engine's own forward path had been fixed in `cy/frame.slang`, and
`m11c:forward-path-reads-the-mip-chain` proved that; it judges a test scene, and this shot does not
draw through that shader.

**The define is in `beauty.slang`, before the material is included**, and not on the recipe's
`slangc` line: the file is the one place that knows it is a fragment-stage lowering, so every caller
that compiles it gets the answer, and nobody writing the next recipe has to rediscover it. Every
module is now four implicit samples and no explicit ones.

**What says so is a measurement of THIS shot.** `just measure-beauty-mip-chain` photographs it twice
with every chain and once with level 0 alone of every **albedo** map — the one texture only the
material samples — and `m11c:beauty-shot-reads-the-mip-chain` requires the difference. At 960 x 540:

| | before the define | after |
|---|---|---|
| albedo chain against albedo level 0 alone | **0 texels — byte-identical** | 64 833 texels (12.51%), mean \|delta\| 0.233/255 |
| neighbour-to-neighbour energy, chain / level 0 | 6.023 / 6.023 | 5.881 / 5.945 — level 0 is the aliased one |
| every chain cut, not just albedo | 15.17% | 15.65% |

The last row is why the control is the albedo map alone: cutting every chain moves the picture
whether or not the material reads it, because the frame's own implicit samples read the normal and
data chains either way. A control that cannot fail is not one.

**The published still changed, and it is the correct one.** Re-captured with
`just capture-beauty-shot` at the published 3840 x 2160, box-filtered to 1920 x 1080: **45 594 of
2 073 600 texels (2.20%) moved, mean |delta| 0.031/255, worst channel 37**, neighbour-to-neighbour
energy 3.838/255 before and 3.832/255 after. The linear still moved in 55 texels by at most 2 steps.
It is small at this size because the 2x supersample already resolves most footprints near level 0;
at 960 x 540 without supersampling the same change moves 122 467 of 518 400 texels (23.6%) at
0.436/255. **All of it is the define**: the old still is byte-identical (0 texels) to the same
binary rendering programs compiled from the `beauty.slang` before this change, so nothing else in
the frame — geometry, light, air, grade — moved. The manifest differs only in its three timings.
**The turntable (`videos/m11c-beauty-shot.mp4`) was NOT re-encoded** and still shows level 0; it is
regenerated by `just capture-beauty-shot --video`.

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
