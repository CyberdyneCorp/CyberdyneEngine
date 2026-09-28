# Tasks

## 1. Establish what exists

- [x] 1.1 Read `Volumetric fog` against the tree and its exemption note: the froxel distribution, the phase function and the slice integral exist in `src/rendering/post/`; nothing fills a volume, samples a shadow map for a medium or applies one to a surface
- [x] 1.2 Read the aerial perspective table (`sky::AerialPerspectiveTable`, `cy/aerial_perspective.slang`) and adopt its basis, its slice distribution and its header, so the fog and the atmosphere share one froxel geometry

## 2. The medium and the march on the host

- [x] 2.1 `fog/medium.h`: the height fog (Koschmieder's visibility, the exponential profile), box, sphere, cylinder and cone volumes with extinction, albedo, emission and `g`, and the mixture's phase
- [x] 2.2 `fog/volume.h`: the view, the light, the shadow lookup, the constants, the stored layout, `fog_at`, `integrate_fog_column` and the independent `single_scattering_reference`
- [x] 2.3 The atmosphere-table composition: each slice's air read out of the table and marched with the fog, per channel

## 3. The march on the device

- [x] 3.1a `fog/shaders/fog_march.slang`, both variants, the host march statement for statement, and `regenerate.py`
- [x] 3.1 Run `regenerate.py` and commit `src/fog_spirv.h` and `src/fog_msl.h` (the module does not compile until they exist)
- [x] 3.2 `FogPass`: the dispatch, the persistent target imported each frame, the per-frame-slot constants, the shadow map and the atmosphere's table as declared reads, and a host readback
- [x] 3.3 `FramePassKind::VolumetricFog` in the forward frame and its producer through `FrameAssembly`

## 4. Seen through

- [x] 4.1 `cy/volumetric_fog.slang`: the lookup, over any texture source, with an exact identity for an empty volume
- [x] 4.2 `cy/frame.slang`: `volumetricFogControl` appended, the forward fragment through the volume; regenerate the frame's committed SPIR-V and MSL
- [x] 4.3 `samples/12-beauty`: `--fog`, the shot's `fog` lines, every scene and sky fragment through the volume
- [x] 4.4 `samples/10-world`: `--fog`, the grade file's valley haze, the table variant bound where the atmosphere's table was

## 5. Tests that can fail

- [x] 5.1 `unit.rendering_fog`, `integration.rendering_fog_march` (the whole-column cases, over the unit budget) and `integration.rendering_fog_air`
- [x] 5.2 `render.volumetric_fog`: off against a reference drawn by the pre-change frame shader; an empty medium identical to off; shafts; the single-scattering integral; attenuation with distance; the table variant
- [x] 5.3 Two cases in `unit.render_forward`: the stage's position and read, and its refusals
- [x] 5.4 Prove each device case red by a mutation, restored and md5-verified (`evidence/falsification.txt`)

## 6. The pictures and the records

- [x] 6.1 `just capture-volumetric-fog` and `just capture-world-fog`; publish the before/after images under `docs/design/images/`
- [x] 6.2 Map `Volumetric fog` in `tools/roadmap/requirements-coverage.toml` to the device cases, keeping the exemption's reasons for what is not built
- [x] 6.3 Update the READMEs of `src/rendering/fog`, `src/rendering/post`, `src/rendering/forward`, `src/rendering/pipeline`, `src/rendering/shaders`, `samples/12-beauty` and `samples/10-world`
- [x] 6.4 `openspec validate add-volumetric-fog --strict`
