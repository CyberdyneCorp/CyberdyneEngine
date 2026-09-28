# `src/rendering/lightmap_bake/` — layer 4

The lightmap bake: every static object of a level packed into shared atlas pages, each texel path
traced by `gi::PathTracer`, denoised, dilated and stitched, encoded as irradiance, directional or
SH L1 planes, and cooked into a payload the build graph caches by content. The dynamic caches'
seeds come from the same run.

**Governed by**: `rendering-global-illumination` — "Lightmap baking", "UV2 and chart packing".
OpenSpec change `add-lightmap-baking`. Issue #36, first slice (stages 1, 2 and 4).

No device: every case in `tests/` runs headless, as `cy::rendering-gi`'s do. The frame half is
`src/rendering/lightmaps/`.

## The files

| File | What it holds |
|---|---|
| `atlas.h` | `pack_atlas` — one rectangle per object in shared pages, sized from world area, the level's texel density and the object's resolution scale, on a block grid with a mip-safe gutter — and the one-word address a draw carries as `gi_address` |
| `scene.h` | `LightmapScene` (meshes with UV2, materials with emission, opacity and alpha masks, instances, lights, sky), `MeshSceneTracer` — the level's triangles behind `gi::SceneTracer` and `gi::Occluder` — and `measure_uv2` |
| `bake.h` | `bake_lightmaps`, the three encodings, `sample_lightmap` (the CPU reference the frame is held to) and `reference_ambient` (the ground truth the atlas is held to) |
| `asset.h` | the cooked payload: half-float planes and per-instance addresses, byte-identical for an unchanged level |

## The pipeline

1. **Pack.** `pack_atlas` sizes each receiving object's rectangle and shelves them into pages,
   tallest first, with the input order as the tie-break — one level packs to one layout everywhere.
2. **Rasterise.** Each object's UV2 triangles into its rectangle. A texel whose centre is inside a
   triangle takes the interpolated position and normal; a texel the triangle only grazes takes the
   nearest point of it, so a chart's border texels are real surface.
3. **Trace.** `gi::PathTracer` over `MeshSceneTracer`, a static BVH of every placed triangle. A hit
   resolves its material through `gi::GiScene` cards built from the same triangles — the real-time
   path's representation. Emission is light; a masked material passes rays through its holes, a
   transparent one stops them with its opacity. A texel whose hemisphere mostly meets back faces is
   inside a neighbour and is left for the dilation.
4. **Denoise** each moment channel with `denoise::Denoiser`, the chart id as the hard boundary, over
   a two-pass cascade (a reach of three texels): the default reach flattened a floor lit from one
   side toward its mean.
5. **Dilate** each rectangle from its own texels out to its edge, ONE CHART AT A TIME: a padding
   texel takes the chart most of its known neighbours belong to, and a buried texel its own chart,
   so a texel between a floor and a wall never carries a mix of the two lights and normals. It runs
   until nothing more can be filled.
6. **Reconcile seams**: where two charts — of one object, or of two objects that meet — share a
   smooth edge in the world, the smallest change to the texels that makes both sides' bilinear
   readings agree, solved by conjugate gradients.
7. **Seed** the surface cache, the radiance cache and the reflection probes from the same tracer.

## Units, and the one convention that differs

A texel holds the AMBIENT RADIANCE the frame multiplies albedo by: the mean incoming radiance over the
cosine hemisphere, `E / pi`. `gi::PathTracer` bounces with `albedo * irradiance` (the surface cache's
convention) where a Lambertian surface leaves `albedo * irradiance / pi` (the frame's and
`BoxProxyScene`'s), so the bake hands the tracer its lights divided by pi; the seeds keep the caller's
lights, in the caches' own convention. `LightmapContent::Indirect`, the default, leaves out the
placed lights' direct term at the receiver, because the frame shades every light itself.

| Mode | Planes | A shading normal `n` reads |
|---|---|---|
| Irradiance | 1 | `rgb`, whatever `n` is |
| Directional | 2 | `rgb * max(0, 1 + v . n) / w`, with `v` the luminance's gradient as the normal tilts, over the luminance, and `w = 1 + v . n_geometric` |
| SH L1 | 3 | per channel `max(0, a + b . n)`, the same fit per colour |

All three are exact at the geometric normal. The directional and SH L1 forms are what keep a normal
map's response in indirect light.

## What the tests hold it to

`integration.render_lightmap_bake`, over rooms built from one quad mesh:

| Case | Measured |
|---|---|
| texels against the path tracer's ground truth | relative error 0.046 over 24 points (bound 0.10), 32 samples per texel denoised against 512 at the point |
| normal maps respond (directional, SH L1), irradiance does not | a 40 degree tilt toward a bright wall: truth +32%, directional +27%, SH L1 +27%, irradiance 0% |
| seams dilated and reconciled, within one object | worst disagreement 52% of the mean unreconciled, 0.03% reconciled (bound 1%) |
| the seam where two objects meet | 63% unreconciled, 0.04% reconciled |
| a wide buried strip filled from its own chart | no dark texel, and the directional factor at the floor's own normal exactly one |
| emissive light, alpha-tested and transparent occlusion, buried texels | see the suite |
| the caches' seeds from the same run | every probe valid on the first frame |
| the cooked payload round-trips and is deterministic | byte for byte |

`unit.render_lightmap_atlas` holds the packer: shared pages, no overlaps, per-object scaling, the
block grid and the gutter, and the address word.

## What it costs

The corner `render.lightmaps` bakes — six boxes, 10 649 texels in one 256 page, 24 samples and one
bounce — takes 0.9 s on one core of the development machine, and 512 KiB on the device as
irradiance (1 MiB directional, 1.5 MiB SH L1). Most of the time is the path tracer's material
lookup: a hit takes its material from the nearest surface card within a metre, and the bake builds
cards at `surfel_spacing` (at most 1.2 m, so every hit finds one). Halving the spacing from 1 m to
0.5 m tripled a room's bake.

## Light mobility and the shadow mask

Each `gi::GiLight` carries a `gi::LightMobility`, and the bake honours it per light:
- `Static`: its direct term at the receiver is baked with its bounce.
- `Stationary`, the default: its bounce is baked, its direct term stays the frame's, and the bake
  writes a SHADOW-MASK channel for it — the fraction of the light's extent (`radius`) each texel
  sees, over `kShadowMaskSamples` points.
- `Movable`: nothing, not even its bounce; the dynamic caches own it.

A level that sets no mobility bakes the same texel planes, byte for byte, that it baked before
mobility existed (`add-lightmap-mobility-and-rebake/evidence`). `LightmapContent::DirectAndIndirect`
now means "bake the direct term of every light that is not `Movable`", and writes no mask.

The mask is `BakedLightmap::shadow_mask`: one RGBA plane, a channel per stationary light in scene
order, with `shadow_lights` naming each channel's light; a fifth stationary light is refused. It is
dilated within each chart like the texels. It is NOT denoised: fully lit and fully shadowed texels
are exact, and a soft penumbra carries sampling grain in steps of 1/64. It is NOT seam-reconciled:
the seam solve's step lengths come from one inner product over every moment
field, so adding the mask to it would move every existing bake's texels. The cooked asset carries it
from format version 2; version 1 payloads still decode, with no mask.

## Incremental rebake

`rebake_lightmaps` takes the previous bake and what moved (each moved instance and its bounds before
the move). It re-solves every object that moved and every object with a surface texel within
`influence_metres` of a moved object's old or new bounds, and copies every texel of every other
object from the previous bake byte for byte. The OBJECT is the unit because the denoiser, the
dilation and the seam solve are all chart-bounded. A seam between a re-solved object and a kept one is
left as the previous bake reconciled it (`report.boundary_seams`). If the level no longer packs to the
same rectangles, or the mode, the page layout or the stationary lights changed, the rebake runs a
full bake and says why in `report.fallback`.

## Chart padding

After rasterising, the bake measures, per object, the gap in empty texels between its distinct
charts on its own raster, and lists every object below `report.required_chart_gap` — two texels of
the coarsest protected mip level — in `report.padding_short`. With `refuse_short_padding` it refuses
the level instead. The default settings DO report shortfalls: the importer unwraps at 16 texels per
metre with a two-texel padding, and a level bakes at 8 with two protected mips. That is the finding
the check exists for.

## What is not here

The frame's use of the shadow mask — the forward shader reading the mask plane for a stationary
light's direct term — and the mask's upload; a device bake; mip levels in the uploaded atlas (the
gutter is laid out for them); the editor's bake command and texel-density view. Each is exempt by
name in `tools/roadmap/requirements-coverage.toml`.
