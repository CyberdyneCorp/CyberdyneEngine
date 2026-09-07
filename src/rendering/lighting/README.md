# `src/rendering/lighting/` — layer 4

Lights in physical units, and the conventional shadow path: the atlas, the directional cascades, and
the filter and bias model.

**Governed by**: `rendering-lighting-and-shadows`, at **Seed** for M3 and **Working** for M7. M3
tasks 4.4.1 and 4.4.2; M7 task 10.3.

## The files

| File | What it holds |
|---|---|
| `units.h` | lux, lumens, candela and nits; the black-body tint; camera exposure from aperture, shutter and ISO; the plausibility check and the documented arbitrary-to-physical conversion |
| `lights.h` | `GpuLight` — the 64-byte record `cy/light.slang` reads — built camera-relative in `f64` |
| `shadow_atlas.h` | tile sizing with hysteresis, allocation with retention, cross-frame caching, and the reported shortfall |
| `cascades.h` | split distances, bounding-sphere stabilisation, texel snapping, the transition band and the distant fade |
| `filtering.h` | the sample counts a specialization constant takes, and the three-term bias |
| `area.h` | linearly transformed cosines for rect, disc, sphere and tube lights: the fitted table, the closed-form spherical-polygon integral with its horizon clip, and the representative-point fallback |
| `decals.h` | the projected oriented box, its two fades, reoriented normal blending, and a decal budget the **arbiter** sets whose eviction is deterministic and reported |
| `light_functions.h` | cookies for the three light types, projected in light space, scrolled without touching a shadow map |
| `channels.h` | a bitfield test at assignment and nothing at shading time, and the bounded deterministic assignment that goes with it |
| `many_light.h` | reservoir sampling with temporal and spatial reuse, the ray budget that replaces the per-cluster bound, and the rule that refuses the path without a denoiser |

## What M7 added, and the three numbers worth knowing

**Area lights are LTC and the table is fitted at startup**, by a coordinate descent on the same
four-parameter matrix the published offline fit uses. `area.h` says at length what that is and is
not; `tests/test_reference.cpp` says what it is worth:

    just test-integration -R integration.render_lighting_reference

    diffuse: worst relative error over 6 configurations 0.00205006%
    specular: mean absolute error 2.73492% of the set's peak over 8 configurations; worst 16.417%

The diffuse half has no fit in it and is exact — it is the closed-form polygon integral, and 0.002%
is the arithmetic. The specular half is the fit, and the error is reported as a fraction of the
set's **peak** rather than as a ratio per configuration, because the one case that is relatively far
off is a narrow lobe pointed away from the emitter where the reference itself is four parts in a
thousand of the brightest configuration. The ratio is printed beside it rather than hidden.

**The reservoir estimator is unbiased**, measured against a sum over every light rather than
asserted: `reservoir estimate 178.31 against a reference sum of 178 — relative bias 0.1739%`.
Getting `weight_sum / (M * target)` wrong does not crash and does not look obviously wrong — it
looks like a scene a few per cent too bright in exactly the places with the most lights, which is
where nobody looks for an estimator bug.

**A decal's normal blend is reoriented normal mapping, and the sign is a trap.** The published form
is written for [0, 1]-encoded normals as `u = d * (-2, -2, 2) + (1, 1, -1)`, which for a unit vector
is `(-d.x, -d.y, d.z)`. Writing `(d.x, d.y, -d.z)` instead — the transcription that looks right —
gives `-base` for a flat detail normal, so every decal inverts the surface it lands on. The comment
is at the site.

## What "physical units" means here, since it is not a scale factor

Three quantities stop being the same number: what an author types (lux for the sun, lumens for a
bulb), what the shader integrates (candela for a punctual light), and what reaches the display (the
integrated luminance divided by a real camera's exposure). A 1000-lumen bulb and a 100 000-lux sun
then coexist without either being tuned. `units.h`'s header comment carries the argument in full.

**One conversion is a genuine choice and it is written down.** A spot light's lumens-to-candela has
two conventions in use — the physical one confines the flux to the cone, the photometric one does
not. This engine uses the photometric one, because an artist narrowing a beam expects the lit region
to shrink rather than brighten. `spot_candela_physical()` sits beside it so the other is available by
name rather than by patching a file.

## The two halves of shadow stabilisation, both of which are needed

A cascade is fitted with a **bounding sphere** rather than a box, because a box changes size as the
camera rotates and every size change moves every texel. And its centre is **snapped to a lattice
fixed in the light's own space** — not to one derived from the centre, which would round zero to zero
and do nothing at all. Fixed size with an unsnapped centre still crawls, at a texel's scale.

## What it does not depend on

No device and no shader compiler, which is the same division `src/rendering/material/` draws: a
shadow that swims is a matrix that moved, and a matrix is testable without a GPU. The passes that
consume all of this are `src/rendering/forward/`'s.
