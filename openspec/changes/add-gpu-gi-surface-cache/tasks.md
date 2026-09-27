# Tasks

- [x] Give `DistanceField` a change journal (`generation`, `last_changes`, `visit_bricks`) and a mirror's view of its levels and brick pool.
- [x] Give `SurfacePage` a revision bumped by allocation, release and invalidation.
- [x] Lift the prioritised, budgeted selection out of `SurfaceCache::service` into `SurfaceCache::select`, unchanged.
- [x] Add `gi::SurfaceShadingBackend`, `SurfaceCache::set_shading_backend` / `collect`, and `IlluminationSystem::set_surface_shading`.
- [x] Add `gi/card_lighting.h`: `ShadowMap`, `ShadowMapOccluder`, `CardGrid`, `CardSnapshot`, `CardGather`.
- [x] Add the headless cases to `integration.render_gi_pipeline`: the journal, the snapshot against the cache's lookup, the shadow map against the field, the seam in the composed system.
- [x] Add `cy::rendering-gi-gpu`: `GpuGiScene` (the page table, the brick pool, the cards, the grid, the trace batch) and `GpuSurfaceShading` (the shade and commit dispatches).
- [x] Add the shaders and commit their SPIR-V and MSL through `shaders/regenerate.py`; `just build-shaders --strict src samples` reports `target_refusals=0`.
- [x] Add `render.gi_gpu`: the trace, the incremental upload, the card radiance in the room and under the shadow-mapped sun, the budget, and the side-by-side image; prove each red by a mutation.
- [x] Publish the side-by-side image, update the module READMEs and the requirements map, and validate this change.
- [ ] Stage 3: the radiance cache and the tracing tiers on the device (issue #35).
- [ ] Stage 4: the per-pixel resolve, the denoiser transcription, per-frame staging, the frame's shadow cascade, and the composite at `probeVolumeAmbient` (issue #35).
- [ ] Stage 5: the device cost in `GiBudget` and the arbiter's lever ladders (issue #35).
