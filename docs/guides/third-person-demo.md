# A third-person game

How `samples/14-third-person` is put together, so that a game of your own can be: a hero written in
Swift that moves, jumps, climbs and is heard; its legs animated through a blend space and planted
by foot placement; a camera that keeps off the walls; a cooked level; and the frame that draws it
all inside a 16.7 ms budget at 1080p. The sample's [README](../../samples/14-third-person/README.md)
is the reference for what it does; this guide is the route through the pieces.

![The run](../design/images/third-person-run.png)

## The split

| Where | What it decides |
|---|---|
| `samples/14-third-person/game/` (Swift) | everything a player would call the game: speeds, acceleration, turning, the jump and its coyote time, when to walk, run, jump, fall and land, where the camera looks from and how far back, which footstep is heard where, when a bird sings |
| `samples/14-third-person/host/` (C++) | content and engine plumbing: the level's colliders, the hero's rig and mesh, the camera rig's topology, the sounds, the adapters bound on the ABI host, the frame loop, the frame |
| `content/third-person/hillside.cymap` | the level: terrain, painting, foliage, placements ([its README](../../content/third-person/README.md)) |

Nothing crosses from the host to the game but what the engine's entries answer. Two components cross
the other way: the hero's facing (`HeroPose`), which the host turns the hero's node by, and the
report the sample prints.

## The hero, in Swift

`game/Hero.swift` is one behaviour on the `/Level/Hero` node. In `onCreate` it gives its own entity a
character controller and an animator:

```swift
try CharacterController.create(on: entity, CharacterController.Description(
    radius: 0.3, height: 1.8, maxSlopeRadians: 0.87, stepOffset: 0.35, start: Pose(position: spawn)))
try Animator.attach(to: entity, rig: "hero")
```

Every fixed tick it reads the stick, turns it into the camera's frame (the camera director's yaw,
from a component on the camera's node), eases its velocity toward the stick's, turns its facing by a
half-life, and moves:

```swift
try character.move(velocity: velocity, jump: jumping ? jumpSpeed : nil)
let after = try character.state
try animator.set("speed", to: groundSpeed)               // the blend space's axis
try animator.set("feet", to: Float(planted ? 1 : 0))     // foot placement's weight
```

The blend space and the feet are the rig's (below); the game sets two parameters and plays the
states the program has: `jump` the tick the jump is taken, `fall` once it is coming down, `land` on
the tick it touches down, and `locomotion` again when the landing has played.

In `onUpdate` it hears its footfalls — the clips carry a `footstep` event at each foot's contact,
its parameter the foot — and plays a step at that foot, stone or grass by the entity the capsule
stands on:

```swift
for event in try Animation.events(for: entity) where event.name == "footstep" {
    let at = try animator.jointPose(event.parameter < 0.5 ? "l_toe" : "r_toe").position
    try Audio.play(groundEntity != terrain ? "step.stone" : "step.grass", at: at)
}
```

## The rig: a blend space through foot placement

`host/hero_rig.cpp` is the cook a content pipeline would run: a skeleton, six clips authored as
functions of their phase, and a pose graph compiled with `graph::pose::compile_pose`, encoded as the
cooked records and loaded back by asset id through `AnimationLibrary`. The locomotion state is:

```
pose.clip idle (0 m/s) ─┐
pose.clip walk (1.8)   ─┼─ pose.blend1d  x_parameter=speed  sync=true ─ pose.foot_ik weight=feet ─ state
pose.clip run  (5.5)   ─┘
```

`sync` aligns the three clips by their MARKERS — `l_down` at the left foot's contact, `r_down` at the
right's — so at any speed between a walk and a run the two put their feet down together. Every clip
in a synchronised space needs the same markers; a clip without them sends the whole space back to
normalised time. Foot placement names the legs' joints (`hip_l`, `knee_l`, `ankle_l` and the right's),
its `pole` is the direction the knees bend (−Z here: the hero faces −Z), `ankle_height` is the
skeleton's, and `max_pelvis_drop` is 0.2 m: on a stair a walk's trailing foot can be three treads
below, and hips that dropped to reach it would climb squatting.

Two things a host must do for the feet to land:

* **Hand the animation system a ground.** `AnimationSystem::set_ground_probe` with an
  `animation::PhysicsGroundProbe` over the world the character walks in. It skips the character's
  own capsule by the entity in its user data, which `CharacterAdapter` puts there.
* **Keep the character's node's WORLD transform current.** The system places each foot's probe
  through the entity's `WorldTransform`; a host that sets the node's local transform must
  `SceneTree::propagate()` before the frame's Animation stage. Without it every probe is cast at the
  world's origin.

## The camera, and what keeps it off the walls

The game asks for a framing; the camera server's rig decides where that puts the camera. The rig
(`third_person_rig` in `host/tps_host.cpp`) is the composition `camera-system` names: a target, an
orbit, a shoulder offset, a collision node and a lens. Each frame:

```cpp
camera.begin_frame();
camera.evaluate(rig, context);          // the collision node publishes a sweep and a ray
resolver.resolve(camera);               // the physics world answers them for the next evaluation
```

`cy::game_backend::CameraCollisionResolver` (`cy/game_backend/camera_collision.h`) answers a
collision query with a sphere swept from the target toward where the rig wants the camera, and an
occlusion query with a ray back. `set_ignored` takes the framed character's own body. The node's
`PullIn` response puts the camera in front of the wall at once and eases it back out by its
`recovery_half_life` when the wall clears.

`Camera.setTarget` carries an absolute yaw, pitch and distance; the orbit takes look deltas and a
normalised zoom. The host turns the difference between the framing asked for and the one last
applied into the orbit's intent. **Ask with a non-zero `blendSeconds`:** zero is a CUT, which resets
the rig's smoothing — and with it the collision's recovery — every frame. The director cuts on its
first request and blends after.

## The level

The RTS test map's cook is a general one: `cy_rts_map_cook` takes any `.cymap`, and the sample's
`CMakeLists.txt` cooks `hillside.cymap` beside the binary. The host loads the package by id and
builds its colliders: the terrain as the triangles the frame draws (one static triangle mesh), each
placement as the box round its mesh, and the courtyard's pieces (`host/level.cpp`) as boxes, the ramp
turned. The sample runs on **Jolt**: the reference backend collides every shape as its bounding box,
which makes a ramp a block and a hillside a plateau.

## The frame

`host/render.cpp` follows `samples/13-rts-api/host/map_render.cpp`: `FrameAssembly` and
`FrameRecorder`, the standard material and its texture table, one geometry lookup for rigid and
skinned draws. What this frame adds:

* **The temporal upscaler.** `AssemblyDescription::post.temporal_upscaling`, `render_width` and
  `render_height` from `upscale::render_extent_for`, `produced_temporal_stage`, the jitter from
  `upscale::jitter_config_for`; `upscale::TemporalUpscalePass` bound to the assembly's temporal
  framework and handed to `FrameSinks::temporal`. Every screen-space stage — ambient occlusion, the
  GI — is created at the RENDER extent; the material sampler takes `upscale::texture_lod_bias`.
* **A sky.** The engine's frame draws no background, so the frame carries a dome round the eye in
  bands of elevation, each a material whose emission is the sky's radiance there, and the sun's disc.
* **The sun's map round the hero**, 52 m across, rather than round what the camera looks at: the
  shadows that matter in a third-person view are the ones at the character's feet.

## Testing a game like this

* **A digest.** The host folds every frame's hero placement, camera position and skinning matrices
  into one number; two runs must print the same. It is what catches a host that is not
  deterministic, before a golden image does.
* **A scripted player** (`host/script.h`): key edges at fixed frames through `InputServer::inject`,
  the door a recorded replay uses. A tap is both edges in one frame.
* **A negative control.** `--no-behaviours` runs every line of C++ with no game behind it.
* **A golden still and a budget** (`smoke.third_person_render`), the drawn run's gameplay held to
  the headless run's.

## Measured

RTX 5060, 1920x1080, Development, no validation layer, the 1200-frame run, a frame's wall time from
its recording to its readback (the device's work included) averaged over the frames that write no
image:

| Frame | ms | of which the GI on the device |
|---|---|---|
| everything, the upscaler at Quality (1286x724) | 7.9 | 2.8 |
| the upscaler at Performance (960x540) | 7.0 | 2.0 |
| native, temporal antialiasing (1920x1080) | 11.9 | 5.4 |
| no GI | 4.4 | — |
| no ambient occlusion | 6.2 | 2.9 |
| no shadows | 7.8 | 2.7 |

Runs on one machine move by about a millisecond (7.5 to 9.3 ms for the first row across the runs
made for this page); the budget, 16.7 ms, is held by `smoke.third_person_render` on every run. The
GI's host update — card shading and the probe gather's selection — is most of the game's own
millisecond: with `--no-gi` the host's frame is 0.1 ms.

## Pitfalls

* **A character on two coplanar floors** — the terrain flattened at zero and a floor box at zero —
  stands on whichever the sweep met first. The courtyard's floor collider is a centimetre higher, so
  the game hears stone there.
* **A placement on the scripted path** is an obstacle the script does not steer round: the log pile
  once stood across the ramp's foot and the hero ran in place against it for a second.
* **Golden images and the device GI**: a GI frame is the same twice only if its screen probes' slots
  are taken in a fixed order. They are (`render.gi_gpu_resolve`'s spent-capacity case); a change
  that brings back an atomic in the placement brings back a flaky golden.

## Not built yet

No mouse look and no window: the sample draws offscreen. No stair, arch or railing mesh in the kit.
The hero is skinned rigidly. Root motion is not used: the clips are in place and the game moves the
capsule. Metal and D3D12 compile the frame's shaders and have not run it.
