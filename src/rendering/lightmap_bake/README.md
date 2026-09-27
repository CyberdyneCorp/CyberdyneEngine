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
4. **Denoise** each moment channel with `denoise::Denoiser`, the chart id as the hard boundary.
5. **Dilate** each rectangle from its own texels out to its edge: padding and gutter hold light, so a
   bilinear tap or a mip level at a border never reads black.
6. **Reconcile seams**: where two charts share a smooth edge, both sides' bilinear footprints are
   moved to their common value.
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
| Directional | 2 | `rgb * max(0, 1 + v . n) / w`, with `v` the luminance-weighted mean incoming direction scaled by its directionality and `w = 1 + v . n_geometric` |
| SH L1 | 3 | per channel `max(0, a + b . n)`, the same fit per colour |

All three are exact at the geometric normal. The directional and SH L1 forms are what keep a normal
map's response in indirect light.

## What the tests hold it to

`integration.render_lightmap_bake`, over rooms built from one quad mesh:

| Case | Measured |
|---|---|
| texels against the path tracer's ground truth | MEASURED_GROUND_TRUTH |
| normal maps respond (directional, SH L1), irradiance does not | MEASURED_NORMAL_MAP |
| seams dilated and reconciled | MEASURED_SEAM |
| emissive light, alpha-tested and transparent occlusion, buried texels | see the suite |
| the caches' seeds from the same run | every probe valid on the first frame |
| the cooked payload round-trips and is deterministic | byte for byte |

`unit.render_lightmap_atlas` holds the packer: shared pages, no overlaps, per-object scaling, the
block grid and the gutter, and the address word.

## What is not here

The shadow mask and a `Stationary` light mobility; incremental rebakes of one moved object's region
(the build graph's content key re-bakes a changed level whole); a device bake; mip levels in the
uploaded atlas (the gutter is laid out for them). Each is exempt by name in
`tools/roadmap/requirements-coverage.toml`, naming #36's next slice.
