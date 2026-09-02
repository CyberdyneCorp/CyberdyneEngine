## REMOVED Requirements

### Requirement: Inverse kinematics and modifiers

Superseded by "Constraint and IK framework", which unifies the solvers and the procedural
modifiers under one ordering model, conflict detection, and debugging view. Every solver and
modifier listed here is preserved there, with full-body IK added.

### Requirement: Retargeting

Superseded by "Retargeting by semantic chains", which replaces `SkeletonProfile` name matching
with a retarget profile describing semantic chains. Offline baking and runtime retargeting are
both preserved.

## ADDED Requirements

### Requirement: Engine-owned animation architecture
The animation runtime SHALL be engine code: the asset model, the graph and rig compilers, pose
evaluation and storage, LOD and pose sharing, motion matching, the constraint framework,
retargeting, and the GPU pose world.

Proven libraries MAY be integrated for bounded problems inside it — source format parsing and clip
compression — behind engine-owned interfaces and formats.

The system SHALL be removable at build time via `CY_ANIMATION`.

#### Scenario: Owned format, integrated codec
- **WHEN** clip compression is implemented
- **THEN** the clip asset format SHALL be engine-owned, and any integrated codec SHALL sit behind
  an engine interface so it can be replaced

#### Scenario: Animation disabled
- **WHEN** `CY_ANIMATION` is disabled
- **THEN** the animation runtime SHALL be excluded, and static meshes SHALL render unaffected

### Requirement: Animation asset model
Animation SHALL be represented by distinct asset types, each with a defined purpose:

| Asset | Contents |
|---|---|
| **Skeleton** | Runtime deformation hierarchy: joints, parent indices, bind poses, bone LOD levels, per-joint metadata |
| **Rig** | Artist-facing control system: controls, IK chains, constraints, space switching, pose drivers, and their maths |
| **Clip** | Keyframed tracks, curves, events, markers, and root motion |
| **Animation graph** | The blend and state logic producing a pose |
| **Pose database** | Extracted features and indexed poses for motion matching |
| **Retarget profile** | Semantic chain mapping between skeletons |
| **Ragdoll profile** | Physics bodies, shapes, joints, and limits derived from a skeleton |

A **skeleton is not a rig**: the skeleton is what the runtime evaluates and skins against; the rig
is what artists manipulate and what compiles into a program run against a skeleton.

#### Scenario: Lean runtime skeleton
- **WHEN** 50,000 instances share a skeleton
- **THEN** the skeleton asset SHALL carry no authoring-only control data, and per-instance state
  SHALL be the pose and a small state block

#### Scenario: Rig is optional
- **WHEN** a character needs no procedural rigging
- **THEN** it SHALL have a skeleton and no rig, and SHALL incur no rig evaluation cost

### Requirement: Compiled animation and rig programs
Animation graphs and control rigs SHALL be **compiled**, not interpreted node-by-node at runtime.

Compilation SHALL proceed: graph → typed IR → optimisation → compact program, with all instances
using an asset sharing one program and per-instance state limited to a small block (current state,
timers, parameters, blend weights, pose handle).

The optimiser SHALL perform at minimum: constant folding of authored parameters, dead-node
elimination, and **pose dependency analysis** — determining which joints each contributing pose
actually influences, so masked-out regions of a clip are never sampled.

Compilation SHALL occur at cook time; the runtime SHALL contain no graph compiler.

#### Scenario: Program is shared
- **WHEN** 5,000 characters use one animation graph
- **THEN** one compiled program SHALL exist and per-instance memory SHALL be the state block and
  pose only

#### Scenario: Masked region is not sampled
- **WHEN** a layer is masked to the upper body
- **THEN** pose dependency analysis SHALL determine that lower-body joints of that layer's clips
  are never read, and they SHALL NOT be sampled

#### Scenario: Compile error is actionable
- **WHEN** a graph or rig fails to compile
- **THEN** the error SHALL identify the node and pin

### Requirement: Pose evaluation separated from skinning
Pose evaluation SHALL produce bone matrices as a distinct stage from mesh skinning, with a defined
handoff, so that where each runs can be chosen independently.

The pipeline SHALL be: sample clips → blend to a local pose → resolve to a global pose → produce
bone matrices → publish → skin.

Skinning strategy SHALL be selectable per instance and per platform — CPU, GPU vertex, GPU compute
— without affecting how the pose was evaluated.

#### Scenario: Same pose, different skinning
- **WHEN** one instance skins on the GPU and another on the CPU
- **THEN** both SHALL consume identically-produced bone matrices, with no difference in evaluation

#### Scenario: Pose without skinning
- **WHEN** a character is evaluated for gameplay purposes but not rendered
- **THEN** pose evaluation SHALL be performable without any skinning work

### Requirement: GPU pose world
The engine SHALL maintain a **GPU pose world**: the shared GPU-side representation of animated pose
data, holding per skeleton instance the current bone matrices, the previous frame's bone matrices,
and derived per-bone velocity where required.

Producers SHALL include CPU pose evaluation uploading results, and GPU pose evaluation writing
directly.

Consumers SHALL include: mesh skinning, motion vector generation, VFX attachment and skeletal-mesh
sampling (see `vfx-system`), and any renderer feature needing bone transforms.

Consumers SHALL read the GPU pose world without CPU readback.

Instances SHALL be added and removed without rebuilding the world.

#### Scenario: VFX attaches to a bone
- **WHEN** a particle effect is attached to a character's hand
- **THEN** it SHALL read that bone's transform from the GPU pose world in a compute shader, with no
  CPU readback

#### Scenario: Motion vectors from previous pose
- **WHEN** a skinned mesh needs motion vectors
- **THEN** the previous frame's bone matrices SHALL be available in the pose world, with the
  documented tolerance for stepped animation

#### Scenario: CPU needs a bone transform
- **WHEN** gameplay needs a bone's world transform on the CPU
- **THEN** it SHALL be obtained from the CPU-side pose where one exists, or read back with
  documented latency where the pose was evaluated on the GPU

### Requirement: Animation level of detail
Every animated instance SHALL be assigned an **animation LOD tier** reducing both evaluation rate
and evaluation fidelity:

| Tier | Evaluation | Skeleton | Typical rate |
|---|---|---|---|
| `Full` | Full graph, all layers, IK, control rig | Full bone set | Up to frame rate |
| `Simplified` | Reduced graph: locomotion only, no IK or control rig | Reduced bone set | ~30 Hz |
| `Cached` | Sampled from the shared pose cache, no per-instance graph evaluation | Reduced bone set | 10–15 Hz |
| `Baked` | Pose texture or vertex animation; no skeleton evaluation | None | Sampled |

Rendering SHALL interpolate between evaluations so a reduced rate is not visible as stepping.

Tier SHALL be derived from distance, screen coverage, visibility, and an importance flag, and
SHALL be pinnable per instance.

Tier transitions SHALL be hysteretic and blended so a promoted instance does not visibly snap.

#### Scenario: Crowd tiering
- **WHEN** 50,000 characters are visible with 2,000 near the camera
- **THEN** near instances SHALL run at `Full` and the remainder at reduced tiers, with total cost
  bounded

#### Scenario: Interpolated display
- **WHEN** an instance evaluates at 10 Hz while rendering at 120 FPS
- **THEN** its pose SHALL be interpolated for display without visible stepping

#### Scenario: Promotion is not visible
- **WHEN** an instance is promoted from `Cached` to `Full`
- **THEN** the transition SHALL be blended rather than snapping to a different pose

### Requirement: Bone level of detail
Skeletons SHALL declare **bone LOD levels**: nested subsets of joints, ordered so that detail
joints — facial, finger, twist, and accessory chains — are dropped first.

Evaluation at a reduced bone LOD SHALL skip dropped joints entirely, and skinning SHALL use a mesh
LOD whose influences reference only retained joints.

#### Scenario: Distant character drops detail joints
- **WHEN** a character is far from the camera
- **THEN** facial, finger, and twist joints SHALL not be evaluated, and the mesh LOD in use SHALL
  not reference them

#### Scenario: Bone LOD is authored, not inferred
- **WHEN** a skeleton is imported
- **THEN** its bone LOD levels SHALL be derivable from naming conventions or authored explicitly,
  and SHALL be inspectable

### Requirement: Pose sharing
Instances evaluating the same clip at a similar phase SHALL be able to share an evaluated pose
through a **pose cache**, keyed on clip, quantised phase, and skeleton LOD.

A shared pose SHALL be evaluated once per frame and sampled by many instances, which MAY then
apply cheap per-instance variation: phase offset, additive upper-body override, aim offset, and
per-instance scaling.

Pose sharing SHALL be tier-gated: disabled at `Full`, available at reduced tiers.

Phase quantisation SHALL be a configurable quality setting.

#### Scenario: Crowd shares a walk cycle
- **WHEN** 10,000 instances play the same walk clip
- **THEN** a bounded number of poses SHALL be evaluated per frame and shared, rather than 10,000
  independent evaluations

#### Scenario: Variation is preserved
- **WHEN** shared-pose instances have differing aim directions
- **THEN** the aim offset SHALL be applied per instance over the shared base pose

#### Scenario: Close instances do not share
- **WHEN** an instance is at `Full` tier
- **THEN** it SHALL evaluate its own pose, so phase quantisation is never visible up close

### Requirement: Motion matching and pose search
The engine SHALL provide **motion matching**: selecting the best-matching animation frame from a
pose database based on a query describing desired motion, rather than requiring hand-authored
transition logic for locomotion.

A **pose database** SHALL be built offline from source clips, extracting per-frame features
including at minimum: root velocity, foot positions and velocities, hip transform, past and future
trajectory samples, facing direction, and foot contact states.

Runtime search SHALL accept a query built from current pose and desired trajectory, apply feature
weighting, and return the best candidate with a blend into it.

Search SHALL support an index (approximate nearest neighbour or equivalent) for large databases,
and SHALL be **deterministic**: the same query against the same database SHALL return the same
result, which excludes any index with non-deterministic tie-breaking or thread-order-dependent
accumulation.

Search SHALL support: continuity bias toward the currently playing clip, exclusion masks, cost
budgets, and per-feature weight tuning exposed to authors.

#### Scenario: Natural turning
- **WHEN** a character's desired trajectory curves right at speed
- **THEN** the search SHALL select a frame from a matching turning animation rather than blending
  a straight run with a rotation

#### Scenario: Deterministic selection
- **WHEN** the same query is issued twice against the same database
- **THEN** the same pose SHALL be returned, so motion matching is usable in deterministic
  simulation

#### Scenario: Continuity is preserved
- **WHEN** the best match is only marginally better than continuing the current clip
- **THEN** continuity bias SHALL prevent a jarring switch

#### Scenario: Search is budgeted
- **WHEN** many characters query in one tick
- **THEN** searches SHALL be batched and bounded by a budget, with over-budget queries deferred
  deterministically

### Requirement: Control rig
The engine SHALL provide a **control rig** system: procedural rigging authored as a graph and
compiled to a program, running both in the editor and at runtime.

Rig graphs SHALL support at minimum: controls, transform maths, curve and remap nodes, IK and FK
chains, constraints, space switching, pose drivers (a joint's transform driving other values), and
custom nodes.

Rigs SHALL be evaluatable at defined points: before the animation graph (pre-process),
after it (post-process), or both.

Rig evaluation SHALL be tier-gated, disabled at reduced animation LOD tiers.

#### Scenario: Procedural foot placement
- **WHEN** a rig raycasts the ground, derives a normal, and drives foot IK targets and pelvis
  height
- **THEN** it SHALL run as compiled rig code with no per-character script call

#### Scenario: Editor and runtime parity
- **WHEN** a rig is previewed in the editor and then run in game
- **THEN** it SHALL produce the same result from the same inputs

#### Scenario: Rig is skipped at distance
- **WHEN** an instance drops to `Simplified` tier
- **THEN** its control rig SHALL not be evaluated

### Requirement: Constraint and IK framework
IK solvers and constraints SHALL be unified in one **constraint framework** with a single ordering
model, conflict detection, and debugging view.

The framework SHALL provide: position, rotation, scale, parent, aim, look-at, and physics
constraints; and the solvers two-bone IK, FABRIK, CCD, spline IK, and **full-body IK**.

**Full-body IK** SHALL solve multiple effectors — hands, feet, head, and hips — as one pose
problem with per-effector weights and priorities, rather than as independent chains.

The framework SHALL additionally provide the procedural modifiers: **foot placement** (raycast to
ground with normal alignment and pelvis adjustment), **spring bones** for secondary motion with
collision, **copy and convert transform**, and **custom modifiers** supplied in Swift or native
code.

Every constraint, solver, and modifier SHALL declare the joints it reads and writes, so ordering
conflicts are detectable, and SHALL support a blend weight so it can be faded in and out.

Evaluation SHALL be tier-gated, skipped at reduced animation LOD tiers.

#### Scenario: Whole-body interaction
- **WHEN** a character grips a weapon with both hands, keeps both feet planted, and looks at a
  target
- **THEN** full-body IK SHALL produce one pose satisfying the effectors by their weights, rather
  than independent solvers fighting

#### Scenario: Conflicting writers
- **WHEN** two constraints write the same joint with no declared ordering
- **THEN** the conflict SHALL be reported at setup rather than resolved arbitrarily

#### Scenario: Constraint fades out
- **WHEN** a look-at constraint's weight animates to zero
- **THEN** its influence SHALL fade smoothly rather than releasing abruptly

#### Scenario: Foot placement on a slope
- **WHEN** a character stands on uneven ground with foot placement enabled
- **THEN** feet SHALL be raycast onto the surface, aligned to its normal within a limit, and the
  hips lowered to keep the pose natural

#### Scenario: Modifiers skipped at distance
- **WHEN** an instance is at `Simplified` tier or lower
- **THEN** IK and secondary motion SHALL be skipped

### Requirement: Retargeting by semantic chains
Retargeting SHALL map animation between skeletons through a **retarget profile** describing
semantic chains — root, spine, neck, head, and left and right arms and legs — rather than by
matching bone names.

The profile SHALL define per chain: joint correspondence, rest-pose reconciliation, rotation
offsets, and translation scaling rules, plus global settings for height scaling and root handling.

Retargeting SHALL preserve, within tolerance: foot contact with the ground, hand positions relative
to held objects, and overall silhouette.

Retargeting SHALL be performable **offline** (baking a retargeted clip at cook time) and **at
runtime** (mapping poses each evaluation), with the cost difference documented; the cooker SHALL be
able to bake frequently used source-target combinations.

#### Scenario: Different proportions
- **WHEN** a clip authored for a tall character is retargeted to a short one
- **THEN** foot contacts SHALL remain on the ground and hands SHALL remain on a held weapon within
  the documented tolerance

#### Scenario: Different bone names
- **WHEN** two skeletons use entirely different naming conventions
- **THEN** the retarget profile SHALL map them by chain semantics with no name matching

#### Scenario: Runtime retarget for user content
- **WHEN** a player-supplied character is loaded at runtime
- **THEN** runtime retargeting SHALL drive it from the existing animation library without a cook
  step

### Requirement: Animation warping
The engine SHALL support warping an animation to meet a gameplay target it was not authored for:

- **Motion warping** — adjusting root motion over a window so the animation ends at an exact target
  transform
- **Stride warping** — scaling stride length to match actual movement speed, reducing foot sliding
- **Orientation warping** — rotating the lower body to match movement direction independently of
  facing
- **Distance matching** — selecting the playback position that matches a distance to a target,
  rather than playing from the start

Warping SHALL declare a window over which it is applied and SHALL blend in and out so it is not
visible as a discontinuity. Warp amount SHALL be reportable so excessive warping is diagnosable.

#### Scenario: Vault to an exact ledge
- **WHEN** a vault animation authored for 2.0 m is used for a 2.5 m gap
- **THEN** motion warping SHALL adjust root motion over the window so the character lands on the
  ledge without sliding

#### Scenario: Foot sliding is reduced
- **WHEN** a character moves faster than the authored stride
- **THEN** stride warping SHALL scale the stride rather than letting feet slide

#### Scenario: Excessive warp is visible in diagnostics
- **WHEN** a warp exceeds a configured threshold
- **THEN** it SHALL be reported, since large warps look wrong and indicate missing source
  animation

### Requirement: Layers, masks, sync groups and markers
Animation SHALL support **layers**, each with a blend weight, a blend mode (override or additive),
and an optional **joint mask** with per-joint weights.

Clips SHALL support **sync groups** and **markers**: named points (foot down, foot up) used to
align playback position when blending between clips of differing length.

When blending clips in a sync group, playback SHALL be synchronised by marker correspondence rather
than by normalised time.

#### Scenario: Walk to run without foot sliding
- **WHEN** a walk and a run clip blend within a sync group
- **THEN** their foot-contact markers SHALL be aligned so contacts coincide, rather than blending
  by normalised time

#### Scenario: Upper-body layer
- **WHEN** an aim layer is masked to spine, chest, and arms
- **THEN** it SHALL override only those joints, with locomotion driving the legs

#### Scenario: Mask weights are per joint
- **WHEN** a mask assigns partial weight to the spine
- **THEN** that joint SHALL blend proportionally rather than being fully overridden or excluded

### Requirement: Animation curves and metadata
Clips SHALL carry named **curves** — arbitrary float tracks evaluated alongside the pose — and
these SHALL be readable by other systems.

Curves SHALL be consumable by: the animation graph itself (driving blend weights or IK weights),
gameplay, VFX (see `vfx-system`), audio, and materials.

Curve values SHALL be blended consistently with the poses they accompany, so a curve from a
half-weighted clip contributes proportionally.

#### Scenario: Curve drives an effect
- **WHEN** a clip carries a `WeaponGlow` curve
- **THEN** a material parameter or VFX input SHALL be drivable from it without gameplay code
  polling the animation

#### Scenario: Curves blend with poses
- **WHEN** two clips blend at 50/50
- **THEN** their shared curves SHALL blend by the same weights

### Requirement: Physics animation
The engine SHALL support **physics-driven animation** integrating with the physics backend:

- **Powered ragdoll** — physics bodies follow an animated target pose through joint motors, with
  configurable strength per body
- **Partial ragdoll** — a subset of the skeleton simulated while the remainder stays
  animation-driven, with a blend region between them
- **Hit reactions** — impulses applied to a powered ragdoll, blending back to animation over time
- **Full ragdoll** — simulation-driven, blended in from the current animated pose rather than
  switched instantaneously

A **ragdoll profile** asset SHALL be generatable from a skeleton — bodies, shapes, joints, and
limits — and then refined by hand.

Blending between animation-driven and simulation-driven SHALL be per body and continuous.

#### Scenario: Hit reaction without going limp
- **WHEN** a character is shot in the shoulder while running
- **THEN** the arm SHALL react physically while locomotion continues, blending back over the
  configured time

#### Scenario: Death is not a switch
- **WHEN** a character transitions to full ragdoll
- **THEN** simulation SHALL begin from the current animated pose and velocity, blending in rather
  than snapping

#### Scenario: Profile generated then refined
- **WHEN** a ragdoll profile is generated from a skeleton
- **THEN** it SHALL produce plausible bodies, shapes, and joint limits that an artist can then
  adjust

### Requirement: Facial animation
The engine SHALL support facial animation through: **blendshapes**, **bone-driven** facial rigs,
and **curve-driven** parameters, combinable on one character.

It SHALL support a **viseme** model mapping phonemes to facial poses, and an audio-driven path
producing viseme weights from speech.

An ML-driven path SHALL be supported through `ml-inference`, subject to that capability's
determinism boundary — facial animation is presentation, so non-pinned inference is permitted.

Facial joints and blendshapes SHALL participate in bone LOD, dropping entirely at distance.

#### Scenario: Lip sync from audio
- **WHEN** a dialogue line plays with audio-driven facial animation enabled
- **THEN** viseme weights SHALL be produced and blended onto the face alongside expression layers

#### Scenario: Facial detail drops with distance
- **WHEN** a character is distant
- **THEN** facial evaluation SHALL be skipped entirely by bone LOD

#### Scenario: ML facial is presentation only
- **WHEN** an ML model drives facial animation
- **THEN** it SHALL be permitted without pinning, because it produces no authoritative gameplay
  state

### Requirement: Animation determinism
Root motion drives the character controller and is therefore **gameplay state**, subject to the
engine's determinism contract.

Root motion SHALL be computed on a **deterministic CPU path** from clips' root tracks with blend
weights composed on the CPU, regardless of where the rest of the pose is evaluated and regardless
of the instance's animation LOD tier.

Consequently:

- Root motion SHALL NOT be derived from a GPU-evaluated pose.
- Animation LOD tier SHALL be a function of simulation state, not of measured frame time, for
  instances whose root motion is authoritative — mirroring the AI system's rule.
- Where an instance evaluates at a reduced rate, root motion SHALL be integrated correctly across
  the interval rather than sampled naively.
- Motion matching search SHALL be deterministic.

Instances whose animation produces no authoritative state — background characters, cosmetic
props — MAY be evaluated non-deterministically, and SHALL declare this.

#### Scenario: Root motion survives re-simulation
- **WHEN** ticks are re-simulated during network reconciliation
- **THEN** root motion SHALL be identical, because it was computed on the deterministic CPU path

#### Scenario: Reduced rate does not change distance travelled
- **WHEN** an instance evaluates at 10 Hz instead of 60 Hz
- **THEN** integrated root motion over a second SHALL match within the documented tolerance

#### Scenario: GPU-evaluated pose is not authoritative
- **WHEN** an instance's pose is evaluated on the GPU
- **THEN** no gameplay state SHALL be derived from that pose

### Requirement: Batched evaluation
Animation evaluation SHALL be **batched by shared program**: instances using the same compiled
graph and skeleton SHALL be evaluated together, so that iteration is over packed per-instance state
rather than scattered objects.

Batches SHALL be distributed across job workers with the batch as the unit of parallelism, and
SHALL be structured for SIMD evaluation of the per-instance inner loop where the program permits.

#### Scenario: Batch by program
- **WHEN** 2,000 humanoids, 6,000 robots, and 500 creatures animate
- **THEN** they SHALL form three batches evaluated over packed state, not 8,500 independent
  evaluations

#### Scenario: Batch is the parallel unit
- **WHEN** batches are scheduled
- **THEN** each SHALL be distributed across workers, with per-instance state contiguous within a
  batch

### Requirement: Clip streaming
Long clips — cinematics and long-form performance — SHALL be **streamable**: metadata and the
active window resident, with the remainder loaded on demand ahead of playback.

Streaming SHALL not stall playback: a not-yet-resident segment SHALL hold the last available pose
and report the underrun rather than blocking.

#### Scenario: Long cinematic
- **WHEN** a thirty-minute cinematic plays
- **THEN** only metadata and a bounded window of pose data SHALL be resident

#### Scenario: Streaming underrun
- **WHEN** a segment has not arrived in time
- **THEN** playback SHALL hold rather than stall the frame, and the underrun SHALL be reported

### Requirement: Rigging and skinning authoring
The editor SHALL provide a rigging workspace with: a skeleton tree, a 3D viewport with bone
manipulation, control rig graph authoring, IK chain setup, constraint configuration, retarget
profile mapping with side-by-side preview, ragdoll profile generation and refinement, and pose
debugging.

Skinning tools SHALL support: automatic weight generation (heat-map or geodesic binding), weight
painting, normalisation, mirroring, pruning below a threshold, and influence-count limiting.

The engine SHALL NOT attempt to replace a DCC application. The stated scope is inspecting, fixing,
and tuning rigs and weights authored elsewhere.

#### Scenario: Fixing imported weights
- **WHEN** an imported character has a weighting problem at the shoulder
- **THEN** it SHALL be fixable in the editor without a round trip to the DCC

#### Scenario: Retarget preview
- **WHEN** a retarget profile is authored
- **THEN** the editor SHALL show source and target side by side, playing the same clip

#### Scenario: Scope is documented
- **WHEN** a user expects full character authoring
- **THEN** the documentation SHALL state that modelling and full rigging remain external

## MODIFIED Requirements

### Requirement: Skeleton
A **skeleton** SHALL be a hierarchy of joints with bind poses, stored as flat arrays ordered
parent-before-child so evaluation is a single linear pass.

Poses SHALL be represented as `Transform` (TRS) per joint, in local space, converted to model
space and then to skinning matrices in one pass.

Skeletons SHALL support: named joints with stable indices, a `SkeletonProfile` mapping engine-
standard humanoid joint names to a skeleton's own names, per-joint user data, **bone LOD levels**,
and per-joint bounds used for skinned AABB computation.

A skeleton SHALL contain no authoring-only control data; controls belong to a rig asset.

#### Scenario: Single-pass evaluation
- **WHEN** a pose is converted to skinning matrices
- **THEN** the joint order SHALL guarantee each parent is computed before its children in one
  linear iteration

#### Scenario: Humanoid mapping
- **WHEN** a skeleton is assigned a humanoid profile
- **THEN** engine systems SHALL address joints by standard names regardless of the source rig's
  naming

#### Scenario: Skeleton stays lean
- **WHEN** a skeleton is inspected
- **THEN** it SHALL contain deformation and LOD data only, with rigging controls living in a
  separate rig asset

### Requirement: Animation evaluation
Animation SHALL be evaluated as: sample active clips into **poses**, blend poses per the compiled
animation program into a final pose, apply the constraint framework and control rig, then publish
bone matrices to the GPU pose world and animated properties to their targets.

Pose blending SHALL be additive-aware: a pose may be **absolute** or **additive** (a delta from a
reference pose), with additive poses composited after absolute blending.

Evaluation SHALL run as a system in the `Animation` stage, batched by shared program and
parallelised across job workers, with per-instance pose buffers allocated from the frame arena.

Evaluation cost SHALL be governed by the instance's animation LOD tier, and MAY be satisfied from
the shared pose cache.

#### Scenario: Parallel evaluation
- **WHEN** 500 characters animate
- **THEN** their evaluation SHALL be distributed across job workers, each writing its own pose
  buffer

#### Scenario: Additive layer
- **WHEN** a breathing animation is applied additively over a locomotion blend
- **THEN** it SHALL be composited as a delta, preserving the base motion

#### Scenario: LOD for animation
- **WHEN** a character is distant
- **THEN** its animation SHALL be evaluated at a reduced rate and reduced fidelity according to its
  animation LOD tier, and MAY be satisfied from the pose cache

### Requirement: Root motion
Clips MAY designate a **root motion** track, extracted rather than applied to the skeleton and
exposed as a per-tick delta transform.

Root motion SHALL expose, distinctly: root translation, root rotation, the motion curve, and
contact states, so consumers can use what they need.

Consumers SHALL choose the handling explicitly: ignore, apply directly to the transform, feed the
character controller, feed motion matching, or extract only.

Root motion SHALL be correctly composed through blending: blended clips SHALL produce a blended
root delta, and warping SHALL adjust it as specified.

Root motion SHALL be computed on the deterministic CPU path (see the determinism requirement).

#### Scenario: Character driven by animation
- **WHEN** root motion is enabled
- **THEN** the per-tick delta SHALL be exposed for the character controller to apply, and the
  root joint SHALL not be moved by the animation

#### Scenario: Blended root motion
- **WHEN** two locomotion clips blend
- **THEN** their root deltas SHALL blend by the same weights, so speed matches the visual gait

#### Scenario: Explicit consumer choice
- **WHEN** a server-authoritative game uses root motion
- **THEN** it SHALL be able to feed the character controller rather than applying directly to the
  transform, so movement remains subject to collision and authority

### Requirement: Animation events
Clips SHALL support **event tracks**: named events with optional typed parameters, fired when
playback crosses their time.

Events SHALL be emitted as **typed data into an event buffer** — carrying at minimum the instance,
the event identifier, and the normalised time — consumed by gameplay, audio, and VFX systems.
Events SHALL NOT invoke arbitrary callbacks scattered across objects.

Events SHALL fire exactly once per crossing, including during looping and reverse playback, and
SHALL fire in time order when a step crosses several.

Event emission SHALL respect animation LOD: instances at reduced tiers MAY have events suppressed
by a declared policy, so a distant crowd does not emit thousands of footstep events.

#### Scenario: Footstep event
- **WHEN** playback crosses a footstep event
- **THEN** an event SHALL be written to the buffer once, and again on the next loop iteration

#### Scenario: Large time step
- **WHEN** a tick advances past several events
- **THEN** all of them SHALL be emitted in time order, not just the last

#### Scenario: Distant crowd does not flood
- **WHEN** 10,000 distant characters would emit footstep events
- **THEN** the declared LOD event policy SHALL suppress or aggregate them

### Requirement: Clip storage and compression
Clips SHALL be stored in a compressed, sample-friendly form: per-track quantised keys with
per-track value ranges, curve fitting to remove keys within an error tolerance, and constant-track
collapsing.

Compression settings SHALL be per clip with per-track overrides, expressed as **error tolerances
in world units** (translation in millimetres, rotation in degrees) rather than opaque quality
numbers, and the achieved compression ratio and worst-case error SHALL be reported.

The compression codec MAY be an integrated third-party implementation behind an engine-owned
interface; the clip asset format SHALL remain engine-owned.

Sampling SHALL be cache-friendly: tracks laid out for sequential access, with a per-clip cursor
so forward playback does not binary-search each frame.

#### Scenario: Error-bounded compression
- **WHEN** a clip is compressed with a 0.1 mm translation tolerance
- **THEN** no sampled position SHALL deviate from the source by more than that, and the report
  SHALL state achieved compression and worst-case error

#### Scenario: Forward playback is O(1) amortised
- **WHEN** a clip plays forward
- **THEN** sampling SHALL advance the cursor rather than searching from the start

#### Scenario: Codec is replaceable
- **WHEN** a different compression implementation is adopted
- **THEN** the clip asset format and the sampling interface SHALL be unchanged

### Requirement: Animation diagnostics
The engine SHALL provide: a pose debug view drawing the skeleton and its constraint targets,
per-node weights in the animation graph, active states and transition progress, IK and full-body
IK effector and target visualisation, control rig node values, and clip compression reports.

It SHALL additionally report: per-instance and per-batch evaluation cost, animation LOD tier
distribution, pose cache hit rate and phase bucket occupancy, motion matching query cost and
selected clip with its match score, warp magnitudes, GPU pose world occupancy, and event emission
rates.

#### Scenario: Debugging a blend
- **WHEN** a character's motion looks wrong
- **THEN** the graph debug view SHALL show each node's weight and the active state machine
  transition

#### Scenario: Diagnosing crowd cost
- **WHEN** animation exceeds its budget
- **THEN** tier distribution and per-batch cost SHALL identify which instances and which programs
  are responsible

#### Scenario: Motion matching inspection
- **WHEN** motion matching selects an unexpected clip
- **THEN** the diagnostic SHALL show the query, the candidate scores, and the per-feature
  contributions
