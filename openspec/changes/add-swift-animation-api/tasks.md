## 1. The runtime

- [x] 1.1 `graph::pose::request_state`, `find_state`, `transition_duration`, `kRequestedTransition`; the evaluator and root motion read the running duration through one function.
- [x] 1.2 `animation::request_state` restarts the entered state's clips.
- [x] 1.3 `AnimationSystem::play`, `stop`, `fire_trigger` (cleared after each tick), `status`, `joint_model_matrix`; cases in `integration.animation_system`.

## 2. The ABI

- [x] 2.1 Fourteen entries appended, `CY_ABI_MINOR` 7 (after 1.6's interface entries), the baseline updated; `just quality-abi` and its selftest green.
- [x] 2.2 Thunks and `AnimationBackend`; `unit.abi`'s `test_game_animation.cpp` and the 1.7 table shape.
- [x] 2.3 `cy::game-backend-animation`'s `AnimationAdapter`; `integration.game_backend_animation`.
- [x] 2.4 The Swift overlay and the Rust SDK regenerated.

## 3. CyberdyneKit

- [x] 3.1 `Animator`, `AnimationName`, `AnimationEvent`, `Animation`; `AnimationTests.swift` through `FakeEngine`.

## 4. The RTS

- [x] 4.1 The host's worker rig cooked and loaded through the asset system; the system installed and the adapter bound.
- [x] 4.2 The Swift game animates its units: walk, idle, a cheer on arrival, idle on the cheer's event.
- [x] 4.3 `integration.rts_api_sample`'s ABI 1.7 case and the `--no-behaviours` control.

## 5. Proofs and documents

- [x] 5.1 Each new case proven red by a recorded mutation (`evidence/falsification.md`).
- [x] 5.2 `docs/guides/animation.md`, `docs/guides/swift.md`, `bindings/swift/README.md`, `src/abi/README.md`, `src/animation/README.md`, the RTS README, `requirements-coverage.toml`.
