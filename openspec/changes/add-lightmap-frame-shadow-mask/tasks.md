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
- [x] Rebase onto the specialised-editor scaffold (#59): draw the lighting panel as a `SpecialisedTool`, add `SpecialisedTool::OPERATIONS` (held to `ExternalEffect` and an MCP tool with no exclusion) for the bake, widen the scaffold's requirement in its delta, and add the MCP case `the_lighting_editor_bake_and_density_view_are_mcp_tools`.
- [x] Keep every changed function within cognitive complexity 15 (`grow_ring`, and the device suite's `before_upload`, `attach` and `floor_shadow` split), and compile every changed translation unit with GCC 16 and the build's `-Werror` flags.
- [x] Move the shadow-mask-in-the-frame, mip-chain and editor exemptions in `requirements-coverage.toml` to the new cases, keeping what is left exempt by name.
- [x] Update the lightmaps, lightmap bake, pipeline and editor READMEs, and validate this change with `--strict`.

Review follow-ups:

- [x] Hold "the darker of that channel and any real-time shadow" to the device: `render.lightmaps` (m) renders and binds the sun's real-time shadow map, hangs a movable occluder no bake sees over the lit floor, and checks the frame through both shadows against the frames through each alone, channel for channel.
- [x] Make `write_lightmaps` write nothing when it refuses: build the light words apart from the view and write them with the rest only when every check holds; `unit.lightmap_frame` checks the refused view against a default one.
- [x] Hold the density view's under-target colour and its exposure compensation in `render.lightmaps` (j).
- [x] Measure what the lightmap costs a frame, `render.lightmaps` (n): host-clock frame time, because `rhi-metal`'s timestamps sample nothing on Apple silicon (#65).
- [x] Regenerate `src/rendering/selection`'s committed shaders, which import `cy.frame`, and compile their MSL with Apple's compiler.
- [x] Declare `cy::rendering-lightmaps`' dependency on `cy::servers-render`, which its public header includes.
