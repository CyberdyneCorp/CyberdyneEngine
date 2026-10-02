# Design

## Two halves, two stages

`animation-and-skinning` puts root motion and events on a deterministic CPU path and lets pose
evaluation be skipped, cached or rate-limited. The system keeps that split structurally:

| Half | Stage | Rate | Work |
|---|---|---|---|
| tick | `PostSimulation` | once per simulation tick | sync instances with `Animator` components; `advance_all` per batch with one `EventBuffer`; route root motion; mark instances due for evaluation |
| pose | `Animation` | once per frame | evaluate the due instances, sliced across job workers; commit the pose world serially |

Running the tick half in the fixed step is what "deterministic under the simulation rules" needs:
two runs that simulate the same ticks with the same requests agree bit for bit, however the ticks
were grouped into frames, and a frame that ran no tick evaluates nothing new. It runs after
gameplay's `Simulation` stage, so the requests a behaviour raised this tick are the ones advanced.

## Batches and handles

One `AnimationBatch` per registered rig. The system's `Slot` table maps a generational
`AnimatorHandle` to (rig, batch index, `PoseHandle`); a batch removes by moving its last instance into
the hole, and only that one slot's index changes. The instance's event identifier is its slot, which
the move does not change, so `entity_of(event)` survives removals.

## Parallel evaluation without a lock

`PoseWorld::publish` flips a parity and widens the dirty range, which is not thread-safe. It is split:
`staging(handle)` is the half the next publish overwrites, writable in place and touching no shared
state, and `commit(handle)` is the flip. Workers evaluate a slice into the scratch arena and write
`to_skinning` straight into `staging`; the system then commits in slot order. The arithmetic is
`publish_pose`'s, so the matrices are bit-identical to the hand-driven path, which a test asserts.

## Level of detail

| Tier | Rate | Bone level | Pose |
|---|---|---|---|
| Full | every tick | 0 | evaluated |
| Simplified | 30 Hz of simulation time | 1 | evaluated |
| Cached | 12 Hz | 2 | evaluated |
| Baked | never | — | the reference pose published at creation |

Every tier is advanced every tick, so root motion and events do not depend on it.

## The clock fix

A state's clips start when its tree is first sampled: when it becomes a blend's target, or when a
cut makes it current. Completing a blend into it no longer restarts them. A clock whose clip the
program compiled non-looping, or whose clip asset is `LoopMode::None`, is clamped to the duration
before root motion and events read the interval, and a held clip over a looping asset is sampled with
`sample_unwrapped`, so the duration is the last key and not the first frame.

## What is not captured

Instance state is the system's, not the world's: it is not in a rollback capture or the state hash.
That is a `StateProvider` and is recorded under "Not built yet".

## Defect register

Both were found by `samples/09b-animated-character`'s `worst joint step between two frames`
measurement at M8.d and worked around in the sample; both are repaired here. Each regression test was
run against the unrepaired code and failed.

| Id | Defect | Row | Regression test | Tier when found |
|---|---|---|---|---|
| D1 | `bake_clip` sampled its last frame at the clip's duration through `Clip::sample`, which wraps a looping clip's duration to zero, so every baked clip ended on its source's first frame | animation-and-skinning, Retargeting by semantic chains | `integration.animation_runtime` "retarget: a baked looping clip ends on the source's last frame, not its first" | Working |
| D2 | A state's clip clocks were reset when a blend into it completed, jumping the incoming clip back by one blend duration; and every clock wrapped by its clip's duration whatever the loop flag, so a non-looping clip restarted | animation-and-skinning, Animation clips | `integration.animation_runtime` "animation clocks: a blend's target starts its clip once, and completing does not restart it", "animation clocks: a non-looping clip holds its last frame" and "animation clocks: a held clip over a looping asset holds the last key, not the first" | Working |
