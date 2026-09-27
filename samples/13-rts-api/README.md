# samples/13-rts-api — an RTS unit, written in Swift, through ABI 1.3

The end-to-end proof of `add-swift-game-api`. A Swift behaviour runs a small RTS: a camera the
keyboard and the screen edges pan, a unit picked under the pointer, sent to a clicked ground point,
heard arriving, and a new unit built with a key. **Every one of those decisions is Swift calling the
engine.** The C++ host binds six adapters and loads content; it does not carry a single value
between the game and a server.

```
just run-sample rts-api                    420 frames of scripted play, headless, then the report
just run-sample rts-api --no-behaviours    the negative control: the same host with no game
ctest -R rts_api_sample                    the test: the claims below, the control, and two runs
```

## What is here

| | |
|---|---|
| `game/` | **The game.** `Commander.swift` (the squad, the selection, the orders, the build key), `RtsCamera.swift` (panning), `Contract.swift` (content names, two collision layers, one report component), `Game.swift` (the module's entry points). |
| `host/rts_host.*` | Servers, the six adapters bound on the ABI host, the Swift module, and the frame loop. |
| `host/level.*` | Content built in code: the ground, a navigation tile, the worker prefab, the arrival click, the input actions. |
| `host/units.*` | Plumbing for navigation agents: an agent's position is its scene node, and it has a kinematic capsule on collision layer 1. |
| `host/script.*` | The scripted player: synthetic key and mouse events, aimed with the camera projection. |
| `tests/test_rts_api_sample.cpp` | `integration.rts_api_sample`, declared from this directory's `CMakeLists.txt`. |

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
* two runs print the same report.

It was proven red by breaking `physics_raycast` (the hit is never written back: selection, order,
arrival and cue fail) and `audio_play` (every play dropped: the cue and voice checks fail), each
restored and md5-verified.

## Known limits

* The navigation funnel returns a staircase rather than a taut line off a cell row
  (`design.md`, "Risks"). Units still arrive; the test checks the arrival, not the path.
* A prefab here is a `SceneDescription`. Spawning a cooked `EntityTemplate` through the ABI is a
  follow-up recorded in the change's tasks.
* Single-player. The click-to-order hand-off is in-process; `gameplay_submit_command` is the append
  a lockstep RTS needs.
