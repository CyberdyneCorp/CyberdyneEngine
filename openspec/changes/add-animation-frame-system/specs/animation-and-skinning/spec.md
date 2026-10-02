# animation-and-skinning Spec Delta

## ADDED Requirements

### Requirement: Animation system in the frame
The engine SHALL provide an `Animator` ECS component and an animation system that animates every
entity carrying one with no per-sample code in the game. The system SHALL advance every instance's
state machine, clip clocks, root motion and events once per simulation tick, in the fixed step and
after gameplay's simulation stage, and SHALL evaluate and publish poses in the `Animation` stage.

Instances SHALL be grouped into one batch per rig, advanced with one event buffer per batch pass,
and evaluated in slices of a batch distributed across job workers, each slice taking its pose
buffers from its worker's scratch memory. Poses SHALL be published into one pose world, and adding or
removing an instance SHALL NOT move any other instance's handle or pose.

The instance's animation LOD tier SHALL decide its evaluation rate, counted in simulation time, its
bone level of detail, and whether its pose is evaluated at all; every tier SHALL be advanced every
tick. Root motion's consumer SHALL be a per-instance setting: ignore, apply to the transform, feed a
character controller, or extract only.

#### Scenario: The system matches the hand-driven path
- **WHEN** an entity's instance and a hand-driven instance of the same rig receive the same requests
  before the same ticks
- **THEN** their published matrices and their root motion SHALL be bit-identical

#### Scenario: Simulation time, not frames
- **WHEN** one run simulates its ticks one per frame and another simulates the same ticks three per
  frame
- **THEN** both SHALL produce bit-identical poses, root motion, transforms and events

#### Scenario: Batches across workers
- **WHEN** 500 instances of three rigs animate on a job system
- **THEN** they SHALL form three batches, more than one participant SHALL evaluate slices, and the
  poses SHALL be bit-identical to a single-threaded evaluation

#### Scenario: Removal mid-run
- **WHEN** one instance's `Animator` is removed while others animate
- **THEN** every other instance's pose handle SHALL stay live and its pose SHALL be unchanged by the
  removal

#### Scenario: LOD rate and fidelity
- **WHEN** a `Full`, a `Simplified` and a `Baked` instance animate for twelve ticks at 60 Hz
- **THEN** they SHALL be posed twelve, six and zero times, the simplified one at bone level 1, and
  the baked one SHALL travel exactly as far as the full one

#### Scenario: Events through the system
- **WHEN** an entity walks a one-second clip with a footstep at half a second for two and a half
  seconds
- **THEN** the system SHALL report the footstep twice, attributed to that entity, and a suppressed
  instance SHALL count the same two crossings instead

### Requirement: Clip clocks across transitions
A state's clip clocks SHALL start when the state starts being sampled — when it becomes the target of
a blend, or when a cut makes it current — and SHALL NOT restart when a blend into it completes. A
clock whose clip is compiled as non-looping, or whose clip's loop mode is none, SHALL hold at the
clip's duration, and the pose there SHALL be the clip's last key even when the clip asset loops. A
clip baked by retargeting SHALL end on its source's last frame.

#### Scenario: Blend completion
- **WHEN** a blend into the walk completes
- **THEN** the walk's clock SHALL advance by exactly one tick on that tick, as on every other

#### Scenario: A death holds
- **WHEN** a non-looping death has played past its duration
- **THEN** its clock SHALL equal the duration and its pose SHALL be its last frame

#### Scenario: A baked loop
- **WHEN** a looping clip is baked onto another rig
- **THEN** sampling the baked clip just before its end SHALL match the source there, not interpolate
  toward its first frame
