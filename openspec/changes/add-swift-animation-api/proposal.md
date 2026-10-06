# Proposal

## Why

Issue #76 stage 4. `bindings/swift` had `SystemStage.animation` and nothing else for animation: no
animator, parameter, event or root-motion call crossed the ABI, so gameplay in Swift could not drive
a character, and the M12 RTS's units could not animate. The engine side existed — an `Animator`
component and the `AnimationSystem` since stage 1, a loader since stage 2 — and had no verbs a game
needs beyond raising the program's own parameters: no "play this state now", no one-shot trigger.

## What Changes

- The runtime: requested crossfades in the pose state machine (`graph::pose::request_state`,
  `kRequestedTransition`, `PoseInstance::requested_duration`), `animation::request_state` restarting
  the entered state's clips, and `AnimationSystem::play`, `stop`, `fire_trigger` (one tick),
  `status` and `joint_model_matrix`.
- ABI 1.7 appends fourteen entries after 1.6's interface entries — `animation_attach`, `_detach`,
  `_play`, `_stop`, `_set_float`, `_set_bool`, `_fire_trigger`, `_get_float`, `_state`, `_events`, `_root_motion`,
  `_take_root_motion`, `_set_root_motion`, `_joint_pose` — with `CyAnimatorDesc`,
  `CyAnimatorState`, `CyAnimationEvent`, `CyRootMotion`, `CyRootMotionMode`, `CyAnimationTier` and
  `CY_NAME_HASH`; thunks over `cy::abi::game::AnimationBackend`.
- `cy::game-backend-animation` (only with `CY_ANIMATION`): `AnimationAdapter` over
  `AnimationSystem`, with rigs registered by name, events snapshotted once per frame, and
  `CY_ROOT_MOTION_CHARACTER` feeding the character controllers of ABI 1.5.
- CyberdyneKit: `Animator`, `AnimationName`, `AnimationEvent` and `Animation`; the overlay and the
  Rust SDK regenerated.
- `samples/13-rts-api`: the host cooks a worker rig and loads it back through the asset system; the
  Swift game animates every unit — walk, idle, a cheer on arrival and idle again on the cheer's
  event.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `native-abi`: the animation entries.
- `swift-scripting`: the animation facade.
- `animation-and-skinning`: requested crossfades and triggers.

## Impact

- `src/graph` (pose state machine), `src/animation` (runtime and system), `src/abi`,
  `src/game_backend/animation` (new), `bindings/swift`, `editor/crates/cy-editor-sdk` (generated),
  `samples/13-rts-api`.
- Append-only: a module compiled against 1.6 or earlier reads the prefix it knows. The RTS module now
  requires 1.7.
