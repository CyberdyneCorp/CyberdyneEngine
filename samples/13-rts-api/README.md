# samples/13-rts-api — an RTS unit, written in Swift, through ABI 1.3, 1.5 and 1.6

The end-to-end proof of `add-swift-game-api`. A Swift behaviour runs a small RTS: a camera the
keyboard and the screen edges pan, a unit picked under the pointer, sent to a clicked ground point,
heard arriving, and a new unit built with a key. **Every one of those decisions is Swift calling the
engine.** The C++ host binds six adapters and loads content; it does not carry a single value
between the game and a server.

```
just run-sample rts-api                    420 frames of scripted play, headless, then the report
just run-sample rts-api --no-behaviours    the negative control: the same host with no game
just run-sample rts-api --no-systems       the scheduler's control: Swift systems never installed
just run-sample rts-api --no-ui            the interface's control: no interface, so no HUD
ctest -R rts_api_sample                    the test: the claims below, the controls, and two runs
```

ABI 1.5 (`add-swift-m12-gaps`) adds four things the game does, each through the engine rather
than the host: the commander and a scout are behaviours on level NODES, so the scene tree's pump
drives their `onEnterTree` and `onReady` and resolves their `@Node` paths; `trainUnits` is a Swift
`@System` the engine's scheduler runs every fixed tick, ordered against a native system reading the
same column; the scout walks a hero with a character controller and jumps once; and it kicks a
crate with an impulse.

ABI 1.6 (`add-swift-ui-bindings`) adds the HUD. The commander mounts, in CyberUI's store, the
resource bar, selection panel with health bars and minimap of `samples/13-rts-selection` — written in
Swift (`game/Hud.swift`), element for element — plus a Build button. Every frame it writes the HUD
from the game: gold less what was spent, food against the cap, the selected unit's health, a dot per
unit and the camera's frame on the minimap. A click on Build reaches the commander through the
engine's `ui_event`, and the next fixed step builds a worker; a left click over the HUD is the HUD's,
not a selection, because the commander asks `UI.hitTest` first.

## What is here

| | |
|---|---|
| `game/` | **The game.** `Commander.swift` (the squad, the selection, the orders, the build key and button, the HUD's model, its tree callbacks), `Hud.swift` (the HUD, and `HudShowcase` for `render.rts_api_hud`), `Scout.swift` (a character-controlled hero and a kicked crate), `RtsCamera.swift` (panning), `Contract.swift` (content names, collision layers, the report components and `Veterancy`), `Game.swift` (the module's entry points and the `trainUnits` system). |
| `host/rts_host.*` | Servers, the adapters bound on the ABI host, the scene bridge, the schedule with the script systems in it, the Swift module, the interface (store, text, `UiAdapter`), and the frame loop. |
| `host/level.*` | Content built in code: the ground, a navigation tile, the worker prefab, the arrival click, the input actions, and the `/Level` nodes with the crate. |
| `host/units.*` | Plumbing for navigation agents (an agent's position is its scene node, and it has a kinematic capsule on collision layer 1), the one entity-to-body map, and the native `Veterancy` reader. |
| `host/script.*` | The scripted player: synthetic key and mouse events, aimed with the camera projection. |
| `tests/test_rts_api_sample.cpp` | `integration.rts_api_sample`, declared from this directory's `CMakeLists.txt`. |
| `tests/test_rts_api_hud_device.cpp` | `render.rts_api_hud`: the Swift HUD against `samples/13-rts-selection`'s C++ HUD, as a primitive stream and on a device, and with its button against `tests/references/rts_api_hud.png`. |

## What the game calls, and in which phase

| The game does | Through | Phase |
|---|---|---|
| resolves `units/worker` and `unit.arrived`, spawns two workers, makes them agents | `spawn_resolve`, `spawn_instantiate_many`, `audio_find_cue`, `nav_agent_configure` | none (`onCreate`) |
| pans the camera with WASD and with the pointer at a screen edge | `input_action_state_by_name`, `input_pointer`, `camera_active`, `camera_view`, `camera_set_target` | frame (`onUpdate`) |
| left click: selects the unit under the pointer | `camera_screen_to_ray`, `physics_raycast` with the unit layer's mask | frame |
| right click: records the ground point under the pointer | `camera_screen_to_ray`, `physics_raycast` with the ground layer's mask | frame |
| sends the selected unit to the recorded point | `nav_agent_move_to` | fixed (`onFixedUpdate`) |
| plays the arrival cue where a unit stops | `nav_agent_state` (the one-tick ARRIVED event), `audio_play` | fixed |
| builds a worker when B goes down | `input_action_state_by_name`, `spawn_instantiate`, `nav_agent_configure` | fixed |
| counts the tree reaching it, finds `../Barracks` and `../Crate` | the vtable's `enter_tree` and `ready`, `node_find` | none (the pump) |
| trains every unit one tick | `register_system`, then the engine's scheduler over `world_chunks` | fixed (Simulation stage) |
| walks the hero, jumps once | `character_create`, `character_move`, `character_state` | none, then fixed |
| kicks the crate | `physics_apply_impulse`, `physics_get_velocity` | fixed |
| mounts the HUD | `ui_root`, `ui_create`, `ui_set_layout`, `ui_set_style`, `ui_set_text`, `ui_set_progress`, `ui_set_visibility` | none (`onCreate`) |
| writes the HUD from the game | `ui_set_text`, `ui_set_progress`, `ui_set_visibility`, `ui_set_layout`, `ui_set_style` — only what changed | frame |
| keeps a click on the HUD from selecting | `ui_hit_test` | frame |
| hears the Build click, builds | the vtable's `ui_event`, then `spawn_instantiate` | frame, then fixed |

The pointer and the camera are refused in a fixed step and spawning is refused in a frame, so a
click is recorded in `onUpdate` and acted on in the next `onFixedUpdate`. That is the pattern
`openspec/changes/add-swift-game-api/design.md` gives for an RTS; a lockstep game would put a command
stream between the two.

## What the test checks

`integration.rts_api_sample` runs the host as a separate process, so each run loads the module into a
fresh image, and reads the report it prints:

* the keyboard pan moved the camera about +6 m and the edge pan about -6 m;
* the entity the scripted player clicked is the one the game selected;
* the game issued one order, the unit arrived within its arrival distance of the clicked point, and
  the unit that was not clicked did not move;
* the game saw one arrival, the audio adapter accepted the cue, and the audio server had a voice;
* the build key made a third worker: three `Worker` nodes, three agents, three bodies;
* with `--no-behaviours` none of that happens;
* two runs print the same report;
* ABI 1.5: `onEnterTree` and `onReady` reached the commander exactly once and both `@Node` paths
  resolved; `trainUnits` ran on all 420 fixed ticks, the native reader saw three units the first of
  which had served 419, and the scheduler ordered the two by their declarations; the hero walked
  about 4 m, was airborne once and stands on the ground; the crate left at 5 m/s and slid;
* with `--no-systems` the system never runs and nothing else changes, and with `--no-behaviours`
  no tree callback, hero or kick happens;
* ABI 1.6: the commander mounted 41 elements; after 420 frames the store shows gold 1150, food 4/10,
  "Selected: 1 unit" with the hurt worker's 64/100 and a 32-pixel fill of a 50-pixel bar, and one
  minimap dot per unit; the scripted click on Build was routed as one click, heard by the
  commander's `onUIEvent`, and built the fourth worker; the unit selected at frame 81 is still
  selected, because the click was the HUD's;
* with `--no-ui` there is no HUD, the same click on the same pixel builds nothing and lands on the
  world (dropping the selection), and nothing else changes.

It was proven red by breaking `physics_raycast` (the hit is never written back: selection, order,
arrival and cue fail) and `audio_play` (every play dropped: the cue and voice checks fail), each
restored and md5-verified. The ABI 1.5 half was proven red by a scheduled body that never runs
(`most` stays 0); the rest of its mutations are in
`openspec/changes/add-swift-m12-gaps/evidence/falsification.md`.

## Known limits

* The navigation funnel returns a staircase rather than a taut line off a cell row
  (`design.md`, "Risks"). Units still arrive; the test checks the arrival, not the path.
* A prefab here is a `SceneDescription`. Spawning a cooked `EntityTemplate` through the ABI is a
  follow-up recorded in the change's tasks.
* Single-player. The click-to-order hand-off is in-process; `gameplay_submit_command` is the append
  a lockstep RTS needs.
