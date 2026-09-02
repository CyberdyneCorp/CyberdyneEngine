# temporal-rendering Specification

## Purpose

Defines **CyberTemporal**: the single framework that owns everything accumulating across frames —
jitter, motion vectors, history buffers, reprojection, camera-cut detection, disocclusion
classification, and history invalidation.

It exists because the alternative is worse. Temporal anti-aliasing, temporal upscaling,
screen-space reflections, screen-space GI, ambient occlusion, volumetrics, and shadow caching all
need the same six things. Specified per effect, they get implemented per effect, disagree about
conventions, and each carry their own history-invalidation bug — and a missed camera cut is the
most visible artefact in the whole category.

Motion vectors are the illustrative case: the GPU scene already stores current and previous
transforms, and the GPU pose world already stores current and previous poses. Motion vectors are
therefore derived from data that exists, not authored, so every moving surface — static, skinned,
virtual geometry, mesh particles, world-space UI — reprojects correctly with no per-system work.

## Requirements

### Requirement: One temporal framework
Temporal reprojection infrastructure SHALL be a single engine subsystem, not reimplemented per
effect. It SHALL own: the jitter sequence, motion vectors, history buffer allocation and
lifetime, reprojection, camera-cut detection, disocclusion classification, and history
invalidation.

Effects that accumulate across frames — temporal anti-aliasing, temporal upscaling, screen-space
reflections, screen-space global illumination, ambient occlusion, volumetric integration, and
shadow caching — SHALL consume this framework rather than implementing their own.

#### Scenario: A new temporal effect reuses the framework
- **WHEN** a new effect needs history
- **THEN** it SHALL request a history resource and reprojection from the framework, and SHALL NOT
  implement jitter, motion vectors, or invalidation of its own

#### Scenario: One convention, no disagreement
- **WHEN** two effects reproject
- **THEN** they SHALL use the same motion vector convention and the same jitter offset, so their
  results are mutually consistent

### Requirement: Jitter
The framework SHALL own the sub-pixel jitter applied to the projection matrix, using a low
discrepancy sequence (Halton by default) with a configurable length, and SHALL expose the current
and previous frame's jitter to every consumer.

Jitter SHALL be applied once, by the framework, and SHALL be disabled coherently when no consumer
requires it.

#### Scenario: Jitter is applied once
- **WHEN** both temporal anti-aliasing and temporal upscaling are active
- **THEN** one jitter offset SHALL be applied to the projection, and both SHALL read it

#### Scenario: No consumer, no jitter
- **WHEN** no active effect requires jitter
- **THEN** the projection SHALL be unjittered

### Requirement: Motion vectors are derived, not authored
The framework SHALL produce a motion vector buffer for every view that requires one, derived from
data the renderer already holds: current and previous instance transforms in the GPU scene,
current and previous poses in the GPU pose world, camera motion, and per-material vertex
animation where declared.

Motion vectors SHALL be produced for every moving surface — static, skinned, virtual geometry,
mesh particles, and world-space UI — without per-system effort.

Surfaces whose motion cannot be represented SHALL be marked so consumers can reject history for
them rather than smearing.

#### Scenario: Skinned motion is correct without extra work
- **WHEN** a skinned character moves
- **THEN** its motion vectors SHALL come from the current and previous poses already stored, and
  temporal effects SHALL reproject it correctly

#### Scenario: Unrepresentable motion rejects history
- **WHEN** a surface's motion cannot be expressed as a screen-space vector
- **THEN** it SHALL be marked, and consumers SHALL fall back to spatial reconstruction there

### Requirement: History resources
Consumers SHALL request **history resources** from the framework by declaring format, resolution
scale, and the number of frames retained.

The framework SHALL own their allocation, double buffering, resizing, and release, and SHALL
integrate them with the render graph so they participate in aliasing where their lifetime permits.

A history resource SHALL carry the view parameters it was produced with, so a consumer can detect
staleness rather than assuming validity.

#### Scenario: Resolution change is handled once
- **WHEN** output resolution changes
- **THEN** every history resource SHALL be reallocated and marked invalid by the framework, and no
  consumer SHALL need to detect the change itself

#### Scenario: History carries its provenance
- **WHEN** a consumer reads history
- **THEN** it SHALL be able to compare the history's view parameters against the current frame's

### Requirement: Invalidation events
The framework SHALL define and broadcast **history invalidation events**: camera cut, teleport,
projection change, resolution change, scene load or reload, and an explicit application-triggered
cut.

On an invalidation event, every history resource SHALL be marked invalid, and consumers SHALL
reconstruct spatially for that frame.

Invalidation SHALL be a framework responsibility rather than a per-effect one, because a missed
cut is the most common and most visible temporal artefact.

#### Scenario: Camera cut does not smear
- **WHEN** a cinematic cuts between camera positions
- **THEN** every temporal effect's history SHALL be invalidated in the same frame and none SHALL
  blend across the cut

#### Scenario: Gameplay teleport
- **WHEN** gameplay teleports the view
- **THEN** it SHALL be able to signal a cut explicitly, and all consumers SHALL respond

### Requirement: Reprojection and disocclusion
The framework SHALL provide reprojection from motion vectors, and SHALL classify each pixel's
history as valid, disoccluded, out of frame, or unrepresentable.

Consumers SHALL be able to read that classification rather than each deriving it, so that
disocclusion handling is consistent across effects.

#### Scenario: Revealed geometry is classified
- **WHEN** geometry is revealed from behind an occluder
- **THEN** those pixels SHALL be classified as disoccluded and consumers SHALL reconstruct
  spatially

#### Scenario: Consistent classification
- **WHEN** two effects sample the same pixel's history
- **THEN** they SHALL agree on whether it is valid

### Requirement: Temporal diagnostics
The framework SHALL expose: history memory in use per consumer, invalidation events and their
causes, the fraction of pixels classified into each history state, and a visualisation of motion
vectors and disocclusion.

#### Scenario: Ghosting is diagnosable
- **WHEN** an artefact appears
- **THEN** the developer SHALL be able to visualise motion vectors and history classification to
  determine whether the cause is reprojection, invalidation, or the consumer's accumulation

### Requirement: Determinism and capture
The framework SHALL support a **pinned mode** in which jitter follows a fixed sequence from a
fixed starting index and history is deterministic, so golden-image tests and capture produce
reproducible output.

#### Scenario: Reproducible golden image
- **WHEN** a golden-image test renders the same scene twice in pinned mode
- **THEN** the temporal state SHALL be identical and the images SHALL match
