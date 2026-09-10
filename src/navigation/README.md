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
| A\*, the funnel, simplification, corner rounding, the async queue | `query.h` |
| Regions, the abstract graph, deferred refinement | `hierarchy.h` |
| Flow fields and the cache that shares them | `flow_field.h` |
| Local avoidance, the crowd, path and field following | `crowd.h` |
| `NavMeshSurface`, `NavAgent`, `NavObstacle`, `NavLink`, `NavArea` | `components.h` |

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

## Suites

| Suite | Kind | What it holds |
|---|---|---|
| `unit.navigation` | unit | tiles, adjacency, stale references, obstacles, links, A\*, the funnel, budgets, partial paths, the async queue, the hierarchy, the components, and streaming |
| `integration.navigation_build` | integration | generation over **both** back ends: erosion, the slope limit, the filters, the refusals |
| `integration.navigation_crowd` | integration | avoidance, priority, tiers, determinism, **eight thousand agents** and teardown under load |
| `integration.navigation_fields` | integration | flow-field generation, determinism, incremental regeneration, the reference-counted cache |

The exit criterion *"8,000 agents hold their budget"* is `integration.navigation_crowd`'s last case
but one. It reports the **median** tick and the worst beside it, never the best, and its threshold is
several times the declared budget so that it detects a regression rather than benchmarking the host.
