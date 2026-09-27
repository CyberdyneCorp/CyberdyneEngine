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
