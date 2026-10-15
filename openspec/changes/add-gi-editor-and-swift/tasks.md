# Tasks

- [x] `GiRuntimeSettings`, `IlluminationSystem::set_runtime_settings` / `runtime_settings`; `RadianceCache::record_region_settings`; `ProbePlacementContext::global`.
- [x] `EmitterOverride`, `EmitterRule`, `emitter_override_for`, `classify_emitters` under rules, `EmitterClass::Disabled`, `EmissiveReport::disabled_*`; `IlluminationSystem::set_emitter_rules` in place (`GiScene::set_cell_emission`, `SurfaceCache::set_emission`).
- [x] `IlluminationSystem::set_object_class`; `GiObject`'s override and parked field; `DistanceField::copy_placement`.
- [x] A scaled object's cards carry their scaled area (regression: a scaled lamp emitted over its unscaled square).
- [x] ABI 1.9: types, entries, thunks, `IlluminationBackend`; `cy::game-backend-illumination`; baseline, Swift overlay and Rust SDK regenerated.
- [x] CyberdyneKit `Illumination` and `GIVolume`; Kit tests; `integration.swift_illumination` over a real Swift module.
- [x] `cy::editor::IlluminationPreview` and `IlluminationService`; `illumination_report_v1.wire`.
- [x] The runtime host: `gi_runtime.h`, `authored_components.h`, the GI overlay in `AuthoredFrame`, `GizmoIntent::debug_view`, the volumes' boxes.
- [x] The editor: `gi.*` commands, `illumination_requests`, the panel's GI half, the Inspector row, the armed explain click, the MCP case, panel and viewport snapshots.
- [x] Prove each new case red by a mutation (`evidence/falsification.txt`).
- [x] READMEs, `docs/guides/global-illumination.md` (with the comparison), `requirements-coverage.toml`.
- [ ] The editor's lit frame lit by the device GI (the views are the host's, drawn on the processor).
- [ ] Material graphs' emission in the editor's GI (cooked standard materials and the default grey only).
- [ ] Swift: transmission and the per-pixel resolve's own settings.
