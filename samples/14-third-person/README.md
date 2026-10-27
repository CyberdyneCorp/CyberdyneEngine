# samples/14-third-person — a third-person game, written in Swift, drawn by the engine

A hero runs across a courtyard, climbs its stairs, jumps, turns the camera round itself until a wall
pushes the camera in, runs down a ramp and up a hill, jumps again and looks back down the valley.
**Every decision is the Swift game's** (`game/`); the C++ host (`host/`) builds the level, binds the
engine's adapters and draws the frame.

![The run: on the stairs, a jump on the platform, the camera pulled in by the north wall, down the ramp, a jump onto the hill, and the view back down](../../docs/design/images/third-person-run.png)

[The video](../../docs/design/videos/third-person.mp4), with the game's own sound: the footfalls on
stone and on grass, the jumps and landings, the wind and the birds.

```
just run-sample third-person                   1200 frames of scripted play, headless, then the report
just run-sample third-person --no-behaviours   the negative control: the same host with no game
cy_sample_third-person --render <dir>          the run drawn at 1920x1080: seven stills
    --width <n> --height <n>   --video <dir> [--video-every <n>]   --audio <file.wav>
    --upscale native|quality|balanced|performance   --no-upscale   --no-gi   --no-ao
    --no-shadows   --no-validation   --stills <every n>   --trace <every n>   --frames <n>
just capture-third-person                      the images and the video below
ctest -R third_person                          the run, its digest, the golden, the budget
```

`samples/04-character` is the same kind of game at ABI 1.0 — a capsule, headless, its every value
carried across components by its host. This one is what the ABI has grown into: the game calls the
engine itself, and the engine draws it.

## What the game does, and through what

| The game does | Through | Phase |
|---|---|---|
| makes the hero a capsule on its own node: 1.8 m, a 0.35 m step offset, a 50-degree slope limit | `CharacterController.create` | none (`onCreate`) |
| gives it an animator over the host's `hero` rig, and plants its feet | `Animator.attach`, `Animator.set("feet")` | none |
| reads the stick, the jump and the sprint | `Input.action` | fixed (`onFixedUpdate`) |
| turns the stick into the camera's frame, accelerates, turns the hero's facing by a half-life, jumps with coyote time | its own arithmetic, `CharacterController.move` | fixed |
| sets the locomotion blend space's speed, fades foot placement out in the air, plays jump, fall and land | `Animator.set("speed")`, `Animator.play` | fixed |
| hears the clips' footfalls and plays a step at the foot that fell, stone or grass by what the capsule stands on | `Animation.events`, `Animator.jointPose`, `CharacterController.state`, `Audio.play(at:)` | frame (`onUpdate`) |
| reads where its feet were placed | `Animator.footPlacement` | frame |
| turns the camera with the look stick, swings it in behind a moving hero, pulls it back at speed, and asks for that framing | `Input.action`, `Camera.active`, `Camera.setTarget` | frame |
| sees how far a wall pulled the camera in | `Camera.view` | frame |
| starts the wind's loop, and a bird in the woods every few seconds | `Audio.play(options: loop)`, `Audio.play(at:)` | none, then fixed |

Two components cross the boundary, both from the game to the host (`game/Contract.swift`):
`HeroPose`, the hero's facing, which the host turns the hero's node by when it moves the node to the
capsule — ABI 1.14 has no entry that turns a scene node — and `HeroReport`, what the game counted,
which the host prints and never acts on.

## What the engine does

| | |
|---|---|
| **the hero's legs** | `cy::animation::AnimationSystem` over the `hero` rig the host cooks and loads back by asset id (`host/hero_rig.h`): twenty joints, six clips, and a program whose locomotion state is a one-dimensional **blend space** over idle, walk and run, synchronised by the clips' foot markers, through **foot placement** on both legs. Its ground is the physics world (`animation::PhysicsGroundProbe`), so the feet stand on the stair the capsule climbed and on the slope it runs up |
| **the stairs and the slope** | `cy::physics::CharacterController` over the Jolt world: the terrain as the triangle mesh the renderer draws, the courtyard's pieces and the map's buildings as boxes. Eight 18 cm risers climbed, a 10-degree ramp, a 20-degree hillside |
| **the camera** | a third-person rig on the camera server — target, orbit, shoulder offset, **collision**, lens (`host/tps_host.cpp`) — whose batched collision and occlusion queries `cy::game_backend::CameraCollisionResolver` answers against the physics world every frame: a sphere swept from the hero's head toward where the rig wants the camera, the hero's own capsule passed through |
| **the sound** | the audio server with a listener at the camera, six clips synthesised at start-up (`host/sounds.h`) |
| **the frame** | `host/render.h`: the cooked hillside, its foliage and kit, the courtyard's pieces and a sky dome, the hero skinned on the device, the sun's 2048-texel shadow map round the hero, ambient occlusion, the device GI over the courtyard's boxes, and the **temporal upscaler** at Quality — the scene at 1286x724 reconstructed to 1920x1080 |

## The frame budget, measured

On the RTX 5060 at 1920x1080, Development, no validation layer (`smoke.third_person_render`'s timed
run): **a frame 7.9 ms on average** (7.5 to 9.3 ms across runs) — recorded, submitted and waited
for — against a budget of **16.7 ms** (sixty frames a second); the device's half about 6.2 ms, of
which the GI 2.6 to 2.8 ms. The game's own frame — the hero, the camera, and the GI's host update,
which is most of it — is about 1 ms. Natively at 1920x1080 with temporal antialiasing instead of the
upscaler the frame is 11.9 ms. The guide has the frame with each feature off.

## What the tests check

`integration.third_person_sample` runs the host as a separate process, headless, and reads the
report: the hero stepped up at least eight risers and its feet reached the platform's 1.44 m and the
hillside above it; it ran at the run's speed and walked; it jumped twice and landed twice, and every
state of the program played; foot placement solved on all but the first frame, moved a foot by more
than four centimetres on the stairs and the slope and lowered the hips; footfalls were heard on
stone and on grass and no voice was dropped; the wind and a bird played; the physics world answered
the camera's sweeps, the north wall pulled the camera to under half its distance, and the game saw
it closer than it asked for. Two runs print the same report and the same **digest** — every frame's
hero placement, camera position and skinning matrices folded together. With `--no-behaviours`
every line of C++ runs and nothing moves, sounds or collides.

`smoke.third_person_render` draws the run three ways — headless, at 480x270 with the validation
layer, at 1920x1080 without — and holds the drawn run's gameplay to the headless one's, frame 880 to
`tests/references/third_person_0880.png` texel for texel, and the 1080p frame to 16.7 ms.

## Three defects this demo found, fixed at their roots

1. **A character stepping from one body onto another was thrown by the distance between their
   origins.** `CharacterController::update_ground` measured a platform's motion against the ground
   body it had just overwritten, so the step from the terrain onto a stair's tread read as a
   platform that moved four metres in a tick: the hero was thrown four metres back from the first
   riser, and forward again from the next. `integration.physics_behaviour`'s "walking from one static
   body onto another is not a ride on a platform".
2. **A camera pulled in by a wall poked through it on every other frame, and snapped out when it
   cleared.** The rig's collision node published its next queries from the position the last answer
   had pulled it to, which the next sweep found clear; and a clear answer did nothing, so the
   recovery never eased. `unit.camera_system`'s two wall cases.
3. **The device GI's frame was not the same twice once its screen probes ran out of room.** The
   adaptive probes took their slots with an atomic, so which candidates a spent capacity refused was
   the order the device reached them in. They now take them in cell order, as the host does.
   `render.gi_gpu_resolve`'s spent-capacity case; the golden here depends on it.

## Known limits

* The level's pieces are boxes, and the ramp a turned one: there is no stair mesh, arch mesh or
  railing in the RTS kit, and the GI holds the ramp as the box round it.
* The hero is built from primitives and skinned rigidly — one joint a vertex — so its elbows and
  knees fold as segments. Its clips are authored in code.
* No mouse look: the camera turns with the arrow keys. `--render` draws offscreen and writes images;
  it does not open a window, and there is no device-input mode.
* Linux and Vulkan only were run. The frame's shaders compile for Metal and D3D12; the sample needs
  Swift, `CY_ANIMATION` and `CY_PHYSICS`, and declares nothing without them.
