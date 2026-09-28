# Design

## Mobility is per light, and the default is today's bake

`LightmapContent` decided for every light at once whether its direct term was baked. Mobility moves
that decision onto the light. The mapping keeps both old settings meaning what they meant:

| mobility     | `content = Indirect`              | `content = DirectAndIndirect` |
|--------------|-----------------------------------|-------------------------------|
| `Static`     | direct + indirect                 | direct + indirect             |
| `Stationary` | indirect, and a shadow-mask channel | direct + indirect           |
| `Movable`    | nothing                           | nothing                       |

`Stationary` is the default because a light with no mobility set was, in effect, one: its bounce was
baked and the frame shaded its direct term. A level that sets no mobility bakes the same texels it
did. The mask is new output beside them, and the texel planes do not change.

`Movable` leaves the path tracer's light list entirely, so its bounce is not baked either. The
dynamic caches' seeds still see every light, because those caches own a movable light's GI.

## The shadow mask

A stationary light's direct term stays dynamic; what is baked is its visibility. A mask texel is the
fraction of the light's extent visible from the texel: `kShadowMaskSamples` points spread over the
light (a disc of `radius` facing the texel for a punctual light, a cone of angular radius `radius`
about the direction for a directional one), each tested with `MeshSceneTracer::occluded` from the
texel lifted off its surface. A point light (`radius = 0`) gives a hard mask, as its dynamic shadow
would. Transparent surfaces transmit on average through the tracer's own hashed stop, and alpha masks
cut through, as they do for every shadow ray of the bake.

**Four channels, one plane.** Channel `c` holds the `c`-th stationary light in the scene's order,
and `BakedLightmap::shadow_lights[c]` is its id. A fifth stationary light is refused by name rather
than packed per texel region. Per-region channel allocation (overlap-aware, as larger engines do) is
a later step, and it can land without changing the plane. A channel with no light is 1, fully lit.

**Dilated, not denoised, not reconciled.** The mask is carried through the chart-bounded dilation
with the texels, so a bilinear tap at a chart border reads that chart's own visibility. It is not
denoised. Fully lit and fully shadowed texels are exact: a point light is one test, and an area
light's 64 samples all agree there. Inside a soft penumbra, each texel is an estimate over 64
hashed points on the light, so the grain there is in steps of 1/64. That is coarser than an eight-bit
plane, and it is the first thing to revisit (more samples, or the denoiser) once the frame reads the
mask. Its seams are not reconciled:
the seam solve's step lengths come from one inner product over all the moment fields, so adding the
mask to it would move every existing bake's texels. A mask seam is also, most often, a shadow edge.
This is stated in the module README as not done.

## Incremental rebake

`rebake_lightmaps(scene, settings, previous, request, out, report)`:

1. **Pack the new scene and compare.** A rigid move keeps an object's world area, so its rectangle
   and every other rectangle are unchanged. If any address, the mode, the page size or the page
   count differs, the rebake runs a full bake, sets `report.incremental = false`, and says why.
2. **Choose the region, at object granularity.** Dirty boxes are the moved instances' new world
   bounds and the caller's `previous_bounds`, each grown by `influence_metres`. An object is
   re-solved when it moved, or when any of its rasterised surface texels lies inside a dirty box.
   The object, not the texel, is the unit because the denoiser, the dilation and the seam solve are
   chart-bounded: a re-solved object is post-processed exactly as a full bake would process it.
3. **Trace only the region.** Texels of other objects are marked empty for the post-process, so the
   denoiser neither filters them nor borrows from them. The dilation fills within an owner, so it
   cannot borrow either. Seams are solved only where both sides are re-solved; a seam between a
   re-solved object and a kept one is left as the previous bake reconciled it, and counted in
   `report.boundary_seams`.
4. **Copy the rest.** Every texel whose owner was not re-solved (or that no rectangle owns) is copied
   from `previous` into `out`: planes, coverage and shadow mask, byte for byte.

A texel's random sequence is a function of its index and the seed, so a re-solved texel traces the
same draw a full bake of the new scene would. The re-solved region agrees with a full bake up to the
light that changed outside the influence distance. That distance is the accuracy/cost trade, and it
is the caller's.

## The chart-padding check, measured on the raster

The cooked mesh records no unwrap resolution or padding, and the producer measures coverage and
aspect from the UV2 stream itself. The check therefore measures what the bake actually drew. After
rasterisation, each covered texel looks through a window of radius `required_gap` for a covered
texel of the same owner and a different chart. The gap is the Chebyshev distance minus one. The
required gap is `2 × 2^mips`, where `mips` is the layout's protected mip count: one texel of the
coarsest declared mip level on each side of the gap. For `mips = 0` that is the two texels bilinear
filtering needs, which is `Uv2Options::padding`'s own default. Objects below it are listed in
`report.padding_short`. With `LightmapBakeSettings::refuse_short_padding` the bake fails instead, naming the
first instance.

Expect the default settings to report shortfalls: the importer unwraps at 16 texels per metre with a
two-texel padding, and a level bakes at 8 with two protected mips. That is the finding this check
exists to surface. Changing the defaults is left to the content that owns them.
