# Design

## The host stays the oracle

`gi/screen_probes.h` computes on the host everything the device's three probe passes do, and each
device function names the host function it transcribes. `render.gi_gpu_resolve` gathers one room
through both on both tiers and on the half grid, frame after frame, and compares the probes by
their cell, the integrated signals, the denoised signals and the published targets (about 2e-7 mean
relative; every probe placed alike). The randomness is `screen_hash` of a probe's id — its tile, or
the uniform count plus its half-tile cell — so the two sides draw the same rays.

## Probes where the grid cannot interpolate

A uniform probe stands at the centre pixel of every tile. A half-tile cell takes an adaptive probe
where the four uniform probes about its centre, weighted bilinearly and by
`screen_probe_geometry_weight` (the distance from the probe's plane against 5 % of the view depth,
falling linearly, times the normals' cosine to the eighth), sum below one half — a depth step, a
crease, the sky's edge — and nowhere else. A frame keeps at most as many adaptive probes as uniform
ones. The device takes the adaptive slots with an atomic, so a slot's index depends on thread order;
nothing reads a slot except through its cell's entry in the table, so the answer does not.

## Second-order harmonics

A pixel reads irradiance about its own normal from a probe, and irradiance is the radiance
convolved with a clamped cosine: bands 0, 1, 2 attenuated by 1, 2/3, 1/4 and higher bands by at
most 1/24. Nine RGB coefficients (27 floats) hold it where an 8x8 octahedral map is 192, and the
integration evaluates them in closed form for each pixel's normal. The rough specular reads the
same coefficients in the reflection direction with the cosine's attenuation scaled by roughness; a
surface smooth enough for its own lobe ray keeps one, as the per-pixel gather does.

## An unbiased estimate, guided

A probe's rays are drawn from a mixture: a guide over a 4x4 grid of the hemisphere's equal-area
(cos theta, phi) square — each cell 2 pi / 16 sr — weighted by the probe's previous radiance where
it has a history and by the radiance cache's answer in each cell's direction where it has none, and
a uniform and a cosine share of what the guide does not take. Each ray is weighted by one over the
mixture's density, so the estimate's mean does not depend on the guide; the uniform share bounds
the weight at 8 pi. The guide takes at most 0.9: a guide cell of zero weight — the previous
radiance's harmonics dip below zero at the horizon — is never drawn, and a guide alone was measured
2.8 % dark under an open sky. A ray that answers nothing (a back face with no cache to stand in)
adds nothing. Treating the estimate as the answered rays' mean instead was the first form; with the
ray origins moved clear of other surfaces (below) such rays are rare, and it moved the room's
error by 0.001, so it was taken out.

## History: the one probe that stands for it

A probe's position lands, through `temporal-rendering`'s motion, on a cell of last frame's grid.
Of the uniform and adaptive probes about that cell it takes the history of the one whose bilinear
and geometric weight is highest, if that reaches a quarter, and none inside an invalidated region.
A blend of all of them was the first form: in a still view it mixed a probe's neighbours into its
history every frame, a blur that took the room from 0.093 to 0.099 of the path tracer. The blend
weight is max(1 / (count + 1), 1 / history_frames), two frames by default: four took a moved light
from 13 frames to 18 for no measurable stillness.

## A ray leaves a voxel clear of other surfaces

On the software tier a probe's rays leave its surface lifted by a voxel and then moved out to one
voxel from any other surface along the field's gradient. A probe on a floor two centimetres from a
10 cm wall, lifted straight up, sits inside the sphere trace's hit threshold; its first step toward
the wall lands inside it, the field's gradient there points anywhere, and the hit's card lookup
reads the far side's card. The sealed room's worst pixel took a quarter of the lit room's light at
1080p; moved out, none.

## One a-trous pass

The integrated signal is an average of many rays: frame to frame it moves a twentieth to a tenth as
much as the one-ray signal. The shared denoiser's later passes — 1.35 ms each at 1080p — change
neither its error against the path tracer nor its stillness (measured with one pass and with
four), so the probe gather caps the cascade at one (`filter_passes`); `gi::screen_signal_config`
is the one place that cap is applied, to the host denoiser by its owner and to the words the device
reads off it.

## What the gather costs, and where the rest of a frame goes

At 1920x1080 the probes' own three passes are 1.8 ms through the hardware tier and 2.7 ms through
the software tier; the full-resolution accumulation, variance and one filter pass are 3.1 ms more,
the reprojection and the publish 0.6: 5.5 / 6.4 ms, where the per-pixel gather was 11.7 / 15.2.
The half-grid lever puts the integration and the denoiser on a quarter of the pixels: 2.4 / 3.3 ms.
