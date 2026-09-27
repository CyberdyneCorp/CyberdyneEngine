# Tasks

## 1. Establish what exists

- [x] 1.1 Read `Aerial perspective` against the tree: the model (`sky::aerial_perspective()`), the table (`sky::AerialPerspectiveTable`), the override (`sky::StylisedDistance`), and the one frame that draws distant terrain (`samples/10-world`, which applied none of them and drew its clear sky from a tuned gradient)
- [x] 1.2 Confirm the committed headers reproduce from their sources with the pinned Slang before changing them: `world_spirv.h` and `world_msl.h` byte for byte; `world_visual_spirv.h` and `world_visual_msl.h` word for word and character for character, the files differing only in the SPDX line a later commit added and in `embed_msl.py`'s newer split of the MSL literals (kept: the SPDX line is restored after regenerating, the split is the script's)

## 2. The table on a device

- [x] 2.1 Add `AerialPerspectiveTable::sample_at()` — bilinear across the frame, linear in depth from an implicit slice at the eye — `view()`, read access to the values, and an optional job system for `update()`
- [x] 2.2 Add `pack_aerial_perspective()`, the one definition of the device layout, packing an unbuilt table as a switched-off header
- [x] 2.3 Write `cy/aerial_perspective.slang`, the device form of `sample_at()` over those words, and stage it with the standard library

## 3. Shade the world frame through it

- [x] 3.1 Integrate the table for the stage's camera every frame from the world's own `Atmosphere` and `AtmosphereTables`, on the world's workers; bind it as set 0, binding 2 of the lit pipeline; skip it with `--no-aerial-perspective`
- [x] 3.2 In `world.slang`'s lit path, multiply by the table's transmittance and add its in-scattering, scaled by the dome's own radiance scale; leave the emissive path alone
- [x] 3.3 Draw the dome's clear sky from `IncrementalSkyView::update_aerial` over the same tables when aerial perspective is on, and let `world_visual.slang`'s cloud pass compose over it; keep the stand-in gradient when it is off, and follow the switch in `verify_cloud_agreement`
- [x] 3.4 Regenerate `world_spirv.h`, `world_msl.h`, `world_visual_spirv.h` and `world_visual_msl.h`; check the MSL keeps the set at `[[buffer(0)]]` and the push block at `[[buffer(1)]]`

## 4. Tests that can fail

- [x] 4.1 `render.world_aerial_perspective`: every wall texel is the processor's `sample_at()` applied; a near wall is unchanged within 0.001; walls further out approach the sky along the horizon
- [x] 4.2 `render.world_aerial_perspective`: a wall 800 km out meets the sky texel above it at the horizon, and does not without aerial perspective
- [x] 4.3 `render.world_aerial_perspective`: more aerosol and a lower sun each move the sky and the distant wall together
- [x] 4.4 `render.world_aerial_perspective`: off is bit-identical to the world's shaders pinned at 1fe6446, and the dome is never hazed
- [x] 4.5 `integration.render_sky_tables`: the surface sampler, the parallel rebuild and the packed layout
- [x] 4.6 Bind a switched-off table in `render.world_cloud_shadow`, whose layout restates the pipeline's
- [x] 4.7 Prove each red by a mutation, restored and md5-verified (`evidence/falsification.txt`)

## 5. The picture

- [x] 5.1 Compare `--no-aerial-perspective` against a build of the base commit frame by frame, and the authoritative digests (`evidence/frame-identity.txt`)
- [x] 5.2 Publish before/after images of distant terrain and forest under `docs/design/images/`
- [x] 5.3 Measure the per-frame cost

## 6. Records

- [x] 6.1 Map `Aerial perspective` in `tools/roadmap/requirements-coverage.toml` to the frame-level case, naming what is still not built
- [x] 6.2 Update the READMEs of `samples/10-world`, `src/rendering/sky`, `src/rendering/shaders` and `tests/render`
- [x] 6.3 `openspec validate add-aerial-perspective --strict`
