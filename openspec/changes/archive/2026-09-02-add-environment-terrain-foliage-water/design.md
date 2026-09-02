# Design: CyberEnvironment

## 1. The field substrate comes first, and it is not part of terrain

The instinct is to put moisture, biome, and snow depth inside terrain, because terrain is where
they are painted. That instinct is wrong, and following it has a specific cost: foliage would
depend on terrain to know whether the ground is wet, water would depend on foliage for wind, and
audio would depend on water for flow. Each dependency is defensible on its own and the set of them
is a knot.

`environment-fields` is therefore specified first and separately: sparse tiled data addressed by
world position, streamed with world cells, with declared semantics and interpolation. Terrain,
foliage, water, VFX, audio, AI, and future weather and procedural systems are all **producers or
consumers** of fields, not owners of each other's data.

The immediate payoff is the shoreline: water writes wetness and flow into fields, terrain
materials read them, foliage placement avoids them, and audio picks flow up for river sound —
without terrain and water knowing anything about each other beyond the field they share.

The rule that keeps it honest: a field has **one producer**. Two systems writing the same field is
a conflict resolved at configuration time, not a last-writer-wins race.

## 2. Terrain is a geometry source, not a second renderer

`virtual-geometry` already reserved this seam, stating that terrain "SHALL produce virtual geometry
rather than requiring a duplicate representation streamed alongside it". This change closes it.

Terrain meshing produces clusters, which enter the GPU scene, get culled, rasterised, shaded, and
shadowed exactly like any other geometry. There is no terrain-specific renderer, no terrain LOD
morphing scheme, no separate terrain shadow path.

The honest qualification: **the heightfield does not go away**. Physics wants a heightfield,
navigation wants a heightfield, gameplay queries want to ask for the height at a point, and none of
those want to intersect clusters. So terrain keeps its source representation and *derives* several
consumer representations from it — rendering clusters, a collision heightfield, a navigation
surface contribution, and a ray tracing proxy. What is unified is rendering; what stays separate is
everything that is not rendering, and that is deliberate rather than incomplete.

## 3. Three representations, one required

A world is rarely only a heightfield. Cliffs, overhangs, arches, and caves are not functions of
`(x, y)`, and an engine whose every consumer assumes a heightmap can never grow them.

So the terrain interface admits **heightfield**, **mesh**, and **signed distance field**
representations, and V1 requires only the heightfield. The value of specifying the other two now is
not that they are built now — it is that no consumer is permitted to assume terrain is a
heightfield, so adding them later is an implementation, not a rewrite.

## 4. Three classes of deformation, because they cost different amounts

A footprint, a crater, and a tunnel are not the same operation, and routing them through one
mechanism makes the cheap case expensive:

| Class | Example | Mechanism | Affects |
|---|---|---|---|
| Visual | Tyre ruts, footprints | Displacement and decal fields | Rendering only |
| Gameplay | Craters, trenches, levelled build sites | Height delta over the cooked terrain | Rendering, physics, navigation |
| Structural | Tunnels, overhangs carved at runtime | Volumetric edit | Everything, and requires the SDF representation |

Only the gameplay and structural classes invalidate collision and navigation. A footprint that
triggered a navigation rebuild would be a performance bug with a plausible-looking cause.

## 5. Terrain deltas reuse the persistence overlay

Runtime terrain change is stored as a **delta over immutable cooked terrain**, which is the same
shape as the world's persistence overlay — and it uses that overlay rather than inventing a second
mechanism. Saves, server persistence, replays, and editor play-mode changes then handle terrain
without any of them knowing terrain is special.

## 6. Foliage: instances by default, entities by exception

A million trees cannot be a million entities, and the architecture cannot be "a million trees are
never interactive" either. The bridge is **promotion**: a GPU instance becomes an ECS entity when
gameplay touches it, and demotes back when it stops mattering.

Two properties make promotion safe, and both are specified rather than assumed:

- **Identity is preserved across promotion and demotion.** A promoted tree carries a persistent
  identity derived from its cluster and index, so damage, removal, and burn state survive the round
  trip. A tree you chopped must not return because it demoted.
- **Demotion is state-preserving or refused.** An instance whose state cannot be represented in the
  compact instance form — mid-fall, partially destructed, physically constrained — stays an entity.
  Silently discarding that state to save memory is worse than the memory.

## 7. Store rules and exceptions, not blades

Procedural placement is derived from a rule graph plus a seed plus a region identifier, so a region
is regenerated identically rather than serialised. Only **exceptions** are stored: instances a
designer moved or deleted, and instances gameplay destroyed.

This has a consequence worth stating plainly, because it is the failure mode: **changing a rule
graph regenerates the region, and exceptions may no longer have anything to attach to.** An
exception is therefore anchored spatially and by stable instance identity, re-resolved after
regeneration, and reported as **orphaned** when it cannot be — surfaced, not discarded. This is
the same decision as the prefab override conflict, for the same reason: silently dropping
deliberate work is the worst available option.

## 8. One wind field

Foliage, cloth, VFX, water, clouds, and audio all need wind. `vfx-system` already lists "wind and
force fields" among its data interfaces, which is exactly the moment to make it a shared field
rather than a VFX-owned one.

Wind is a field with layered contributions — prevailing, weather-driven, local gusts, and transient
sources such as explosions and vehicle wakes — sampled identically by every consumer. Trees, smoke,
and water then agree about which way the wind is blowing, which they will not if each integrates
its own.

## 9. The water contract that matters: physics and rendering must agree

The classic water bug is a boat floating on a flat plane while the screen shows four-metre swell.
It happens because the renderer displaces the surface on the GPU and physics samples an analytic
approximation, and the two drift.

So a water body declares a **displacement contract**: which frequency bands are **authoritative**
— evaluated identically for rendering, physics queries, and gameplay — and which are
**visual-only**. Large swell is authoritative. Capillary detail is not, and is explicitly declared
so, rather than being an undocumented discrepancy.

Everything that asks where the surface is goes through one query, and the query is defined to
return the authoritative bands. A visual-only band that gameplay can feel is a specification
violation, not a tuning problem.

## 10. Water is one abstraction over several backends

Ocean, lake, river, and pool are not four systems. They are one **water body** abstraction —
identity, material, level, bounds, streaming — with a declared **simulation backend**: cascaded
spectral synthesis for oceans, smaller spectral models for lakes, spline flow for rivers, and an
optional shallow-water solver for shorelines, floods, and gameplay water.

Particle and volumetric fluid solvers are excluded from this change and the backend interface is
what keeps them addable. `vfx-system` already defers fluids with reserved seams; those two seams
are the same seam and are noted as such.

## 11. Shoreline is where the three systems meet, and it is owned by water

The shoreline needs terrain height, slope, and material; water level and wave approach; and it
produces wetness, foam, and shore darkening consumed by terrain materials and VFX.

Ownership has to go somewhere or it becomes a shared mutable mess. It goes to **water**, which
reads terrain through fields and the terrain interface and **writes wetness and flow back into
fields**. Terrain never reads water directly; it reads a field. That keeps the dependency acyclic,
which matters because these are exactly the two systems most likely to end up circularly dependent.

## 12. What this makes more urgent

**Virtual textures.** Terrain material is the strongest case for them in the entire engine: a
kilometre-scale surface with unique albedo, normal, roughness, and layer data cannot be a
conventional texture set. Terrain is specified against virtual texturing as a seam, and the seam is
now load-bearing.

**Weather.** Wind, precipitation, and temperature are specified as **field producers** against a
system that does not exist. The fields exist and can be driven manually or by a project; a weather
capability would become a producer without changing any consumer.

**Hydrology and erosion.** Rainfall to runoff to streams to rivers to lakes, and erosion feeding
back into terrain, is a coherent future capability. Its inputs and outputs are all already fields,
which is the argument for having built the substrate first.

## 13. Build order

| Phase | Contents |
|---|---|
| 1 | Tiled heightfield terrain, world partition integration, terrain collision, basic terrain material, static GPU foliage, flat water bodies |
| 2 | Terrain to virtual geometry, virtual terrain textures, hierarchical streaming, biome fields, procedural foliage, wind field |
| 3 | Rivers and splines, shoreline, water depth and refraction, foam, water queries and buoyancy |
| 4 | Terrain runtime deformation with navigation invalidation, GPU grass, foliage interaction and promotion, shallow water |
| 5 | Spectral ocean, waterfalls, underwater rendering, caustics, foliage destruction and environmental state |
| 6 | Signed distance field terrain, caves, runtime excavation, erosion and hydrology |

**Phase 2 is the milestone that matters.** Phase 1 is a terrain system like any other engine's;
phase 2 is the point at which terrain stops being a special case and becomes geometry, foliage
stops being placed by hand, and the field substrate has real producers and consumers. Everything
after builds on that.
