## MODIFIED Requirements

### Requirement: Soft bodies and additional simulation
The engine SHALL support, where the backend provides them: soft bodies (cloth), and vehicle
simulation with wheels, suspension, and a drivetrain.

**Ragdolls** SHALL be supported as a configuration of bodies and constraints derived from a
skeleton via a ragdoll profile asset (see `animation-and-skinning`), supporting:

- **Full ragdoll** — simulation-driven, blended in from the current animated pose and velocity
- **Powered ragdoll** — bodies follow an animated target pose through joint motors, with
  configurable strength per body
- **Partial ragdoll** — a subset of bodies simulated while the rest remains animation-driven, with
  a blend region between them

Blending between animation-driven and simulation-driven SHALL be per body and continuous, so
transitions are not instantaneous switches.

#### Scenario: Ragdoll activation
- **WHEN** a character's ragdoll is activated
- **THEN** its bodies SHALL be driven by physics from their current animated pose and velocity,
  with an optional blend period

#### Scenario: Unsupported feature
- **WHEN** a backend does not implement soft bodies
- **THEN** the capability query SHALL report it and creation SHALL fail with a clear diagnostic

#### Scenario: Powered ragdoll follows animation
- **WHEN** a powered ragdoll's motors are configured to follow an animated pose
- **THEN** the bodies SHALL track that pose while still colliding with the world, and an external
  impulse SHALL produce a physical reaction that recovers toward the animation
