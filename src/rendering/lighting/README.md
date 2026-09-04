# `src/rendering/lighting/` — layer 4

Lights in physical units, and the conventional shadow path: the atlas, the directional cascades, and
the filter and bias model.

**Governed by**: `rendering-lighting-and-shadows`, at **Seed** for M3. Tasks 4.4.1 and 4.4.2.

## The files

| File | What it holds |
|---|---|
| `units.h` | lux, lumens, candela and nits; the black-body tint; camera exposure from aperture, shutter and ISO; the plausibility check and the documented arbitrary-to-physical conversion |
| `lights.h` | `GpuLight` — the 64-byte record `cy/light.slang` reads — built camera-relative in `f64` |
| `shadow_atlas.h` | tile sizing with hysteresis, allocation with retention, cross-frame caching, and the reported shortfall |
| `cascades.h` | split distances, bounding-sphere stabilisation, texel snapping, the transition band and the distant fade |
| `filtering.h` | the sample counts a specialization constant takes, and the three-term bias |

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
