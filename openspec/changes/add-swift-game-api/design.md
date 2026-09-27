# Design

## Context

See `proposal.md` for motivation. The subsystems exist and are tested: `cy::input::InputServer`
resolves actions per tick into `ActionState` records and command frames; `cy::physics::PhysicsServer`
answers const, thread-safe `raycast`, `raycast_all`, `shape_cast` and `overlap` queries
(`cy/servers/physics/queries.h`); `cy::camera` projects screen points to rays and world points to the
screen over an `EvaluatedCamera`; `cy::navigation` has `find_path`, a deterministic `PathQueue` and a
`Crowd`, joined to entities by the `NavAgent` component; `cy::audio::AudioServer` plays voices on a
`BusGraph`; `serialization-and-prefabs` spawns cooked `EntityTemplate`s and `SceneTree` loads scenes
and destroys subtrees. What is missing is the boundary: none of it is reachable through
`cy_get_interface`.

The ABI's own rules are fixed and this change keeps every one: C constructs only, append-only within
a major, failure as `CyResult`, no engine pointer escaping, `struct_size` on growable structs, one
description generating both overlays, and a compatibility gate that refuses anything but an append.

## Goals / Non-Goals

**Goals:**

- Every verb an RTS needs from these seven services is callable from Swift through the table.
- Every entry states its phases, its determinism, its ownership and its errors, and the phase rule is
  enforced rather than documented.
- `cy_abi` gains no dependency on any server; a fake backend tests every thunk.
- Three implementers can work at once on disjoint files, and nothing they do touches the table.

**Non-Goals:**

- Networked command submission (`gameplay-framework`'s command stream) — see Risks.
- Scene-tree lifecycle callbacks other than `frame_update` (`onEnterTree`, `onReady`, …).
- UI, animation, VFX, save and replay entries; they are the next appends.
- Changing any subsystem's own contract.

## Decisions

### D1. One appended block, ABI 1.3

38 entries are appended after `service_poll`, grouped time, input, camera, physics, navigation,
audio, spawning. `CY_ABI_MINOR` becomes 3. `CyBehaviourVTable` gains `frame_update` after `user_data`;
it carries `struct_size` and `register_behaviour` copies only the agreed prefix, so a module compiled
before 1.3 registers with `frame_update` null and is never scheduled for a frame. The gate
(`tools/abi/abi_gate.py`) classifies all of it as appends, and the baseline, the Swift overlay and the
Rust SDK are regenerated from the one description.

*Alternative rejected:* one minor per service. Seven minors for one milestone's worth of surface
would make "which minor has navigation" a question a game author has to answer, and the gate
guarantees the same compatibility either way.

### D2. Thunk, backend, adapter

```
entry (cy_abi.h)  ->  thunk (src/abi/src/game/<group>_thunks.cpp)
                  ->  backend (cy/abi/game/<service>.h, abstract)  ->  adapter (src/game_backend/)
```

The thunk owns everything about the boundary: null checks, the phase check, UNAVAILABLE for an
unbound backend, `struct_size` normalisation (`read_sized` / `write_sized`), dropping presentation
writes while resimulating, and bumping the ABI world's epoch after a structural call. The backend is
an abstract class per service, bound field by field on `CyEngine_T::game` exactly as 1.2's
`EditorServiceBackend` is bound on `editor_service`. The adapter implements it over the real server
in `cy::game-backend`, a new module at the `abi` layer beside `cy::editor-backend`.

This keeps `cy_abi` free of every server (it may name only the core and the ECS), lets `unit.abi`
test each thunk against a fake backend in microseconds, and makes a missing service — a dedicated
server with no audio device, a tool with no camera — an honest UNAVAILABLE rather than a crash.

### D3. Phases, enforced

`CyPhase` is `NONE`, `FIXED_UPDATE`, `FRAME_UPDATE`. Every entry declares `[N F U]` in the header and
its thunk calls `require_phase` first; a violation is `CY_RESULT_PERMISSION_DENIED` naming the entry
and the phase, in every build, because the check is a comparison and a rule that holds only where
`CY_ASSERT` is compiled in is not a rule. The phase lives in `GameClock` on the host and is set with
`PhaseScope` around behaviour dispatch (`fixed_update` in F, `frame_update` in U) and around the ECS
stages (`CY_STAGE_PRE_SIMULATION`..`POST_SIMULATION` in F, `FRAME`..`UI` in U; `RENDER` and every
boundary are N). `time_get` is never refused: it is how a caller learns the phase.

### D4. Determinism

Anything allowed in F answers from simulation state only, so a replay or a lockstep peer gets the
same answer:

| Entry family | Why it is deterministic in F |
|---|---|
| `input_action_state*` | reads the `ActionState` resolved for the tick — the state the committed command frame was built from — never live devices |
| `physics_*` | const queries over the last completed step; lists ordered by distance, then entity value, then body creation order; overlap by entity value, each once |
| `nav_find_path` | A* over the mesh is deterministic; points are the straightened path |
| `nav_request_path` / `nav_poll_path` | `PathQueue` completes after a fixed tick latency regardless of load; ids are issued in submission order |
| `nav_agent_*` | moves take effect in the fixed step's navigation update, not at the call, so call order within a tick cannot matter |
| `spawn_*` | entities are allocated in template-index order, instance by instance |
| `time_get` | `frame_delta` and `interpolation` are written as zero in F |

Device state (`input_pointer`, `input_modifiers`) and presentation state (the camera reads) are not
callable in F. Presentation *writes* (camera target and pose, audio) are callable in F because nothing
in the simulation reads them back; while `CY_TIME_RESIMULATING` is set their thunks answer OK and do
nothing, so a rolled-back tick does not play its sounds twice. A voice handle is not simulation
state and a fixed step must not branch on one.

### D5. Memory and ownership

- Results are values, caller-owned structs, or caller-supplied buffers. No entry returns an engine
  pointer; every service handle (`CyInputAction`, `CyInputContext`, `CyCamera`, `CyNavQuery`,
  `CyAudioCue`, `CyAudioBus`, `CyAudioVoice`, `CyPrefab`) is an integer the engine resolves and,
  where the underlying server has generations, generation-checks.
- Strings in are borrowed for the call. No string is returned.
- Variable-length results use `world_chunks`' sizing pattern: `*out_count` (or `point_count`) is
  always the total, a null buffer asks for it, and a short buffer is filled with the first
  `capacity` in the stated order and answers `CY_RESULT_BUFFER_TOO_SMALL`. `nav_poll_path` differs
  in one way: a READY path that does not fit is not written and not consumed, so the caller retries.
- Every growable struct begins with `struct_size`, set by the caller, in and out; zero means "this
  header's size". Structs passed in arrays (`CyScreenPoint`, `CyPhysicsHit`, `CyPose`) carry none and
  are fixed forever; they grow only by a new struct and a new entry.
- A zeroed struct is always valid input: zero fields mean "default" (navigation, audio, spawn) and an
  all-zero quaternion is the identity.

### D6. Errors

| Code | When |
|---|---|
| `INVALID_ARGUMENT` | null engine, null required pointer, malformed `struct_size`, non-finite or negative value where the entry forbids it |
| `PERMISSION_DENIED` | a phase the entry does not list |
| `UNAVAILABLE` | no backend bound; physics mid-step; no active camera; asset not resident outside N |
| `NOT_FOUND` | unknown name; stale or null handle; dead entity; entity without the needed component; consumed query |
| `OUT_OF_RANGE` | input user index; negative bus volume |
| `ALREADY_EXISTS` | pushing a context already on the stack |
| `BUFFER_TOO_SMALL` | see D5 |
| `NOT_IMPLEMENTED` | never, once the change is complete (the stubs' answer while it was built) |

"Nothing hit" and "no path" are not errors: `out_has_hit` false, `CY_NAV_PATH_FOUND` clear.

### D7. Threads

The game thread, except the four `physics_*` entries and `nav_find_path`, which are also safe from a
job worker inside an F or U stage (const queries over state nothing mutates in that stage; the
navigation adapter uses per-call scratch). `GameClock` is written only on the game thread at stage
boundaries, before any job of that stage is dispatched.

### D8. Hot reload

Service handles are engine state and survive a module reload unchanged; a Swift behaviour that
cached a `CyInputAction` keeps it. They do not survive the world or level that issued them, after
which they answer NOT_FOUND. A behaviour's `onAfterReload` is therefore not required to re-resolve.

### D9. The Swift layer

`CyberdyneCore` is generated as always: `Engine` gains one throwing method per entry (`timeGet`,
`physicsRaycast`, …) and `Phase`, `ShapeKind`, `NavPathStatus`, `NavQueryState` are generated enums.
`CyberdyneKit` gets hand-written facades, one file per service, over those methods:

```swift
Input.action("select").justPressed            // F or U; throws CyberdyneError.status(.permissionDenied, …) where refused
Input.pointer().position                      // U
Camera.active()?.ray(through: pointer.position)
Physics.raycast(ray, filter: .init(mask: units))  -> Hit?
Physics.overlap(.box(halfExtents), at: pose)       -> [Entity]
Navigation.findPath(from:to:)                      -> Path
NavAgent(entity).move(to: point); NavAgent(entity).state.status == .arrived
Audio.play(Audio.cue("ui.click"), at: point)       -> Voice?
Spawn.prefab("units/tank.cyprefab").instantiate(at: pose) -> Entity
Time.now.tick
```

`GameTypes.swift` (declared here) holds what more than one facade needs: `Pose`, `Ray`, the tuple
conversions C arrays import as, and `GameServices.engine()`, which throws UNAVAILABLE before
bring-up rather than trapping.

## Entry reference

`B:` is the backend method the thunk calls after its checks. Every entry also returns the D6 boundary
errors; the column lists only its domain errors.

| Entry | Phases | Deterministic in F | Out / ownership | Domain errors | B: |
|---|---|---|---|---|---|
| `time_get` | N F U | yes | `CyTime` (sized) | — | (clock; no backend) |
| `input_find_action` | N F U | yes | `CyInputAction` | NOT_FOUND | `find_action` |
| `input_action_state` | N F U | yes | `CyInputActionState` (sized) | NOT_FOUND, OUT_OF_RANGE | `action_state` |
| `input_action_state_by_name` | N F U | yes | as above | NOT_FOUND, OUT_OF_RANGE | `find_action` + `action_state` |
| `input_pointer` | N U | n/a | `CyInputPointer` (sized) | OUT_OF_RANGE | `pointer` |
| `input_modifiers` | N U | n/a | `uint32_t` mask | OUT_OF_RANGE | `modifiers` |
| `input_find_context` | N F U | yes | `CyInputContext` | NOT_FOUND | `find_context` |
| `input_push_context` | N F U | yes (next tick) | — | NOT_FOUND, ALREADY_EXISTS, OUT_OF_RANGE | `push_context` |
| `input_pop_context` | N F U | yes (next tick) | — | NOT_FOUND, OUT_OF_RANGE | `pop_context` |
| `camera_active` | N U | n/a | `CyCamera` | UNAVAILABLE | `active` |
| `camera_view` | N U | n/a | `CyCameraView` (sized) | NOT_FOUND | `view` |
| `camera_screen_to_ray` | N U | n/a | `CyRay` | NOT_FOUND | `screen_to_ray` |
| `camera_world_to_screen` | N U | n/a | caller's `CyScreenPoint[count]` | NOT_FOUND | `world_to_screen` |
| `camera_set_target` | N F U | presentation | — | NOT_FOUND | `set_target` |
| `camera_set_pose` | N F U | presentation | — | NOT_FOUND | `set_pose` |
| `camera_clear_pose` | N F U | presentation | — | NOT_FOUND | `clear_pose` |
| `physics_raycast` | N F U | yes | `CyPhysicsHit` + `bool` | UNAVAILABLE mid-step | `raycast` |
| `physics_raycast_all` | N F U | yes, ordered | caller's `CyPhysicsHit[capacity]`, total | BUFFER_TOO_SMALL | `raycast_all` |
| `physics_shape_cast` | N F U | yes | `CyPhysicsHit` + `bool` | INVALID_ARGUMENT for a bad shape | `shape_cast` |
| `physics_overlap` | N F U | yes, ordered | caller's `CyEntity[capacity]`, total | BUFFER_TOO_SMALL | `overlap` |
| `nav_find_path` | N F U | yes | caller's `float[3*capacity]`, `CyNavPathResult` | BUFFER_TOO_SMALL, NOT_FOUND (world) | `find_path` |
| `nav_request_path` | F | yes | `CyNavQuery` | NOT_FOUND (world) | `request_path` |
| `nav_poll_path` | F | yes | as `nav_find_path` | NOT_FOUND, BUFFER_TOO_SMALL | `poll_path` |
| `nav_cancel_path` | F | yes | — | NOT_FOUND | `cancel_path` |
| `nav_agent_configure` | N F | yes | — (structural once) | NOT_FOUND (entity) | `agent_configure` |
| `nav_agent_move_to` | F | yes | — | NOT_FOUND | `agent_move_to` |
| `nav_agent_stop` | F | yes | — | NOT_FOUND | `agent_stop` |
| `nav_agent_state` | N F U | yes | `CyNavAgentState` (sized) | NOT_FOUND | `agent_state` |
| `audio_find_cue` | N F U | presentation | `CyAudioCue` | NOT_FOUND | `find_cue` |
| `audio_play` | N F U | presentation; dropped while resimulating | `CyAudioVoice` (optional) | NOT_FOUND | `play` |
| `audio_stop` | N F U | presentation | — | — (idempotent) | `stop` |
| `audio_voice_playing` | N F U | presentation | `bool` (false on any error) | — | `playing` |
| `audio_find_bus` | N F U | presentation | `CyAudioBus` | NOT_FOUND | `find_bus` |
| `audio_set_bus_volume` | N F U | presentation | — | NOT_FOUND, OUT_OF_RANGE | `set_bus_volume` |
| `spawn_resolve` | N F U | yes | `CyPrefab` | NOT_FOUND, UNAVAILABLE (not resident outside N) | `resolve` |
| `spawn_instantiate` | N F | yes | root `CyEntity` (structural) | NOT_FOUND | `instantiate` |
| `spawn_instantiate_many` | N F | yes, all or nothing | caller's `CyEntity[count]` (structural) | NOT_FOUND | `instantiate_many` |
| `spawn_destroy` | N F | yes | — (structural) | NOT_FOUND | `destroy` |

## File ownership

Frozen by this change (the declaring author's; a change to any of them is a follow-up the three
implementers agree on, not an edit in passing): `cy_abi.h`, `abi_baseline.json`, `interface.cpp`,
`host.h`, `cy/abi/game/services.h`, `src/abi/src/game/thunks.h` and `services.cpp`, both
`CMakeLists.txt` files under `src/abi/`, `src/game_backend/CMakeLists.txt`, `src/CMakeLists.txt`,
`tools/gen/**`, the generated overlays, `test_game_services.cpp`, `test_layout.cpp`,
`CyberdyneKit/GameTypes.swift`, `Tests/CyberdyneKitTests/FakeEngine.swift` and `GameTypesTests.swift`.

| | A — input + camera | B — physics queries + navigation | C — audio + spawning + time |
|---|---|---|---|
| Backend contract (read-only) | `cy/abi/game/input.h`, `camera.h` | `physics.h`, `navigation.h` | `audio.h`, `spawn.h`, clock in `services.h` |
| Thunks | `src/abi/src/game/input_thunks.cpp`, `camera_thunks.cpp` | `physics_thunks.cpp`, `navigation_thunks.cpp` | `audio_thunks.cpp`, `spawn_thunks.cpp`, `time_thunks.cpp` |
| Thunk tests (`unit.abi`) | `src/abi/tests/test_game_input.cpp`, `test_game_camera.cpp` | `test_game_physics.cpp`, `test_game_navigation.cpp` | `test_game_audio.cpp`, `test_game_spawn.cpp`, `test_game_time.cpp` |
| Adapters | `src/game_backend/src/input_backend.cpp`, `camera_backend.cpp` + their headers | `physics_backend.cpp`, `navigation_backend.cpp` + headers | `audio_backend.cpp`, `spawn_backend.cpp` + headers |
| Adapter suites (`integration.game_backend_<service>`) | `src/game_backend/tests/test_input_backend.cpp`, `test_camera_backend.cpp` | `test_physics_backend.cpp`, `test_navigation_backend.cpp` | `test_audio_backend.cpp`, `test_spawn_backend.cpp` |
| Swift facades | `CyberdyneKit/Input.swift`, `Camera.swift` | `Physics.swift`, `Navigation.swift` | `Audio.swift`, `Spawn.swift`, `Time.swift` |
| Swift tests | `Tests/CyberdyneKitTests/InputTests.swift`, `CameraTests.swift` | `PhysicsTests.swift`, `NavigationTests.swift` | `AudioTests.swift`, `SpawnTests.swift`, `TimeTests.swift` |
| Wiring | — | — | `PhaseScope` and `GameClock` updates in `src/abi/src/module.cpp` (`BehaviourRuntime::fixed_update`, a new `frame_update`), `CyberdyneKit/BehaviourBridge.swift` (`frame_update` → `onUpdate`) |
| Docs | its rows of `src/abi/README.md`'s 1.3 table | its rows | its rows, plus `bindings/swift/README.md` |

No file is in two columns. Test registration needs no CMake edit: `src/abi/tests/CMakeLists.txt`
compiles `test_game_<group>.cpp` when it exists, and `src/game_backend/CMakeLists.txt` registers
`test_<service>_backend.cpp` when it exists.

## Implementation contract (each implementer)

1. Replace each stub in place, in this order: null engine → `require_phase` with the header's
   `[N F U]` → backend bound → required pointers → `read_sized` / `write_sized` → resimulation drop
   (presentation writes only) → backend call → epoch bump (structural calls only) →
   `cy::abi::clear_last_error()` on success.
2. In the adapter, map every domain failure to the D6 code with a message naming the missing thing,
   and assert any enum mirrored in C against the engine's own (`CyNavPathStatus` against
   `cy::navigation::NavPathStatus`, `CyNavQueryState` against `QueryState`) with `static_assert`.
3. C++ tests of each entry: at least one success case, one case per domain error, the phase refusal,
   UNAVAILABLE with no backend, and a short-`struct_size` caller — in `unit.abi` against a fake
   backend; the adapter's behaviour against the real server in its integration suite, including the
   D4 ordering and determinism claims (the same query twice gives byte-identical output; equal
   distances come back in entity order).
4. Swift tests through `CyberdyneKit` with `FakeEngine`: each facade call reaches its entry with the
   right arguments, converts the answer, and turns a refusal into `CyberdyneError.status`.
5. Prove each new test red: apply a mutation to the code under test (for example, drop the phase
   check, swap the sort key, skip the epoch bump), run the suite, restore, and md5-verify the file.
6. Keep `just quality-abi`, `just generate-swift --check`, the Rust SDK check, clang-format,
   clang-tidy and `quality-swift-format` green; build in debug and dev.

## Integration obligations outside the three columns

- **Binding.** The game host (M12's, and `samples/04-character`'s when it migrates) constructs the
  adapters over its servers and calls each adapter header's `bind(host, &adapter)`.
- **Clock.** The host updates `host.game.clock` — tick, fixed delta, frame delta, interpolation,
  resimulation and pause flags — at the frame and tick boundaries it already has; C's wiring task
  covers the behaviour runtime, the host covers its own loop.
- **Agent locomotion.** The navigation adapter owns stepping the crowd for configured agents and
  writing the result where the entity's motion comes from (a character controller when it has one,
  its transform otherwise). That is B's, inside `navigation_backend.cpp`.

## Risks / Trade-offs

- **Lockstep input.** An RTS order issued in U from a pointer click must reach the simulation as a
  command, not as a direct call in F, or peers diverge. This change makes the verbs available; a
  `gameplay_submit_command` entry over `gameplay-framework`'s command stream is the next append and
  M12 needs it before multiplayer. Single-player is correct without it: U reads, F acts on what the
  game recorded.
- **Shape casts need shapes.** The physics server sweeps a `ShapeHandle`; the adapter keeps a small
  cache keyed by `CyShape` bytes rather than creating a shape per call.
- **Synchronous scene instantiation.** A large scene asset spawned in F costs its whole
  instantiation in that tick. Budgeted loading stays a `SceneTree` concern; the ABI exposes the
  synchronous verb only.
- **"Primary view".** `camera_active` answers for the primary view. Split-screen RTS is out of scope;
  a per-view entry would be an append.
- **The funnel zig-zags off-axis (found by B, not fixed here).** `cy::navigation::straighten` on a
  grid of quads returns portal corners rather than a taut line whenever the path is not along a
  cell row — (3,3) to (13,13) comes back as (4,2), (4,4), (6,4), … — because `funnel_segment`'s
  side tests have the opposite sign to `triarea2`'s "positive is left" convention. Existing
  navigation tests only straighten along a row, where both signs agree. Agents still arrive, but
  walk a staircase; `integration.game_backend_navigation` keeps its paths on cell rows and does not
  assert tautness off-axis. The fix belongs in `src/navigation/src/query.cpp` with a diagonal
  regression case in `unit.navigation`, before M12 moves units.
- **Physics tie-break.** The server exposes no body creation order, so the third sort key is the body
  handle (slot index, then generation): the server's deterministic allocation order, equal to
  creation order until a slot is reused.
- **Stubs while building.** Until each group landed, its entries answered NOT_IMPLEMENTED: callable,
  failing as values, surfaced by a Swift facade as thrown errors. All three groups land in this
  change, and the stub helper is deleted with the last stub.
