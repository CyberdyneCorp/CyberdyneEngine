## MODIFIED Requirements

### Requirement: Signal-specific configuration
Each signal SHALL declare its denoising configuration: expected frequency content, whether it is
lobe-dependent (specular) or hemispherical (diffuse), how roughness modulates the filter, whether
it is a visibility term or a radiance term, its history length, and how far a new sample may stray
from its history before the history is cut.

Specular denoising SHALL account for the reflection lobe: a rough surface tolerates a wide filter,
a smooth one does not.

Visibility terms (shadows, ambient occlusion) SHALL be denoised as occlusion rather than as
radiance, since blurring them as colour loses contact detail.

A signal that declares no history rejection SHALL be accumulated exactly as before rejection
existed.

#### Scenario: Smooth reflection stays sharp
- **WHEN** a near-mirror reflection is denoised
- **THEN** the filter SHALL remain narrow, so the reflection is not smeared

#### Scenario: Shadow contact is preserved
- **WHEN** ray-traced shadows are denoised
- **THEN** contact hardening SHALL be preserved rather than filtered as a colour signal

#### Scenario: A history the signal has left behind is cut
- **WHEN** a signal that declares a history rejection tolerance steps past it from its history
- **THEN** the history SHALL keep a stated share of its count, so the new value is followed within
  a frame or two, and within the tolerance the accumulation SHALL be the one without rejection
