# Add a rendered third-person demo, and fix what it found

## Why

`samples/04-character` is a third-person controller written in Swift — it moves, jumps, collides
and is followed by a camera director — and it runs headless, a capsule whose every value its host
carries across components. Since then the ABI has grown entries for input, the camera, characters,
audio and the animation runtime (blend spaces and foot placement at 1.14), and the frame has grown
the device GI and a temporal upscaler. Nothing shows them composed into the thing a third-person
game is: a skinned character running, jumping and climbing through a level, a camera that keeps off
the walls, the steps heard, drawn inside a frame budget.

## What changes

- **A rendered third-person demo, `samples/14-third-person`.** A Swift game (`Hero`,
  `CameraDirector`, `Ambience`) over ABI 1.14 drives a skinned hero whose locomotion is a
  synchronised blend space through foot placement; climbs a courtyard's stairs and runs down a ramp
  and up a hill on a cooked level (`content/third-person/hillside.cymap`, the RTS test map's format
  and cook); turns a camera rig whose collision the physics world answers; and plays footsteps, jumps,
  landings, wind and birds. The host draws it with the engine's frame — shadows, ambient occlusion,
  the device GI, the temporal upscaler at Quality and a sky — within 16.7 ms at 1080p.
- **The camera's collision queries answered by a physics world.** `cy::game_backend::
  CameraCollisionResolver` answers a `CameraServer`'s batched collision and occlusion queries — a
  swept sphere and a ray — against a `PhysicsServer` world, passing through the bodies it is told
  to ignore.
- **Three defects fixed at their roots**, each with a regression test proved red on the old code:
  - a character stepping from one ground body onto another took the distance between the two
    bodies' origins as a platform's motion and was thrown by it (`CharacterController`);
  - a camera's collision node asked its next question about the position the last answer had
    pulled it to, so a camera against a wall poked through it every other frame, and a clear answer
    never eased it back out (`rig_evaluate.cpp`);
  - the device GI's adaptive screen probes took their slots with an atomic, so once a frame's
    capacity was spent, which candidates were refused — and so the frame — differed from run to run
    and from the host oracle (`gi_screen_probes_common.slang`, a rank pass).
- **Tests**: `integration.third_person_sample` (the run headless: stairs, slope, jumps, feet,
  footfalls, the camera kept off the wall, the digest twice, the negative control),
  `smoke.third_person_render` (gameplay against the headless run, a golden still, the 1080p budget),
  `integration.game_backend_camera_collision`, and cases in `unit.camera_system`,
  `integration.physics_behaviour` and `render.gi_gpu_resolve`.
- **Docs**: the sample's README, `content/third-person/`, `deps/shipped-content.md` and
  `THIRD_PARTY.md`, `docs/guides/third-person-demo.md`, the GI module's README, `just
  capture-third-person` and its images and video, the requirements map.

## Scope

In: the sample, its level, its tests and its measurements; the resolver; the three fixes. Out: a
window and device input for the drawn run, mouse look, stair and arch meshes, smooth-skinned
characters, root motion, and running the frame on Metal or D3D12, which compile its shaders and were
not run.

## Impact

Additive apart from the three fixes. No ABI change. The fixes change behaviour only where it was
wrong: a character crossing between two ground bodies, a camera whose collision answer changes, and
a GI frame whose adaptive probe capacity is spent. The GI fix adds one compute dispatch of one
workgroup a frame.
