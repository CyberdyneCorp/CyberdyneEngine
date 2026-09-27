# Bake lightmaps with the path tracer and shade the frame with them

## Why

`rendering-global-illumination` requires static lighting baked into lightmap atlases by the offline
path tracer, and UV2 charts packed into those atlases with padding for bilinear filtering and the
mip chain. Both requirements were exempt at M11.e. The tree had the pieces around the hole: an
xatlas-backed UV2 unwrap at import (`cy::import::generate_uv2`), `gi::PathTracer` with its ground
truth and cache seeds, a `Lightmap` radiance source that `exclusion_for()` already handles, and a
`gi_address` word in every forward draw that nothing filled. Nothing produced a lightmap and nothing
in `cy/frame.slang` sampled one (#36).

## What changes

- **UV2, closed.** An unchanged reimport already runs no unwrap — the source's derivation key hits
  the cook cache, and the build graph does not run the node — and a test now says so. A reimport
  whose source changed and whose geometry did not now copies the previous unwrap from
  `import::Uv2Cache`, keyed by `uv2_geometry_key` (every attribute array, the sections and every
  `Uv2Options` field), instead of running xatlas again.
- **Shared atlases.** `lightmap_bake::pack_atlas` places every receiving object of a level in shared
  pages as one rectangle each, sized from its world area, the level's texel density and the object's
  own resolution scale, on a block grid with a gutter that survives bilinear filtering and the
  declared mip levels. A rectangle's address is one 32-bit word — the draw's `gi_address`.
- **The bake.** `lightmap_bake::bake_lightmaps` rasterises each object's UV2 triangles into its
  rectangle and path traces every texel through `gi::PathTracer` over `MeshSceneTracer`, a static
  BVH of the level's triangles behind `SceneTracer` and `Occluder`: a configurable bounce count,
  emissive surfaces as light, alpha-tested and transparent occlusion, buried texels rejected. It
  denoises with `denoise::Denoiser` (the chart as the hard boundary), dilates every rectangle to its
  edge, reconciles seams between charts that share a smooth edge, and encodes irradiance,
  directional or SH L1 planes. The dynamic caches are seeded by the same run's tracer.
- **A cooked, content-keyed asset.** `encode_lightmap_asset` / `decode_lightmap_asset`, and a
  `lightmap` producer in the build graph that bakes a `cylightmap 1` level description over its
  upstream import bundles, so an unchanged level is never re-baked.
- **The frame.** `cy::rendering-lightmaps` uploads the planes and writes two appended frame words
  (`FrameViewData` 512 → 544 bytes). The forward pipelines take the cooked `TexCoords2` stream as a
  fourth vertex stream (UV0 bound in its place for a source without one), and the forward fragment
  takes a lightmapped draw's ambient from `lightmapAmbient` where the irradiance volume's was. Which
  source a surface takes is read off `gi::exclusion_for()`.
- **A bug fixed.** `exclusion_for(Baked, has_lightmap)` excluded only the dynamic sources, so a
  lightmapped surface inside a volume resolved to the mean of its lightmap and the volume. It now
  excludes the volume for a surface with a lightmap, with a regression case.

## Scope

The first slice of #36: stages 1, 2 and 4. NOT built, and exempt by name in the requirements map:
the shadow mask and a `Stationary` light mobility (stage 3), incremental rebakes of one moved
object's region, the editor's bake command and texel-density view (stage 5), and mip levels in the
uploaded atlas (the padding is laid out for them; the upload is one level).
