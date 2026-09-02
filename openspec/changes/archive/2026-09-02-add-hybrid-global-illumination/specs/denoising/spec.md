## ADDED Requirements

### Requirement: One denoising framework
Denoising SHALL be a single engine subsystem consumed by every stochastic signal — indirect
diffuse, indirect specular, ray-traced shadows, ambient occlusion, and stochastic direct lighting —
rather than each effect implementing its own filtering.

A consumer SHALL supply a noisy signal, its sample count or variance, and the guidance buffers,
and SHALL receive a stable result. It SHALL NOT implement temporal accumulation, variance
estimation, or spatial filtering itself.

#### Scenario: A new noisy signal reuses the framework
- **WHEN** a new stochastic effect is added
- **THEN** it SHALL request denoising with its guidance inputs, and SHALL NOT add its own blur

#### Scenario: Shared behaviour
- **WHEN** GI and reflections are both denoised
- **THEN** they SHALL use the same accumulation, variance estimation, and edge-stopping rules

### Requirement: Denoising pipeline
Denoising SHALL proceed as: **temporal accumulation** over reprojected history, **variance
estimation** from the accumulated samples, **spatial filtering** whose radius is driven by
variance and sample count, and **history validation** rejecting invalid history.

Filter strength SHALL fall as confidence and sample count rise, so a converged signal is not
blurred.

Temporal accumulation SHALL use the framework in `temporal-rendering` for jitter, motion vectors,
reprojection, disocclusion classification, and invalidation. Denoising SHALL NOT implement its own
history handling.

#### Scenario: Converged signal is not blurred
- **WHEN** a region has accumulated many samples with low variance
- **THEN** spatial filtering SHALL be reduced or skipped there

#### Scenario: Disoccluded region is reconstructed
- **WHEN** history is unavailable for a region
- **THEN** the spatial filter SHALL widen to compensate for the missing temporal samples

### Requirement: Denoising is material and geometry aware
Filtering SHALL NOT cross boundaries where a shared value would be wrong. Edge-stopping weights
SHALL incorporate: depth difference, normal difference, roughness difference, motion, and
**material and instance identity**.

Where a visibility buffer is available, its instance and material identifiers SHALL be used
directly, since they give exact boundaries rather than inferred ones.

#### Scenario: No bleeding across an object edge
- **WHEN** a foreground object is denoised against a distant background
- **THEN** samples SHALL not be shared across the boundary, and no halo SHALL appear

#### Scenario: Identity beats inference
- **WHEN** two adjacent surfaces have similar depth and normals but different materials
- **THEN** the material identifier SHALL prevent them being filtered together

### Requirement: Signal-specific configuration
Each signal SHALL declare its denoising configuration: expected frequency content, whether it is
lobe-dependent (specular) or hemispherical (diffuse), how roughness modulates the filter, whether
it is a visibility term or a radiance term, and its history length.

Specular denoising SHALL account for the reflection lobe: a rough surface tolerates a wide filter,
a smooth one does not.

Visibility terms (shadows, ambient occlusion) SHALL be denoised as occlusion rather than as
radiance, since blurring them as colour loses contact detail.

#### Scenario: Smooth reflection stays sharp
- **WHEN** a near-mirror reflection is denoised
- **THEN** the filter SHALL remain narrow, so the reflection is not smeared

#### Scenario: Shadow contact is preserved
- **WHEN** ray-traced shadows are denoised
- **THEN** contact hardening SHALL be preserved rather than filtered as a colour signal

### Requirement: Denoising quality and budget
Denoising quality SHALL be a lever available to the GI budget allocation: filter passes, filter
resolution, and history length SHALL be adjustable, with the trade-off between cost and residual
noise reported.

Denoising SHALL be disableable for reference comparison and validation, so the raw stochastic
signal can be inspected.

#### Scenario: Quality reduces under budget pressure
- **WHEN** the GI allocation is reduced
- **THEN** denoiser passes or resolution SHALL be reduced as one of the declared levers

#### Scenario: Raw signal is inspectable
- **WHEN** validating against a path-traced reference
- **THEN** denoising SHALL be disableable so the underlying signal is visible

### Requirement: Denoising diagnostics
The framework SHALL expose per signal: accumulated sample count, estimated variance, filter radius
applied, fraction of pixels with rejected history, and cost.

A vendor or machine-learning denoiser MAY be integrated as an optional backend behind the same
interface, and the engine's public interfaces SHALL NOT depend on vendor types.

#### Scenario: Residual noise is explained
- **WHEN** noise remains visible
- **THEN** the diagnostics SHALL show whether the cause is low sample count, rejected history, or
  a filter constrained by edge-stopping weights
