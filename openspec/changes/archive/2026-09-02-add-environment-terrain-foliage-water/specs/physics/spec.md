## ADDED Requirements

### Requirement: Heightfield and terrain collision
The physics interface SHALL support **heightfield collision shapes** as a first-class shape type,
registered and updated in bulk rather than as individual bodies.

Terrain collision (see `terrain`) SHALL be registered on cell activation in bulk, and **updated
regionally** when terrain deformation of the gameplay or structural class occurs, without
rebuilding unaffected regions.

Heightfield holes SHALL be representable, so a cave entrance is not blocked by an invisible floor.

Terrain collision resolution SHALL be independent of terrain rendering detail, and a region MAY be
visible with coarse or absent collision according to its streaming channels.

#### Scenario: A region's collision arrives at once
- **WHEN** a cell containing terrain is activated
- **THEN** its collision SHALL be registered in bulk rather than one shape at a time

#### Scenario: A crater updates collision locally
- **WHEN** terrain is deformed by an explosion
- **THEN** only the affected region's collision SHALL be updated

### Requirement: Buoyancy and water interaction
The physics interface SHALL support **buoyancy** driven by water queries (see `water`): displacement
force from submerged volume or from multiple sample points on a body, linear and angular drag
through water, and advection by the water's surface velocity.

Buoyancy SHALL sample the **authoritative** displacement bands declared by the water body, so that
physics and rendering agree about where the surface is.

Character controllers SHALL support swimming and wading states derived from water depth.

Buoyancy SHALL respect the determinism guarantees stated for physics: within a platform, the same
inputs SHALL produce the same result.

#### Scenario: A vessel pitches on swell
- **WHEN** a vessel longer than the wave length floats on swell
- **THEN** multi-point sampling SHALL make it pitch and roll rather than translate rigidly

#### Scenario: Physics and rendering agree
- **WHEN** the renderer displaces the surface by an authoritative band
- **THEN** buoyancy SHALL sample the same displacement, and a floating body SHALL not sit beneath
  or above the visible surface
