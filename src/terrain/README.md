# `src/terrain/` — CyberTerrain

The ground surface as a **streamed hierarchical dataset** rather than a giant mesh or a giant
heightmap. M10 section 2 (tasks 2.1 and 2.2), and `terrain` reaching **Working**.

Terrain is a **producer into `environment-fields`** and a consumer of it. It writes exactly one
standard field — `soil` — through `environment::FieldRegistry::claim()`, and it reads moisture,
wetness, snow depth, water distance, depth and flow like every other consumer. It does not own them,
and `test_producer.cpp` holds the refusal a second producer of `soil` gets.

## What is here

| file | what it carries |
|---|---|
| `include/cy/terrain/tile.h` | `TileLayout` and the replaceable coordinate scheme, `TileId`, `TerrainTile`, the sparse `TerrainStore`, the cook-time macro derivation, and the report of why a position is at the detail it is |
| `include/cy/terrain/surface.h` | The query interface: `SurfaceSource`, the required `HeightfieldSource`, the `MeshSource`, and `TerrainQuery`'s column, casts, ray and batch |
| `include/cy/terrain/material.h` | The bounded texel and the overflow report, the four frequency bands, the rules that read environment fields, painting, and the runtime virtual-texture page producer |
| `include/cy/terrain/deform.h` | The three deformation classes, the two delta channels, the overlay round trip and the bounded replication message |
| `include/cy/terrain/meshing.h` | `mesh_tile()`: the grid, the level stitch, the holes, the fallback path and the cache key |
| `include/cy/terrain/collision.h` | The collision heightfield physics takes, and the navigation surface derived from it with declared cost mappings |
| `include/cy/terrain/stack.h` | The non-destructive modifier stack, its derivation key, its declared halo and the cook that flattens it |
| `include/cy/terrain/system.h` | The `soil` producer claim, the declared consumption, the cooked field-tile loader, and the one call that propagates a deformation's invalidation |
| `include/cy/terrain/cook.h` | `cy::terrain-cook`: a tile through `rendering::vg::build_geometry()`, and the surface material through the material compiler |

## The seven decisions a reader should know before changing anything

**1. The query primitive is a COLUMN, not a height.** `terrain` requires the interface to answer
"including one with multiple surfaces above a point", and a `height_at()` returning one float cannot.
So `TerrainQuery::column()` writes every surface highest-first and `sample()` is spelled in terms of
it. A heightfield answers with one entry, a mesh cliff with two, an SDF cave will answer with as many
as it has. Nothing in this module is named `heightmap`, and the required representation is a
`SurfaceSource` like any other.

**2. Heights are quantised against the LAYOUT, not against the tile.** A per-tile height range is the
obvious encoding and it makes the shared edge between two tiles two different numbers. One range,
declared on the layout, makes a boundary bit-exact by construction — and it is what makes
`derive_coarse()` an exact decimation, so a level transition has no seam either.

**3. There are TWO delta channels and different callers read them.** `height_delta()` is the gameplay
surface — queries, collision and navigation. `visual_delta()` is rendering only. A single array with a
flag beside it satisfies the invalidation half of "a footprint is cheap" and fails the query half: a
gameplay query that read the array a footprint wrote would return ground half a centimetre lower where
somebody walked. `HeightDeltaSource`, the interface the query path sees, exposes only the first.

**4. Deltas live at level 0 and every level reads them.** A coarse tile's sample stands at the same
place in the world as one of the fine lattice's, so a macro query resolves its delta by walking down.
Writing the stamp into every level would cost four thirds of the memory, would make a replicated
message carry a coarse tile spanning the whole map, and would leave two answers to keep in step.

**5. Watertightness has two cases and only one of them needs a rule.** Adjacent tiles at one level
share their boundary because a tile owns 64 quads and 65 sample lines and both derive the position
from one absolute lattice index — no stitching at all. Between LEVELS the fine tile yields onto the
coarse tile's segment, and the coarse tile is never touched: a tile whose mesh depended on its
neighbours' neighbours would not be cacheable.

**6. A structural deformation is REFUSED, naming the representation it needs.** This engine has no
signed distance field; `terrain`'s own table marks it Planned. `TerrainDeltaStore::apply()` returns
`NotImplemented` and changes nothing, rather than carving a height delta and reporting a tunnel.

**7. The renderer is a separate target.** `cy::terrain` names no device, no pipeline and no material
compiler; `cy::terrain-cook` is the two calls that name the renderer —
`rendering::vg::build_geometry()` and an ordinary `rendering::material::MaterialGraph`. That split is
what makes "there SHALL NOT be a terrain-specific renderer" checkable: there is nowhere else in the
module for one to be.

## Designed for the rows that write into this one

- **PCG.** `ModifierStack` is shaped to be lowered into `procedural-content-generation` rather than to
  replace it, and it already obeys the four conditions M10's spike made binding (design.md §1.6):
  randomness is `determinism::RandomStream` keyed by stable identifiers, every modifier declares its
  reach and evaluation reads nothing outside it, an iterative modifier runs all its declared
  iterations rather than a budget, and evaluation is a pure function of the stack and the tile.
- **Foliage.** `ModifierStack::suppresses_foliage()` is the declared mapping a road's decoration layer
  answers, so suppression is a question foliage asks rather than a special case it carries.
- **Water.** Terrain reads `water-distance`, `water-depth` and `water-flow` as ordinary material rule
  inputs; it computes none of them.

## What is NOT here, and what M10's later rows owe

- **No streaming binder.** `world::Channel::Terrain` exists and `cell_tile_footprint()` is the
  arithmetic a cooker and a streamer both need, but nothing yet pages terrain tiles through
  `residency::ResidencyServer` the way `environment::FieldStreaming` pages field tiles.
  `TerrainStore::note_wanted()` is the seam the streamer reports through, and `explain()` already
  distinguishes a budget refusal from a distance.
- **No signed distance field**, and therefore no runtime excavation — see decision 6.
- **No shader.** `cy::terrain-cook` authors the surface material as a graph and compiles it; binding
  the produced virtual-texture pages on a device belongs to the renderer-facing work that first
  samples them.
- **No cooked tile format on disk.** `ModifierStack::flatten()` produces tiles into a store;
  serialising them into a cell's `Terrain` channel is `save-and-persistence`'s encoding.

## Suites

    ctest --test-dir build/<label> -R "terrain" --output-on-failure

`terrain` is the unit suite. `terrain_authoring` (the modifier stack), `terrain_persistence` (the
overlay round trip and the replication message), `terrain_geometry` (meshing, watertightness,
collision and navigation) and `terrain_cook` (the two engine builders) are integration suites,
because each of them evaluates tens of thousands of lattice samples and the taxonomy in
`testing-and-quality` places a case that expensive in the next suite up.
