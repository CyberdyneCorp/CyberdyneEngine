## ADDED Requirements

### Requirement: The frame's indirect light is resolved per pixel on a device
The engine SHALL be able to resolve a frame's indirect diffuse and rough specular per pixel on a
graphics device — from the radiance cache traced to each pixel's probes, one final-gather ray
through the world tier, and the far-field sky, combined under the double-counting rule — and SHALL
denoise both through the shared denoising framework over a history reprojected by
`temporal-rendering`. The forward frame SHALL composite the result where the irradiance volume's
ambient sits, under the same rule. The host implementation SHALL remain the oracle the device is
checked against, and a frame that attaches no producer SHALL be the frame it was.

#### Scenario: The device resolve and denoiser match the host
- **WHEN** the same view is resolved and denoised on the device and on the host over the same
  caches, through the software tier or the hardware tier, at full resolution or on a half grid
- **THEN** the noisy signals, the denoised signals and the published targets SHALL agree within a
  stated bound, and the history SHALL be identical

#### Scenario: A final-gather ray counts in full whatever it met
- **WHEN** a pixel's ray escapes to the sky in one frame and meets a stale card in another
- **THEN** the ray SHALL enter the combine with the same weight in both, its confidence deciding
  only how much of its value the cache's answer stands in for

#### Scenario: The per-pixel GI is close to the path tracer
- **WHEN** a converged frame of the GI test room is compared with the path tracer at stated points
- **THEN** its relative error SHALL be below a stated bound and below half the radiance cache's alone

#### Scenario: No light passes a 10 cm wall
- **WHEN** a lit room and a sealed room share a 10 cm wall
- **THEN** the sealed room's indirect light SHALL be below a stated fraction of the lit room's, on
  average and at its worst pixel

#### Scenario: A moved light converges within a stated frame count
- **WHEN** a light moves after the frame has converged
- **THEN** the frame SHALL be within a stated distance of the new light's converged frame within a
  stated number of frames

#### Scenario: A still view is temporally stable
- **WHEN** a converged view does not move
- **THEN** the published signal SHALL change by less than a stated amount a frame

#### Scenario: Off is the frame it was
- **WHEN** the producer is attached with its words at none, or under a mode that admits no dynamic
  bounce
- **THEN** the frame SHALL be byte-identical to the frame without it

### Requirement: The device GI's cost drives the GI budget and the arbiter
The GI SHALL report the GPU time of its per-pixel work, measured with timestamps, as its own cost to
the GI budget; the budget's tracing-resolution and denoiser-quality levers SHALL reach the device
resolve; and the GI budget SHALL report its measured cost and lever position to the renderer budget
arbiter and SHALL relax only on the arbiter's grant.

#### Scenario: Over budget, the levers drop in the declared order and the cost falls
- **WHEN** the device GI's measured cost exceeds its allocation for several frames
- **THEN** the levers SHALL drop in the budget's declared reduction order to the reserved minimum,
  and the measured cost SHALL fall below a stated share of the authored quality's

#### Scenario: Relaxation waits for the arbiter
- **WHEN** the cost is back under the allocation but the arbiter has granted no relaxation
- **THEN** no lever SHALL relax; and when it grants one, at most one SHALL

#### Scenario: The arbiter sees the GI's own cost
- **WHEN** the GI budget submits to the arbiter
- **THEN** the arbiter SHALL receive the measured GPU time, marked measured, at the ladder rung the
  levers add up to
