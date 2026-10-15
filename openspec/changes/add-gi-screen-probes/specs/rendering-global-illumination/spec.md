## ADDED Requirements

### Requirement: Screen-probe final gather
The frame's final gather SHALL by default be a screen-probe gather: probes placed on a screen grid
every 8 or 16 pixels (configurable), and adaptive probes placed where a depth or normal
discontinuity leaves a pixel no uniform probe on its own surface; rays traced from each probe
through the same world tiers the per-pixel gather traces, drawn from a defensive mixture that
importance-samples the probe's previous radiance or, with no history, the radiance cache; each
probe's radiance stored compactly and blended with its reprojected history; and every pixel
integrated at the resolve grid from the probes about it, weighted by its own normal and by plane
distance, before the shared temporal and spatial denoiser. The per-pixel gather SHALL remain
selectable behind a lever, unchanged, and the host implementation SHALL remain the oracle the
device is checked against.

#### Scenario: Adaptive probes appear at discontinuities and nowhere else
- **WHEN** probes are placed over a flat floor, and over the same floor with a block standing on it
- **THEN** the flat floor SHALL take no adaptive probe, the block's view SHALL take adaptive probes,
  and every adaptive probe SHALL lie within one tile of a depth or normal step

#### Scenario: A probe's history is reprojected, and none survives an invalidation
- **WHEN** a probe's position lands, through the camera's motion, on last frame's probes
- **THEN** it SHALL take the history of the one that stands for it best, a probe in an invalidated
  region SHALL take none, and its blend SHALL move that history toward this frame's estimate by
  a stated weight

#### Scenario: The estimate is unbiased whatever the guide draws
- **WHEN** probes under an open sky draw none, half or the most allowed of their rays from the guide
- **THEN** their mean irradiance SHALL equal the sky's within a stated bound in every case

#### Scenario: The lever keeps the per-pixel gather
- **WHEN** the per-pixel gather is selected
- **THEN** every pixel SHALL be the per-pixel resolve's bit for bit, and with either gather attached
  at none the frame SHALL be byte-identical to the frame without the GI

#### Scenario: The device probes match the host
- **WHEN** the same view is gathered through screen probes on the device and on the host, through
  the software or the hardware tier, at full resolution or on a half grid
- **THEN** the probes SHALL be placed alike and their coefficients, the integrated signals, the
  denoised signals and the published targets SHALL agree within a stated bound

#### Scenario: The probes are as near the path tracer as the per-pixel gather
- **WHEN** a converged 1920x1080 frame of the GI test room through screen probes is compared with
  the path tracer at stated points
- **THEN** its relative error SHALL be within a stated margin of the per-pixel gather's on the same
  view and tier, and the sealed room behind a 10 cm wall, a moved light and a still view SHALL hold
  their stated bounds

#### Scenario: The probes' cost is measured per stage
- **WHEN** the screen-probe gather runs at 1920x1080 and for a 3840x2160 output
- **THEN** the GPU time of placement, tracing and integration SHALL be reported beside every other
  stage, through both tiers
