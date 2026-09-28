## MODIFIED Requirements

### Requirement: Depth of field
Depth of field SHALL be computed from a physically parameterised circle of confusion derived from
focal length, aperture, focus distance, and sensor size, with an artistic override.

The implementation SHALL use a gather-based approach with separate near and far fields, a
configurable bokeh shape (circular, hexagonal, or a custom aperture texture), and correct
occlusion handling so near-field blur bleeds over in-focus geometry.

#### Scenario: Near-field bleeding
- **WHEN** an out-of-focus object is in front of a focused one
- **THEN** its blur SHALL extend over the focused object, not be clipped to its silhouette

#### Scenario: Autofocus
- **WHEN** autofocus targets an entity
- **THEN** focus distance SHALL track it with a configurable speed

#### Scenario: The blur is the lens's
- **WHEN** a surface behind the focus plane is defocused
- **THEN** its blur radius in pixels SHALL be half the thin-lens circle of confusion, as a fraction
  of the sensor height, times the image height

#### Scenario: The far field stays behind
- **WHEN** a focused object is in front of a defocused background
- **THEN** the background's blur SHALL NOT spread onto the object, and the object's colour SHALL
  NOT spread into the background's blur

#### Scenario: In focus is unchanged
- **WHEN** a pixel's circle of confusion is smaller than a pixel and no near-field blur covers it
- **THEN** the stage SHALL write it unchanged, and a pinhole aperture SHALL leave the frame
  byte-identical to the frame without the stage
