# Tasks

## 1. Establish what exists

- [x] 1.1 Read `cy/aerial_perspective.slang`, the stage's table and set 0, and `water.slang`'s three terms; confirm the refraction and reflection pictures are drawn by world.slang's lit path with the frame's eye, so they already carry air
- [x] 1.2 Confirm the committed `water_spirv.h` and `water_msl.h` reproduce from their source with the pinned Slang before changing them

## 2. The air on the water

- [x] 2.1 Bind the table in `water.slang` at set 0, binding 2, as world.slang does
- [x] 2.2 Take the refraction picture's bed back to its lit radiance by inverting the table at the bed
- [x] 2.3 Compose `F reflected + T ((1 - F) refracted + glitter) + (1 - F) S`, and `T foam + S` for foam; keep the arithmetic from before when the table is off
- [x] 2.4 Regenerate `water_spirv.h` and `water_msl.h`; check the MSL buffer order

## 3. Tests that can fail

- [x] 3.1 `render.world_water_aerial_perspective`: every water and land texel against the processor's `sample_at()`, dark and lit
- [x] 3.2 The shoreline: the water's in-scattering beside the land's at equal distance, every row
- [x] 3.3 Far water: the same beyond 2 km, with real haze
- [x] 3.4 Near water within 10 m of an eye 2 m up is unchanged within 0.001
- [x] 3.5 Off is bit-identical to the water's fragment stage pinned before this change
- [x] 3.6 Prove each red by a mutation, restored and md5-verified (`evidence/falsification.txt`)

## 4. The picture

- [x] 4.1 Compare `--no-aerial-perspective` against a build of the base commit frame by frame (`evidence/frame-identity.txt`)
- [x] 4.2 Publish before/after images of samples/10-world at day and dusk under `docs/design/images/`

## 5. Records

- [x] 5.1 Name the new cases in the `Aerial perspective` coverage note and drop the shaded sea from what is not built
- [x] 5.2 Update the READMEs of `samples/10-world` and `tests/render`
- [x] 5.3 `openspec validate add-aerial-perspective-on-water --strict`
