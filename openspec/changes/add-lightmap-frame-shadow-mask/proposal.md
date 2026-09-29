# The frame shades baked lights through the shadow mask; the atlas carries its mip chain; the editor bakes

## Why

`rendering-global-illumination` — "Lightmap baking" — requires a **shadow mask** that lets a
stationary light keep its dynamic direct term with baked shadows ("Stationary light with shadow
mask": the direct light, computed at run time, uses the baked shadow term). `add-lightmap-mobility-and-rebake`
baked the mask and left its use exempt by name: nothing uploaded it and `cy/frame.slang` did not read
it, so a stationary light's direct term was shaded unshadowed on a lightmapped surface, and a static
light's direct term — already in the texels — was shaded again on top of them. "UV2 and chart
packing" requires padding for bilinear filtering **and mip generation**: the atlas's gutter was laid
out for the protected mip levels, but the upload was one level. And the "Lighting & lightmaps" row of
the editor's missing tools (#29) had no bake command and no texel-density view.

The device suites that would hold any of this linked only Vulkan and skipped on the development Mac,
so they proved nothing there.

## What changes

- **The frame uses the shadow mask.** `LightmapTextures` uploads `BakedLightmap::shadow_mask` beside
  the planes. `write_lightmaps` matches the bake's light ids against the frame's lights and writes
  `lightmap_shadow_lights` (the frame light each mask channel shadows) and `lightmap_direct_lights`
  (a 128-bit set of frame lights whose direct term is in the texels); `lightmap_layout.w` is the
  mask's slot. On a lightmapped surface `cy/frame.slang` reads the mask once, at the ambient term's
  atlas coordinate, and the light loop takes `min(realtime visibility, mask)` for a stationary light
  and nothing for a light whose direct term is baked. The light's intensity and colour stay the
  frame's, so a stationary light dimmed at run time keeps its baked shadow. With no lightmap bound
  the frame is byte-identical to main's.
- **The bake names the directly baked lights.** `BakedLightmap::direct_lights`: every `Static` light,
  and under `DirectAndIndirect` every light that is not `Movable`.
- **A per-chart mip chain.** `build_lightmap_mips` fills the levels `AtlasLayout::mip_levels`
  protects, for the planes and the mask: a coarse texel takes one chart's mean, and padding belongs
  to the chart nearest its footprint at the base resolution and is dilated from it, so a coarse tap
  at a chart's edge reads only that chart. The upload carries every level, and the frame samples the
  planes and the mask with the implicit level of detail. Cooked asset version 3 carries the chain and
  the directly baked lights; versions 1 and 2 still decode.
- **Progress and cancellation.** `LightmapBakeProgress` reports the bake's stages and its traced
  texels and stops it cooperatively; an uncancelled bake with it writes the bytes a bake without it
  writes.
- **`cy_build lightmap`** bakes one `cylightmap 1` description outside the build graph with the
  producer's own code, prints progress lines, and stops at a `cancel` line on stdin, writing
  nothing. A description may name a project's cooked mesh (`.cy/cooked/<id>.cyasset`).
- **The editor.** `lighting.bake-lightmaps` runs it as an operation — the progress surface shows the
  traced texels — and `lighting.cancel-lightmap-bake` stops it. The lighting and lightmap baking
  specialised editor opens onto a form over the two. `DebugViewMode::LightmapDensity` (and the
  editor's `viewport.view-mode.lightmap-density`) is an engine debug view drawn by the frame: each
  lightmapped surface a checker of its own texels, green on the level's density, blue below, red
  above.
- **Metal legs.** `render.lightmaps` gains `render.lightmaps_metal`, which runs on an M2 Max; its
  no-lightmap case is held to a reference drawn there by main's frame shaders.

## Scope

Not in this change, and exempt by name in `tools/roadmap/requirements-coverage.toml`:

- **A Vulkan run of the new frame paths.** The Vulkan leg compiles and its SPIR-V is regenerated,
  but this tree's only Vulkan device is the Linux RTX 5060; its pinned no-lightmap reference and the
  new cases (g)–(l) have run only on Metal.
- **The editor-hosted runtime drawing baked lightmaps.** The runtime behind the editor's viewport
  (`samples/05b-editor-window`) loads no cooked lightmap and routes no `DebugViewMode` to its frame —
  for any of the twenty modes — so the density view the editor requests is drawn by the engine's
  frame (`render.lightmaps` (j)) but not yet in the editor's own viewport.
- **A GPU bake** (out of scope in #36) and **per-region mask channels** (a fifth stationary light is
  still refused).
