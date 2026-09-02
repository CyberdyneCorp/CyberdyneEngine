## MODIFIED Requirements

### Requirement: Screen-space signed distance field
The engine SHALL rasterise 2D occluders into a screen-space signed distance field, sized by a
configurable oversize and scale factor.

The SDF SHALL be sampleable from 2D materials, and SHALL be exposed to the VFX system as a data
interface so 2D effects can collide against occluder geometry (see `vfx-system`).

#### Scenario: Particle collides with 2D geometry
- **WHEN** a 2D VFX effect samples the screen-space SDF data interface for collision
- **THEN** its particles SHALL collide with occluder geometry sampled from the SDF

#### Scenario: Oversize prevents edge popping
- **WHEN** the SDF is oversized beyond the viewport
- **THEN** occluders just off screen SHALL still contribute
