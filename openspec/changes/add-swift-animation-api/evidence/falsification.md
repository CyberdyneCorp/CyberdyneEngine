# Mutation proofs — `add-swift-animation-api`

Each mutation was applied to the working tree, the named targets rebuilt with the dev profile (the
Swift game module through `cy_sample_rts-api_game`), the named cases run, and the file restored and
its md5 checked against the original before the next. Every one went red; the restored tree is
green.

| Mutation | Where | Red in |
|---|---|---|
| A fired trigger is never cleared (`clear_triggers()` removed from `step`) | `src/animation/system/src/animation_system.cpp` | `integration.animation_system` "a trigger is read by exactly one tick"; `integration.game_backend_animation` "floats and bools reach the program, and a trigger lives one tick" |
| A requested state's clips keep their old clocks (`reset_state_times` skipped in `request_state`) | `src/animation/src/evaluate.cpp` | `integration.animation_system` "play crossfades to any state, and its clips start at zero": `clock_run` stays at 0.4 |
| A frame with no tick hands the last frame's events out again (the tick-count check removed) | `src/game_backend/animation/src/animation_backend.cpp`, `begin_frame` | `integration.game_backend_animation` "every event is delivered once…": the second read is not empty |
| `animation_play` allowed in the frame phase (`kPhaseAny`) | `src/abi/src/game/animation_thunks.cpp` | `unit.abi` "requests are simulation, reads are any phase…"; `integration.game_backend_animation` "what it refuses, by name" |
| Events written into a buffer too small for them (the capacity check removed) | `animation_thunks.cpp`, `animation_events` | `unit.abi` "struct_size both ways, and the events' sizing pattern": OK instead of BUFFER_TOO_SMALL, and the caller's buffer overwritten |
| The game plays idle whatever its agent is doing | `samples/13-rts-api/game/Commander.swift`, `listen(to:)` | `integration.rts_api_sample` "ABI 1.7 — units walk, cheer on arrival and stand down": `walked=0`, `footsteps=0` |
| The game never stands a unit down on the cheer's event | `Commander.swift`, `onUpdate` | `integration.rts_api_sample` "ABI 1.7 …": `idle_after=0` |
