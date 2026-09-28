# Proposal

## Why

`rendering-post-processing` requires volumetric fog "computed in a froxel volume … through: density
injection (global fog plus fog volumes), lighting injection (per-froxel light evaluation with shadow
sampling), filtering, and front-to-back scattering and transmittance integration", with light shafts
from per-froxel shadow sampling and aerial perspective composited with it rather than applied
separately. The coverage map records `Volumetric fog` as `exempt:m11e`: the froxel arithmetic is
tested (`froxel_slice_depth`, `henyey_greenstein`, `integrate_froxel` in `src/rendering/post/`) and
the volume is never filled — there is no injection, no shadow sampling, no fog volume, no device pass
and no surface that is seen through fog.

## What Changes

- New module `src/rendering/fog/` (`cy::rendering-fog`):
  - `medium.h` — the participating medium in radiative-transfer quantities only: a height fog
    (extinction at a base altitude, an exponential scale height above it, albedo, Henyey-Greenstein
    `g`), stated through Koschmieder's meteorological visibility (`sigma_t = 3.912 / V`), and up to
    eight authored fog volumes — box, sphere, cylinder and cone, each with an extinction, albedo,
    emission, `g` and a soft edge. A mixture's sun scattering is each medium's coefficient times its
    own phase function.
  - `volume.h` — the camera the volume is built for (the aerial perspective table's basis and
    `froxel_slice_depth`, word for word), the light it is lit by (the frame's own sun and ambient),
    the directional shadow map as the march reads it, the device constants, the stored layout, the
    host lookup `fog_at`, the host march `integrate_fog_column`, and `single_scattering_reference` —
    the single-scattering equation by brute-force quadrature, independent of the march.
  - `fog_pass.h` — `FogPass`: one compute dispatch that marches every froxel column front to back,
    sampling the medium and the shadow map at fixed sub-steps of each slice and integrating each
    sub-step analytically with `integrate_froxel`. It writes either a float texture the forward pass
    reads (`FogTarget::Texture`) or the atmosphere table's own layout with the air composited in
    (`FogTarget::AerialTable`).
- `src/rendering/shaders/cy/volumetric_fog.slang` — the lookup a shading pass makes, over any
  texture source.
- The forward frame gains `FramePassKind::VolumetricFog` (after the shadow pass, before opaque),
  `FrameFeatures::volumetric_fog`, the producer seam `FrameDescription::volumetric_fog_stage` and the
  imported target; `FrameAssembly` switches it on from the post chain's `volumetric_fog` and passes
  the producer through `FrameSinks::volumetric_fog`.
- `cy/frame.slang` appends `volumetricFogControl` to `CyFrameData` (528 bytes); with a volume slot
  the forward fragment is attenuated and added to through it, without one it is the frame as it was.
- `samples/12-beauty` gains `--fog on|off` and the shot's `fog` lines: the Colonnade's shafts, lit by
  the same sun through the same shadow map as the surfaces. `samples/10-world` gains `--fog` and the
  grade file's valley haze, composited with the atmosphere's table and bound where it was.
- Tests: `unit.rendering_fog`, `integration.rendering_fog_march`, `integration.rendering_fog_air`,
  `render.volumetric_fog`, and two cases in `unit.render_forward`.

## What does not change, and what is still not built

- With fog off nothing is declared and nothing is bound: the frame is the frame before, byte for
  byte (pinned against a reference drawn by the pre-change frame shader).
- NOT BUILT: temporal reprojection (there is no noise to remove — the march samples fixed sub-step
  midpoints — and no history), a separate filter pass, indirect lighting from the radiance cache (the
  ambient term is the frame's own ambient radiance, which is what its surfaces are lit with),
  punctual and spot lights in the medium, and arbitrary density functions. The requirement's
  exemption keeps those, with their reasons.
