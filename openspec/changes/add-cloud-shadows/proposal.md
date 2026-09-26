# Proposal

## Why

`atmosphere-sky-and-clouds` requires clouds to cast shadows onto the world through a coarse
world-scale field "consumed by terrain, foliage, water, and illumination". The producer exists
(`sky::CloudShadowField`, M10), the device sampler exists (`cy/cloud_shadow.slang`, M11.a), and an
illumination library reads the field (`rendering::apply_cloud_shadows`, M11.c). Nothing shades a
surface through any of them. M11.c task 5.1 measured it: `cy/cloud_shadow.slang` is imported by no
shader, and `samples/10-world` dims its whole sun by one scalar measured at the camera. The row's
coverage map records `Cloud shadows` as `exempt:m11e` for that reason.

## What Changes

- `samples/10-world` claims the `cloud-shadow` field through `sky::CloudShadowField`, declares its
  four consumers, and rewrites the field every frame from the same `CloudField` the sky's lighting
  marches — driven by weather's `CloudDrive` and advected by each layer's wind at the frame's time.
- The world's lit fragment path, which draws the terrain, the foliage and the water, samples the
  field per fragment through `cy/cloud_shadow.slang` and attenuates the DIRECT sun with it: the
  Lambert term and water's specular lobe. The ambient term and the emissive sky dome are untouched.
  Under cloud shadows the sun is placed before the clouds (`SkyLighting::clear_sun_illuminance`), so
  no surface is shaded twice by the cloud over the viewer.
- `cy/cloud_shadow.slang` gains the image-bound form (`cyCloudShadowAtImage`,
  `cyCloudShadowAttenuateImage`) that a frame binding its field images directly uses; the bindless
  form is built on it, so both paths share one line of arithmetic.
- `--no-cloud-shadows` draws the frame as it was before; headless runs never produce the field.
- New tests: `integration.render_sky_fields` gains the wind-motion case and the sky/ground agreement
  case; `render.world_cloud_shadow` draws with the sample's committed SPIR-V on a Vulkan device and
  asserts darker-under, unchanged-beside, byte-identical off and under a clear sky, and the sky and
  ambient term unattenuated.
- `tools/roadmap/requirements-coverage.toml` maps `Cloud shadows` to the frame-level case instead of
  the exemption.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `atmosphere-sky-and-clouds`: `Cloud shadows` gains scenarios that make its consumers observable:
  direct sunlight only, no cost where there is no cloud, the shadow drifting with the wind, and the
  sky and the ground reading one cloud.

## Impact

`src/rendering/shaders/cy/cloud_shadow.slang`, `src/rendering/sky` (one additive `SkyLighting`
field), `samples/10-world` (world, stage, main, `world.slang` and its generated SPIR-V and MSL),
`tests/render`, `src/rendering/sky/tests`, the coverage map and the READMEs. No change to the post
chain or to `cy/frame.slang`: the engine's forward frame has no environment-field binding and is
not the frame that draws a world (see design.md). No authoritative, saved, replayed or networked
state changes; the authoritative digest of the headless take is unaffected.
