# Tasks

- [x] ABI 1.15: `effect_find`, `effect_play`, `decal_find_material`, `decal_spawn`, `decal_status`; `abi::game::EffectsBackend` and `DecalsBackend`; the thunks; `unit.abi`'s cases and layouts; the baseline, the Swift overlay and the Rust SDK regenerated.
- [x] `cy::game-backend-effects` over `vfx::SimulationWorld` and `integration.game_backend_effects`; `cy::game-backend-decals` over `DecalBudget` and `integration.game_backend_decals`.
- [x] CyberdyneKit's `Effect`, `DecalMaterial` and `Decal` with `EffectsTests`.
- [x] `vfx::EffectSpawn::spawn_seconds`, and its cases in `integration.vfx`.
- [x] The Jolt server's temp arena sized from Jolt's own constraint size, and its regression case in `integration.physics_jolt`.
- [x] samples/15-first-person: the Swift game, the host, the AI targets, the effects, the GI, the renderer; `integration.first_person_sample` and `smoke.first_person_render` with the golden still and the 1080p budget.
- [x] Prove the new cases red by mutation, and record it in `evidence/`.
- [x] The READMEs, `docs/guides/first-person-demo.md`, the requirements map and the specifications.
