# Add blend spaces, sync groups, foot placement and aim

## Why

Issue #92, stage 1 and the foot-placement half of the constraint stage. `animation-and-skinning`
names `Blend1D` and `Blend2D` nodes, sync groups that align clips "by marker correspondence rather
than by normalised time", root deltas that "blend by the same weights", and foot placement —
"raycast to ground with normal alignment and pelvis adjustment" — among its modifiers. The
requirements map recorded all of them as `exempt:m11e`: there was no blend-space node, nothing
aligned two clips by their markers, and `solve_unimplemented(FootPlacement)` refused by name because
the runtime may not issue a raycast. The FPS and TPS demos need locomotion that blends speed and
direction without sliding feet, feet that stand on stairs and slopes, and an aim.

## What changes

- **Blend spaces.** `pose.blend1d` and `pose.blend2d` nodes (samples on pins `s0`..`s15`, each placed
  by the property of the same name) lower to `PoseOp::BlendSpace` over three new program tables —
  `BlendSpace`, `BlendSample`, `BlendTriangle`. One dimension interpolates the two samples around the
  parameter; two dimensions use a Delaunay triangulation computed at cook time and the barycentric
  weights of the point, clamped to the hull's nearest point outside it. `blend_space_weights` is the
  one weighting the reference evaluator, the runtime evaluator, root motion and events share. Only
  the samples with weight are walked.
- **Sync groups.** A space with `sync` plays its clip samples on one phase in MARKER SPACE: the
  stride segment between two sync markers and the fraction through it. Each clip maps the phase onto
  its own timeline segment by segment, so foot-downs coincide across clips of different length, and
  the phase advances through a segment at the weighted mean of the clips' lengths of it. Clips whose
  markers do not correspond (by name, in cyclic order) fall back to normalised time. The phase is a
  program parameter (`phase.<node>`), per-instance state like a clip clock.
- **Root motion and events** of a space blend by its weights; a space's events come from its
  heaviest sample only, so a walk/run blend is heard as one set of footsteps.
- **Foot placement.** `foot_placement.h`: `GroundProbe` (the host answers a downward ray),
  `solve_foot_placement` (legs to the ground under each foot with the animated lift kept, the hips
  lowered by the lower foot's offset, each foot turned to the ground's normal within a limit, all
  followed with a time constant over simulation time), `PlaneGround`. `pose.foot_ik` names the
  pelvis and both legs by joint name; `AnimationRig::bind` resolves them. Zero weight is the pose
  before, bit for bit.
- **Aim.** `solve_aim` spreads a turn over up to four joints; `pose.aim` names them and its target
  parameters (`<target>.x/.y/.z`, model space).
- **The two-bone solver** solves the knee's position directly and writes both rotations through
  their parents' model rotations (regression: a chain under a rotated parent missed its target).
- **The frame system** hands each instance the ground (`AnimationSystem::set_ground_probe`), the
  entity's `WorldTransform` and the simulation time since its last evaluation, and reports each
  solve (`foot_placement(entity)`).
- **Grounds.** `cy::animation-ground`: `PhysicsGroundProbe` (a physics world, the character's own
  bodies and triggers skipped) and `TerrainGroundProbe` (a terrain query, holes refused).
- **Cooked programs** gain version 2, appending the five tables; a program without them stays
  version 1, byte for byte.
- **ABI 1.14** appends `animation_aim_at` and `animation_foot_placement` (`CyFootPlacement`);
  CyberdyneKit adds `Animator.locomotion(velocity:facing:)`, `setLocomotion`, `aim(_:at:)` and
  `footPlacement`; the Swift overlay and the Rust SDK are regenerated.
- **Editor.** The engine's animation catalogue offers the four nodes with their properties (vector
  properties for 2D positions and poles); the panel's parameters run past one; a graph with foot
  placement gets a preview-ground slope, which the engine's preview stands the character on.
- **Artefact.** `samples/09b-animated-character --course` walks stairs, a landing and a ramp;
  `just capture-foot-placement` records it with foot placement off and on, side by side.

## Not in this change

The `Custom` node, curve consumers, display interpolation, tweens, clip streaming, the control rig,
full-body IK and the other solvers, motion matching, warping, facial animation and the debug views
(issue #92's later stages). Reverse playback of a synchronised space holds its phase.
