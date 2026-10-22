# Tasks

- [x] Add `cy::gameplay-fog`: `FogOfWar` (integer grid, shared vision, ghosts, refusals, digest) and `unit.gameplay_fog` with a committed digest; hold it headless.
- [x] Add `cy::rendering-fog-of-war`: `VisibilityField` (fade, pack), `sample_visibility` and `shade_fog` (the host twins), `compose_minimap_fog`, and `unit.render_fog_of_war`.
- [x] Add `fog_of_war.slang` and `FogOfWarPass` (upload, dispatch, readback, timestamps); regenerate the committed SPIR-V and MSL; register the header group with `quality-shader-headers`.
- [x] Append `FramePassKind::FogOfWar`, `FrameFeatures::fog_of_war`, `FrameDescription::fog_of_war_stage` and `FrameResources::fog_of_war`; declare the stage after the temporal one and before depth of field; refuse it without a producer, or multisampled without the prepass; cases in `unit.render_forward`.
- [x] Append `AssemblyDescription::fog_of_war`, `FrameAssembly::set_fog_of_war` and `FrameSinks::fog_of_war`.
- [x] Add `render.fog_of_war`: the device texture against the grid, the dispatch against the twins, explored and unexplored ground, enemy units by sight, smooth edges, the fade, and off against a committed reference.
- [x] Append ABI 1.10 (`fog_*`), `abi::game::FogBackend`, the thunks and `unit.abi`'s cases; add `cy::game-backend-fog` and `integration.game_backend_fog`; regenerate the Swift and Rust overlays and the baseline.
- [x] Add CyberdyneKit's `FogOfWar` and `FogOfWarTests`.
- [x] Give `samples/13-rts-api` the grid, the camp, the Swift game's visions and minimap dots, `--render --fog`, `smoke.rts_api_fog`, and the fog cases of `integration.rts_api_sample`.
- [x] Draw the fog on the HUD's minimap, C++ and Swift alike, with a case in `render.rts_api_hud`.
- [x] Prove every new case red by a mutation, and record it in `evidence/`.
- [x] Update the READMEs, `docs/guides`, the requirements map and the living specifications, and validate this change.
