# Design

## What a game asks for, and where each request lands

| Game call (Swift) | ABI 1.7 | Engine |
|---|---|---|
| `Animator.attach(to:rig:…)` | `animation_attach` | the entity's `Animator` added; `AnimationSystem::sync` at once |
| `play(_:crossfade:)`, `stop(blend:)` | `animation_play`, `animation_stop` | `AnimationSystem::play` / `stop` → `animation::request_state` |
| `set(_:to:)` | `animation_set_float`, `animation_set_bool` | `AnimationSystem::set_parameter` |
| `fire(_:)` | `animation_fire_trigger` | `AnimationSystem::fire_trigger` |
| `float(_:)`, `state`, `rootMotion` | `animation_get_float`, `animation_state`, `animation_root_motion` | `parameter`, `status`, `root_motion` / `travelled` |
| `takeRootMotion()`, `setRootMotion(_:)` | `animation_take_root_motion`, `animation_set_root_motion` | `take_root_motion`; the `Animator`'s `RootMotionMode` |
| `Animation.events()` | `animation_events` | `AnimationAdapter::begin_frame`'s snapshot of `AnimationSystem::events()` |
| `jointPose(_:)` | `animation_joint_pose` | `joint_model_matrix` through the node's `WorldTransform` |

## Play: a requested transition

The program's transitions are conditioned on parameters, and a game needs to say "play this state
now" whatever edges the program has — a one-shot on arrival, a cut to a death. `request_state`
starts a blend to the state marked `kRequestedTransition` (0xFFFE) with its own duration in
`PoseInstance::requested_duration`; `transition_duration` is the one place the running blend's
duration is read, by the state machine, the pose evaluator and root motion alike. A requested blend
runs to completion — the program's transitions are considered again from the state it lands in — so
a program edge cannot silently cancel what the game asked for. A request during a blend starts from
the blend's source state. The entered state's clips restart at the request (the clocks would
otherwise carry whatever the state last played), because `advance` only sees the state change it
made itself.

## Triggers

A trigger is a parameter set to 1 and recorded; `AnimationSystem::step` clears every recorded
trigger after the tick's batches have advanced, so a transition conditioned on it is taken by
exactly one tick however long the game takes to clear it, and a trigger fired in a frame is read by
the next tick.

## Names cross as hashes

A 1.3 game entry returns values, never an engine pointer. A state or an event name therefore crosses
as `CY_NAME_HASH`: FNV-1a, 64 bits, over the UTF-8 bytes, with the offset basis and prime defined in
`cy_abi.h`. `cy::abi::game::name_hash` (constexpr) and Kit's `AnimationName.hash` are the two
implementations and both are held to the published FNV-1a vectors. Strings passed in are borrowed
for the call and looked up with `Name::find`, so a name nobody interned is answered NOT_FOUND without
growing the intern table.

## Events: once per frame

The system's event buffer holds the events of the ticks since its last evaluation and is cleared by
the next tick. A frame that ran no tick would hand the same events out again, so the adapter
snapshots them in `begin_frame` only when the system's tick count moved since the last snapshot.
`animation_events` is `[N U]` and uses `world_chunks`' sizing pattern.

## Root motion into a character

`CY_ROOT_MOTION_CHARACTER` is the system's `Controller` mode consumed by the adapter: `update(dt)`,
called once per tick after the system's advance, takes each such entity's accumulated delta, turns
it into the world by the node's placement, and moves the entity's character controller (ABI 1.5's
`CharacterBackend`) by it at the tick's length. Attaching or switching to the mode requires a
character and is NOT_FOUND without one; `animation_take_root_motion` refuses a character-driven
animator, so the motion is not taken twice.

## The RTS rig

The sample's host is also the cook: it authors an eight-joint skeleton, three clips (idle, a walk
with `footstep` events, a held cheer with a `cheer_done` event) and a three-state program with no
transitions, encodes each as the cooked record, writes them into a memory mount, and loads them back
by asset id through `AssetSystem` and `AnimationLibrary`. The game decides every state.

## What is not done

- A module cannot load or register a rig; the host registers names.
- A requested blend cannot be interrupted by a program transition.
- No per-frame bone writes from a script; IK targets go through float parameters.
- The events a frame delivers are its ticks'; there is no history query.
