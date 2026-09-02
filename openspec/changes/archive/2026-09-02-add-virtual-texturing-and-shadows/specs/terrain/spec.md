## MODIFIED Requirements

### Requirement: Terrain material layers
Terrain surface materials SHALL be described by **compact per-texel layer data** — a small number
of dominant layer indices with weights — rather than one weight map per layer across the world.

The number of layers blended at a texel SHALL be bounded and configurable, and the cooker SHALL
report where content exceeds the bound and what was dropped.

Terrain material data SHALL be stored in **virtual textures** (see `virtual-texturing`), and the
composed surface SHALL be produced through a **runtime virtual texture producer**: the terrain
material graph is evaluated when a page is produced and sampled cheaply thereafter, rather than
evaluated per terrain pixel per frame.

Produced terrain pages SHALL be cached and invalidated only where their inputs change — deformation,
a road, a decal, a field update — so an edit re-produces a region rather than the world.

Where virtual texturing is unavailable on a device, terrain materials SHALL degrade to conventional
tiled textures with reduced unique detail and the limitation reported.

Terrain shading SHALL go through the material compiler like any other material; a monolithic
terrain shader carrying every layer and every feature SHALL NOT be produced.

#### Scenario: Layers are bounded per texel
- **WHEN** a terrain uses twenty material layers across a region
- **THEN** each texel SHALL store only its dominant few, and shading cost SHALL not scale with the
  layer count of the region

#### Scenario: Exceeding the bound is reported
- **WHEN** an author blends more layers at a point than the bound permits
- **THEN** cooking SHALL report it and name the location, rather than silently dropping a layer

#### Scenario: The layer graph is evaluated once per page
- **WHEN** an expensive terrain material graph is used
- **THEN** it SHALL be evaluated when a virtual texture page is produced, and sampling that page
  thereafter SHALL cost an ordinary texture fetch

#### Scenario: A local edit re-produces locally
- **WHEN** terrain is deformed in one place
- **THEN** only the pages covering it SHALL be invalidated and re-produced
