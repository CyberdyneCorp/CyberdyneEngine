# Environment: fields, terrain, foliage, and water

## Why

The engine can render a virtualised world, stream it, light it, and partition it — and has nothing
to put in it. Terrain, vegetation, and water are the content that makes an open world an open
world, and all three are currently absent.

They have to be specified together, because the interesting problems are all at their boundaries:

- A shoreline needs terrain height, water level, and a wetness value that a terrain material
  reads and a water system writes.
- Foliage placement needs slope and altitude from terrain, moisture from a hydrology model that
  does not exist yet, and exclusion from roads and water.
- Terrain material needs to know its distance to water; water needs to know the shape of the bed.
- Wind moves foliage, water, cloth, VFX, clouds, and audio — and if each invents its own, they
  visibly disagree in the same frame.

Specified separately, each would grow its own masks, its own noise, its own wind, and its own
notion of "moisture", and the seams between them would be the permanent source of visual bugs.

So the first thing this change specifies is not terrain. It is **`environment-fields`** — a sparse,
streamed, world-scale data substrate that terrain, foliage, water, VFX, audio, AI, and future
weather and procedural systems all sample by world position. Putting moisture inside terrain would
make foliage depend on terrain to know whether it is wet. Putting wind inside foliage would make
water depend on foliage. The substrate belongs underneath all three.

Three rules govern the rest:

> **1. Terrain, vegetation, and water are world-scale streamed datasets, not collections of
> heavyweight objects.**

> **2. Representation detail is independent of simulation detail.** Visible terrain, foliage, or
> water does not imply full physics, navigation, or ECS residency — the same separation of axes
> the world partition already establishes.

> **3. Environmental data is shared.** One substrate, sampled by every system that reasons about
> the world, rather than one set of masks per subsystem.

## What changes

**`environment-fields`** — CyberField. Sparse tiled fields addressed by world position, with
declared semantics, resolution, and interpolation; producers and consumers; streaming tied to world
cells; the **wind field** as a named shared field with global, weather, and local contributions;
and determinism rules for fields that gameplay reads.

**`terrain`** — CyberTerrain. Three representations behind one interface (heightfield, mesh, signed
distance field), with only the heightfield required initially; tiled hierarchical storage; terrain
as a **geometry source producing virtual geometry clusters**, closing the seam `virtual-geometry`
reserved rather than adding a parallel renderer; compact per-texel material layers over virtual
textures; macro-to-micro material frequency separation; the three classes of deformation and the
terrain delta overlay; collision and navigation as separate derived representations; a
non-destructive modifier stack that cooking flattens; and terrain HLOD to planetary scale.

**`foliage`** — CyberFoliage. Compact GPU instances in spatial clusters, not entities;
**promotion to ECS** when gameplay needs it and demotion when it does not, with identity and state
preserved across both; deterministic procedural placement from rules plus seed, storing only
exceptions; GPU-generated grass; foliage-specific geometry and rendering classes; hierarchical wind
response; a GPU interaction field; and regional environmental state.

**`water`** — CyberWater. One water body abstraction over several simulation backends; cascaded
spectral ocean; spline river networks with flow fields; lakes; an optional shallow-water solver;
the **shared displacement contract** that keeps physics and rendering agreeing about where the
surface is; shoreline as a joint terrain–water computation that writes wetness back into the field
substrate; a water surface material closure with absorption and scattering; underwater rendering
driven by body parameters; foam as a persistent advected field; water queries and buoyancy; water
navigation; and streaming as body segments rather than independent objects.

## Impact

- **New**: `environment-fields`, `terrain`, `foliage`, `water`
- **Modified**: `virtual-geometry` (terrain ceases to be a deferred deformation class),
  `vfx-system` (wind and environment data come from the shared substrate),
  `physics` (heightfield and buoyancy interfaces), `navigation` (environment-driven change and
  water navigation), `world-partition-and-streaming` (environment payloads in the contracts
  table), `rendering-materials-and-shading` (a water shading model), `material-compiler`
  (environment-aware material inputs), `editor-architecture` (environment authoring tools),
  `thirdparty-dependencies`
- **Gaps this change makes more urgent rather than closes**: **virtual textures** — terrain
  materials are the strongest case for them yet; **weather** — wind and precipitation are specified
  as field producers against a system that does not exist; **procedural generation** beyond foliage
  rules; and **hydrology and erosion**, whose seams are reserved
