# navigation Spec Delta

## ADDED Requirements

### Requirement: Tiled bake with tile identity
The engine SHALL bake a navigation world by building every tile whose XZ bounds overlap the source region with `build_tile`, then adding it to the world's `NavMesh`.

The bake SHALL follow these rules:
- It SHALL report per-tile progress and aggregate diagnostics: triangles in, filtered and steep, spans, polygons, vertices, and the back end used.
- A tile with no walkable cells SHALL be reported as empty and SHALL NOT fail the bake.
- Each tile SHALL have a deterministic digest over its coordinate, bounds, vertices, polygons and corners. Wall-clock timing SHALL be excluded from the digest.

#### Scenario: Bake equals build_tile tile by tile
- **WHEN** a known map is baked through the bake API and each tile is also built directly with `build_tile` using the same settings, geometry and tile bounds
- **THEN** both sides SHALL produce the same set of tile coordinates and equal digests for every tile

#### Scenario: Empty tile
- **WHEN** a tile inside the region contains no walkable cells
- **THEN** the bake SHALL succeed and SHALL report that tile as empty

### Requirement: Incremental rebake of affected tiles
Editing an area volume, a surface volume or source geometry SHALL rebuild only the tiles that overlap the union of the edit's old and new bounds. Moving or editing an obstacle SHALL re-apply its footprint without a voxel rebuild. In every case, the update SHALL report the tiles it affected. Tiles outside the dirty region SHALL keep their digest and slot salt.

#### Scenario: Area edit rebuilds one tile
- **WHEN** an area volume that lies entirely within one tile of a multi-tile mesh changes its area type
- **THEN** only that tile SHALL be rebuilt, and every other tile's digest and salt SHALL be unchanged

#### Scenario: Obstacle blocks and restores a path
- **WHEN** an obstacle is added over the only corridor between two points
- **THEN** the path between them SHALL no longer be found in full
- **AND WHEN** the obstacle is removed
- **THEN** the full path SHALL be found again with its original cost

### Requirement: Navmesh persistence and cook identity
A bake SHALL be encodable as a versioned `.cynavmesh` asset. The asset SHALL hold the settings, the back end, the source fingerprint, the bake identity, and each tile with its digest.

Decoding SHALL re-derive every tile digest and SHALL refuse a mismatch or an unknown version. A `navmesh` cook producer SHALL validate the saved asset against the world's recorded bake identity. Its content digest and the producer version SHALL contribute to the world's cook identity.

#### Scenario: Round trip
- **WHEN** a bake is encoded and decoded
- **THEN** every decoded tile SHALL have the same digest as the baked tile

#### Scenario: Corrupted asset refused
- **WHEN** one byte of a tile's data in a saved asset is changed
- **THEN** decoding and cooking SHALL refuse the asset with a named reason

#### Scenario: Rebake changes the cook identity
- **WHEN** a world's navmesh is rebaked with different settings
- **THEN** the navmesh producer's node key SHALL change

### Requirement: Stale bake detection
The engine SHALL compute a source fingerprint over the bake inputs: source triangles, per-triangle layer, tag and area, surface and area volumes, the settings, the back end and the producer version. It SHALL store the fingerprint with the bake. The engine SHALL report a bake as stale when the fingerprint recomputed from the current sources differs from the stored one.

#### Scenario: Geometry edit makes the bake stale
- **WHEN** a mesh that contributes to a baked world is moved or its geometry changes
- **THEN** the navigation status SHALL report the bake as stale

#### Scenario: Undone edit is not stale
- **WHEN** that geometry edit is undone
- **THEN** the navigation status SHALL report the bake as current
