# Tasks

## 1. Establish what exists

- [x] 1.1 Read `Cloud shadows` against the tree: producer (`sky::CloudShadowField`), device sampler (`cy/cloud_shadow.slang`), illumination library (`rendering::apply_cloud_shadows`), and the one frame that draws a world (`samples/10-world`, which dimmed its sun by one scalar at the camera)
- [x] 1.2 Confirm the committed `world_spirv.h` reproduces bit for bit from `world.slang` with the pinned Slang 2026.9.2 before changing it

## 2. Shade the world frame through the field

- [x] 2.1 Add the image-bound `cyCloudShadowAtImage` / `cyCloudShadowAttenuateImage` to `cy/cloud_shadow.slang` and build the bindless form on them
- [x] 2.2 Add `SkyLighting::clear_sun_illuminance`, the sun before the clouds
- [x] 2.3 Claim the field and declare its four consumers in `samples/10-world`, rewrite it every frame from the sky's own `CloudField`, time and sun; skip it headless; add `--no-cloud-shadows`
- [x] 2.4 Sample the field per fragment in `world.slang`'s lit path and attenuate the direct sun (Lambert and specular) only; bind the image and its placement as set 0 of the lit pipeline
- [x] 2.5 Regenerate `world_spirv.h` and `world_msl.h`; check the MSL puts the set at `[[buffer(0)]]` and the push block at `[[buffer(1)]]` in both stages

## 3. Tests that can fail

- [x] 3.1 `render.world_cloud_shadow`: ground under a known cloud darker by the field's amount, ground beside it bit-identical
- [x] 3.2 `render.world_cloud_shadow`: off, and under a clear sky with the field read, every texel bit-identical to the frame without cloud shadows
- [x] 3.3 `render.world_cloud_shadow`: the emissive sky path and ambient-only ground bit-identical with the field on and off
- [x] 3.4 `integration.render_sky_fields`: the shadow's centroid moves by wind times time; `compose_sky()`'s cloud transmittance toward the sun agrees with the field at the same ground point
- [x] 3.5 Prove each red by a mutation, restored and md5-verified (`evidence/falsification.txt`); fix the relative-tolerance defect the still-sky mutation exposed in 3.4

## 4. The picture

- [x] 4.1 Take the 192-frame day with cloud shadows on and off
- [x] 4.2 Compare `--no-cloud-shadows` against an origin/main build frame by frame (192 of 192 identical) and the authoritative digests (identical) (`evidence/frame-identity.txt`)
- [x] 4.3 Publish two before/after pairs under `docs/design/images/`
- [x] 4.4 Measure the producer's cost and choose the levers by it

## 5. Records

- [x] 5.1 Map `Cloud shadows` in `tools/roadmap/requirements-coverage.toml` to the frame-level case, naming what is still not built
- [x] 5.2 Update the READMEs of `samples/10-world`, `src/rendering/sky`, `src/rendering/shaders` and `tests/render`
- [x] 5.3 `openspec validate add-cloud-shadows --strict`
