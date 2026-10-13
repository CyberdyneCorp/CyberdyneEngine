# Design

## The host stays the oracle

`gi/screen_resolve.h` computes, on the host, everything the device does for a frame's pixels; every
device dispatch names the host function it transcribes and writes it in the same order.
`render.gi_gpu_resolve` runs the host and the device over one room frame for frame and compares the
noisy signals, the history, the denoised signals and the published targets (about 1e-7 mean
relative, both tiers, both grid divisors; the history exact). The forward frame's composite is held
by `render.gi_gpu_frame`: inert with its words at none, changing the picture when bound, and
inert again under a mode that admits no dynamic bounce.

## Probes traced, not trusted

The radiance cache's visibility term is six axis distances per probe, which stop light along the
axes and not across a thin wall at an angle. A pixel's probes are therefore traced from the pixel
through the field (`sample_traced`): those within one spacing, each kept only when nothing in the
field stops the trace short of the probe. A probe a 10 cm wall hides from the pixel contributes
nothing, and the sealed room behind the wall stays dark to 2.6e-5 of the lit room's mean.

## One ray a pixel counts in full

The final-gather ray is one draw of the hemisphere; its average over frames is the hemisphere's only
if its weight does not depend on the draw. Its confidence therefore decides only how much of its
value the cache's answer stands in for — a stale card, an uncovered hit, a back face — and the ray
always enters the combine with weight one. Weighting it by its own confidence let the certain sky
outweigh stale cards, and the denoiser accumulated an estimate pulled toward the sky.

## The half grid is the first lever step

At 1080p the cache query and the denoiser cost more than the rays, so `TracingResolution`'s first
step resolves every other pixel each way (`ScreenGrid`, divisor two) and upsamples against the
full-resolution normals and depths; its second halves the rays as well. The history starts over when
the divisor changes.

## The frame owns the composite

The resolve publishes two `Rgba16Sfloat` targets; the frame samples them through two slots of its
own texture table (`gi_control`), so the forward pass binds nothing new. `gi_admission` answers
`exclusion_for()` once per mode for the three kinds of surface the frame draws, and the shader
applies the answer: a lightmapped surface keeps its lightmap unless the mode admits the dynamic
bounce there; inside an irradiance volume the GI replaces the volume or blends with it.

## Units

The GI shades a card as `albedo * direct`, so a light in lux would make every bounce pi times the
frame's own `albedo / pi * E`; the beauty shot divides its sun by pi exactly as `lightmap_bake`
does for its path tracer, and fits the GI's sky gradient so that an upward face receives the frame's
mean sky radiance.
