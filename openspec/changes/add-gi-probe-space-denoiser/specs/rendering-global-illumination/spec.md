## ADDED Requirements

### Requirement: Probe-space denoising of the screen probes
The screen-probe gather SHALL by default denoise in probe space: each probe's radiance averaged with
its neighbouring probes on its own surface — weighted by their screen offset, by their distance
against a stated fraction of the probe's view depth, and by plane distance and normal agreement —
before any pixel integrates it, the probes' own history never taking the filtered radiance; and the
integrated pixels then accumulated over time with no spatial pass, the history cut where the new
value strays from it past a stated tolerance. The shared denoiser's path SHALL remain selectable
behind a lever, unchanged, and the host implementation SHALL remain the oracle the device is
checked against. The default SHALL be no worse than the shared path on the guide's axes — error
against the path tracer, light through a 10 cm wall, a still view and a moved light — at a stated
lower cost.

#### Scenario: The filter averages a probe with its neighbours on its own surface
- **WHEN** the probes of a frame are filtered
- **THEN** each probe's filtered radiance SHALL be its own and its neighbours' weighted by the
  stated tent, reach and geometry weights, a probe with no neighbour within reach SHALL keep its
  own, and the probes' history SHALL be the unfiltered radiance

#### Scenario: The lever keeps the shared denoiser
- **WHEN** the shared denoiser is selected for the screen probes
- **THEN** the probes SHALL be integrated unfiltered and the shared denoiser's accumulation,
  variance and cascade SHALL run as before

#### Scenario: The device matches the host
- **WHEN** the same view is gathered through screen probes on the device and on the host, under
  either denoiser, past the pixels' history length
- **THEN** the filtered coefficients, the integrated, denoised and published signals SHALL agree
  within a stated bound, with no validation error on the full grid or the half grid

#### Scenario: The budget's floor traces fewer probes
- **WHEN** the GI budget lowers the screen probes' ray density below one
- **THEN** that fraction of the probes SHALL trace each frame, chosen alike on the host and the
  device, every other probe SHALL keep the probe it stands for, a probe with no history SHALL trace,
  and the levers at their floor SHALL lower the device cost by a stated share

#### Scenario: Cheaper, and no worse
- **WHEN** a 1920x1080 frame of the GI test room is gathered through the screen probes under each
  denoiser, through each tier
- **THEN** the probe-space denoiser's GPU time SHALL be below the shared path's, and its error, the
  sealed room's light, its still-view motion and its settle frames SHALL each be no worse

### Requirement: A comparison picture against the path tracer
The engine's capture SHALL be able to photograph one view three ways — without the dynamic GI,
with it, and shaded through the offline path tracer's indirect diffuse in place of the GI's,
everything else the frame draws unchanged — beside a per-pixel map of the GI's error against the
path tracer, and report that error in aggregate over every traced pixel and the GI's GPU time.

#### Scenario: An interior where indirect light dominates
- **WHEN** a closed room lit by the sun through one window is captured with the comparison
- **THEN** the three frames and the error map SHALL be published with the aggregate error and the
  GI's GPU milliseconds in the caption
