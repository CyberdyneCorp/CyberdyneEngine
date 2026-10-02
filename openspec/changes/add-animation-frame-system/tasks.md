## 1. Runtime defects

- [x] 1.1 `bake_clip` reads its last frame with `Clip::sample_unwrapped`; regression `retarget: a baked looping clip ends on the source's last frame, not its first`, red with the old `sample` call.
- [x] 1.2 A state's clips start when it becomes a blend's target and not when the blend completes; regression `animation clocks: a blend's target starts its clip once, and completing does not restart it`, red with the old `state_after != state_before` reset.
- [x] 1.3 Non-looping clocks hold at the duration and a held clip over a looping asset samples the last key; regressions `animation clocks: a non-looping clip holds its last frame` and `... over a looping asset holds the last key, not the first`.

## 2. Runtime additions

- [x] 2.1 `AnimationBatch::remove`, `PoseScratch::adopt` / `transforms_needed`, `PoseWorld::staging` / `commit`.

## 3. The system

- [x] 3.1 `cy::animation-system`: `Animator`, `register_animator`, `AnimationSystem` (`sync`, `advance`, `evaluate`, `run`, `install`).
- [x] 3.2 `integration.animation_system`: hand-driven equivalence bit for bit, ticks not frames, transitions, LOD, events, root motion modes, removal, 500 instances over three rigs on a job system, two runs that agree, refusal of an unknown rig.

## 4. The sample and the documents

- [x] 4.1 `samples/09b-animated-character` animated by the system; `StateClocks` removed.
- [x] 4.2 `docs/guides/animation.md`, `src/animation/README.md`, `requirements-coverage.toml`, `falsifiability.toml`.
