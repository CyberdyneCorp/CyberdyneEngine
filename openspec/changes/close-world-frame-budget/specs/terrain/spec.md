# Spec Delta

## ADDED Requirements

### Requirement: Integrated terrain substrate shading is device work
The rendered world demonstration SHALL evaluate its per-vertex terrain substrate from the water
distance, wetness, snow depth, and vegetation fields on the graphics device. The result SHALL agree
with the deterministic CPU reference within each field's declared precision and the shading
tolerance recorded by the device-agreement test.

#### Scenario: Terrain fields change during a rendered frame
- **WHEN** weather or water changes one of the four terrain substrate fields
- **THEN** a device dispatch SHALL evaluate the visible terrain vertices and the rendered terrain SHALL consume its output

#### Scenario: Device result diverges from the reference
- **WHEN** the device output exceeds the recorded field or shading tolerance
- **THEN** the device-agreement test SHALL fail
