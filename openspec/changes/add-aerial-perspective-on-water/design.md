# Design

## Context

`add-aerial-perspective` put the atmosphere between the eye and every surface
`samples/10-world/shaders/world.slang` lights. `add-water-shading`, merged while it was in review,
moved the sea to `shaders/water.slang`, which reads no table. The default frame therefore hazes
the land and the sky but not the sea in front of them: the far water keeps the contrast of water a
hundred metres away.

The water shader composes three terms, and two of them are pictures drawn by world.slang's lit
path in passes before the frame — so they already carry air:

| term | what it is | the air it already carries |
|---|---|---|
| refracted | the refraction picture's bed, seen through Beer-Lambert over the water column, plus what the column scatters back | the bed was hazed over the whole straight line from the eye to the bed; below the surface that line is water, not air |
| reflected | the reflection picture: the dome and the terrain mirrored in the sea level | a reflected hill was hazed over its own distance from the eye; the dome is emissive and is the sky itself |
| the surface's own | the sun's glitter lobe and the foam | none |

## Goals / Non-Goals

**Goals:**

- The water is attenuated by the air between the eye and its surface and receives the light that
  air scatters, from the same table the land reads, once.
- At the shoreline, and far out, water and land at the same distance receive the same haze.
- A near water surface is left as it was lit, and the frame with aerial perspective off is the
  frame before.

**Non-Goals:**

- Re-hazing the reflection picture along the mirrored path. world.slang's lit path is shared with
  the land and the frame; changing how it hazes the mirrored pass is a change to the land's shader
  for an error the table's own interpolation is of the same order as (below).
- The engine forward frame and volumetric media, which remain as `add-aerial-perspective` recorded.

## Decisions

### The formula

With T and S the table's transmittance and in-scattering between the eye and the surface point W,
and F the Fresnel weight, the eye receives the air in front of W applied to what leaves W:

    L_eye = T (F R_W + (1 - F) L_refr + glitter) + S

where R_W is the reflected radiance arriving at W. The reflection picture does not hold R_W: for a
hill P it holds `T_EP L_P + S_EP`, the hill hazed over its straight distance from the eye, which is
the mirrored path eye -> W -> P short by a fraction `2 h e / r^2` — to that order the whole
reflected path's air, `T R_W + S`. So `T F R_W = F (picture - S)`, and substituting,

    L_eye = F picture + T ((1 - F) L_refr + glitter) + (1 - F) S

which is what `seenThroughAir()` computes. Where the picture is the dome, it holds the sky in the
mirrored direction, `J (1 - T_inf)` for an air of in-scattering J along a path to space; the exact
answer there is `T F sky + S`, and the two agree to first order wherever the air along the
reflected ray is the air along the eye's ray — at the horizon exactly, where the sea is farthest
and the mirror covers most of the surface. Rather than branching on what the reflection picture
holds (it would need its depth, a fourth binding), the one rule is used.

Foam covers the surface, so it takes `T foam + S`, and the result is `lerp` by the foam fraction
between the two, as the shader always blended them.

### The bed is un-hazed, not hazed twice

`L_refr = bed Tw + column`, and the bed must be the radiance the bed was lit with. The refraction
picture holds `T_EB bed + S_EB`, hazed at the bed's own point B, which the shader already
reconstructs from the refraction depth for the column's length. The shader inverts it there,
`(picture - S_EB) / T_EB`, with the same `cyAerialPerspectiveAt()` world.slang applied — the same
table, the same interpolation — so the inversion is exact up to the picture's half-float storage.
Without it, the bed's term would be `T (T_EB bed + S_EB) Tw`: attenuated twice, and carrying air's
light through the water column as though the column had scattered it.

### No free parameters

T and S are the table's, integrated from the world's `Atmosphere` and `AtmosphereTables`; F is the
shader's existing Schlick term with the body's own F0. The only new constant is the `1e-4` floor
under the transmittance divided by, which only keeps a zero from becoming an infinity: a bed whose
transmittance is that small is one the air has hidden, and whatever the division makes of it is
multiplied by the surface's own transmittance, the same small number, on its way to the eye.

### Off is exact

When the table's `enabled` word is zero the shader reads nothing more from it and returns through
the arithmetic it had before, `lerp(refracted, reflected, fresnel)`, `+ glitter`, `lerp(…, foam)`,
in that order: the formula above with T = 1 and S = 0 is the same number mathematically but not
bit for bit, so it is not used for off. `render.world_water_aerial_perspective` pins the water's
fragment stage from before this change and compares every texel, dark and lit.

The same arithmetic is not enough on its own. The first version kept one body that branched on the
word as it went, and with aerial perspective off the sample's day differed from main's in one sea
pixel by one 8-bit step on 14 of 192 frames. The test scene never showed it. The body is therefore
`shadeWater<let kAir : bool>`, with the entry point choosing `shadeWater<true>` or
`shadeWater<false>` once, and the off copy is the old body operation for operation. With that, the
off frames match main's 192 of 192. The investigation is in `evidence/frame-identity.txt`: moving
the off composition back into place with the runtime branch kept still gave 14 frames, so what the
driver generates depends on the shape of the code, not on its arithmetic.

### Set 0, binding 2, is already there

The water pipeline's set 0 is the world pipeline's own layout, and the stage already writes the
table into binding 2 of that set every frame. The only change is the third member of
`WaterCloudShadow` in `water.slang`; the Metal buffer order (set 0 at `[[buffer(0)]]`, set 1 at
`[[buffer(1)]]`, the push block at `[[buffer(2)]]`) is unchanged in the regenerated `water_msl.h`.

## Risks / Trade-offs

- The reflection of a hill is hazed along a slightly shorter path than the mirrored one. Stated in
  the shader and the sample README with its size.
- The un-haze divides by T. The floor keeps it finite, and the result is multiplied by the surface's
  own transmittance again on the way out.

## Backends

- **Vulkan** — the target on this host; `render.world_water_aerial_perspective` runs the shipped
  SPIR-V.
- **Metal** — `water_msl.h` regenerated from the same source; compiled, not run, on this host.
- **D3D12** — `just build-shaders --strict src samples` compiles `water.slang` for it; the sample
  builds no D3D12 path.
