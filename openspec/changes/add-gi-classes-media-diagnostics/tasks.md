# Tasks

- [x] `gi/classification.h`: the classes, `classify_illumination`, `select_proxy_fidelity`, `sphere_occlusion`, `sphere_hit_distance`, `GiObject`.
- [x] `IlluminationSystem::add_object`, `move_object`, `pose_object`, `remove_object`; SlowDynamic coalescing; FastDynamic proxies in `indirect_diffuse` and `indirect_specular`.
- [x] `gi/media.h`: the staging table, the reserved interfaces and their refusal, thin transmission, `MediaIndirectGrid` and `media_indirect`.
- [x] `fog::FogIndirect`, `pack_fog_indirect`, the march on the host and the device; regenerate the fog headers.
- [x] `gi/diagnostics.h`, `RadianceCache::contributions`, `explain`, `explain_region`, `profile`, `add_stage_time`, `recent_invalidations`; the tracer's screen hits and tier times; the denoiser's cost into the profile.
- [x] The per-axis visibility test, host and device; regenerate the gi_gpu headers.
- [x] `gi/debug_views.h` and `render_gi_debug_view`; the `DebugViewMode`s; the chart view and the overlay in `cy/frame.slang`; `lightmaps::GiDebugOverlayTexture`, `write_gi_debug_view`, `write_lightmap_chart_view`; regenerate the frame, selection, particle and strip headers.
- [x] The editor's view-mode mirror.
- [x] Suites: `render_gi_classes`, `render_gi_media`, `render_gi_diagnostics`, `render_gi_debug_views` with nineteen goldens, the fog cases, `render.volumetric_fog` (g), `render.lightmaps` (o) and (p), `unit.lightmap_frame`'s overlay case.
- [x] Prove each new case red by a mutation (`evidence/falsification.txt`).
- [x] READMEs, `docs/guides/global-illumination.md`, `requirements-coverage.toml`.
- [ ] A directional depth per probe, so a doorway's edge probes stop leaking.
- [ ] The profile in the one trace, and an editor panel over `explain` and the profile.
- [ ] A runtime owner of the GI system that feeds `pose_object` from the GPU pose world and hands the fog its grid (issue #35 stage 4).
- [ ] Swift: classify an object, set a transmission, place a GI volume.
