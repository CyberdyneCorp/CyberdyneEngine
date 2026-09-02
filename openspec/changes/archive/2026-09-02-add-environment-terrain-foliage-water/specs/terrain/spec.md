## ADDED Requirements

### Requirement: Terrain representations
Terrain SHALL be addressed through one interface admitting three **representations**:

| Representation | Purpose | Status |
|---|---|---|
| `Heightfield` | The common ground surface | Required |
| `Mesh` | Cliffs, overhangs, arches, authored formations | Planned |
| `SignedDistanceField` | Caves, tunnels, runtime excavation | Planned |

A world MAY combine them: a heightfield surface with mesh cliffs and volumetric caves.

No consumer SHALL assume terrain is a heightfield. Queries, collision, navigation, and rendering
SHALL be expressed against the terrain interface, so adding a representation is an implementation
rather than a rework of every consumer.

#### Scenario: Only the heightfield is required initially
- **WHEN** the first implementation ships
- **THEN** it SHALL provide the heightfield representation, and the interface SHALL already admit
  the others

#### Scenario: A consumer cannot assume a heightmap
- **WHEN** a system asks for the surface at a position
- **THEN** it SHALL use the terrain query interface, which SHALL be able to answer for any
  representation, including one with multiple surfaces above a point

### Requirement: Tiled hierarchical storage
Terrain SHALL be stored as **tiles** in a hierarchy of levels, never as one global grid.

A tile SHALL carry: height or volumetric data, material layer data, biome assignment, holes, and
runtime metadata, at multiple resolutions.

Tile identity SHALL be stable and derived from a tile coordinate and level, so saves, patches,
and streaming caches key on it. The coordinate system SHALL be replaceable — a planar grid
initially, a cube-face quadtree for planetary worlds later — without changing consumers.

Tiles SHALL stream as part of world cells (see `world-partition-and-streaming`), with terrain data
cooked as a cell channel.

#### Scenario: No global heightmap
- **WHEN** a world is hundreds of kilometres across
- **THEN** terrain SHALL be stored as tiles at multiple levels, and no global grid SHALL exist

#### Scenario: Coordinate system is replaceable
- **WHEN** planetary terrain is introduced
- **THEN** the tile coordinate scheme SHALL change without altering the terrain query, collision,
  or rendering interfaces

### Requirement: Terrain is a geometry source
Terrain rendering SHALL be produced by **meshing tiles into virtual geometry clusters** that enter
the GPU scene like any other geometry (see `virtual-geometry`).

There SHALL NOT be a terrain-specific renderer, a terrain-specific level-of-detail morphing scheme,
or a terrain-specific shadow path.

Meshing SHALL be deterministic and cacheable, and SHALL produce watertight boundaries between
adjacent tiles and between levels, so no cracks appear at tile or detail transitions.

Where virtual geometry is unavailable on a device, terrain SHALL fall back to a conventional tiled
mesh path with the reduced detail reported, exactly as other geometry does.

#### Scenario: Terrain is culled and shaded like everything else
- **WHEN** terrain is visible
- **THEN** its clusters SHALL be culled, rasterised, shaded, and shadowed by the same passes as
  other geometry

#### Scenario: No cracks between tiles
- **WHEN** adjacent terrain tiles are rendered at different detail
- **THEN** their shared boundary SHALL be watertight

#### Scenario: Fallback path
- **WHEN** a device lacks virtual geometry support
- **THEN** terrain SHALL render through the conventional path with reduced detail, not disappear

### Requirement: Terrain material layers
Terrain surface materials SHALL be described by **compact per-texel layer data** — a small number
of dominant layer indices with weights — rather than one weight map per layer across the world.

The number of layers blended at a texel SHALL be bounded and configurable, and the cooker SHALL
report where content exceeds the bound and what was dropped.

Terrain material data SHALL be stored in virtual textures where available (recorded as a seam;
see `virtual-geometry` and the gaps below), and SHALL degrade to conventional tiled textures with
reduced unique detail otherwise.

Terrain shading SHALL go through the material compiler like any other material; a monolithic
terrain shader carrying every layer and every feature SHALL NOT be produced.

#### Scenario: Layers are bounded per texel
- **WHEN** a terrain uses twenty material layers across a region
- **THEN** each texel SHALL store only its dominant few, and shading cost SHALL not scale with the
  layer count of the region

#### Scenario: Exceeding the bound is reported
- **WHEN** an author blends more layers at a point than the bound permits
- **THEN** cooking SHALL report it and name the location, rather than silently dropping a layer

### Requirement: Material frequency separation
Terrain materials SHALL separate spatial frequencies explicitly: **macro** (biome and geology at
kilometre scale), **meso** (material distribution at hundreds of metres), **detail** (breakup at
metres), and **micro** (procedural structure at centimetres).

Frequencies SHALL be composed rather than layered by hand, so obvious tiling does not appear at any
viewing distance.

#### Scenario: No visible repetition
- **WHEN** terrain is viewed from a mountain top and from ground level
- **THEN** neither view SHALL show obvious texture repetition, because different frequencies
  dominate at each scale

### Requirement: Environment-aware terrain material inputs
Terrain materials SHALL be able to read, as first-class inputs: world position, slope, altitude,
curvature, and any **environment field** — biome, moisture, wetness, snow depth, water distance,
water depth, and flow (see `environment-fields`).

Authors SHALL be able to express rules such as steep slopes becoming rock, low flat ground becoming
grass, ground near water becoming wet mud, and high altitude accumulating snow, without painting
each by hand.

Painted data and rule-driven data SHALL compose, with painting able to override rules locally.

#### Scenario: Rules replace painting
- **WHEN** an author specifies that slopes above 40 degrees are rock
- **THEN** terrain SHALL shade accordingly without a painted mask

#### Scenario: Painting wins locally
- **WHEN** an author paints an exception over a rule-driven region
- **THEN** the painted value SHALL apply there and the rule SHALL apply elsewhere

### Requirement: Deformation classes
Terrain deformation SHALL be classified, because the classes cost different amounts and must not
share one mechanism:

| Class | Example | Mechanism | Invalidates |
|---|---|---|---|
| `Visual` | Tyre ruts, footprints | Displacement and decal fields | Rendering only |
| `Gameplay` | Craters, trenches, levelled build sites | Height delta over cooked terrain | Rendering, collision, navigation |
| `Structural` | Tunnels, overhangs carved at runtime | Volumetric edit; requires the SDF representation | Everything |

A `Visual` deformation SHALL NOT invalidate collision or navigation.

Only the affected region SHALL be invalidated, at tile granularity or finer.

#### Scenario: A footprint is cheap
- **WHEN** a character leaves footprints
- **THEN** they SHALL be visual deformation and SHALL NOT trigger collision or navigation rebuilds

#### Scenario: A crater is not
- **WHEN** an explosion craters the ground
- **THEN** the height delta SHALL apply, and collision and navigation SHALL be invalidated for the
  affected region only

### Requirement: Terrain deltas use the persistence overlay
Runtime terrain modification SHALL be stored as a **delta over immutable cooked terrain**, and
cooked terrain data SHALL never be rewritten at runtime.

Deltas SHALL be recorded in the world persistence overlay (see `world-partition-and-streaming`), so
that saves, dedicated server persistence, replays, and editor play-mode changes handle terrain
through the existing mechanism rather than a second one.

```
cooked terrain + terrain delta = current terrain
```

Deltas SHALL be spatially scoped and replicable, so a networked terrain change is a bounded message
rather than a resend of terrain data.

#### Scenario: A mining pit survives a save
- **WHEN** players excavate a pit and the game is saved and reloaded
- **THEN** the cooked terrain SHALL load unchanged and the delta SHALL reproduce the pit

#### Scenario: One persistence mechanism
- **WHEN** terrain, entity, and layer changes are saved together
- **THEN** they SHALL use one overlay, not separate save paths

### Requirement: Terrain collision
Terrain SHALL produce a **collision representation independent of its rendering representation**: a
collision heightfield for heightfield terrain, meshes for mesh terrain, and an appropriate
volumetric or mesh form for SDF terrain.

Collision resolution SHALL be independently configurable and typically coarser than rendering, and
SHALL be streamable at its own granularity: a distant tile MAY be visible without high-detail
physics.

Collision SHALL be registered with physics in bulk on cell activation (see `physics`), and
invalidated regionally on gameplay and structural deformation.

Holes in the terrain surface SHALL be represented in collision, so a cave entrance is not blocked
by an invisible floor.

#### Scenario: Visible without full physics
- **WHEN** a distant tile is visible
- **THEN** it MAY have coarse or no collision resident, according to streaming channels

#### Scenario: Holes are consistent
- **WHEN** a terrain hole is authored for a cave entrance
- **THEN** collision SHALL have the hole, matching the rendered surface

### Requirement: Terrain navigation contribution
Terrain SHALL contribute a **navigation surface** to `navigation`, derived from its collision
representation and its material and slope data.

Terrain change of the gameplay or structural class SHALL emit a **navigation dirty region**, and
navigation SHALL rebuild affected tiles incrementally rather than globally.

Terrain material and environment fields MAY modify navigation cost — mud slowing movement, water
raising cost — through declared mappings rather than bespoke code.

#### Scenario: A trench changes pathing
- **WHEN** a trench is dug across a route
- **THEN** the affected navigation tiles SHALL be rebuilt and agents SHALL path around or through
  it according to slope limits

#### Scenario: Material affects cost
- **WHEN** an area is deep mud
- **THEN** its navigation cost SHALL reflect the declared mapping without special-case code

### Requirement: Terrain hierarchical level of detail
Distant terrain SHALL use progressively coarser representations: real tiles near, coarser virtual
geometry at mid distance, and a **macro terrain representation** at long range, up to continental
and planetary scale.

Transitions SHALL be driven by the same streaming and detail machinery as other content, and SHALL
not be visible as popping.

Macro representations SHALL be derived at cook time, not authored.

#### Scenario: A mountain range is visible from 100 km
- **WHEN** distant terrain is visible
- **THEN** it SHALL render from a macro representation without its tiles being resident

#### Scenario: Transitions are not visible
- **WHEN** the camera approaches distant terrain
- **THEN** representations SHALL transition without a visible step

### Requirement: Terrain modifier stack
Terrain authoring SHALL be **non-destructive**: a terrain SHALL be defined as a generator plus an
ordered stack of modifiers — noise, erosion, spline roads, river carving, area flattening, craters,
and artist sculpting — each of which can be reordered, disabled, or edited after later ones exist.

Cooking SHALL flatten the stack; the runtime SHALL NOT carry it.

Stack evaluation SHALL be deterministic, so a terrain cooks identically from the same inputs, and
SHALL be incremental where a modifier's bounds are known, so editing one modifier does not
re-evaluate the world.

#### Scenario: Reordering does not require redoing work
- **WHEN** an author inserts an erosion pass beneath an existing sculpt
- **THEN** the sculpt SHALL be preserved and reapplied above it

#### Scenario: Cooking flattens
- **WHEN** terrain is cooked
- **THEN** the runtime SHALL receive tile data, and the modifier stack SHALL be absent

### Requirement: Terrain holes and decoration
Terrain SHALL support **holes**: regions excluded from the surface, consistently in rendering,
collision, navigation, and queries, so that cave entrances, building interiors, and pits can cut
through it.

Terrain SHALL support attaching **decoration** — roads, paths, and surface features — as spline or
mask-driven modifications that participate in material blending and can suppress foliage.

#### Scenario: A hole is a hole everywhere
- **WHEN** a hole is authored
- **THEN** rendering, collision, navigation, and height queries SHALL all agree that there is no
  surface there

#### Scenario: A road suppresses vegetation
- **WHEN** a road spline crosses a forest
- **THEN** the road SHALL blend into the terrain material and suppress foliage placement along its
  width

### Requirement: Terrain queries
The engine SHALL provide terrain queries usable by gameplay, AI, VFX, and audio: height at a
position, surface normal, slope, material at a position, biome, hole test, and a ray or vertical
cast against the surface.

Queries SHALL be answerable from the terrain representation without a physics query, and SHALL be
batchable.

A query in a region whose terrain is not resident SHALL return the coarsest resident answer with a
resolution indicator, and SHALL NOT block.

#### Scenario: Placing an object on the ground
- **WHEN** gameplay places an object on the terrain
- **THEN** it SHALL query height and normal directly, without a physics raycast

#### Scenario: Query into unstreamed terrain
- **WHEN** a query targets a region whose fine data is not resident
- **THEN** it SHALL return the coarsest resident answer and indicate the resolution used

### Requirement: Terrain diagnostics
The engine SHALL provide debug views for: tile bounds, levels and residency, meshing and cluster
output, material layer counts and dominant layers, holes, deformation deltas, collision
representation, navigation contribution, and per-tile memory and cost.

The system SHALL report, per tile, why it is at its current detail and residency.

#### Scenario: Why is this tile coarse
- **WHEN** terrain appears coarser than expected
- **THEN** the diagnostics SHALL state whether the cause is streaming, budget, distance, or a
  missing representation
