# Tasks

- [x] Append `LightKind` `Rect`, `Disc`, `Sphere`, `Tube`, `LightDescription::source_width`, `source_height`, `two_sided` and `LightMobility`; pack area lights into `GpuLight` with a derived reach; `area_shading.h` as the host twin; cases in `unit.render_lighting` and `integration.render_area_lights`.
- [x] Shade area lights in `cy/light.slang` and `cy/frame.slang` (LTC, Fresnel at the gather direction, the diffuse term without a table); upload the LTC table (`LtcTableTexture`); append `FrameViewData::lighting_control` and `directional_lights`; regenerate the committed SPIR-V and MSL.
- [x] Leave a placeholder for a static light in the assembly; count static and area lights in `AssemblyReport`.
- [x] Add `cy::rendering-lights-gpu`: `GpuLightAssignment` (tile-frustum cull, nearest 64 per slice, device-local light records) and `ManyLightsPass` (alias table, RIS, temporal and spatial reuse with balance-heuristic weights, one visibility ray, the denoiser transcribed); register the header group with `quality-shader-headers`.
- [x] Append `FramePassKind::ManyLights`, `FrameFeatures::many_lights`, the cluster-assignment and many-lights stages and their refusals; `AssemblyDescription::device_light_assignment`, `many_lights`, `set_many_lights`.
- [x] `LightingPathSelector` over the cluster bound's drops and `shading_allocation_ms(arbiter)`; fix `cap_reservoir` (regression case).
- [x] Add `render.area_lights` (a)–(e) and `render.many_lights` (a)–(k).
- [x] Hand area lights to CyberGI (`gi_gpu::append_gi_lights`) with `unit.render_gi_frame_lights`.
- [x] `TransientLights`; append ABI 1.11 (`light_*`), `abi::game::LightsBackend`, the thunks and `unit.abi`'s cases; add `cy::game-backend-lights` and `integration.game_backend_lights`; regenerate the Swift and Rust overlays and the baseline; CyberdyneKit's `Light` and `LightsTests`.
- [x] Append the area and mobility fields to `LightSource` (reflection, identity, state schema, extraction); `scene.create-light` for rect, disc, sphere and tube with editor tests; the editor's frame and GI preview read them; the lightmap description skips area lights.
- [x] Prove every new case red by a mutation, and record it in `evidence/`.
- [x] Update the READMEs, `docs/guides`, the requirements map, the capability matrix and the living specifications, and validate this change.
