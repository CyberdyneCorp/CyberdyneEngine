# `src/abi/` — layer 6

The stable flat C ABI exported to scripting languages and extensions. Symbols are `cy_*`; the ABI is
the boundary at which C++ types stop.

**Governed by**: `native-abi`. Landed at M4.

## The one exported symbol

```c
const CyInterface* cy_get_interface(uint32_t requested_major, uint32_t requested_minor);
```

Everything else is reached through the returned table. A module is a shared library exporting
`cy_module_entry`, described by a `module.toml`; the loader is `cy/abi/module.h`.

## What is here

| | |
|---|---|
| `include/cy/abi/cy_abi.h` | **The ABI.** Pure C. Opaque handles, POD structs, function-pointer table, `CyResult` codes. The only file a Swift, Rust or C consumer needs. |
| `include/cy/abi/errors.h` | `cy::Expected<T, Error>` → `(CyResult, out-parameter)`, and the thread-local last error |
| `include/cy/abi/host.h` | What is behind `CyEngine`, `CyWorld` and `CyBehaviourType` |
| `include/cy/abi/var.h` | `CyVar` marshalling: the reference-counted heap payload and the inline constructors |
| `include/cy/abi/module.h` | Manifests, images, **generations**, and the reload sequence |
| `include/cy/abi/live_reload.h` | Performing that sequence against a runtime that is still ticking |
| `abi_baseline.json` | The committed machine-readable description. The gate diffs against it; the Swift overlay and the Rust SDK are generated from it. |
| `abi_approvals.toml` | Reviewed, recorded exceptions to the gate. Empty, and meant to stay that way. |

## What ABI 1.1 added, and why

M5's task 1.2. The table had 31 entries and no way to look at a component the *engine* registered,
no way to read a chunk, and no `CyStage` or `CySeverity` — so two Swift enums were hand-copied from
engine enums with nothing to check them against, and **one of them was wrong**: `Severity` carried
six enumerators against the engine's three, `Log.info` put 2 on the wire, and every informational
line from a Swift behaviour arrived in the engine's log as `[error]` on a run that reported green.

| Added | Why |
|---|---|
| `CySeverity`, `CyStage` | The two enums the overlay copied by hand. Generated now, and `src/interface.cpp` asserts every value — and the stage *count* — against `cy::DiagnosticSeverity` and `cy::ecs::Stage`. |
| `CY_VAR_I8` … `CY_VAR_U64` | One integer width was not enough: every reflected engine component disagrees with it. The payload is always the widened value in `as_i64`; the tag is the storage width, and a write that does not fit is refused rather than truncated. |
| `world_component_count`, `world_component_info`, `world_component_field` | A generated inspector is "enumerate what is there, describe each field, read it, write it". Two of those four did not exist. |
| `world_parent`, `world_set_parent`, `world_child_count`, `world_child` | An outliner. |
| `world_chunks`, `CyChunk` | The bulk read. `Systems.swift` said it plainly: at 1.0 "NONE of them hands a module a chunk". |
| `service_open` … `service_poll`, `CyServiceRequest`, `CyServiceEvent` | ABI 1.2's asynchronous, cancellable editor-service envelope. Payload schemas remain independently versioned. |
| `vfx_effect_parameter_set`, `vfx_effect_parameter_get` | ABI 1.4's typed access to an exposed parameter on one playing scene effect entity. The runtime binds the VFX owner to the host; a build without VFX reports `UNAVAILABLE`. |

**A component the engine registered is now reachable through every existing entry**, not only
through the three above. `CyWorld_T::record_or_import` turns the engine's `reflect::TypeInfo` into
the same `ComponentRecord` a module registration produces, once, on first use — so
`component_get_var`, the typed fast paths and `world_borrow_component` all work on a scene's
`LocalTransform` without thirteen thunks learning about a second kind of component. See
`include/cy/abi/host.h`.

## What ABI 1.3 adds, and why

`add-swift-game-api`. Up to 1.2 a Swift game reached input, physics, cameras and audio only through
components a C++ host carried for it (`samples/04-character/game/Contract.swift`). An RTS needs the
verbs themselves, so 1.3 appends 38 entries — time, input, camera, physics queries, navigation,
audio, spawning — and `CyBehaviourVTable.frame_update`. The per-entry reference (phases,
determinism, ownership, errors) is `openspec/changes/add-swift-game-api/design.md`; the header states
the shared rules once, above `CyPhase`.

| | |
|---|---|
| Phases | `CyPhase`: none, fixed update, frame update. Each entry lists `[N F U]`; any other phase is `CY_RESULT_PERMISSION_DENIED`, in every build. |
| Determinism | Everything allowed in a fixed step answers from simulation state with a total order on every list. The pointer, the modifier keys and camera reads are frame-update only. |
| Ownership | Values, caller-owned structs, caller buffers with `world_chunks`' sizing pattern. Service handles are integers, never engine addresses. |
| The seam | Thunks in `src/abi/src/game/`, one file per group; each calls an abstract backend (`include/cy/abi/game/`) bound on `CyEngine_T::game`. `cy_abi` names no server. The adapters are `src/game_backend/`. |

Every 1.3 entry is implemented; none answers `CY_RESULT_NOT_IMPLEMENTED`. `unit.abi` covers the
table shape, the phase rule, `struct_size` in both directions and the 1.3 layouts
(`test_game_services.cpp`), then each group against fake backends.

### Input and camera

| Entries | Phases | Behind them | Notes |
|---|---|---|---|
| `input_find_action`, `input_action_state`, `input_action_state_by_name` | N F U | `cy::game_backend::InputAdapter` over `InputServer` | The state is the record `resolve_tick` wrote for the last tick, never a device: a press and a release inside one tick read `press_count` 1 and `release_count` 1, and a replay fed the same events reads the same bytes. An action index the user lacks is NOT_FOUND; a user the server lacks is OUT_OF_RANGE. |
| `input_pointer`, `input_modifiers` | N U | the mouse and keyboard the server routes to that user | Device state. Position and held buttons are the devices' as of the last resolved tick; the edges (buttons pressed and released, motion, wheel) are "since the previous frame update", which the server does not keep, so the host calls `InputAdapter::observe_pending()` before every `resolve_tick` and `begin_frame()` once per frame. `IN_WINDOW` and `OVER_UI` are the host's (`set_pointer_focus`). No pointing device is OK with `PRESENT` clear. The engine has no Super key, so `CY_INPUT_MOD_SUPER` is never set. |
| `input_find_context`, `input_push_context`, `input_pop_context` | N F U | the server's registered `MappingContext`s and each user's stack | A handle is the server's `ContextHandle` bits. A push marks the user's table dirty and is resolved at the next tick, never mid-tick. ALREADY_EXISTS for a context already on the stack, OUT_OF_RANGE for a full one. |
| `camera_active`, `camera_view`, `camera_screen_to_ray`, `camera_world_to_screen` | N U | `cy::game_backend::CameraAdapter` over `CameraServer` and the `cy::camera` projections | A camera is a `RigHandle`'s bits; a destroyed rig is NOT_FOUND. The host names the primary view (`set_primary_view(rig, RenderViewRequest)`); reads produce that view from the rig's last evaluation. The pick ray starts on the near plane and `max_distance` reaches the far plane (an infinite far plane is `screen_rect_to_frustum`'s 10^6 m stand-in). `ON_SCREEN` also requires the point to be in front of the near plane. |
| `camera_set_target`, `camera_set_pose`, `camera_clear_pose` | N F U | the rig: `set_target`, `override_pose`, `clear_pose_override` | Presentation: OK and nothing while `CY_TIME_RESIMULATING` is set. `set_target` binds the focus (an entity by its `CyEntity` value, which the host samples like every binding), cuts when `blend_seconds` is zero, and records yaw, pitch and distance as the rig's `CameraAdapter::Framing`: the server has orbit intents (look deltas, zoom) and no absolute orbit, so the host's rig reads it. A rig whose definition has no Target node refuses a target (INVALID_ARGUMENT). An all-zero quaternion is the identity. |

Tests: `unit.abi` (`test_game_input.cpp`, `test_game_camera.cpp`, against fake backends) and
`integration.game_backend_input` (a real `InputServer`, live and replayed) /
`integration.game_backend_camera` (a real rig; a world point projected and cast back lies on the ray,
and a resimulated `camera_set_target` changes nothing).

### Time, audio and spawning

| Entries | Phases | Behind them | Notes |
|---|---|---|---|
| `time_get` | N F U | the host clock (`GameServices::clock`), no backend | `frame_delta` and `interpolation` are zero in F. `BehaviourRuntime::fixed_update` runs in F and the new `frame_update` in U, each under a `PhaseScope`. |
| `audio_find_cue`, `audio_play`, `audio_stop`, `audio_voice_playing`, `audio_find_bus`, `audio_set_bus_volume` | N F U | `cy::game_backend::AudioAdapter` over `AudioServer` and its `BusGraph` | Presentation: `play`, `stop` and `set_bus_volume` are OK and do nothing while `CY_TIME_RESIMULATING` is set. Handles carry the server's generations, so a stale voice stops nobody. Fades, fade-ins and attached voices are advanced by `AudioAdapter::update`, once per frame. No free voice is OK with a null voice. |
| `spawn_resolve` | N F U (loads only in N) | `cy::game_backend::SpawnAdapter` over `SceneTree` | A prefab is a `SceneDescription` (node 0 the root). Outside N a prefab that is not resident is UNAVAILABLE. |
| `spawn_instantiate`, `spawn_instantiate_many`, `spawn_destroy` | N F | as above | Structural: each success bumps the world's epoch. A batch is all or nothing and writes the caller's roots only on success; destroy runs the destroy callbacks child first. A null parent attaches under the tree's root. |

Tests: `unit.abi` (`test_game_time.cpp`, `test_game_audio.cpp`, `test_game_spawn.cpp`, against fake
backends) and `integration.game_backend_audio` / `integration.game_backend_spawn` against the real
servers.

### Physics queries and navigation

| Entries | Phases | Behind them | Notes |
|---|---|---|---|
| `physics_raycast`, `physics_raycast_all`, `physics_shape_cast`, `physics_overlap` | N F U | `cy::game_backend::PhysicsQueryAdapter` over `PhysicsServer`'s const queries | A null filter is layer 0, every layer, nothing ignored. Lists are ordered by distance, then entity, then body handle (overlaps by entity, each once), and the single raycast is the head of that order, so a fixed step gets the same answer on every run. The ignore list's entities become bodies through the embedder's `EntityBodies`. UNAVAILABLE while the step runs, in every build. Shapes are cached by their `CyShape`; the first use of a new shape creates it, so do it on the game thread or `prewarm()` it. Safe from a job worker. |
| `nav_find_path` | N F U | `cy::game_backend::NavigationAdapter`: `find_path` + `straighten` over the world's mesh | No path is OK with `CY_NAV_PATH_FOUND` clear. Per-call scratch, so safe from a job worker. An unbound world is NOT_FOUND. |
| `nav_request_path`, `nav_poll_path`, `nav_cancel_path` | F | the world's `PathQueue` | Delivered a fixed number of ticks after submission whatever the load. A query id is the world in the high 32 bits and the queue id plus one in the low. A READY path that does not fit is held, not lost: BUFFER_TOO_SMALL with `point_count`, and a larger buffer takes it. A cancelled query polls CANCELLED once, then NOT_FOUND. |
| `nav_agent_configure` | N F | `NavAgent` component + the world's `Crowd` | Structural the first time (adds `NavAgent`, bumps the epoch). |
| `nav_agent_move_to`, `nav_agent_stop` | F | `NavigationAdapter::update` | Recorded at the call and applied in the tick's navigation update, in entity order, so script order within a tick cannot matter. The adapter is the agent system for configured agents: it steps the queue, takes paths, steers through the crowd and moves agents (through `AgentMotion` when the host binds one). |
| `nav_agent_state` | N F U | `NavAgent` + the crowd | `CY_NAV_AGENT_EVENT` is set by the update that arrived or failed and cleared by the next: exactly one tick. |

Tests: `unit.abi` (`test_game_physics.cpp`, `test_game_navigation.cpp`, against fake backends) and
`integration.game_backend_physics` (reference server) / `integration.game_backend_navigation` (a real
mesh, queue and crowd).

## What ABI 1.5 adds, and why

`add-swift-m12-gaps`. M12's RTS needs three things 1.4 could not give a Swift game: systems the
engine schedules, the tree callbacks and `@Node`, and physics beyond queries. 1.5 appends eleven
entries and five `CyBehaviourVTable` members; the per-entry reference is
`openspec/changes/add-swift-m12-gaps/design.md`.

| Entries | Phases | Behind them | Notes |
|---|---|---|---|
| `register_system` | N | the host's system registry; `cy::abi::ScriptSystems` (`include/cy/abi/systems.h`) | One record per name per generation. `ScriptSystems::install(schedule)` adds one `ecs::Schedule` entry per name with the declared access, so the scheduler orders it against native systems by `jobs::AccessSet`'s rules; the entry resolves the current generation's record on every run, so a reload changes what runs without touching the schedule. A later generation that changes a system's stage or access is refused (UNSUPPORTED) and so is its reload (`ReloadFailure::SystemChanged`). `ScriptSystems::run` sets the stage's phase once; the world is held iterating while a body runs. |
| `node_find` | N F U | `cy::abi::game::SceneBackend`, implemented by `cy::game_backend::ScriptSceneBridge` over `SceneTree` | Relative to a node, or absolute. NOT_FOUND leaves the output alone. |
| `physics_apply_force`, `physics_apply_impulse`, `physics_apply_torque`, `physics_set_velocity` | N F | `PhysicsBodyBackend`, implemented by `PhysicsBodyAdapter` | A body that cannot move is INVALID_ARGUMENT rather than silently ignored; force and torque wake the body; UNAVAILABLE during the step. |
| `physics_get_velocity` | N F U | as above | Either output may be null. |
| `character_create`, `character_destroy` | N F | `CharacterBackend`, implemented by `CharacterAdapter` over `cy::physics::CharacterController` | One per entity; zero fields are the description's defaults; the body carries the entity. |
| `character_move` | F | as above | One step of the clock's fixed delta, applied at the call. |
| `character_state` | N F U | as above | `struct_size` both ways. |
| vtable `enter_tree`, `ready`, `enable`, `disable`, `exit_tree` | the pump's | `BehaviourRuntime::tree_callback`, called by `ScriptSceneBridge` from the scene tree's pump | Through the creating generation's vtable; null for a module compiled before 1.5. |

Tests: `unit.abi` (`test_game_systems.cpp`, `test_game_scene.cpp`, `test_game_bodies.cpp`,
`test_game_character.cpp`, the 1.5 layouts and table shape), `integration.abi_reload` (a C module's
system run through the scheduler, rebound by a reload, and a reload that moves it refused), and
`integration.game_backend_bodies` / `_character` / `_scene` against the real servers and tree.

## What ABI 1.6 adds, and why

`add-swift-ui-bindings`, issue #91's follow-up to #102. CyberUI reached the screen with no way for a
module to touch it: the strategy HUD was C++ game code because nothing else could build it. 1.6
appends fourteen `ui_*` entries and one `CyBehaviourVTable` member; the reference is
`openspec/changes/add-swift-ui-bindings/design.md`.

| Entries | Phases | Behind them | Notes |
|---|---|---|---|
| `ui_root`, `ui_create`, `ui_destroy` | N U | `cy::abi::game::UiBackend` (`include/cy/abi/game/ui.h`), implemented by `cy::game_backend::UiAdapter` (`cy::game-backend-ui`, behind `CY_UI`) over the embedder's `ElementStore` | Five kinds: panel, label, image, progress bar, button. `CyUiElement` is the store's `ElementId` flat, generation high, so a destroyed element's handle is NOT_FOUND rather than its slot's next occupant. A module reaches the root and the elements it created; the embedder's own (the developer console) are NOT_FOUND. |
| `ui_set_layout`, `ui_set_style`, `ui_set_text`, `ui_set_image`, `ui_set_progress`, `ui_set_visibility`, `ui_set_opacity` | N U | as above | `CyUiLayout` and `CyUiStyle` read zero as each field's default, so a zeroed layout is the engine's (a content-sized, stretching flex child). A write the kind does not take, a NaN, or an opacity outside [0, 1] is INVALID_ARGUMENT; a progress fraction is clamped. Each marks the dirty state `samples/13-rts-selection/hud.cpp` marks for the same change. |
| `ui_element_rect`, `ui_hit_test`, `ui_focus`, `ui_set_focus` | N U | as above | A hit is the topmost element under the point, or its nearest ancestor the module created; over the world it is null, not a failure. Only a button takes focus. |
| vtable `ui_event` | U | `BehaviourRuntime::ui_event`, called by the embedder with what `UiAdapter::route_pointer` queued | A press and a release of the left button over one button is a CLICK; a focus change is BLUR then FOCUS. Delivered to every live instance on the element's owner, through the creating generation's vtable; null for a module compiled before 1.6. |

**The interface is presentation**: a fixed step is refused, so nothing in the simulation can depend
on an element and a resimulated tick cannot build one twice. A click arrives in the frame; a game
acts on it in the next fixed step, as with a key.

Tests: `unit.abi` (`test_game_ui.cpp`: the phase rule, the unbound backend, each entry's checks, the
table shape, `ui_event` dispatch and the pre-1.6 prefix; `test_layout.cpp`: the 1.6 layouts),
`integration.game_backend_ui` (the adapter over a real store), and `render.rts_api_hud` (a Swift HUD
against the C++ one).

## Reload while the runtime is live

`include/cy/abi/live_reload.h`, M5's task 1.1. `module.h` has the reload *sequence* and M4 proved
it; nothing performed it against a runtime that had not stopped. `LiveReload` is the three things
that were missing — noticing a new generation on disk, deciding that a frame boundary is one
(`apply_at_frame_boundary` refuses while the world is iterating), and remembering what happened —
and `integration.abi_live_reload` is a tick loop that never stops while the image under it changes.

## The compatibility gate

```
just quality-abi              # the header against abi_baseline.json
just quality-abi --selftest   # prove the gate still refuses a reorder, a removal and a break
just quality-abi --update     # accept the current header as the new baseline
```

`native-abi` requires the description to be diffed in CI and any non-append to fail. design.md §1
fixes *when*: **with the first exported symbol, not the first consumer** — the obligation starts the
moment anything links the table, and by M5 that is the editor.

Registered as `integration.abi_baseline` (the header matches the baseline) and `integration.abi_gate`
(the gate still fails when it should), so `just test-all` runs both.

### Adding an entry

1. Append it below the marker comment at the end of `CyInterface`. **Never above it, never
   between.**
2. Increment `CY_ABI_MINOR`.
3. Add the thunk at the corresponding position in `src/interface.cpp`.
4. `just quality-abi --update`, and commit the baseline with the change.

Anything else — a reorder, a removal, a changed signature, an inserted struct member, a changed enum
value — is refused, naming the entry and printing the approval stanza that would record the break.
The whole three-way demonstration (reorder stops, removal stops, append passes) is
`tools/abi/selftest.py`, run over the live header rather than over a copied fixture.

## Layout is computed, and then checked

`tools/abi/abi_describe.py` derives every struct's size and offsets from the declarations, under the
layout model the C ABI fixes. It does not compile anything: a description produced by compiling
would be a description of one toolchain on one machine, and the baseline is diffed across the whole
matrix.

That is a claim, so `tests/test_layout.cpp` asserts the compiler's `sizeof` and `offsetof` against
exactly those numbers. If the model is ever wrong on a platform, that test fails there rather than
the overlays being generated against a struct that does not exist.

## Hot reload: the image is retired, not unloaded

The reload sequence is in `include/cy/abi/module.h`, and it does everything that can fail before
anything that cannot be undone:

```
open the new image → serialize every instance through its own vtable → open the next generation and
run the new entry point → check every live type and schema → (point of no return) destroy and shut
down → recreate and restore by name
```

**There is no `dlclose`.** M4's spike measured that unloading a Swift image is unsafe whenever the
Swift runtime outlives the module — the foreign-type-metadata cache and the protocol-conformance
section list both keep pointers into the unloaded image, and the next image is mapped over the same
addresses. The amendment to `native-abi`'s "Hot reload" requirement carries the measurements.

The cost is stated rather than hidden: **58-85 kB of address space per reload, never reclaimed**, and
0.1-0.6 ms per reload, flat across 40 generations. A thousand reloads is under 90 MB. The mitigation,
if it is ever needed, is a process restart.

Every instance carries the generation that created it and is called through **that** generation's
vtable. Version-2 code run against a version-1 object reported `health = 17` and `mana = 3.5e18` with
no trap and no diagnostic; that is the failure this rule exists for.

## Not behind an option

`src/abi/` is compiled in every build and every profile. M3's gate found the shape of the mistake to
avoid: `CY_RENDERER_VULKAN` defaulted off, so the real backend was the one nothing tested. A table
that exists only in some configurations is a table whose gate runs only in some configurations.
