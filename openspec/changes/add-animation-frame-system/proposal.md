# Proposal

## Why

The animation runtime in `src/animation/` is built and tested, and nothing runs it. `ecs::Stage::Animation`
exists and the engine installs no system in it, there is no animation component, and every sample
calls `advance`, `evaluate` and `publish_pose` itself — `samples/09b-animated-character`'s `step` is
the whole per-frame path, written by hand. Issue #76 stage 1 makes animation part of the frame: an
entity carries an `Animator`, and the engine advances, evaluates and publishes it.

Two runtime defects the sample worked around block that, because a frame system cannot carry the
sample's private `StateClocks`:

1. `bake_clip` ends a baked looping clip on the source's first frame.
2. A state's clip clock restarts when its incoming blend completes, and `animation::advance` wraps
   every clock whatever the loop flag, so a non-looping clip restarts.

## What Changes

- Fix both defects, each with a regression test proven red on the old code: `Clip::sample_unwrapped`
  samples the end of a clip without the loop mode's wrap, and `bake_clip` reads its last frame with
  it; a state's clips start when it becomes a blend's target (or a cut makes it current) and not
  when the blend completes; a clock compiled non-looping, or over a clip whose loop mode is `None`,
  holds at the duration.
- Add `AnimationBatch::remove` (swap-remove, the moved instance keeps its state), `PoseScratch::adopt`
  (evaluation buffers from caller memory), and `PoseWorld::staging` / `commit` (`publish` in two
  halves, so workers write their own instances' matrices concurrently and commits are serial).
- Add `cy::animation-system` (`src/animation/system/`): the `Animator` component and
  `AnimationSystem`, installed as two systems — the deterministic half (sync, `advance_all` with one
  `EventBuffer`, root motion) once per simulation tick in `Stage::PostSimulation`, and the pose half
  in `Stage::Animation`, sliced across job workers with buffers from each worker's scratch arena and
  published into one `PoseWorld`. Instances are added and removed without rebuilding anything.
  Level of detail sets the evaluation rate (in simulation time), the bone level and whether a pose is
  evaluated at all. Root motion's consumer is a per-instance `RootMotionMode`: ignore, apply to the
  transform, feed a controller, or extract only.
- Remove `StateClocks` and the hand-driven path from `samples/09b-animated-character`.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `animation-and-skinning`: the frame system and the clip-clock rules are added as requirements.

## Impact

- `src/animation/` (clip, evaluate, pose world, retarget), new `src/animation/system/`.
- `samples/09b-animated-character`: animated by the system.
- `CY_ANIMATION=OFF` removes the system with the runtime.
