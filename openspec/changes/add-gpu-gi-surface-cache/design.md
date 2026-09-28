# Design

## One module, two classes, and the host stays the authority

`GpuGiScene` owns the device copy of the scene and the three compute pipelines; `GpuSurfaceShading`
is a `gi::SurfaceShadingBackend` over it. Neither solves a brick, allocates a card or decides what to
update: the field, the surface cache and its scheduler stay in `cy::rendering-gi`, and the device
copy follows them. That keeps the layering the issue asks for — layer-4 GI stays device-free — and
it keeps the host implementation the oracle by construction, because the device path runs the host
path's decisions and replaces only its arithmetic.

## The field on the device: a toroidal page table over the host's brick pool

The host field keys bricks by world brick coordinate in a hash per level and retires every brick
outside a level's window at each scroll. The device copy is one word per brick of each level's
window, addressed by the brick coordinate modulo the window: inside the window a cell holds exactly
the brick the hash holds, and a coordinate outside the window is exactly a key the hash does not
hold, which the device answers with the level's far value as the host does. The brick pool is the
host's, slot for slot, so a change record carries the slot and the upload is one copy of 64 floats.

Incremental by generation. `DistanceField::generation()` counts scrolls and `last_changes()` lists
every brick the last scroll solved — re-solved because an invalidation reached it, or solved because
a scroll exposed it. A device copy exactly one generation behind applies that list; anything else
rebuilds the table from `visit_bricks` and says so (`FieldUploadReport::full`). The invalidation
causes reach the field through `IlluminationSystem::service_invalidations`, which already skips it
for `SkyChanged`, so a sky change uploads no brick.

## The cards: a revision per page, and a grid both sides index

`SurfacePage::revision` is bumped by allocation, release and invalidation — every change the host
makes to a page — and by nothing a shading pass does. The device copy writes a page whose revision
moved and skips the rest. A shading backend records the revision it submitted and drops a result
whose page moved while it was in flight, so an invalidation between submit and retire is never
undone by a stale answer.

A traced hit resolves through the card that `SurfaceCache::radiance_at` would pick: within the lookup
radius, inside the 75-degree alignment cone, least `distance^2 / alignment`. The host finds it with a
dynamic BVH, which a device cannot index; `CardGrid` is a hashed uniform grid at the lookup radius,
built by a counting sort so its bytes are a function of the pages alone, and the device and the host
snapshot index the same grid. Equal scores go to the lower handle on both sides; the cache breaks
them by tree order, and `integration.render_gi_pipeline` checks the snapshot against the cache's own
lookup over every card of the room with and without a nudge.

## Card lighting: two dispatches, because a gather reads what the update writes

A card's gather looks up other cards, and some are being shaded by the same dispatch. So
`cyGiShadeCards` reads last frame's state everywhere and writes only a results buffer, and
`cyGiCommitCards` scatters the results into the card state after the graph's barrier. The host
oracle is the same: `CardSnapshot` is a copy of the cache taken before the update, and `CardGather`
reads it.

The terms, in `SurfaceCache::shade`'s order: direct light is `shaded_direct` through
`ShadowMapOccluder` — a shadow ray pointing back along the shadow map's light within 0.9999 is the
map's, any other is the field's, `SoftwareTracer::occluded` transcribed; the gather is
`CardGather::gather` — cosine rays from `hemisphere_direction`, the field's sphere trace, a hit read
from the snapshot, a miss answered by `SkyTerm::radiance`; accumulated is albedo times the gather.

This is the surface-cache radiosity of the issue's stage 2, and it is deliberately not a gather from
the radiance cache: the radiance cache is host-side until stage 3, and a device cache that read it
would need it on the device first.

## The schedule is the host's, shared rather than copied

`SurfaceCache::select` is the prioritised selection lifted out of `service` without a change of
behaviour: starved pages first, then visible, invalid, error and age, then handle, the first
`budget` of them. The host update and the device backend call the same function, and
`render.gi_gpu`'s budget case recomputes it from the device cache's own pages and requires the
dispatch to have shaded exactly those pages and changed no other.

## Asynchronous seam, synchronous suites

`SurfaceShadingBackend::submit` takes a selection; `retire` folds results back and runs at the start
of the next update (or `SurfaceCache::collect`). A frame therefore sees a bounce one frame late,
which is the latency `system.h` already documents for swapping its steps 6 and 7. The suites drain
each frame, so every comparison is of the same update on both sides.

## Floating point

A driver may fuse a multiply and an add where the host does not, and a sphere trace turns a
last-bit difference into a different step count only where a march grazes a surface. The bounds are
on the fraction of answers beyond a small tolerance and on the typical difference, and the suite
prints the measured numbers beside each bound.
