# `samples/12-beauty` — the first frame in which a material decided a pixel

> **M11.c's closing artefact.** Section 7 of `openspec/changes/implement-m11c-image/tasks.md`.
>
> ```
> just capture-beauty-shot                          # the picture, the before/after and the manifest
> just capture-beauty-shot --video                  # ... and the turntable
> just capture-beauty-shot --regenerate-textures    # ... regenerating the source images first
> just capture-ambient-occlusion                    # the shot with ambient occlusion off and on
> just measure-beauty-mip-chain                     # does the shot read its cooked mip chain?
> just capture-beauty-bloom                         # the same shot with and without bloom
> just capture-soft-shadows                         # the shot with soft and contact shadows off and on
> just capture-beauty-grading                       # the shot ungraded, warm and cool
> just capture-beauty-depth-of-field                # the shot focused on the sphere and on a column
> just capture-decals                               # the shot with its decals off and on (and the world's ground marker)
> ```
>
> No CTest entry: the picture needs a graphics device, and on a machine without one the program says
> which device it did not find and writes nothing.

![The Colonnade](../../docs/design/images/m11c-beauty-shot.png)

The provenance — what a person authored, what the renderer produced, and what this frame does not
contain — is [`docs/design/beauty-shot.md`](../../docs/design/beauty-shot.md), and the machine-readable
half is `docs/design/images/m11c-beauty-shot.manifest`, built from the frame's own `AssemblyReport`
rather than from anything this program believes.

## The sentence this artefact exists for

M11.c's ledger opens with a measurement rather than a complaint:

> There is no texture in this repository. Outside `docs/`, the tree holds exactly six image files —
> four editor identity marks and two golden references. Every material in every picture the project
> has produced is constants.

M11.c's spike then walked the path that would have to close to change that, and came back with
**four of six junctions closing and the two ends missing**: the material editor refused to open, the
importer named BC7 and delivered uncompressed RGBA8, nothing in the tree bound the material texture
table, and no assembled frame drew a compiled material program. This program is the other end of all
four.

## What it runs, in order

| | |
|---|---|
| **author** | three material graphs, placed and wired on `cy_editor_interface::specialised::graph::GraphCanvas` — THE canvas, the one `editor-architecture` forbids a sixth bespoke graph editor beside |
| **compile** | `lower_material` → `lower_graph` → `compile_material` → `assemble_translation_unit`. Twelve programs a material, one cook key each |
| **shader** | `slangc` over the generated module and `shaders/beauty.slang`, which is the fragment-stage lowering the engine does not generate — `slang_program.h` says so in as many words |
| **cook** | `cy::import::TextureImporter`: PNG in through M11.c's own DEFLATE decoder, BC7 and BC5 blocks with a nine-level mip chain out through M11.c's own encoder |
| **bind** | the blocks uploaded and bound at **(set 0, binding 1)**, which is where `cy/material.slang` declares `cyMaterialTextures[]` — imported rather than restated, so the Slang compiler checks the agreement rather than a comment |
| **assemble** | `cy::rendering::assembly::FrameAssembly`: eight passes, the post chain's three stages, the exposure the shot file commits and `cy/fullscreen.slang`'s tone curve |
| **capture** | the 8-bit output AND the linear scene colour, read back out of **one** submission, plus a manifest |

## The files

| | |
|---|---|
| `shot.h` | the committed scene's shape, and the artefact's honesty contract in its header |
| `shot.cpp` | the `.cyshot` parser and the `.cyprim` loader. Every refusal is about something the picture would have been wrong about |
| `stage.cpp` | the device: the cook, the uploads, the descriptor sets, the shadow map, the frame, and the two readbacks |
| `shaders/beauty.slang` | five entry points and the one thing it decides that the material cannot — which attributes the material sees, and what the engine's light loop does with the surface it returns |
| `main.cpp` | the command line, the sidecar check, and the turntable. `--albedo-levels 1` is a CONTROL: level 0 of every albedo map's cooked chain and nothing beneath it |
| `make_cool_look.py` | writes `content/beauty/looks/cool.cube` from the formula in its docstring |
| `measure_mip_chain.py` | `just measure-beauty-mip-chain`'s judge: the full chain rendered twice must agree, and against albedo level 0 alone must DIFFER and be the less aliased |

## Three things in here that were found by looking at a picture

Each is commented where it is fixed, because each is the kind of defect that produces a plausible
image rather than an obvious one:

1. **A BC5 normal map has no blue channel.** Reading `.xyz` from it gives z = 0, which decodes to
   −1 and turns every normal inside out. Every upward-facing surface in the frame was black and
   nothing else said so. `texture.h` chose BC5 for exactly the reason that makes this happen — *"Z is
   reconstructed in the shader from X and Y"* — and the shader now does.
2. **A zeroed material parameter block draws a black material however the graph was authored.** The
   authored defaults live in the module's own declarations, so `cy_material author` writes them into
   the sidecar and the frame uploads them.
3. **The sky's ambient term is its mean RADIANCE, not its irradiance.** The factor of π between them
   is the difference between a picture with a sun in it and a flat one: multiplying by the irradiance
   made the ambient π times too strong, the sun invisible against it, and every shadow in the frame a
   shade of the same grey.

## Bloom, with and without

`--bloom` puts bloom into the frame's post chain at step 9 with the grade the shot file carries
(`bloom threshold-stops`, `intensity`, `scatter`, `levels`): the threshold is written in stops above
the white the exposure maps to 1.0 and converted to scene units by `bloom_threshold_for_exposure`.
Without the flag bloom is absent — no pass, no target — and the frame is the one M11.c published:
`bloom-beauty-off.png` is pixel-identical to `m11c-beauty-shot.png`.
`just capture-beauty-bloom` photographs both from one set of compiled programs:

| Without bloom | With bloom |
|---|---|
| ![](../../docs/design/images/bloom-beauty-off.png) | ![](../../docs/design/images/bloom-beauty-on.png) |

`docs/design/images/bloom-beauty-on.manifest` is the bloomed frame's provenance; its post-stage
list names `Bloom` at step 9, before exposure.

## What it does not claim

The full list is in the provenance, and the short version is: no anti-aliasing stage (the frame is
supersampled and the manifest says three post stages, none of them temporal), no global illumination
pass, no ambient occlusion pass in the published frame (it is `--ambient-occlusion on`, at the end
of this file), procedural geometry, and a normal map and an occlusion channel sampled by the FRAME
because the compiled-material closure vocabulary has no term for either.

**The air is the one thing in this frame that is not content.** Three ember emitters are authored in
`embers.cpp` — node by node, on `cy::vfx`'s shipping node library and through its shipping compiler
— because there is no on-disk VFX asset format in this tree to author them into, and no VFX graph
editor to author them with. What the field proves is the runtime, not an authoring path, and the
provenance says so where the rest of this list is.

## Ambient occlusion, off and on

`--ambient-occlusion on|off` is the setting, off by default. On, the program records the frame's
own `DepthPrepass` stage from its own buffers (`record_prepass`, drawing `sceneVertex` with
`scenePrepassFragment`, which writes the geometric normal octahedrally), switches
`PostChainConfig::ambient_occlusion` on, hands the stage to `occlusion::AmbientOcclusionPass`, and
shades with `sceneFragmentOccluded` against the prepass depth — tested, not written. The term
multiplies the sky term only; `sceneFragment`, the entry point the published frame is drawn with, is
unchanged in what it computes. The radius (1.5 m) and power (1.5) are content, in `shot.cyshot`.

| Ambient occlusion off | Ambient occlusion on |
|---|---|
| ![](../../docs/design/images/ambient-occlusion-off.png) | ![](../../docs/design/images/ambient-occlusion-on.png) |

![Off, on, and the difference amplified eight times](../../docs/design/images/ambient-occlusion-detail.png)

`just capture-ambient-occlusion` writes `docs/design/images/ambient-occlusion-{off,on,detail}.png`
and `ambient-occlusion-on.manifest`, and `tools/docs/compare_ambient_occlusion.py` fails it unless
the off picture is `m11c-beauty-shot.png`'s pixels exactly and no pixel got brighter.

## Soft and contact shadows, off and on

`--soft-shadows on|off` is the setting, off by default. On, the program records the depth and
normal prepass, declares the frame's `ContactShadows` stage through
`contact_shadows::ContactShadowPass`, and shades through `sceneFragmentSoft`: the sun's map
filtered by `cy/shadow.slang`'s percentage-closer soft filter — a blocker search and a kernel whose
radius is `(blocker - receiver) * tan(sun angular radius)`, so the far ends of the long shadows
soften and their feet stay sharp — and darkened further by the contact term where the trace toward
the sun found an occluder the 3.7 cm map texels and the 5 cm normal offset lose. The sun's angular
radius (0.265 degrees) and the trace's reach are content, in `shot.cyshot`. Ambient occlusion
composes with it.

| Soft shadows off | Soft shadows on |
|---|---|
| ![](../../docs/design/images/soft-shadows-beauty-off.png) | ![](../../docs/design/images/soft-shadows-beauty-on.png) |

![Off, on, and the difference amplified eight times](../../docs/design/images/soft-shadows-beauty-detail.png)

`just capture-soft-shadows` writes `docs/design/images/soft-shadows-beauty-{off,on,detail}.png` and
`soft-shadows-beauty-on.manifest`, and `tools/docs/compare_soft_shadows.py` fails it unless the off
picture is `m11c-beauty-shot.png`'s pixels exactly.

## Colour grading, ungraded, warm and cool

`--look <file.cygrade>` grades the frame at step 12 of the post chain — after the tone curve, on
display-referred colour — through `grading::GradingRenderer`'s resolve in place of the frame's own:
the same exposure (the shot's stops), the same curve, then one lookup into a 33³ table baked from the
look. Two looks are committed in `content/beauty/looks/`: **warm** is the parametric controls alone
(white balance toward 4 600 K, gain, lift, contrast, saturation); **cool** is a slightly cool white
balance and `cool.cube`, an exported-style creative LUT in sRGB encoding written by
`make_cool_look.py`, baked together into one table. Without a look the frame is the one M11.c
published, and `just capture-beauty-grading` fails unless `grading-beauty-none.png` is
`m11c-beauty-shot.png`'s pixels exactly (`tools/docs/compare_grading.py`).

| Ungraded | Warm | Cool |
|---|---|---|
| ![](../../docs/design/images/grading-beauty-none.png) | ![](../../docs/design/images/grading-beauty-warm.png) | ![](../../docs/design/images/grading-beauty-cool.png) |

![Ungraded, warm and cool, side by side](../../docs/design/images/grading-beauty-triptych.png)

`docs/design/images/grading-beauty-{warm,cool}.manifest` are the graded frames' provenance; their
post-stage lists name `ColourGrading` at step 12, after `Tonemap`.

## Depth of field, on the sphere and on a far column

`--depth-of-field <target>` puts depth of field into the frame's post chain at step 7, before bloom,
through `depth_of_field::DepthOfFieldPass`, focused on one of the targets the shot file names:
`sphere`, the copper orb 8 m out, and `column`, the colonnade's last pillar on the right of the frame,
33 m out. The lens is the camera's own — the focal length the shot's 42-degree horizontal field of
view implies on a 36 mm full-frame sensor, cropped to this image's aspect — at the shot's f/1.4, and
the focus distance is the target's distance along the view axis. Nothing else is chosen: the blur
of every pixel is the thin-lens circle of confusion at its depth. Without the flag the stage is
absent and the frame is the one M11.c published; `just capture-beauty-depth-of-field` fails unless
`depth-of-field-beauty-off.png` is `m11c-beauty-shot.png`'s pixels exactly, and unless each focused
frame keeps its target's detail and loses the other's (`tools/docs/compare_depth_of_field.py`).

| Focused on the sphere | Focused on the far column |
|---|---|
| ![](../../docs/design/images/depth-of-field-beauty-sphere.png) | ![](../../docs/design/images/depth-of-field-beauty-column.png) |

![The sphere and the column, each without the stage, focused on the sphere and focused on the column](../../docs/design/images/depth-of-field-beauty-detail.png)

`docs/design/images/depth-of-field-beauty-{sphere,column}.manifest` are the focused frames'
provenance; their post-stage lists name `DepthOfField` at step 7, before `Tonemap`. The embers are
drawn without depth, so the stage blurs each by the surface behind it.

## Volumetric fog, off and on

`--fog on|off` is the setting, off by default. On, the post chain's `volumetric_fog` stage is
switched on and declared through `fog::FogPass`: a froxel volume — 160 by 90 columns, 96 slices out
to the shot's `fog far`, four sub-steps a slice — marched through the shot's height fog, lit by the
same sun through the same shadow map and by the same sky term the surfaces are shaded with, and
every scene and sky fragment is seen through it (`throughFog` in `shaders/beauty.slang`). Where a
column stands between the sun and the air, the froxels behind it scatter no sunlight: the shafts
between the Colonnade's columns. The haze is content, in `shot.cyshot`'s `fog` lines — a
meteorological visibility, a base altitude and scale height, an albedo and a Henyey-Greenstein
`g` — and nothing else about it is tunable.

![the Colonnade through volumetric fog](../../docs/design/images/volumetric-fog-beauty-on.png)
![the same shot with fog off, M11.c's published pixels](../../docs/design/images/volumetric-fog-beauty-off.png)
![off, on, and the difference amplified eight times](../../docs/design/images/volumetric-fog-beauty-detail.png)

Captured with `just capture-volumetric-fog` on Vulkan: the off picture is `m11c-beauty-shot.png`
pixel for pixel; on, 2073328 of 2073600 pixels change, 255428 darker by more than one step and
1651930 brighter by more than one step, the largest by 120 of 255, with 0 validation errors.

`just capture-volumetric-fog` writes `docs/design/images/volumetric-fog-beauty-{off,on,detail}.png`
and `volumetric-fog-beauty-on.manifest`, and `tools/docs/compare_volumetric_fog.py` fails it unless
the off picture is `m11c-beauty-shot.png`'s pixels exactly.
