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
  frame with no ambient occlusion producer attached

#### Scenario: A still view is still
- **WHEN** a still view is rendered on consecutive frames with ambient occlusion enabled
- **THEN** the term SHALL change between frames by no more than 1e-3

### Requirement: The ambient occlusion filter is the shared denoiser
The device filter of the ambient occlusion term SHALL be configured from the shared denoiser's
ambient occlusion signal configuration and quality ladder, and SHALL agree with the denoiser run on
the same raw term within the precision of its storage.

#### Scenario: The device filter matches the denoiser
- **WHEN** the raw term of a frame is read back and denoised by the shared denoiser as ambient
  occlusion
- **THEN** the device's filtered term SHALL match it to within 4e-3
