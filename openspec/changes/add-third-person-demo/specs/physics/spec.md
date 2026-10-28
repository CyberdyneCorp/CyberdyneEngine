## MODIFIED Requirements

### Requirement: Character controller
The engine SHALL provide a capsule-based `CharacterController` implementing collide-and-slide
movement with: ground detection and a maximum slope angle, step offset for stairs, ceiling
detection, a skin width, a maximum iteration count, pushing of dynamic bodies with configurable
force, and **moving platform** support that inherits platform motion.

Two modes SHALL be supported: **grounded** (gravity, slopes, stairs) and **floating** (six
degrees of freedom, for swimming and flying).

#### Scenario: Walking up stairs
- **WHEN** a character moves into a step below the step offset
- **THEN** it SHALL be lifted onto the step without a jump

#### Scenario: Steep slope
- **WHEN** a slope exceeds the maximum angle
- **THEN** the character SHALL be reported as touching a wall rather than grounded, and SHALL
  slide down

#### Scenario: Moving platform
- **WHEN** a character stands on a kinematic platform that moves
- **THEN** the platform's motion SHALL be applied to the character, and on leaving, the configured
  inherited-velocity policy SHALL apply

#### Scenario: Stepping from one body onto another
- **WHEN** a grounded character walks from one static body onto another — a stair's tread, the next
  slab
- **THEN** no platform motion SHALL be applied: a platform's motion is the same ground body's
  displacement across a step, never the distance between two bodies
