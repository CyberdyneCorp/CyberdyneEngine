# Proposal

## Why

`atmosphere-sky-and-clouds` requires distance to be the atmosphere's: "Distance attenuation SHALL be
produced by the atmosphere model — scattering and transmittance over distance — and applied to
opaque shading", "consistent with the sky above it". The model has existed since M7
(`sky::aerial_perspective()`) and its froxel table since M10 (`sky::AerialPerspectiveTable`), both
integrated from the same `Atmosphere` as the sky. Neither is applied to any surface: M11.c task 5.1
found no caller outside `src/rendering/sky/` and its tests, and the coverage map records `Aerial
perspective` as `exempt:m11e` for that reason. `samples/10-world`, the one frame that draws distant
terrain, has no distance attenuation at all, and its dome's clear sky is a tuned gradient rather
than the atmosphere — so even a correct haze would fade the terrain toward a colour the sky beside
it does not have.

## What Changes

- `samples/10-world` integrates `sky::AerialPerspectiveTable` every frame for its own camera, from
  the world's `Atmosphere` and `AtmosphereTables` — the ones its sky lighting is composed from — and
  its lit fragment path (terrain, foliage, and the sea when it is drawn with `--no-water-shading`)
  multiplies each fragment by the table's
  transmittance and adds its in-scattering, divided by the same exposure the sky is.
- With aerial perspective on, the dome's clear sky is the same atmosphere's
  (`sky::IncrementalSkyView::update_aerial`, the same tables), and the device cloud pass composes
  its clouds over it instead of over the stand-in gradient. That is what makes the horizon meet: the
  air in front of a distant hill and the sky behind it are one model evaluated twice.
- `src/rendering/shaders/cy/aerial_perspective.slang`, a new standard-library module, is the
  device sampler: it reads the words `sky::pack_aerial_perspective()` writes and interpolates in
  depth from the eye, so a near surface is left as it was lit.
- `sky::AerialPerspectiveTable` gains `sample_at()` (the device sampler on the processor, the oracle
  the render test compares against), `view()`, read access to its values, and an optional
  `jobs::JobSystem` for its rebuild, bit-identical to the serial one. `pack_aerial_perspective()`
  defines the device layout once.
- `--no-aerial-perspective` draws the frame as it was before: no table read, the stand-in sky.
- New tests: `render.world_aerial_perspective` draws with the sample's committed SPIR-V on a Vulkan
  device; `integration.render_sky_tables` gains the surface-sampler, parallel-rebuild and packing
  cases. `render.world_cloud_shadow` binds the table switched off, since the pipeline's set 0 now
  has a third binding.
- `tools/roadmap/requirements-coverage.toml` maps `Aerial perspective` to the frame-level case
  instead of the exemption.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `atmosphere-sky-and-clouds`: `Aerial perspective` gains scenarios that make "consistent with the
  sky" observable: the horizon meeting the sky, a changed atmosphere changing both, near geometry
  left alone, and off being the frame before.

## Impact

`src/rendering/sky` (additive: `tables.h` and `tables.cpp`), `src/rendering/shaders` (one new
module), `samples/10-world` (world, stage, main, `world.slang`, `world_visual.slang` and their
generated SPIR-V and MSL), `tests/render`, `src/rendering/sky/tests`, the coverage map and the
READMEs. No change to `cy/frame.slang`, the post chain or the render graph: the engine's forward
frame draws no world and is being edited by parallel changes (see design.md). No authoritative,
saved, replayed or networked state changes; the headless take never builds the table.
