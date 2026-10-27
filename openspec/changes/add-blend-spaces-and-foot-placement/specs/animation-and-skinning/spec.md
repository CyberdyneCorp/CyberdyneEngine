## ADDED Requirements

### Requirement: Blend spaces with marker-space sync
The animation graph SHALL provide `pose.blend1d` and `pose.blend2d` nodes whose samples are poses
placed in a one- or two-dimensional parameter space. One dimension SHALL weight the two samples
around the parameter linearly; two dimensions SHALL weight the three samples of the containing
triangle of a triangulation computed at cook time by barycentric coordinates, and a point outside
the samples SHALL be weighted as the nearest point of their hull. Weights SHALL be non-negative,
SHALL sum to one, and SHALL be the same for the same parameters. Samples at zero weight SHALL NOT be
evaluated. Root motion SHALL blend by the space's weights, and a space's events SHALL come from its
heaviest sample.

A space marked `sync` SHALL play its clip samples on one phase in marker space — the segment between
two sync markers and the fraction through it — mapped onto each clip segment by segment, advancing
through a segment at the weighted mean of the clips' lengths of it. Clips whose markers do not
correspond SHALL be synchronised by normalised time.

#### Scenario: Barycentric locomotion weights
- **WHEN** a 2D space with idle at the centre and four directions is evaluated at lateral 0.25,
  forward 0.5
- **THEN** idle, forward and right SHALL weigh 0.25, 0.5 and 0.25, the other two SHALL weigh zero
  and SHALL NOT be sampled

#### Scenario: Feet in phase across clips of different length
- **WHEN** a one-second walk and a 0.6 s run that starts on the other foot blend in a synchronised
  space while the speed sweeps between them
- **THEN** both clips SHALL be at the same point of the stride on every tick

#### Scenario: Blended cadence
- **WHEN** the walk and the run weigh half each
- **THEN** a stride SHALL last 0.8 s and the root SHALL travel at the weighted mean of the clips'
  speeds at that cadence

### Requirement: Foot placement on the host's ground
The runtime SHALL place a character's feet on ground the HOST answers for through a downward probe,
without naming a physics server or a terrain. A `pose.foot_ik` node SHALL name the pelvis and both
legs' hip, knee and ankle joints, resolved when a rig is bound. The solve SHALL keep each foot's
animated lift above the ground under it, lower the hips by the lower foot's offset within a limit,
turn each foot to the ground's normal within a limit, and follow the ground with a time constant
over simulation time. At zero weight, at a tier that skips modifiers, or with no ground, the node
SHALL leave the pose exactly as its input produced it. The frame system SHALL give each solve the
entity's world placement and SHALL call the probe from job workers concurrently.

#### Scenario: Planted on a slope
- **WHEN** a standing pair of legs is placed on a 20° slope across its feet
- **THEN** each ankle SHALL be its height above the slope along the normal to a millimetre, each
  sole SHALL face the slope within a degree, and the hips SHALL drop by the lower foot's offset

#### Scenario: Off is the frame before
- **WHEN** a program's foot placement is at zero weight, or no ground is set
- **THEN** the published pose SHALL be byte-identical to that of the program without the node

#### Scenario: Smoothing is simulation time
- **WHEN** the ground steps up under a foot with a 0.1 s time constant
- **THEN** one solve 0.1 s later and two solves 0.05 s apart SHALL move the foot the same
  `1 − 1/e` of the way
