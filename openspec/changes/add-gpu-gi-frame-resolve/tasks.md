# Tasks

- [x] Add the host oracle `gi/screen_resolve.h`: `resolve_screen_pixel`, `ScreenResolve`, `screen_history`, `ScreenGrid`, `screen_upsample`; `RadianceCache::sample_traced`; `SurfaceBounce::Cards`.
- [x] Give every field brick its far corner (`kBrickSampleEdge`), with the 10 cm wall as the regression.
- [x] Add `GpuScreenResolve` and the shaders `gi_surfaces`, `gi_resolve_common`, `gi_resolve`, `gi_resolve_hardware`, `gi_hardware_tier`, `gi_denoise`, `gi_publish`; regenerate the headers.
- [x] Attach it as the frame's `ScreenSpaceGi` producer and composite it in `cy/frame.slang` under `exclusion_for()` (`gi_control`, appended).
- [x] Upload a probe the host gathered again to the device mirror, with its regression.
- [x] Count a final-gather ray in full whatever it met, on the host and the device, with its regression.
- [x] Add `GpuGiBudget`, `apply_levers`, `arbiter_position` and `BudgetArbiter::declaration()` (stage 5).
- [x] Add `gi/mesh_cards.h`, and light `samples/12-beauty` with the GI behind `--gi on`; `just capture-beauty-gi`.
- [x] Write `integration.render_gi_screen`, `render.gi_gpu_resolve`, `render.gi_gpu_frame`, and the mesh cases in `integration.render_gi_pipeline`.
- [x] Prove each case red by a mutation (`evidence/falsification.txt`) and check the frame off (`evidence/off-reference.txt`).
- [x] Update the READMEs, `docs/guides/global-illumination.md` and the requirements map; validate with `--strict`.
- [ ] `samples/10-world`: the terrain in a GI scene, a prepass, and the world shader's ambient replaced.
- [ ] The beauty shot's card shading and probe gather on the device (stages 2 and 3 installed there).
- [ ] The editor and Swift: the dynamic GI's settings, volumes and diagnostics.
