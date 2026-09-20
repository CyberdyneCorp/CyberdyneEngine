## ADDED Requirements

### Requirement: Bounded iOS reference workload

The iOS Metal reference workload SHALL bound both its shaded pixel count and terrain shader work.
It SHALL render below native display resolution while UIKit UI remains at native resolution, and
it SHALL use documented limits for procedural terrain octaves and intersection steps.

#### Scenario: Mobile pixel cost is bounded

- **WHEN** the iOS reference workload starts on a high-density display
- **THEN** its Metal drawable SHALL use a documented linear scale below 1.0
- **AND** the UIKit overlay SHALL remain sized in native view coordinates

#### Scenario: Scene identity survives optimization

- **WHEN** the optimized workload is captured
- **THEN** procedural terrain, atmospheric fog, the orbiting camera, stars, sun, and day/night cycle
  SHALL remain visible or active

### Requirement: Machine-checkable mobile quality evidence

The iOS reference workload SHALL report its native dimensions, drawable dimensions, render scale,
terrain octave count, maximum march steps, selected Metal device, and measured presentation rate.
The physical-device runner SHALL reject absent or internally inconsistent quality diagnostics.

#### Scenario: Scale does not match dimensions

- **WHEN** reported drawable dimensions do not match the native dimensions and scale within
  rounding tolerance
- **THEN** the evidence run SHALL fail

#### Scenario: Performance target is missed

- **WHEN** the median of at least eight post-launch one-second samples is below 55 FPS on the iPhone
  16 reference device
- **THEN** the optimization SHALL remain incomplete

#### Scenario: Physical evidence is reviewable

- **WHEN** the optimized workload satisfies the performance target
- **THEN** the repository SHALL contain its screenshot and a before/after report naming the GPU,
  dimensions, quality limits, FPS samples, and median
