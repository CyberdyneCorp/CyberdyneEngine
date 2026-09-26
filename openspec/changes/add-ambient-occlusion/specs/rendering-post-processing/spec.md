## ADDED Requirements

### Requirement: Ambient occlusion executes on the device
When the post chain enables ambient occlusion, the frame SHALL compute the term on the device from
the prepass depth and normals, filter it with the shared denoiser's visibility-domain configuration,
and multiply the ambient term of opaque shading by it before any later stage reads the colour.

#### Scenario: A contact is darkened
- **WHEN** ambient occlusion is enabled and a surface meets another at a contact
- **THEN** the pixels at the contact SHALL be darker than with ambient occlusion disabled

#### Scenario: An open surface is unchanged
- **WHEN** ambient occlusion is enabled and a flat surface has no occluder within the radius
- **THEN** its visibility SHALL be one and its pixels SHALL change by at most one 8-bit step

#### Scenario: Direct light is untouched
- **WHEN** ambient occlusion is enabled with default settings and a pixel receives no ambient light
- **THEN** the pixel SHALL be byte-identical to the frame with ambient occlusion disabled

#### Scenario: Disabled means absent
- **WHEN** ambient occlusion is disabled
- **THEN** no ambient occlusion pass SHALL be declared and the frame SHALL be byte-identical to a
  frame with no ambient occlusion producer attached, which SHALL match the frame the shader drew
  before ambient occlusion existed

#### Scenario: A still view is still
- **WHEN** a still view is rendered on consecutive frames with ambient occlusion enabled
- **THEN** the term SHALL change between frames by no more than 1e-3

#### Scenario: The temporal resolve shows no crawl
- **WHEN** a still view is rendered on consecutive frames with temporal anti-aliasing jittering the
  depth, once with ambient occlusion enabled and once without
- **THEN** over the occluded pixels the frame without the stage leaves unchanged, the resolved colour
  with it SHALL change between frames by no more than 0.035 of an 8-bit step per channel on
  average, and the term by no more than 0.045
