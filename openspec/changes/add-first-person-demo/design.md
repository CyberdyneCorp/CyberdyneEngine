# Design: the first-person demo

## Presentation crosses the ABI as a verb, not a component

A game that wants sparks where its shot landed could write the hit into a component and let the
host's renderer read it. That is how `samples/04-character` carried input before ABI 1.3, and it is
the shape ABI 1.3 replaced: the host would be deciding presentation from game data. So the burst and
the mark are entries, beside `light_spawn`: the host registers what may be played under a name, the
game resolves the name once and plays the handle. Both are `[N U]` for the reason lights are: a
resimulated tick would play its burst and leave its mark again.

An effect's parameters cross as floats, four a parameter in the effect's declaration order — the
layout `EffectSpawn::parameter_values` already has — so a direction to spray along or a floor height
to bounce on is a declared parameter, not a field the ABI has to know about. A decal crosses as a pose
and half extents; the box projects along its local −Z, so an impact's mark has +Z on the struck
surface's normal, which CyberdyneKit's `Decal.Description.mark(_:at:normal:radius:)` builds.

A decal that the budget evicts on arrival is not an error: `decal_spawn` answers OK and
CY_DECAL_NULL. "Nothing appeared" is the symptom somebody will look for, so `decal_status` counts the
evictions.

## A spawn that ends

`release_on_completion` released an instance only when `spawning` was false, and only `stop` cleared
it. A burst a game plays and forgets is exactly the instance nobody stops. `spawn_seconds` ends the
spawn by the instance's own age, after at least one sub-step — so a burst shorter than a sub-step
still spawns once at a reduced simulation rate.

## The aim crosses from the frame to the step

The first-person camera is presentation; a fixed step may not ask it where the weapon points. The
game records, in `onUpdate`, the eye the last frame was drawn from and the weapon's aim (recoil
included), and the next `onFixedUpdate` shoots along them. The shot is a raycast in the step, where
it is deterministic, and its flash, burst, sparks, mark and sounds are queued for the next frame.
The host captures each shot the moment the game's step returns — before the targets move or the
world steps — and casts its own ray along the same line: that is what "a shot hits what the raycast
says" is held to.

## Crouching is the controller

`CharacterController` has no resize. A crouch destroys the character and creates a shorter one with
its feet where they were; standing creates the tall one again only when `Physics.overlap` of a
standing capsule at that place is empty. Under the beam the stand is refused tick after tick, and the
player stands the tick he is clear.

## The targets are the engine's

Four agents of `cy::ai`: one batched `PerceptionScheduler` update a tick, its visibility batch traced
through the physics world; a behaviour tree compiled by `cy::graph::behaviour`; and a hit read from
the `Health` component the game writes, perceived as `SenseKind::Damage`. `BehaviourHost` takes no
agent, so each agent thinks in its own `think` call with one-element spans, and every task succeeds
the tick it runs so the tree is walked from the root every tick.

## The frame

Everything is `FrameAssembly` and `FrameRecorder`: the arena's static boxes baked in world space with
texture coordinates in metres, the moving boxes as the unit cube placed; the decal table packed
relative to the eye and staged into the frame's graph before the sampling pass; the particles in the
transparent stage; the flashes as lights; the weapon on its own layer through
`viewmodel::assembly_view`. The muzzle burst is placed where the weapon LENS draws the barrel's tip —
its eye-space offset scaled by the ratio of the two lenses' half-angle tangents — or it would hang in
the air beside the barrel.
