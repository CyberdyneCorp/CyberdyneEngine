# Add irradiance volumes to the frame

## Why

`rendering-global-illumination` requires irradiance volumes: 3D grids of spherical-harmonic probes,
sampled trilinearly across the eight surrounding probes with a visibility term, lighting what moves
through an environment. The tree had the pieces around the hole and not the volume: the radiance
cache encodes SH L1 and records a visibility term, `exclusion_for()` treats an irradiance volume as
a source, and `SurfaceProperties::irradiance_volume_radiance` is a field `indirect_diffuse` reads —
but nothing produced one, and the requirements map recorded the requirement as `exempt:m11e`. The
forward frame's ambient term was one flat colour for every surface, `albedo * sky irradiance`, so a
wall the sun does not reach was the same colour at its foot as at its top and a white surface beside
a red wall stayed white.

## What changes

- `gi::IrradianceVolume` (`src/rendering/gi/`): a regular grid of SH L1 probes, captured by a fixed
  Fibonacci set of rays through the seams every GI consumer holds — `SceneTracer`, `RadianceLookup`
  and the `SkyTerm` — so a capture is one bounce of scene light plus the sky, and further bounces when
  the lookup feeds the volume back in. Sampling is trilinear over eight probes weighted by validity
  (a probe inside geometry weighs zero), a backface factor, and a per-axis free-distance test that
  zeroes a probe on the far side of a wall. A stated update policy: a bake (`capture_all`), region
  invalidation, and a per-update probe budget under `OnInvalidation` or `Amortised`.
- `gi::BoxProxyScene`: axis-aligned proxy boxes implementing `SceneTracer`, `Occluder` and
  `RadianceLookup`, so a volume can be captured from blockout or bounding geometry.
- `cy::rendering-light-probes` (`src/rendering/light_probes/`), a new module joining GI and the
  frame: the probes as an `Rgba16Sfloat` texture in the frame's texture table, uploaded only when the
  volume changed, and `write_probe_volume` for the view block.
- `cy/frame.slang` appends `probeVolumeControl`, `probeVolumeOrigin` and `probeVolumeParams` to the
  frame block (`FrameViewData` 432 → 480 bytes). With a volume slot the forward fragment replaces
  the flat ambient by `probeVolumeAmbient` — a transcription of `IrradianceVolume::ambient` — and
  ambient occlusion still multiplies the result. Without one the frame is unchanged. The frame's
  committed SPIR-V and MSL are regenerated; the fullscreen entries are unchanged.
- Tests: `integration.render_gi_volume` on the host and `render.light_probes` on a device.

## Scope

This is the first slice of global illumination, not dynamic GI. It does NOT re-capture by itself
when lights move (a caller invalidates, or runs `Amortised`), does not capture on the device, has
no specular term, places probes on a regular grid only (adaptive subdivision and hand placement are
not built), has no baked-volume asset or cook, and the visibility term is six axis distances per
probe, which misses occluders off those axes. The requirements map records each by name.
