## ADDED Requirements

### Requirement: Motion blur executes on the device
When motion blur is enabled, the frame SHALL blur the temporal resolve's linear HDR colour after the
temporal stage and before bloom, by a per-tile maximum of the prepass motion vectors, the maximum
over each tile's neighbourhood, and a gather along that direction whose weights let a nearer surface
cover a farther one only as far as its own streak reaches. The blur's length SHALL be the shutter's
open fraction of the frame's motion, the fraction SHALL be derivable from the shutter time the
exposure is computed from, and the camera's share of a pixel's motion SHALL be scalable apart from
the objects'.

#### Scenario: The streak follows the shutter
- **WHEN** an object moves across the frame at a 180 degree and at a 360 degree shutter
- **THEN** its streak SHALL be half and a whole of its per-frame motion respectively

#### Scenario: The background behind a fast object stays sharp
- **WHEN** an object moves quickly across a still background
- **THEN** every background pixel farther from the object than half its shutter-open motion SHALL be
  its unblurred colour

#### Scenario: A closed shutter is no stage
- **WHEN** the shutter angle is zero
- **THEN** the frame SHALL be byte-identical to the frame without the motion blur stage

#### Scenario: The device is the reference
- **WHEN** the gather runs on the device
- **THEN** its output SHALL agree with the host reference filter evaluated over the inputs the
  device read
