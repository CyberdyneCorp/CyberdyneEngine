# Tasks

## 1. Establish what exists

- [x] 1.1 Read `Water surface shading` and `Caustics` against the tree: src/water/'s closure, Beer-Lambert, in-scatter, reflection escalation and caustic tiers (built, tested, called by no frame), and `samples/10-world`'s water (a per-vertex colour for a nominal 14 m column, a noise foam, the Lambert path's glitter lobe)
- [x] 1.2 Confirm the committed `world_spirv.h` reproduces bit for bit from `world.slang` with the pinned Slang 2026.9.2, so the regeneration recipe is the tree's

## 2. Shade the water

- [x] 2.1 `water_surface.{h,cpp}`: `build_water_params()` through src/water/'s own functions, `select_caustic_trains()`, `mirrored_rows()` with the near plane moved onto the water, and the device half (pipeline, set 1, parameter buffer, four targets)
- [x] 2.2 `shaders/water.slang`: refraction with Beer-Lambert over the surface-to-bed path plus in-scatter, Schlick's Fresnel against the planar reflection, shoreline foam from the column depth, surface-derived caustics from the trains' analytic Laplacian, the glitter lobe unchanged
- [x] 2.3 Generate `water_spirv.h` and `water_msl.h` as the file's header says; check the MSL puts set 0 at `[[buffer(0)]]`, set 1 at `[[buffer(1)]]` and the push block at `[[buffer(2)]]` in both stages
- [x] 2.4 Declare the refraction and reflection passes before the assembled frame, draw the water run with the water pipeline, declare its three reads; add `--no-water-shading`
- [x] 2.5 `World::ocean_model()` and `World::water_time()`

## 3. Tests that can fail

- [x] 3.1 `render.world_water`: off, byte-identical to `references/world_water_off.png`, drawn with world.slang's SPIR-V from before this change; on, the picture moves
- [x] 3.2 `render.world_water`: every water pixel is Fresnel times the mirrored sky, with the bed unlit
- [x] 3.3 `render.world_water`: every water pixel over the bed matches src/water/'s transmittance and in-scatter over its path; deeper bins darker and bluer
- [x] 3.4 `render.world_water`: foam brightens the shallow half of the band and changes nothing over deeper water
- [x] 3.5 `render.world_water`: caustics differ between two instants, nothing else does, and each pixel matches the surface's own focus
- [x] 3.6 Prove each red by a mutation, restored and md5-verified (`evidence/falsification.txt`)

## 4. The picture

- [x] 4.1 Take the day with water shading on and off
- [x] 4.2 Compare `--no-water-shading` against a build of the tree before this change, frame by frame (`evidence/frame-identity.txt`)
- [x] 4.3 Publish before/after pairs under `docs/design/images/`
- [x] 4.4 Measure what the two extra passes cost

## 5. Records

- [x] 5.1 Map `Water surface shading` and `Caustics` in `tools/roadmap/requirements-coverage.toml`, naming what is still not built
- [x] 5.2 Update the READMEs of `samples/10-world`, `src/water` and `tests/render`
- [x] 5.3 `openspec validate add-water-shading --strict`
