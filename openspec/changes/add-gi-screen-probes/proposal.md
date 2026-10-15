# Gather the frame's indirect light through screen probes

## Why

Issue #35's stage 4 lights a frame's pixels with a per-pixel final gather: every pixel a traced
query of the radiance cache and one ray through the world tier. At 1920x1080 on the RTX 5060 that is
11.5 ms through the hardware tier and 15.2 ms through the software tier, most of it work two
neighbouring pixels of one surface repeat; only the half-resolution grid with the denoiser lever
brought it to 2.24 ms. The issue's program names the alternative that engines in this class use —
Lumen's Screen Probe Gather: gather points on a screen grid that trace many rays, and every pixel
integrating the ones on its own surface — and asks for it measured on the same scenes and axes.

## What changes

- **The screen-probe gather, on the host** (`gi/screen_probes.h`, the oracle): a uniform probe at
  the centre of every 8- or 16-pixel tile, and an adaptive probe on each half-tile cell whose
  centre the uniform probes cannot interpolate (their plane-distance and normal weights sum below a
  threshold); each probe's rays drawn from a defensive mixture — a guide over the hemisphere's
  equal-area (cos theta, phi) square weighted by the probe's reprojected previous radiance, or by
  the radiance cache, beside uniform and cosine shares that keep the estimate unbiased; the
  estimate projected onto second-order spherical harmonics and blended with the history of the
  one previous probe that stands for it best where it lands; every pixel integrated from the
  probes about it by bilinear, plane-distance and normal weights, about its own normal. On the
  software tier a probe's rays leave one voxel clear of any other surface (DDGI's relocation), so a
  probe beside a thin wall does not step inside it.
- **The lever** (`gi::GiGather` in `ScreenResolveSettings`): `ScreenProbes` is the default and
  `PerPixel` is the per-pixel gather unchanged. `screen_inputs.h` holds the types both gathers read.
- **On the device** (`gi_gpu::GpuScreenResolve`): three passes in place of the per-pixel resolve —
  place, trace (one workgroup a probe, a lane a ray, summed in ray order), integrate — through both
  tiers (`gi_screen_probes.slang`, `gi_screen_probes_hardware.slang`), the adaptive slots taken with
  an atomic and every probe addressed by its cell. Timestamps per stage (`probe_place_ms`,
  `probe_trace_ms`, `integrate_ms`). The shared denoiser runs one a-trous pass over the probes'
  integrated signal (`ScreenProbeSettings::filter_passes`, `gi::screen_signal_config`).
- **Measured**, beside the per-pixel gather, on the guide's scenes and axes at 1920x1080 and for a
  3840x2160 output, through both tiers (`smoke.gi_gpu_probe_quality`), and on the host at 128x72
  (`smoke.render_gi_screen_probes_quality`).

## Scope

The follow-up of #35's stage 4. Not in this change: the screen tier in the probes' rays (the resolve
has no previous frame's colour to march against; the probes trace the world tiers, as the per-pixel
gather does); the full-resolution integration within ~4 ms at 1080p, which the shared denoiser's
full-resolution passes prevent (the half-grid lever reaches it).
