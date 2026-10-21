## ADDED Requirements

### Requirement: Temporal upscaling executes on the device
When a frame renders its scene below its output extent, a temporal upscaler SHALL reconstruct the
output from the jittered render-extent samples, the prepass motion vectors and a history kept at the
output extent, at the frame's temporal stage — step 5 of the chain — so that every stage before it
(shading, ambient occlusion, contact shadows, global illumination, volumetric fog, translucency) runs
at the render extent and every stage after it (depth of field, motion blur, bloom, exposure,
tonemapping, colour grading, the selection outlines and the interface) at the output extent. The
history SHALL be discarded where the surface it holds was hidden last frame, where most of this
frame's samples fall outside what the history around them holds, and where translucency changed the
pixel without a motion vector of its own; it SHALL be clipped toward this frame's neighbourhood as far
as this frame observed the pixel, except where a lock protects a thin feature. A contrast-adaptive
sharpening pass SHALL run on the reconstruction without feeding back into the history. Quality
(67 %), Balanced (58 %), Performance (50 %) and native antialiasing modes SHALL be offered, and the
stages after the upscaler SHALL read depth and motion resampled to the output extent.

#### Scenario: A still frame converges to the native one
- **WHEN** a still scene is rendered for forty frames at each mode's render extent
- **THEN** the reconstruction SHALL be within a stated mean error of the scene reconstructed at the
  output extent, and at least twice as close as the first frame's spatial reconstruction

#### Scenario: A moving object leaves no trail
- **WHEN** an object slides across a still background
- **THEN** the pixels it uncovered SHALL hold the background, against a control with rejection,
  the shading-change test and the clip turned off that holds the object

#### Scenario: A moving object keeps its detail by its motion vectors
- **WHEN** a finely textured object moves across the frame at a quarter of the output's pixels
- **THEN** its interior SHALL be closer to the supersampled object than the same reconstruction
  reprojecting by the camera alone, by a stated factor

#### Scenario: A thin feature is stable and keeps its coverage
- **WHEN** a line narrower than a render pixel is reconstructed over a whole jitter cycle
- **THEN** its pixels SHALL flicker less than a fifth as much as a reconstruction with no history,
  and the line SHALL keep at least half of its supersampled energy

#### Scenario: The interface is drawn at the output extent
- **WHEN** an interface is drawn over a frame reconstructed at any mode
- **THEN** every pixel it covers SHALL be byte-identical to the same interface over the frame
  rendered at the output extent

#### Scenario: The frame without an upscaler is the frame before it
- **WHEN** no temporal upscaler is attached and the render extent is the output extent
- **THEN** the frame SHALL be byte-identical to the frame from before temporal upscaling existed

#### Scenario: Resolution is the arbiter's lever
- **WHEN** the budget arbiter reduces its resolution allocation because the frame is over budget,
  and raises it when headroom returns
- **THEN** the render extent SHALL follow it within the configured bounds, and the history SHALL
  survive every move without being invalidated
