# Design

## Which of the frame's lights the lightmap speaks for

The bake knows its lights by `gi::GiLight::id`; the frame knows its lights by their place in
`AssemblyView::lights`, which is the order `FrameAssembly::build_lights` writes `cyLights` in.
`write_lightmaps` takes the frame's lights' stable ids in that order and matches them:

| field | what it holds | default |
|---|---|---|
| `lightmap_layout.w` | the shadow mask's set 0 slot | `kNoMaterialTexture` |
| `lightmap_shadow_lights` (uint4) | per mask channel, the index into `cyLights` of the light it shadows | `kNoLightmapLight` (~0) |
| `lightmap_direct_lights` (uint4) | a 128-bit set over `cyLights` indices: lights whose direct term is in the texels | 0 |
| `lightmap_debug` (uint4) | x: the texel-density view; y: its target, texels per metre | 0 |

**Why the frame block and not `GpuLight`.** The light record is 64 bytes and full: the shader's
`padding` pair is `shadow_slot` and `layer_mask`, both read on the CPU, and a wider record would move
every committed module that reads `Light`. A mask channel is a property of the lightmap, not of the
light — a light shadows one lightmap's mask channel 2 and another's channel 0 — so it belongs with
the lightmap's words. Four channel indices and a 128-bit set are 32 bytes appended to `CyFrameData`
(592 → 640), which, being appended, leaves every other committed module's offsets where they were.

**A static light past the 128th.** Refused by `write_lightmaps` rather than shaded twice.

## The frame

On a lightmapped surface — `draw.giAddress != 0` and a lightmap mode the frame admits — the forward
fragment reads the mask once, at the atlas coordinate the ambient term reads, before the light loop,
so the read is in draw-uniform control flow and its implicit level of detail is defined. The light
loop then asks `bakedLightFactor(index, mask)` of every light it shades:

- a light in `lightmap_direct_lights`: 0 — its direct term is in the texels;
- a light a mask channel names: that channel;
- any other light: 1.

A directional light takes `min(realtime visibility, factor)`: the baked mask is the shadow of the
static world, the shadow map (when one is bound) the shadow of everything, and the darker of the two
counts an occluder both see once. A punctual light's sample is scaled by the factor before shading.
Its intensity and colour stay the frame's, which is what makes a stationary light dimmed at run time
keep its baked shadow.

**Byte-identical without a lightmap.** On a draw with no lightmap the factor is never evaluated and
the light loop's arithmetic is unchanged: the directional visibility is the same value, and a punctual
sample's illuminance is the same value, selected rather than recomputed. The frame's defaults name no
mask channel and no baked light, so even an evaluated factor would be exactly one. `render.lightmaps`
(c) holds the Metal frame to a reference drawn on the same machine by main's frame shaders.

**Cost.** On a lightmapped pixel, one more texture read (the mask) and, per light, a bit test and four
compares; nothing on any other pixel. It is not measured here: no case times the forward pass.

## The mip chain

Rectangles sit on the block grid and a block is at least `2^levels` texels, so a 2x2 box never
straddles two objects at a protected level. It does straddle two charts of one object: the padding
between them was dilated half from each side, and a coarse texel over the middle averages both. So
each level is built the way the base was dilated:

1. a coarse texel takes the mean of the fine texels of ONE chart — the chart most of its four fine
   texels belong to — and belongs to it;
2. a coarse padding texel belongs to the chart nearest its footprint at the base resolution
   (Chebyshev distance, in base texels, carried down as the minimum over the four fine texels) and is
   dilated from that chart's filled neighbours, one ring per pass.

The base dilation's own rule — "the chart most of its known neighbours belong to" — does not carry
down: in a gap split unevenly across the coarse grid, a padding texel between the two charts has one
known neighbour of each, and a tie that goes to the chart whose taps never reach the texel leaves the
chart whose taps do reading its neighbour's light. The chart padding `required_chart_gap` asks for, two texels
of the coarsest protected level, is what makes the nearest chart the one whose taps reach the texel.

Every level is rounded through half precision before the next is filtered from it, so the device, the
cooked asset and the host hold the same values. The chart ids come from the bake's rasteriser: the
covered texels' `TexelSurface::chart`, padding `kNoChart`.

The frame samples the planes and the mask with the implicit level of detail. A lightmap uploaded with
one level reads level 0, as it did.

## Progress and cancellation

`LightmapBakeProgress{report, user, cancel}` is an optional last argument to `bake_lightmaps` and
`rebake_lightmaps`. The trace — nearly all of a bake's time — checks it every `kProgressTexels`
atlas texels; the stages between check it once each. A raised `cancel` stops the bake at the next
check, sets `report.cancelled` and fails with `ErrorCode::Unavailable`. Reporting draws nothing from
the bake's sequences, so an uncancelled bake writes the bytes a bake without progress writes.

## `cy_build lightmap`, and the editor

The editor reaches the engine's tools only as processes (`tools/` is layer 7, which nothing links),
as it does the importer. `cy_build lightmap --description --project --out` runs the producer's own
`bake_lightmap_description`, with bundles read from the project's files instead of the graph's
artefacts, and speaks lines: `progress <stage> <done> <total>`, `baked key=value…`, `cancelled`. A
`cancel` line on stdin raises the bake's cancel; the output is written only by a bake that finished,
atomically, so a cancelled bake leaves the previous cooked lightmap in place.

`LightmapBakeService` starts `cy_build lightmap` as an `OperationService` operation: the progress
surface shows the trace's texels, and cancelling the operation — from the surface's row or with
`lighting.cancel-lightmap-bake` — sends the tool its `cancel`. `lighting.bake-lightmaps` is
`ExternalEffect`, like `project.build`: it runs a tool and writes derived data. The lighting and lightmap
baking specialised editor is a form over the two commands and the density view's command, and reads
the bake's state from the operation service by its request identity, so it cannot disagree with the
footer.

The texel-density view is an engine debug view (`editor-viewport-and-gizmos`: requested by the
editor, not drawn by it): `DebugViewMode::LightmapDensity`, which `lightmaps::write_lightmap_debug_view`
turns into the frame's `lightmap_debug` words. The frame measures the density rather than looking it
up: texels per metre are the screen-space derivatives of the atlas coordinate over those of the
position, so what it draws is what the frame samples. The colour is divided by the frame's exposure,
so the view reads the same under any sun.

## Metal legs, and the reference

`render.lightmaps` is declared once per backend from one source (`CY_TEST_LIGHTMAPS_METAL`), as the
pipeline suites are. Metal rasterises the corner a step or two away from Vulkan (62 956 of 129 600
pixels differ), so the Metal leg's no-lightmap case is held to its own reference,
`references/lightmaps_absent_metal.png`, captured on the M2 Max by main's frame shaders (1b7373a5)
before this change touched them. The Vulkan leg keeps `lightmaps_absent.png`.

The corner's cube UV2 cells were given the padding one protected mip level needs (0.1 → 0.18 of a
cell): at 0.1 five boxes were short of `required_chart_gap`, and the directional encoding's level-1
taps at a far cube's edge read the next face's normal. The suite now asserts that the corner's bake
reports no padding shortfall.
