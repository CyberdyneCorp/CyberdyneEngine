# `src/navigation/` — the navigation stack

The navigation mesh, the queries over it, the region hierarchy above it, flow fields, off-mesh links,
local avoidance and the crowd. `navigation`, reaching **Working** at M8.b (tasks 6.1 and 6.2).

## The line between engine code and an integrated algorithm

`ai-system` draws it once and this module keeps it:

> Proven algorithms MAY be integrated where they are **not differentiating** — navmesh generation and
> pathfinding continue to use the navigation stack's dependencies.

**Recast is that algorithm and voxelisation is that step.** It is declared in `deps/manifest.toml`
under `Zlib`, gated by `CY_NAVIGATION`, and reachable from exactly one translation unit —
`src/build_recast.cpp`, linked `PRIVATE` so `#include <Recast.h>` does not resolve anywhere else in
the engine.

**Detour is deliberately not integrated**, and the reason is not taste. `navigation` requires
navigation tiling independent of world cells, asynchronous queries completing on a *deterministic
tick*, and flow fields and hierarchical refinement that Detour does not have. A runtime owning its
own allocation, its own tile identity and its own query scheduling would have to be worked around at
each of those three points. `deps/manifest.toml`'s `source_subdir = "Recast"` makes the exclusion a
build fact rather than a promise: Detour's `CMakeLists.txt` is never read.

**Both back ends are always compiled.** `NavBuildBackend` is a parameter of `build_tile()`, not an
`#if`, so `integration.navigation_build` asserts the same contract — erosion by the agent radius, the
slope limit, the declared filters — over Recast *and* over the engine-owned rasteriser, in one build.
With `-D CY_NAVIGATION=OFF` nothing is fetched, `recast_available()` answers false, and every suite
still runs.

## What is where

| Concern | Header |
|---|---|
| Tiles, polygons, adjacency, obstacles, off-mesh links | `navmesh.h` |
| Generation from source geometry, and the Recast boundary | `build.h` |
| Baking a world: tiling, incremental rebakes, area volumes, obstacle tile reports | `bake.h` |
| Tile digests, the source fingerprint and the bake identity | `tile_identity.h` |
| The `.cynavmesh` saved-bake codec | `bake_codec.h` |
| A\*, the funnel, simplification, corner rounding, the async queue | `query.h` |
| Chunk-local tilemap polygons and planar path queries | `navigation2d.h` |
| Sparse-voxel 3D paths and surface/volume query dispatch | `volume.h` |
| Renderer-neutral debug overlays and aggregate diagnostics | `debug.h` |
| Regions, the abstract graph, deferred refinement | `hierarchy.h` |
| Flow fields and the cache that shares them | `flow_field.h` |
| Local avoidance, the crowd, path and field following | `crowd.h` |
| `NavMeshSurface`, `NavAgent`, `NavObstacle`, `NavLink`, `NavArea` | `components.h` |
| Path following written once, for `f32` and `Fixed` arithmetic | `follow.h` |
| What navigation guarantees, declared from its worlds' arithmetic | `determinism.h` |

**Fixed worlds.** Under `CrossPlatform` and `Lockstep` (openspec/changes/add-deterministic-math), a
navigation world declares `NavArithmetic::Fixed` and its baked mesh is converted once, at load, into
`cy::movement::FixedNavMesh` ([`src/movement/`](../movement/README.md)), where A*, the funnel, flow
fields and path following run in `Fixed`. `navigation_determinism()` declares the subsystem
`Lockstep` only when every authoritative world is `Fixed` and none takes runtime rebuilds as
authoritative input, and `SamePlatform` otherwise, so a lockstep session with a float world is
refused naming `navigation`. Everything in this module stays `f32`, and this module does not link
the deterministic math module. `follow_path` here and `cy::movement::follow_path` are the two
instantiations of `follow_points` (`follow.h`): one loop, two kinds of arithmetic.

`NavWorlds` binds each navigation world ID to a mesh and its deterministic query queue. An agent
is updated only by the binding matching `NavAgent::world`; obstacles and links are authored with the
same world ID and must be published to that world's mesh. The default `update_agents` overload
continues to update only world zero. Bindings are non-owning, so remove a binding before destroying
its mesh or queue.

A world may bind both a surface mesh and a sparse `NavVolume` under one ID. `NavAgent::representation`
selects the corresponding queue; volume cells are six-connected, can carry area/cost data, and
may be added or removed independently. `NavigationSpace` gives both representations the same
`PathFilter`/`PathResult` and `Vec3` point-path query. `SpatialPathQueue` schedules either source
for deterministic delivery on `submit_tick + latency`; it preserves submission order within that
tick. A blocked volume destination returns a partial path ending on reachable space rather than
silently snapping through the blockage. The volume uses a sparse hash map of occupied cells, so
only navigable air/water/space consumes cell storage.

`NavMesh2D` accepts the navigation polygons authored for each tilemap cell and publishes one mesh
tile per chunk. Rebuilding a changed chunk leaves other chunks' polygon references valid; empty
chunks remove only their own tile. Its path query takes and returns `Vec2` coordinates. Polygons
must fit within a cell, be convex and consistently wound, and use the same units as `cell_size`.
For 2D collision geometry, `rebuild_chunk_from_collision` voxelises convex world-space collision
footprints into the same chunk grid and conservatively excludes every overlapping cell. Its
resolution is `cell_size`, so narrow passages need a correspondingly small cell size.

`draw_navigation_mesh`, `draw_navigation_path`, and `draw_navigation_agents` emit into a
renderer-neutral `NavDebugSink`. Flags select polygon areas, tile boundaries, adjacency, links,
obstacle footprints, corridors, paths, avoidance velocities and neighbour sets independently.
`NavMetrics` can be attached to `PathQueue`, `SpatialPathQueue`, and `NavWorlds` to collect query
counts, elapsed time, path length and repath rate. Passing it to `build_tile` additionally records
voxelisation time per tile. These timing measurements are diagnostic wall time and must never feed
deterministic path or simulation decisions.

## Baking a navigation world

`bake.h` is the loop above `build_tile`, and what the engine's editor service runs for issue #28.
The editor never voxelises, tiles or hashes: it sends a request and records what comes back.

- **Settings.** `NavBakeSettings` holds the authored values: agent radius, height, max slope and step
  height, cell size and height, tile size, layers, tags and the back end. `build_params` maps them
  onto `NavBuildParams` (step height is `agent_max_climb`), `agent_profile` onto the `AgentProfile`
  for `NavMesh::set_profile`, and the tile size is the `NavMesh` constructor argument.
  `validate_bake_settings` refuses `Automatic`: a saved bake names Engine or Recast.
- **Sources.** `NavBakeSource` is the geometry plus the `NavMeshSurface` volumes (`NavSurfaceVolume`),
  the `NavArea` volumes (`NavAreaVolume`), and the obstacles and links. `surface_region` is the union
  of the including surfaces; excluding surfaces become `exclude_volumes`.
- **Tiling.** `bake_tiles` visits every tile whose XZ extent `[x * t, (x + 1) * t]` overlaps the region,
  by z then x, and calls `build_tile` with `bake_tile_bounds`: that extent, and the geometry's Y range
  widened by the step height and a cell below and the agent height above. Each non-empty tile is
  published with `NavMesh::add_tile`. A tile with no walkable cell is reported `empty` and is not an
  error; any previous occupant is removed. A full bake also removes resident tiles outside the region.
- **Progress and cancellation.** A `NavBakeObserver` is told `(done, total, tile)` after every tile and
  answers `Continue` or `Cancel`. A cancelled bake returns its report with `cancelled` set and the
  tiles it finished published.
- **Report.** `NavBakeReport` sums the `NavBuildReport` counters (the triangle counters describe the
  source, the rest are summed) and lists each tile's coordinate, polygon count, emptiness and digest.
- **Incremental rebake.** `rebake_tiles` rebuilds only the tiles overlapping a dirty box, such as the
  union of an edited volume's old and new bounds. Every other tile keeps its digest and slot salt.
  `rebake_surface_tiles` is the incremental form of a bake over the surface region: of the tiles
  under the dirty box it rebuilds those the surfaces cover and removes those they no longer cover.
  It equals a full bake when the mesh was one, every change lies in the dirty box, and the
  geometry's height range held; `bake.h` says why each condition matters.
- **Recast seams.** The Recast back end voxelises a tile with a border of `walkableRadius + 3`
  cells, so the agent-radius erosion does not carve a gap along every tile edge and paths cross
  from tile to tile.
- **Areas and costs.** `assign_triangle_areas` gives each triangle the area of the highest-`node` area
  volume containing its centroid, or its own area. `area_costs` builds the world's `NavAreaCosts`:
  uniform, with each volume's cost on its area. Cost is painted by placing area volumes.
- **Obstacles.** `place_obstacle`, `move_obstacle` and `clear_obstacle` apply `NavMesh::add_obstacle`
  and `remove_obstacle` without a rebuild, and report the resident tiles under the old footprint, the
  new footprint, or both.

### Tile digest, source fingerprint and bake identity

`tile_identity.h` holds three 64-bit FNV-1a values over a fixed little-endian encoding, so they are
the same on every host and can be stored (`hash_bytes` is seeded per process and cannot).

- `tile_digest(NavTileData)` hashes the coordinate, bounds, vertices, polygons (first corner, corner
  count, area, cost, centre) and the corner array. `NavBuildReport::duration_ns` is never part of it.
  `mesh_tile_digest(NavMesh, slot)` computes the same value from a resident tile, so a bake can be
  compared with a direct `build_tile` tile by tile.
- `source_fingerprint(settings, source, producer_version)` hashes the vertices, indices, each
  triangle's layer, tag and area (defaults filled in), the surface and area volumes, the settings,
  the back end and the producer version. Obstacles and links are excluded: they are runtime overlays.
  A saved fingerprint that differs from a recomputed one means the bake is stale.
- `bake_identity(fingerprint, ordered digests)` names a bake. `mesh_bake_identity` computes it over a
  mesh's resident tiles in `ordered_tile_slots` order (layer, z, x).

### The `.cynavmesh` codec

`encode_nav_bake` writes a versioned blob: the magic `CYNAVMSH`, the version, the settings, the
source fingerprint and the bake identity, then each resident tile in coordinate order with its
digest. `decode_nav_bake` re-derives every tile digest and the identity and refuses, with a reason
in the error message, a bad magic, an unknown version, a truncated blob, trailing bytes, a polygon
naming a corner or vertex the tile does not have, a digest mismatch or an identity mismatch.
`install_nav_bake` publishes a decoded asset into a mesh of the same tile size.

`draw_navigation_mesh` reports each polygon with `NavMesh::effective_area`, so an obstacle that marks
polygons with an area recolours them on the overlay.

## Four properties the whole module is shaped by

**A reference into a released tile resolves to nothing, not to something else.** A `PolyRef` is
(tile, salt, polygon) and the salt advances on every publication and every removal, so
`navigation`'s *"the agent's path SHALL be invalidated cleanly and a repath triggered, rather than
dereferencing released data"* is a checkable property. `NavMesh::poly()` answers null; `unit.navigation`
publishes into the same slot and asserts the old reference still does not resolve.

**Navigation owns its own tile layout.** `TileCoord` is in units of `NavMesh::tile_size()` and is
unrelated to any world cell coordinate — *"navigation tiling SHALL be unaffected, since it owns its
own layout"*.

**An asynchronous query completes on a tick derived from its submission, in submission order.**
`PathQueue` does the work in `update()` and delivers on `submit_tick + latency` whatever the work
actually cost, because `ai-system` requires AI decisions that survive replay and reconciliation.

**A region outlives the tile it was built from.** `NavHierarchy::release_tile()` marks a region
non-resident and keeps its identity, its bounds and its edges, so a plan still crosses it
(`AbstractPath::complete` is false and `first_unresident` says where) and `refine()` answers
`Unavailable` rather than dereferencing a tile that is gone. `forget_tile()` is the other operation
and is deliberately separate.

## Avoidance produces a velocity, never a position

`navigation`: *"Avoidance SHALL compute a desired velocity adjustment, not a position: the agent's
controller remains responsible for movement and for physics collision."* So `Crowd::step()` writes
`CrowdAgent::velocity` and touches nothing else; `Crowd::integrate()` is a separate, documented
stand-in for the character controller.

The formulation is **sampled reciprocal velocity obstacles**: a fixed candidate lattice scored
against the time to collision with each neighbour under the reciprocal assumption. It is chosen over
an ORCA linear program because it is deterministic by construction, costs a constant per agent (which
is what makes a tier budget mean anything), and degrades by sampling less — which is exactly what
*"agents at reduced tiers SHALL use cheaper avoidance"* asks for. `crowd.h` carries the table of what
each tier gets.

Two agents exactly head-on are a degenerate case that floating-point noise resolves in a real crowd
and a deterministic simulation must not rely on. Each agent perceives its neighbour displaced along
the perpendicular of the vector to it; that vector points opposite ways for the two of them, so the
bias sends them to opposite sides — with no identity comparison, no shared state and no randomness.

## Streaming has no class, and that is the design

`navigation` asks for four properties, not for a component: tiles and regions load and unload with
the world, the hierarchy is updated, in-flight queries remain valid or fail cleanly, and navigation
keeps its own tile layout. Each is a property of `NavMesh`, `NavHierarchy` and `update_agents()`
working together, and a `NavStreamer` owning all three would only be a place for a fifth policy to
accumulate. `tests/test_streaming.cpp` is what makes the four checkable.

**What is not wired, stated rather than implied**: nothing consumes
`world-partition-and-streaming`'s cell lifecycle events, and no cooked cell channel carries a
navigation payload. Both are wiring into `cy::world`, and both are named in this milestone's
handover.

## The funnel

`straighten()` pulls a corridor taut with the simple stupid funnel. Every side test in it reads
`triarea2`'s sign as **positive is left** (counter-clockwise in the XZ plane), and `portal_sides()`
orders each portal's endpoints by the same convention; the two must agree, and a funnel whose tests
disagree with its portals still pulls a path along one cell row straight — both signs agree there —
while walking every diagonal as a staircase of portal corners. `unit.navigation`'s funnel cases pin
the convention with diagonal and off-axis paths in both directions, a path around an obstacle and
one through an L-shaped corridor.

The path is taut **within the corridor it is given**. On a grid of quads, A\* may return a staircase
of cells that does not contain the straight line between two points, and over that corridor the taut
path legitimately bends at a cell corner. A vertex shared by consecutive portals is emitted once.

## Suites

| Suite | Kind | What it holds |
|---|---|---|
| `unit.navigation` | unit | tiles, adjacency, stale references, obstacles, links, A\*, the funnel, budgets, partial paths, the async queue, the hierarchy, the components, and streaming |
| `unit.navigation` (`test_bake_codec.cpp`) | unit | the `.cynavmesh` round trip and its refusals |
| `integration.navigation_bake` | integration | the bake API: bake equals `build_tile` tile by tile on **both** back ends, empty tiles, cancellation, incremental rebakes, area volumes and costs, obstacle tile reports, the fingerprint and the identity |
| `integration.navigation_build` | integration | generation over **both** back ends: erosion, the slope limit, the filters, the refusals |
| `integration.navigation_crowd` | integration | avoidance, priority, tiers, determinism, **eight thousand agents** and teardown under load |
| `integration.navigation_fields` | integration | flow-field generation, determinism, incremental regeneration, the reference-counted cache |

The exit criterion *"8,000 agents hold their budget"* is `integration.navigation_crowd`'s last case
but one. It reports the **median** tick and the worst beside it, never the best, and its threshold is
several times the declared budget so that it detects a regression rather than benchmarking the host.
