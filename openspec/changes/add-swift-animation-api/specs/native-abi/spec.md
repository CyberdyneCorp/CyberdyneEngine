# native-abi Spec Delta

## ADDED Requirements

### Requirement: Animation entries
The interface table SHALL let a module drive a character's animation: attach and detach an animator
over a rig the host registered by name; play any state of the rig's program with a crossfade
duration, or cut to it, and blend back to the entry state; set float and boolean parameters and fire
one-tick triggers; read a parameter, the state machine, the last tick's root motion and its running
total; take accumulated root motion in a fixed step and choose where root motion goes, including
into the entity's character controller; read the events every animator fired in the ticks since the
previous frame, once per frame, as typed data; and read a joint's world placement.

What changes a character SHALL be callable only in no phase or a fixed step, events and a joint's pose
only in no phase or a frame update, and every other read in any phase. A state or an event name SHALL
cross as a value — a 64-bit FNV-1a hash of its text, with constants the header states — and never as
an engine pointer.

#### Scenario: Play from the table
- **WHEN** a module plays `walk` then `run` on an entity through the table, and the engine's own
  `AnimationSystem::play` is called with the same states at the same ticks on another
- **THEN** the two published poses and root motion SHALL be bit-identical

#### Scenario: Events exactly once
- **WHEN** an animator walks a one-second clip with a footstep at its middle for four seconds, five
  ticks to a frame, and the events are read every frame and again at a frame boundary with no tick
- **THEN** four footsteps SHALL be delivered, each once, and the second read SHALL deliver none

#### Scenario: A character destroyed under its animator
- **WHEN** two animators feed their root motion to character controllers and the first one's
  controller is destroyed while its animator stays attached
- **THEN** every later tick's character update SHALL succeed and the other character SHALL keep
  walking

#### Scenario: A request in the wrong phase
- **WHEN** a module calls `animation_play` during a frame update
- **THEN** the entry SHALL answer PERMISSION_DENIED and nothing SHALL be played
