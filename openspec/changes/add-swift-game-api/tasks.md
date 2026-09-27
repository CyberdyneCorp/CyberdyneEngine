## 0. Declare (this change's first commit)

- [x] 0.1 Append ABI 1.3 to `cy_abi.h`: `CyPhase`, the time, input, camera, physics, navigation, audio and spawning types, 38 table entries after `service_poll`, and `CyBehaviourVTable.frame_update`; `CY_ABI_MINOR` 3; layout asserts for every new struct.
- [x] 0.2 Declare the backend seam: `cy/abi/game/services.h` (phases, `GameClock`, `GameServices`, `require_phase`, `PhaseScope`, `read_sized` / `write_sized`), one abstract backend header per service, `CyEngine_T::game`.
- [x] 0.3 Put stub thunks in the table, one file per group, each answering `CY_RESULT_NOT_IMPLEMENTED`; declare `cy::game-backend` with one adapter source per service and per-service test registration by file existence.
- [x] 0.4 Regenerate `abi_baseline.json`, the Swift overlay (with the four new enums) and the Rust SDK; teach both generators the new entries and enums.
- [x] 0.5 Add `unit.abi` cases for the shared rules (table shape, phase refusal, phase scope, `struct_size` both ways) and the 1.3 layout; add `GameTypes.swift` and the `FakeEngine` Swift test harness, with `GameTypesTests.swift` proving both.
- [x] 0.6 Write `design.md` with the entry reference and the file ownership split; validate the change `--strict`.

## 1. Implementer A — input and camera

- [x] 1.1 Implement the eight `input_*` thunks; `unit.abi` cases in `test_game_input.cpp` against a fake `InputBackend`, including F refusal of `input_pointer` / `input_modifiers` and a short-`struct_size` caller; each proven red by a mutation.
- [x] 1.2 Implement `InputAdapter` over `cy::input::InputServer` (resolved `ActionState`, per-frame pointer edges, modifier keys, named contexts); `integration.game_backend_input` shows an injected press and release inside one tick report `press_count` 1 and `release_count` 1 on both a live and a replayed run.
- [x] 1.3 Implement the seven `camera_*` thunks (`test_game_camera.cpp`) and `CameraAdapter` over the camera server and `cy::camera` projections; `integration.game_backend_camera` round-trips a world point through `world_to_screen` and `screen_to_ray`, and shows a resimulated `camera_set_target` has no effect.
- [x] 1.4 Write `Input.swift` and `Camera.swift` with `InputTests.swift` and `CameraTests.swift` through `FakeEngine`.

## 2. Implementer B — physics queries and navigation

- [x] 2.1 Implement the four `physics_*` thunks (`test_game_physics.cpp`), including the sizing pattern and the default filter for a null one.
- [x] 2.2 Implement `PhysicsQueryAdapter` over `PhysicsServer` queries with a `CyShape`-keyed shape cache; `integration.game_backend_physics` proves the total order (equal distances in entity order), the ignore list, the layer mask, and UNAVAILABLE during the step.
- [x] 2.3 Implement the eight `nav_*` thunks (`test_game_navigation.cpp`) and `NavigationAdapter` over `find_path`, `PathQueue` and `Crowd`, stepping configured agents; `static_assert` `CyNavPathStatus` and `CyNavQueryState` against the engine's enums; `integration.game_backend_navigation` proves poll completion lands on the same tick on two runs and that a moved agent reports ARRIVED with the event flag for exactly one tick.
- [x] 2.4 Write `Physics.swift` and `Navigation.swift` with `PhysicsTests.swift` and `NavigationTests.swift` through `FakeEngine`.

## 3. Implementer C — audio, spawning and time

- [x] 3.1 Implement `time_get` (`test_game_time.cpp`), wire `PhaseScope` and the clock into `BehaviourRuntime::fixed_update`, add `BehaviourRuntime::frame_update`, and dispatch `frame_update` to `onUpdate` in `BehaviourBridge.swift`; prove F hides the frame delta.
- [x] 3.2 Implement the six `audio_*` thunks (`test_game_audio.cpp`) and `AudioAdapter` over `AudioServer` and its `BusGraph`; `integration.game_backend_audio` shows a stale voice handle is not stopped by `audio_stop` and a resimulated `audio_play` starts nothing.
- [x] 3.3 Implement the four `spawn_*` thunks (`test_game_spawn.cpp`) and `SpawnAdapter` over `EntityTemplate` and `SceneTree`; `integration.game_backend_spawn` proves identical entities across two runs, all-or-nothing batches, epoch bumps, and child-first destroy callbacks.
  - Delivered over `SceneTree` with `SceneDescription` prefabs (loaded on demand in N). A cooked `EntityTemplate` prefab is not a scene node, so it cannot take a node parent or be destroyed through `destroy_node`; spawning one through the ABI is a follow-up.
- [x] 3.4 Write `Audio.swift`, `Spawn.swift` and `Time.swift` with their tests through `FakeEngine`.

## 3b. The end-to-end sample

- [x] 3b.1 `samples/13-rts-api`: a Swift `Commander` behaviour (camera pan by keys and screen edge, select under the pointer with a camera ray and a unit-layer raycast, right-click order to a ground-layer raycast point through `NavAgent.move(to:)`, an arrival cue on the one-tick ARRIVED event, a prefab spawn on a key) over a host that binds the six adapters and carries no gameplay value.
- [x] 3b.2 `integration.rts_api_sample` drives the host with synthetic input (`InputServer::inject`) and asserts the selection, the arrival within the arrival distance, the untouched bystander, the cue, the spawn, the `--no-behaviours` control and a reproducible report; proven red by breaking `physics_raycast` and `audio_play`, each restored and md5-verified.
- [x] 3b.3 "Writing an RTS unit in Swift" in `bindings/swift/README.md`; `samples/13-rts-api/README.md`; the game joins the swift-format gate's roots and its module edge is declared in `tools/roadmap/incremental.toml`.

## 4. Close

- [x] 4.1 Delete `not_implemented` once no stub remains.
  - Deleted with the helper's declaration; no thunk under `src/abi/src/game/` names `CY_RESULT_NOT_IMPLEMENTED`. A `unit.abi` case calling all 38 entries with every fake backend bound is not written; each group's suite covers its own entries.
- [ ] 4.2 Complete the 1.3 section of `src/abi/README.md` and `bindings/swift/README.md`; map the satisfied `native-abi` and `swift-scripting` requirements in `tools/roadmap/requirements-coverage.toml` to the cases above.
- [ ] 4.3 Build debug and dev; run `unit.abi`, the six `integration.game_backend_*` suites, `integration.swift_package`, `integration.swift_reload`, the ABI gate and both overlay checks; clang-format, clang-tidy and swift-format clean.
