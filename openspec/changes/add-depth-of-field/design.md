# Design: depth of field

## The radius is the lens's

`circle_of_confusion(settings, d)` is the blur circle's diameter on the sensor as a fraction of the
sensor height: `A |d - F| / d * f / (F - f) / s`, signed negative in front of the focus. The image
is the sensor, so the radius in pixels is `H/2` of it. `make_dof_constants` factors the distance
out — `r(d) = K (d - F) / d` — and pushes `K` and `F`; the shader recovers `d` from the depth buffer
through the projection's own terms, `d = (m23 - z m33) / (m22 - z m32)`, exact for the finite,
infinite and orthographic reversed-Z matrices. A pinhole (`N = inf`) is `K = 0`. The sky is at
infinity: `r = K`.

A caller that has a field of view rather than a focal length uses
`focal_length_for_field_of_view(fov, sensor_height)`, so the circle is computed for the lens the
projection draws with.

## Scatter as gather, two fields

A defocused point is a disc of its own radius carrying its light spread over the disc's area. A
pixel receives from every texel whose disc covers it. The gather visits taps around the pixel and
weights each by `a / (S rho^2)` — the area the tap stands for over the disc's area — times a
one-texel ramp across the disc's edge (the aperture's shape: a circle, or the polygon its blades
inscribe, `S` its area at unit circumradius).

- **Far field** (behind the focus): gathered over the pixel's own radius from far texels only, and
  normalised. A focused pixel gathers nothing and so is never blurred by the background behind it;
  a focused texel has no radius and so never reaches a background pixel.
- **Near field** (in front): gathered over the dilated tiles' reach and NOT normalised — the sum is
  the fraction of the pixel covered by near-field discs. The composite lays the near field over the
  sharp or far-blurred pixel by that coverage: near-field bleeding.

The taps span a little more than the disc they gather. Ring `k` of `n` sits at `k R' / (n + 1/2)`,
so over the disc's own radius the outermost ring would sit half a spacing inside its rim and every
texel past it would go unsampled: a polygonal aperture would be drawn as the tap grid's circle. The
gather spans `R' = (R + 1/2)(n + 1/2)/n` (`cyDofSpan`), which puts the outermost ring on the outer
edge of the reach's one-texel ramp. Tap areas still sum to the sampled disc, so the near field's
coverage keeps its meaning.

The half-resolution layer takes each 2x2 block's NEAREST surface's radius, and its colour from the
texels within a pixel of it, so a near silhouette is not thinned and a focused silhouette stays
focused. The far upsample is bilateral for the same reason; the near upsample is plain, on
premultiplied colour.

## The free parameters

- `max_radius_fraction` (default 0.02 of the height): bounds the taps and sets the tile width. A
  larger circle is clamped.
- `max_rings` (default 8, 217 taps): sampling density, not physics.
- The focus band, one pixel of radius: the target's resolution limit. Inside it a pixel is written
  as read — the in-focus plane and a pinhole are bit-identical — and the composite reaches the
  blurred layers one pixel beyond it, where the half-resolution layers first resolve a disc.

## Why a stage and not a post-process callback

Bloom must glow around a defocused highlight's disc, not have the disc cut out of its glow, and
bloom runs before the post-process. So depth of field is its own stage before bloom, declared
through the same seam the contact shadows and the outlines use, and the frame — not the producer —
owns the target every later stage reads.

## Binding order after the stage

The Vulkan backend binds descriptor sets at the last bound pipeline's bind point. The stage is the
first compute work directly before the post-process, and the post-process bound its sets before its
pipeline, so they went to the compute point and the resolve drew with an earlier pass's pass set. The
frame recorder's temporal resolve and post-process, and the beauty sample's resolve, now bind the
pipeline first.

## Measured

`render.depth_of_field` on this machine's Vulkan device, dev and debug:

| Case | Measured |
|---|---|
| in-focus plane | 65536 of 65536 texels unchanged at 2 m, and at 2.05 m (circle 0.445 px) |
| far edge | lens radius 13.68 px, fitted disc 14.05 px |
| near edge (circle 12.16 px) | coverage 0.391, 0.298, 0.173, 0.103 at 0.1, 0.3, 0.5, 0.7 r against discs 0.422, 0.319, 0.177, 0.095 |
| far behind | brightest background 0.159 against the brightest stripe 0.1875; stripe contrast 0.0099 of 0.125 |
| six blades | edge over corner 0.871 (cos 30 degrees is 0.866); the circle 1.000 |
| pinhole | byte-identical to the frame without the stage; f/1 changes 13310 of 129600 texels |
| off | 129600 of 129600 texels byte-identical to `depth_of_field_absent.png` |

`just capture-beauty-depth-of-field`: without the stage the still is `m11c-beauty-shot.png`'s pixels
exactly; focused on the sphere it keeps 100.0 % of the sphere's detail and 1.7 % of the column's,
focused on the column 99.8 % of the column's and 32.5 % of the sphere's.

## Mutation proofs

Each applied to the source, the headers regenerated where it is a shader, the suite rebuilt (dev)
and run, and the file restored and md5-verified, headers included.

| Mutation | Red |
|---|---|
| the focus band a quarter pixel (`kFocusBandPixels`) | the in-focus plane |
| the device radius scaled by 1.5 (`cyDofRadius`) | far edge, near edge, six blades |
| the composite drops the near field | near edge |
| the far upsample not bilateral | far behind |
| the reach ignores the aperture's shape | six blades |
| the gather spans the disc only (the defect it had) | six blades |
| the composite's output darkened by 2 % | pinhole, in-focus plane, far edge, near edge, far behind |
| the assembly puts the stage in every frame | off, pinhole |
| the host twin takes the diameter for the radius | `unit.render_dof`: the radius in pixels |
| the host span is the disc only | `unit.render_dof`: the outermost ring |
| the stage handed the raw colour rather than the chain's | `unit.render_forward`: the stage's place and inputs |
| the MSAA-without-prepass refusal dropped | `unit.render_forward`: the refusals |
| the post-process binds its sets before its pipeline (the defect it had) | the sets after the stage's dispatches |

Two mutations stayed green and are recorded as such. The focus band narrowed only in
`cyDofDefocus`: the half-resolution layer and the bilateral upsample still test the band, so no
in-focus pixel moves. The far gather accepting focused texels: a focused texel's radius is zero, so
its reach is zero and it adds nothing — the property the design states, not a mutation a case could
see.
