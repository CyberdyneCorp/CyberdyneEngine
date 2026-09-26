# Proposal

## Why

`water` requires the surface to be shaded for "reflection, refraction, wavelength-dependent
absorption and scattering through the water column, surface roughness, normals, and foam coverage",
with colour following "physically-inspired attenuation over the water column thickness", and
requires caustics whose default tier is derived from the water surface. `src/water/` has had the
arithmetic since M10 — `water_transmittance()`, `water_in_scatter()`, `build_closure()`'s Fresnel
F0, `select_caustic_tier()` — and nothing draws with it. The one frame that draws the sea,
`samples/10-world`, shades it through the world's Lambert path: a per-vertex colour computed once for
a nominal 14 m column, a white noise pattern for foam, and a Blinn-Phong glitter lobe. The water has
no reflection, nothing under it shows through, the fjord is one colour from the beach to the middle
of the channel, and there are no caustics.

## What Changes

- `samples/10-world/shaders/water.slang`: a water fragment stage that evaluates, per pixel, the
  refracted bed through Beer-Lambert over the path from the surface to the bed plus the column's
  in-scatter (the body's own `WaterOptics`), Schlick's Fresnel against a planar reflection of the
  sky and the scene, shoreline foam from the depth of the column under the pixel, surface-derived
  caustics on the bed from the analytic Laplacian of the ocean's own wave trains, and world.slang's
  glitter lobe unchanged.
- `samples/10-world/water_surface.{h,cpp}`: the processor half — `build_water_params()` turns the
  body's optics into the shader's extinction and in-scatter through src/water/'s functions, reduces
  each wave train's phase to this frame in f64, and inverts the frame's matrix;
  `select_caustic_trains()` keeps the sixteen trains that curve the surface most; `mirrored_rows()`
  builds the mirrored matrix with its near plane on the water — and the device half (pipeline, set,
  parameter buffer, four targets).
- `samples/10-world/stage.cpp`: with water shading on, two passes are declared into the render graph
  before the assembled frame — the terrain alone from the frame's camera (the refraction), and the
  sky, terrain, stars and plants mirrored in the sea level (the reflection) — both drawn with
  world.slang's own pipeline; the opaque pass draws the water run with the water pipeline and
  declares the three textures as fragment reads. `--no-water-shading` draws the frame as before.
- `World::ocean_model()` and `World::water_time()` expose what the caustics are derived from.
- New suite `render.world_water`: five cases on a Vulkan device with the sample's committed SPIR-V
  and its own `WaterSurface`, each seen red under a mutation.
- `tools/roadmap/requirements-coverage.toml` maps `Water surface shading` and `Caustics` to their
  cases, naming what is still not built.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `water`: `Water surface shading` gains scenarios for the reflected sky, shoreline foam and the
  frame without water shading; `Caustics` gains the surface-derived tier's focusing rule and a
  scenario that the pattern moves with the water's clock.

## Impact

`samples/10-world` (stage, main, world accessors, a new shader with its generated SPIR-V and MSL, a
new source file), `tests/render` (a new suite and one committed reference), the coverage map and the
READMEs. `world.slang`, `world_visual.slang` and their generated headers are untouched, as are
`cy/frame.slang`, `src/rendering/**`, `src/backends/**` and `src/water/`. No authoritative, saved,
replayed or networked state changes; the headless take draws nothing and its digest does not move.
