# Design

## Where the 3.1 ms went

At 1920x1080 through the screen probes, the probes' own passes were 1.8 ms (hardware) / 2.7 ms
(software). The rest was the shared denoiser at full resolution over both signals: the
accumulation 1.06 ms, the variance 0.54, one a-trous pass 1.50 and the publish 0.23 — each a pass
over two million pixels and two signals, bound by memory, not arithmetic (the accumulation alone
moves about 365 MB). Its later stages buy little for the probes' signal: an integrated pixel is an
average of 32 rays a probe over four probes, and moves a tenth as much from frame to frame as the
per-pixel gather's one ray.

## The candidates, measured on the guide's axes

Every candidate ran in `smoke.gi_gpu_probe_quality` at 1920x1080 on the RTX 5060 — the path tracer
at 220 points, the sealed room behind 10 cm, a still view, a light moved 4.9 m — and its cost per
stage. Figures are hardware / software.

| Candidate | GPU ms | error | still view | settle |
|---|---|---|---|---|
| the shared denoiser, one pass (before) | 5.47 / 6.34 | 0.066 / 0.076 | 0.0017 / 0.0019 | 13 / 14 |
| probe-space filter, temporal only, history 16 | 2.74 / 3.62 | 0.066 / 0.073 | 0.0018 / 0.0025 | 14 / 15 |
| ... history 32 | 2.74 / 3.62 | 0.066 / — | 0.0014 / — | 13 / — |
| ... history 32, rejection 0.25 | 2.74 / 3.62 | 0.065 / 0.072 | 0.0012 / 0.0016 | 11 / 10 |
| ... history 32, rejection 0.2 (chosen) | 2.74 / 3.62 | 0.064 / 0.071 | 0.0011 / 0.0016 | 10 / 9 |
| ... history 32, rejection 0.15 | 2.74 / 3.62 | 0.065 / 0.072 | 0.0009 / 0.0016 | 9 / 9 |
| ... history 32, rejection 0.1 | 2.74 / 3.62 | — / 0.076 | — / 0.0026 | — / 7 |
| ... a 5x5 tile filter, rejection 0.25 | 2.76 / 3.63 | — / 0.071 | — / 0.0016 | — / 10 |
| ... on the half-resolution grid, upsampled | 1.82 / 2.77 | 0.064 / 0.071 | 0.0011 / 0.0016 | 10 / 9 |

The pixels' accumulation dominates stillness: with no cascade it is the only thing between the
probes' signal and the frame, and its weight floor decides how far a converged pixel moves. At
history 16 the still view moved 0.0025 a frame through the software tier; at 32 — the shared
denoiser's own — 0.0016, below the shared path's 0.0019. History rejection then decides settle:
the accumulation's weight is one over the count since the history was last cut, so after an
invalidation it averages every frame the caches spent converging; cut where the signal moves past
its noise, it follows them. 0.1 cuts the noise itself (the still view moves 0.0026), 0.15 and 0.2
cost nothing measurable; 0.2 keeps the wider margin to that cliff. A 5x5 tile filter changed the
error by 0.001 and blurs twice as far for it. The half-resolution grid — the denoiser and the
integration at a quarter of the pixels, upsampled against the frame's depths and normals — measured
the same at the 220 points, which fall on its samples; what it gives up lies between them, at
edges, and it stays the `TracingResolution` lever's first step rather than the default. Sharing work with the temporal anti-aliasing was not taken: the
frame's TAA (`post` step 5) runs on the composited colour, after the GI's two targets are sampled,
and its neighbourhood clamp would reject the GI's frame-to-frame noise rather than average it; the
whole denoiser it could replace is now 0.58 ms.

## Filtering in probe space

`ScreenProbeGather::filter` runs after `trace`. Each live probe — uniform or adaptive — takes its
own coefficients at weight one, and the uniform probe of every tile within `filter_tiles` of its
own, weighted by a tent over the tile offset (1/2 at one tile), by its distance against
`filter_reach` of the probe's view depth (linear to zero), and by
`screen_probe_geometry_weight` — the probes' plane distance and normal agreement, the weight the
integration already uses. It writes `filtered`, never `coefficients`: the next frame reprojects
the unfiltered history, so the filter does not compound frame over frame, and the order the probes
are filtered in changes nothing. The pixels integrate `filtered`.

The world reach is what keeps the filter's footprint a property of the scene rather than of the
resolution. At 1080p a 16-pixel tile is about 1.2 % of the view depth, and every neighbour is well
inside a tenth of it; at the host suite's 128x72 a tile is a metre of the room's wall, and a
filter by tiles alone averaged three metres of it — 0.13 from the path tracer against 0.10
unfiltered, past the suite's bound. With the reach it is 0.108.

## One pass in place of five

With no cascade the variance is never read, and `Denoiser::filter` with no pass returns the
accumulation itself. `cyGiDenoiseTemporal` (gi_temporal.slang) is `Denoiser::accumulate` for both
signals of one pixel — one thread reads the history source once — and on the full grid it writes
the two targets the frame samples, as `screen_upsample` at a divisor of one does. Its history has a
layout of its own, one float4 a pixel a signal a slot with no moments beside it: a pass that wrote
every other float4 of the shared layout paid for whole cache lines it half wrote (1.6 ms in the
first version, 0.58 now). `GpuScreenResolve` starts the history over whenever a frame moves
between the two layouts. On a coarser grid it hands the accumulation to `cyGiPublish`.

The targets are declared as the pass's writes on every grid, because the pass binds them on every
grid: a descriptor names an image in the layout its pass declared, and an undeclared binding on the
half grid was a write-after-write hazard against the publish that the validation layer caught.

## The budget's floor, once the denoiser is cheap

Stage 5's suite requires the levers at their floor to take the device cost below 0.6 of the
authored one. With the shared denoiser most of the frame, the half grid and the denoiser lever did
that; with the probe-space denoiser the frame is mostly the probes' trace, and the floor's other
step — half of each probe's rays — moved a 960x540 frame from 0.96 ms to 0.67 (0.70). A probe's
trace is mostly what it does once: its reprojection, the cache's answer at it, its guide, and one
lane summing its rays in order; half the rays cost 0.92 of all of them. Below a ray density of one
the probes now trace that fraction of themselves a frame, chosen by a hash of the probe and the
frame (`screen_probe_traces`, stream 9), and the rest keep the probe they stand for — its radiance,
count, cache answer and openness — at none of a trace's cost; a probe with no history always
traces. The floor is 0.52 ms (0.53). At a density of one nothing changes.

## History rejection, in the shared denoiser

`SignalConfig::history_rejection` is the shared denoiser's, on the host and in both device
accumulations (gi_denoise.slang, gi_temporal.slang), so the transcription stays complete: where
|new - history| exceeds the tolerance times the larger of the two (luminance, or the visibility
channel), the history keeps (tolerance x larger / deviation)^2 of its count. Zero — every signal's
default — is the accumulation as it was. It belongs to signals whose frame-to-frame noise is well
below the tolerance; a one-ray signal's is not, which is why only `screen_signal_config` under the
probe-space denoiser sets it.

## The comparison as a picture

`BeautyGi::reference_image` reads the frame's surfaces back, path traces `gi::PathTracer`'s
irradiance over the exact triangles at every fourth pixel each way (`gi::ScreenGrid`), each pixel
its own sequence, threads each with their own tracers (both count rays in a plain member), and
upsamples it with `gi::screen_upsample` against the frame's depths and normals. `Stage` uploads it
and binds it where `sceneFragmentGi` reads the GI's diffuse target, draws the frame once more, and
binds the GI's target back: the reference frame differs from the GI's in the indirect diffuse and
in nothing else. The error map is |GI - reference| over the reference's luminance, floored at a
tenth of its mean so a near-black corner is not all error; the figure quoted is
`compare_against_reference`'s over every traced pixel.
