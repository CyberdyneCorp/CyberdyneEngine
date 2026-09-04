# `src/rendering/culling/` — layer 4

A scene of instances becomes a per-view visible set: spatial indexing, frustum culling, level of
detail, visibility ranges and HLOD, and shadow caster culling.

**Governed by**: `rendering-culling-and-lod`, at **Seed** for M3. Task 4.4.3.

## The files

| File | What it holds |
|---|---|
| `spatial.h` | the two `DynamicBvh`s, the flat always-visible array, and the dense bounds/flags/mask arrays the broad phase actually reads |
| `cull.h` | `cull_view()` — layer, then frustum, then distance — its typed result lists, its diagnostics, and the shadow caster cull |
| `lod.h` | screen coverage, level selection with hysteresis, the cross-fade band, visibility ranges and HLOD resolution |

## Four things worth knowing before changing anything here

**The tight bounds are here and the fat ones are in the tree.** `cy::DynamicBvh` stores bounds grown
by a margin, which is what makes a moving object free until it leaves its expansion — and which makes
those bounds useless as a cull result, because they accept a margin's worth of instances that are
really outside. The tree skips subtrees; the dense array answers.

**The order of the three tests is a requirement, not an optimisation.** "Layer rejection precedes
geometry" is a scenario in the specification. A layer test is one AND over a word already loaded; a
frustum test is six dot products.

**The merge is in partition order, never completion order.** `jobs::JobSystem` partitions from the
count and the grain alone, so a parallel cull produces a byte-identical list whatever the workers do.
That is design.md §6 one level below the sort key: an order that depended on thread timing would make
the sort's tie-break the only thing between the frame and non-determinism.

**Occlusion culling and GPU-driven culling are M6's.** The specification names both; what is here is
the seam — `CullStatistics::rejected_by_occlusion` exists and reads zero, and `kSpatialIgnoreOcclusion`
is already a flag — so the day the hierarchical depth buffer lands, nothing above this module changes
shape.

## What it does not depend on

No device, no render graph, no shader. Culling is arithmetic over bounds, flags and layer masks, so
every case in `tests/` runs headless. That is a property of the dependency list rather than a rule
this file states.
