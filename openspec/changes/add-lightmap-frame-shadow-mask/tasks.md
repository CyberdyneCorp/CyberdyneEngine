# Tasks

- [x] Give `render.lightmaps` a Metal leg (`render.lightmaps_metal`) from one source, and pin its no-lightmap case to a reference drawn on the M2 Max by main's frame shaders before any shader change.
- [x] Name the directly baked lights in `BakedLightmap::direct_lights`.
- [x] Add `build_lightmap_mips`: one chart per coarse texel, padding owned by the nearest chart and dilated from it, every level rounded through half precision; fill it from every bake and rebake.
- [x] Carry the chain and the direct lights in cooked asset version 3; keep versions 1 and 2 decoding.
- [x] Add `LightmapBakeProgress` to `bake_lightmaps` and `rebake_lightmaps`: stage progress, the trace's texels, cooperative cancellation, and no change to an uncancelled bake's bytes.
- [x] Upload the shadow mask and every mip level in `LightmapTextures`.
- [x] Append `lightmap_shadow_lights`, `lightmap_direct_lights` and `lightmap_debug` to `CyFrameData` / `FrameViewData`, with `lightmap_layout.w` the mask's slot; match the bake's lights to the frame's in `write_lightmaps`.
- [x] Shade a stationary light's direct term through its mask channel and skip a baked light's direct term in `cy/frame.slang`; sample the lightmap and the mask with the implicit level of detail; add the texel-density view.
- [x] Regenerate `frame_spirv.h` / `frame_msl.h` with the new `shaders/regenerate.py`, and compile every frame module with Apple's Metal compiler on the M2 Max.
- [x] Add `cy_build lightmap` (progress lines, a `cancel` line on stdin, nothing written when cancelled) and `.cyasset` meshes in a description; add `integration.build_lightmap_cli`.
- [x] Add `lighting.bake-lightmaps`, `lighting.cancel-lightmap-bake`, `LightmapBakeService`, the lighting and lightmap baking form and its panel, `DebugViewMode::LightmapDensity` and `viewport.view-mode.lightmap-density`.
- [x] Add the cases: `integration.render_lightmap_mips`, the new `integration.render_lightmap_bake` and `unit.lightmap_frame` cases, `render.lightmaps` (g)–(l), and the editor's `lightmaps` and `lighting` tests; prove each claim red by a mutation in `evidence/falsification.txt`.
- [x] Move the shadow-mask-in-the-frame, mip-chain and editor exemptions in `requirements-coverage.toml` to the new cases, keeping what is left exempt by name.
- [x] Update the lightmaps, lightmap bake, pipeline and editor READMEs, and validate this change with `--strict`.
