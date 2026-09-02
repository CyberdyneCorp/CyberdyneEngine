# animation-and-skinning Specification

## Purpose

Defines animation: clip storage and sampling, the skeleton model, the animation graph (state
machines and blend trees), retargeting, inverse kinematics and procedural modifiers, root motion,
and property tweening.

## Requirements

### Requirement: Animation clips
An **animation clip** SHALL be a set of tracks, each targeting a property path with keyframes.

Track kinds: `Translation`, `Rotation`, `Scale`, `BlendShape`, `Property` (any reflected field),
`Event` (named triggers), and `Reference` (asset swaps).

Interpolation modes: `Step`, `Linear`, `Cubic` (with in/out tangents), and `Spherical` for
rotations.

Clips SHALL declare: duration, a loop mode (`None`, `Loop`, `PingPong`), and a sample rate hint.

#### Scenario: Rotation interpolation
- **WHEN** a rotation track is sampled between keys
- **THEN** quaternions SHALL be interpolated along the shortest arc, with neighbouring keys
  hemisphere-aligned at import so no long-way rotation occurs

#### Scenario: Property track
- **WHEN** a clip animates a material parameter
- **THEN** the track SHALL target the reflected field and write it through reflection

### Requirement: Clip storage and compression
Clips SHALL be stored in a compressed, sample-friendly form: per-track quantised keys with
per-track value ranges, curve fitting to remove keys within an error tolerance, and constant-track
collapsing.

Compression settings SHALL be per clip with per-track overrides, expressed as **error tolerances
in world units** (translation in millimetres, rotation in degrees) rather than opaque quality
numbers.

Sampling SHALL be cache-friendly: tracks laid out for sequential access, with a per-clip cursor
so forward playback does not binary-search each frame.

#### Scenario: Error-bounded compression
- **WHEN** a clip is compressed with a 0.1 mm translation tolerance
- **THEN** no sampled position SHALL deviate from the source by more than that, and the report
  SHALL state achieved compression

#### Scenario: Forward playback is O(1) amortised
- **WHEN** a clip plays forward
- **THEN** sampling SHALL advance the cursor rather than searching from the start

### Requirement: Skeleton
A **skeleton** SHALL be a hierarchy of joints with bind poses, stored as flat arrays ordered
parent-before-child so evaluation is a single linear pass.

Poses SHALL be represented as `Transform` (TRS) per joint, in local space, converted to model
space and then to skinning matrices in one pass.

Skeletons SHALL support: named joints with stable indices, a `SkeletonProfile` mapping engine-
standard humanoid joint names to a skeleton's own names, and per-joint user data.

#### Scenario: Single-pass evaluation
- **WHEN** a pose is converted to skinning matrices
- **THEN** the joint order SHALL guarantee each parent is computed before its children in one
  linear iteration

#### Scenario: Humanoid mapping
- **WHEN** a skeleton is assigned a humanoid profile
- **THEN** engine systems SHALL address joints by standard names regardless of the source rig's
  naming

### Requirement: Animation evaluation
Animation SHALL be evaluated as: sample active clips into **poses**, blend poses per the
animation graph into a final pose, apply modifiers (IK, procedural), then write to the skeleton
and to animated properties.

Pose blending SHALL be additive-aware: a pose may be **absolute** or **additive** (a delta from a
reference pose), with additive poses composited after absolute blending.

Evaluation SHALL run as a system in the `Animation` stage, parallelised across animated
instances, with per-instance pose buffers allocated from the frame arena.

#### Scenario: Parallel evaluation
- **WHEN** 500 characters animate
- **THEN** their evaluation SHALL be distributed across job workers, each writing its own pose
  buffer

#### Scenario: Additive layer
- **WHEN** a breathing animation is applied additively over a locomotion blend
- **THEN** it SHALL be composited as a delta, preserving the base motion

#### Scenario: LOD for animation
- **WHEN** a character is distant
- **THEN** its animation SHALL be evaluated at a reduced rate or with a reduced joint set,
  according to its animation LOD setting

### Requirement: Animation graph
The engine SHALL provide an **animation graph** asset composed of nodes:

| Node | Behaviour |
|---|---|
| `Clip` | Plays a clip with speed and loop control |
| `Blend1D` | Blends clips along one parameter |
| `Blend2D` | Blends clips over a 2D parameter space by triangulated weights |
| `BlendMask` | Blends two poses with a per-joint weight mask |
| `Additive` | Applies one pose additively over another |
| `StateMachine` | States with transitions, conditions, durations, and interruption rules |
| `Layer` | Composites a sub-graph with a weight and a joint mask |
| `IK` | Applies an IK modifier |
| `Custom` | A user-supplied node in Swift or native code |

Graph parameters SHALL be typed (float, int, bool, trigger) and settable from script.

State machine transitions SHALL support: conditions on parameters, exit-time conditions,
blend duration and curve, interruption sources and priority, and automatic transitions.

#### Scenario: Locomotion blend space
- **WHEN** a 2D blend space is driven by forward and lateral speed
- **THEN** the surrounding clips SHALL be blended by barycentric weights, producing smooth
  directional locomotion

#### Scenario: Transition interruption
- **WHEN** a higher-priority transition becomes valid mid-blend and interruption is allowed
- **THEN** it SHALL take over, blending from the current partially-blended pose

#### Scenario: Layer with a mask
- **WHEN** an upper-body aim layer is masked to the spine and arms
- **THEN** it SHALL override only those joints, leaving locomotion driving the legs

### Requirement: Root motion
Clips MAY designate a **root motion** track, extracted rather than applied to the skeleton and
exposed as a per-tick delta transform.

Root motion SHALL be correctly composed through blending: blended clips SHALL produce a blended
root delta.

#### Scenario: Character driven by animation
- **WHEN** root motion is enabled
- **THEN** the per-tick delta SHALL be exposed for the character controller to apply, and the
  root joint SHALL not be moved by the animation

#### Scenario: Blended root motion
- **WHEN** two locomotion clips blend
- **THEN** their root deltas SHALL blend by the same weights, so speed matches the visual gait

### Requirement: Inverse kinematics and modifiers
The engine SHALL provide skeleton modifiers applied after graph evaluation, in a defined order:

- **Two-bone IK** — analytic solve for limbs, with a pole target
- **FABRIK** — iterative chain solver for longer chains
- **CCD IK** — iterative alternative with per-joint constraints
- **Look-at** — aims a joint at a target with limits and smoothing
- **Foot placement** — raycasts against the ground and adjusts feet and hips
- **Spring bones** — secondary motion for hair, cloth, and accessories, with collision shapes
- **Copy / convert transform** — drives one joint from another with an optional transformation
- **Custom modifiers** — user-supplied in Swift or native code

Modifiers SHALL declare which joints they read and write so ordering conflicts can be reported.

#### Scenario: Foot placement on a slope
- **WHEN** a character stands on uneven ground with foot placement enabled
- **THEN** feet SHALL be raycast onto the surface, aligned to its normal within a limit, and the
  hips lowered to keep the pose natural

#### Scenario: Conflicting modifiers
- **WHEN** two modifiers write the same joint
- **THEN** the ordering SHALL be explicit, and an unordered conflict SHALL be reported at setup

### Requirement: Retargeting
The engine SHALL support retargeting animation between skeletons that share a `SkeletonProfile`,
correcting for differing bone lengths, rest poses, and orientations.

Retargeting SHALL be performable at import (baking a new clip) or at runtime (mapping poses
each frame), with the trade-off documented.

#### Scenario: Shared animation library
- **WHEN** several characters with different proportions share a humanoid profile
- **THEN** one clip SHALL drive all of them, with per-skeleton correction preserving foot contact
  and hand positions within tolerance

### Requirement: Animation events
Clips SHALL support **event tracks**: named events with optional parameters, fired when playback
crosses their time.

Events SHALL fire exactly once per crossing, including during looping and reverse playback, and
SHALL be delivered as an event channel and as behaviour callbacks.

#### Scenario: Footstep event
- **WHEN** playback crosses a footstep event
- **THEN** it SHALL fire once, and SHALL fire again on the next loop iteration

#### Scenario: Large time step
- **WHEN** a tick advances past several events
- **THEN** all of them SHALL fire in time order, not just the last

### Requirement: Property animation and tweening
Beyond skeletal animation, the engine SHALL provide:

- **Property tracks** in clips, animating any reflected field
- **Tweens** — procedural, code-driven interpolation of properties with easing, sequencing,
  parallel groups, delays, callbacks, and looping

Tweens SHALL be bound to an entity so they are cancelled when it is destroyed.

Tweens SHALL be for **gameplay and scene properties**. UI animation is internal to the UI system
(see `ui-system`), so that UI transitions do not cross the scripting boundary per element per
frame and are not coupled to entity lifetime.

#### Scenario: Gameplay transition
- **WHEN** a tween animates a door entity's rotation with an ease-out curve over 0.3 s
- **THEN** it SHALL update per frame and complete with a callback

#### Scenario: Tween outlives its target
- **WHEN** a tween's target entity is destroyed
- **THEN** the tween SHALL be cancelled rather than writing to a dead entity

#### Scenario: UI transition
- **WHEN** a UI element transitions on hover, or a menu animates in
- **THEN** it SHALL be driven by the UI system's own animation rather than by an entity-bound
  tween, so it neither crosses the scripting boundary per frame nor depends on an entity's
  lifetime

### Requirement: Animation diagnostics
The engine SHALL provide: a pose debug view drawing the skeleton, per-node weights in the
animation graph, active states and transition progress, IK target and effector visualisation,
per-instance evaluation cost, and clip compression reports.

#### Scenario: Debugging a blend
- **WHEN** a character's motion looks wrong
- **THEN** the graph debug view SHALL show each node's weight and the active state machine
  transition
