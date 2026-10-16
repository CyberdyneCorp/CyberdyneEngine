# Denoise the screen probes in probe space, and show the GI beside the path tracer

## Why

The screen probes made CyberGI's default gather half the per-pixel gather's cost at 1920x1080 for
the same error, but the frame was still 5.5 ms through the hardware tier and 6.4 ms through the
software tier, and 3.1 ms of it was the shared denoiser run at full resolution over a signal that
is already an average of many rays: an accumulation, a variance estimate, an a-trous pass and a
publish, about a gigabyte of memory traffic a frame. A moved light also settled two or three frames
later through the probes (14 / 13) than through the per-pixel gather (9 / 11). Issue #35 asks for
the default gather within about 4 ms at full resolution, with no worse error, leak, stability or
settle time; and for a picture of the GI where indirect light dominates, beside the path tracer.

## What changes

- **A probe-space denoiser, the screen probes' default** (`gi::ScreenProbeDenoiser::ProbeSpace`):
  each probe's harmonics averaged with the uniform probes of the 3x3 tiles about it on its own
  surface — a tent over the tiles, a world reach of a tenth of the probe's depth, and the probes'
  plane and normal weight — before any pixel integrates them (`ScreenProbeGather::filter`); the
  pixels then accumulated over time and nothing more (`screen_signal_config`: no cascade). On the
  device the accumulation and, on the full grid, the publish are one pass
  (`gi_temporal.slang`'s `cyGiDenoiseTemporal`) over a history of its own, with no moments.
  `ScreenProbeDenoiser::Shared` keeps the previous path selectable, unchanged.
- **The budget's ray density under the probes**: below one, that fraction of the probes trace a
  frame (`gi::screen_probe_traces`) and the rest keep the probe they stand for — where it was half
  of each probe's rays, which saved 8 % of a probe's trace. With the denoiser no longer most of
  the frame, it is what lets stage 5's floor still halve the device cost (0.98 to 0.52 ms at
  960x540).
- **History rejection** (`denoise::SignalConfig::history_rejection`, zero by default for every
  signal): where a new sample strays from the history by more than a fraction of the larger of
  the two, the history keeps the square of that fraction over the deviation of its count. The
  probe-space denoiser sets it to 0.2; the shared denoiser's signals are as they were.
- **The comparison as a picture** (`samples/12-beauty`, `just capture-gi-interior`): a sunlit room
  (`content/beauty/interior.cyshot`, a red and a green wall, white plaster, one window) and the
  Colonnade, each without the GI, with CyberGI, and through the path tracer's indirect diffuse
  traced every fourth pixel and upsampled, beside the per-pixel error map. The shot format gains
  `material <key> base-color` and `air none`.

## Measured (RTX 5060, 1920x1080, `smoke.gi_gpu_probe_quality`)

| | before (shared) | after (probe space) |
|---|---|---|
| GPU time, hardware / software | 5.47 / 6.34 ms | 2.74 / 3.62 ms |
| error against the path tracer | 0.066 / 0.076 | 0.064 / 0.071 |
| sealed room behind 10 cm | 0 / 0 | 0 / 0 |
| a still view, per frame | 0.0017 / 0.0019 | 0.0011 / 0.0016 |
| settle on a moved light | 13 / 14 frames | 10 / 9 frames |

## Scope

The screen-probe gather's denoiser, the shared denoiser's new history rejection, and the beauty
sample's comparison mode. Not in this change: sharing work with the temporal anti-aliasing (the
frame's TAA runs after the composite and keeps no GI history), and the screen tier in the probes'
rays.
