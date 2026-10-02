# Falsification: each claim of `add-swift-m12-gaps`, broken on purpose

Every mutation below was applied to one line, the named target rebuilt (GCC 13, the dev profile),
the named suite run, and the file restored and checked against its md5. A mutation that failed to
compile is not counted: one (M6, `false && a || b` tripping `-Wparentheses`) was replaced by a
compiling form (M6b). All went RED; none survived.

| # | Mutation | File | Suite | Result |
|---|---|---|---|---|
| M1 | the scheduler is given no access terms for a script system | `src/abi/src/systems.cpp` | `unit.abi` — a script system is ordered against a native one | RED, 4 checks |
| M2 | no `IterationGuard` while a script system runs | `src/abi/src/systems.cpp` | `unit.abi` — cannot change the world's structure | RED (`created` 9, not null) |
| M3 | `reload` ignores a refused system registration | `src/abi/src/module.cpp` | `integration.abi_reload` — a reload that moves a scheduled system is refused | RED, 4 checks |
| M4 | the bridge forwards no tree callback | `src/game_backend/src/scene_bridge.cpp` | `integration.game_backend_scene` | RED |
| M5 | **the old code**: `onEnterTree` not guarded per attachment | `src/scene/src/tree.cpp` | `unit.scene` — a node created under one attached in the same frame enters once | RED (the regression test fails on the pre-fix code) |
| M6b | a push on a static body is not refused | `src/game_backend/src/body_backend.cpp` | `integration.game_backend_bodies` | RED |
| M6c | `apply_force` pushes with a zero force | `src/game_backend/src/body_backend.cpp` | `integration.game_backend_bodies` | RED |
| M7 | a character's body does not carry its entity | `src/game_backend/src/character_backend.cpp` | `integration.game_backend_character` — a ray cast names the character | RED |
| M8 | `character_move` allowed in every phase | `src/abi/src/game/character_thunks.cpp` | `unit.abi` — moves only in a fixed step | RED |
| M9 | `node_find` resolves relative paths from the root | `src/game_backend/src/scene_bridge.cpp` | `integration.game_backend_scene` — node_find | RED |
| M10 | a scheduled script system's body is never called | `src/abi/src/systems.cpp` | `integration.rts_api_sample` | RED (`most` 0, not 419) |
| S1 | `ready` does not resolve `@Node` | `bindings/swift/Sources/CyberdyneKit/BehaviourBridge.swift` | `integration.swift_package` (TreeCallbackTests) | RED |
| S2 | `EngineChunkSource` ignores `Without` | `bindings/swift/Sources/CyberdyneKit/Systems.swift` | `integration.swift_package` (SystemEngineTests) | RED |
| S3 | `enter_tree` registered for every class | `bindings/swift/Sources/CyberdyneKit/BehaviourBridge.swift` | `integration.swift_package` (TreeCallbackTests) | RED |
| S4 | a `@System` is never handed to the engine | `bindings/swift/Sources/CyberdyneKit/Systems.swift` | `integration.swift_package` (SystemEngineTests) | RED |
| M11 | **the old code**: only a reload's first registration of a name is held to the earlier declaration | `src/abi/src/host.cpp` | `unit.abi` — a second registration in a later generation is held to the earlier declaration too | RED, 4 checks (the regression test fails on the pre-fix code) |

The harness is a 40-line script: replace one exact string (refusing an anchor that does not occur
exactly once), `cmake --build build/dev --target <suite>`, run the suite, restore, verify the md5.

## The suites, green on the change

`unit.abi` (130 cases), `unit.scene`, `integration.abi_reload`, `integration.abi_baseline`,
`integration.game_backend_{physics,spawn,bodies,character,scene}`, `integration.rts_api_sample`
(5 cases) and `integration.swift_{overlay,overlay_gen,no_runtime,package,module_depends,reload}`
(`swift test`: 161 cases). The sample's report on this machine:

```
rts tree     entered=1 readied=1 barracks=1 crate_found=1
rts system   installed=1 runs=420 rows=3 most=419 ordered=1
rts hero     x=8.000 ground=0 airborne=1
rts body     kicks=1 crate_speed=5.000 crate_moved=16.814
```

with `--no-systems`: `installed=0 runs=0 rows=3 most=0`; with `--no-behaviours`: no tree callback,
no hero, no kick.
