# Tasks

- [x] Combat in `LockstepSession` (`lockstep_combat.h`, `lockstep_combat.cpp`): arming, structures, the attack-move and attack orders, targets, chases, melee, projectiles, splash, deaths, the event list, the hashes only with combat; `integration.game_backend_lockstep`'s combat cases.
- [x] ABI 1.16: the six entries, the order kinds and types; `LockstepBackend`'s defaults; the adapter's event history; `unit.abi`'s cases and layouts; the baseline, the Swift overlay and the Rust SDK regenerated.
- [x] CyberdyneKit's combat API and `LockstepCombatTests`.
- [x] `host/battle.h`, its C++ twin, published by `determinism.cross_leg` and compared by `tools/ci/cross_leg_digests.py` (`detmath-battle`).
- [x] `cy::rendering-minimap` and `unit.render_minimap`.
- [x] `UiRenderer::stage_atlas` and `render.ui`'s case.
- [x] The VFX slot reuse defect, and its regression case in `integration.vfx`.
- [x] samples/13-rts-api `--battle`: the Swift battle, the host's stage, the effects, the lights and cues, the renderer's soldiers, buildings, particles and lights, the topographic minimap; `integration.rts_api_sample`, `integration.rts_map` and `smoke.rts_api_map`'s battle cases.
- [x] Prove the new cases red by mutation, and record it in `evidence/`.
- [x] The READMEs, `docs/guides/rts-combat.md`, the requirements map and the specifications.
