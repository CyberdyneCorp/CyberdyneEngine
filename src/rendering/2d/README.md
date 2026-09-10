# `src/rendering/2d/` — layer 4

**Cyberdyne's 2D pipeline**: primitives, the sort key, batching, tilemaps, 2D lights and shadows, the
screen-space signed distance field, and `Camera2D`.

**Governed by**: `rendering-2d`, which reaches **Working** at M8.b — task 9.5.

## The CPU half, and why that is the whole module

`rendering-2d`: "2D rendering SHALL use the same RHI, render graph, shader system, and material model
as 3D, but a distinct pipeline optimised for sorted, batched quad and mesh submission."

This module is that pipeline's decisions — the sort key, the batch break, the per-instance data, the
chunk rebuild, the shadow geometry and the distance field — and it names no device, no command buffer
and no pipeline object. The submission is `src/rendering/`'s, the way `culling/` produces a visible
set that `forward/` draws. The consequence is the point: every requirement this module covers is
tested without a GPU.

## Four things worth knowing

**The sort key is one integer.** Layer, order, an optional Y value and a stable tiebreak, packed most
significant first, so sorting is an integer comparison and "identical sort keys do not flicker" is a
property of the key rather than of the sort.

**The batcher does not move the caller's draws.** `build_batches` fills an ORDER array. Two reasons,
and the second decided it: a caller's draw array is often a view over data it owns, and packing the
key once per draw rather than once per comparison is the difference between five thousand sprites
costing a millisecond and costing a tenth of one.

**A chunk has two dirty flags, not one.** An animated tile changes the instance data and not the
geometry, which is the requirement's own distinction — "re-submitted with an updated frame index
**without rebuilding geometry**" — and conflating them would rebuild a chunk's collision every tenth
of a second because a torch is flickering.

**A one-sided occluder is decided by winding.** `edge_casts` compares which side of the directed edge
the light is on, which is what lets a light inside a room escape through the wall's inner face and be
blocked from outside.

## What `rendering-2d` asks for that this module does not yet have

* **The passes themselves.** There is no 2D render pass, no instanced draw, no shader. The primitive
  stream and the batch list are produced; nothing consumes them yet.
* **Canvas groups and effects**, and **2D in world space**: both are render-target work, which is the
  half this module deliberately does not do.
* **Normal-map and specular response** for 2D sprites: the lights carry what a shader would need and
  no shader reads it.
* **Convolution of the SDF into a VFX data interface**: `SignedDistanceField::sample` is the function
  a data interface would wrap, and `vfx-system` is M8.c's.
* **Tilemap physics, navigation and occlusion registration**: the per-chunk shape counts are produced
  and registering them with the servers is the caller's, because a rendering module may name neither.

## Testing

`unit.rendering_2d` — 28 cases over the sort key, the batcher and its break reasons, nine-slice and
line expansion, animation frames, chunked tilemaps and their two dirty flags, the four grid shapes,
negative coordinates, the terrain solver, light gathering and the unlit-layer measurement, one-sided
occluders, shadow volumes, the distance field's sign and oversize, and `Camera2D`'s smoothing, limits,
pixel snapping and canvas strategies.

`integration.rendering_2d_scale` — the five-thousand-sprite batching case the specification states at
its own scale. It measured just over the unit tier's millisecond, and hard rule 7 puts a case that
expensive in the tier above rather than shaving it to a number the specification does not state.
