# Proposal

## Why

`atmosphere-sky-and-clouds` requires distance to be the atmosphere's: "Distance attenuation SHALL be
produced by the atmosphere model — scattering and transmittance over distance — and applied to
opaque shading". `add-aerial-perspective` applied `sky::AerialPerspectiveTable` to every surface
`samples/10-world/shaders/world.slang` lights, but the sea is drawn by `shaders/water.slang` since
`add-water-shading`, and that shader reads no table. So in the default frame the distant sea keeps
its full contrast while the land behind it and the sky above it haze, and the coverage note for
`Aerial perspective` records the shaded sea as not built.

Applying the table to the water's finished colour, as the land does, would be wrong in a way the
land cannot be: two of the water's three terms are pictures that world.slang's lit path has already
drawn through the air. The refraction picture holds the bed hazed over the whole line to it, and
the reflection picture holds each reflected hill hazed over its own distance. Hazing the result
again would attenuate them twice and add the air's light twice.

## What Changes

- `shaders/water.slang` binds the table where world.slang binds it — set 0, binding 2, already
  written by the stage and already in the layout the water pipeline shares — and reads it through
  `cy/aerial_perspective.slang`.
- The bed read from the refraction picture is taken back to the radiance it was lit with by
  inverting the table's own transmittance and in-scattering at the bed's position, before the water
  column is applied to it.
- The surface's own light — the refracted column, the sun's glitter — is attenuated by the air
  between the eye and the surface once; the reflected picture is not attenuated again, and the
  in-scattering the mirror does not already carry, `(1 - F) S`, is added. Foam, which covers the
  surface, takes `T foam + S`.
- With the table's `enabled` word zero, the shader takes the arithmetic it had before, operation for
  operation, and the frame with `--no-aerial-perspective` is unchanged.
- New test: `render.world_water_aerial_perspective` draws with the sample's committed SPIR-V and
  water device half on a Vulkan device, and pins the water's fragment stage from before this change
  in `tests/render/water_before_aerial_perspective_spirv.h`.
- `tools/roadmap/requirements-coverage.toml`: the `Aerial perspective` note no longer records the
  shaded sea as not built, and names the new cases.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `atmosphere-sky-and-clouds`: `Aerial perspective` gains the rule for a surface whose shading reads
  pictures already drawn through the air, and scenarios for water beside land at the same distance
  and for a reflection that is not hazed twice.

## Impact

`samples/10-world/shaders/water.slang` and its generated `water_spirv.h` / `water_msl.h`,
`tests/render` (one new suite and one pinned header), the coverage map, and the READMEs of
`samples/10-world` and `tests/render`. No C++ in the sample changes: the stage already binds the
table in the set the water pipeline shares. No change to `world.slang`, the render graph, the
forward frame or any RHI backend.
