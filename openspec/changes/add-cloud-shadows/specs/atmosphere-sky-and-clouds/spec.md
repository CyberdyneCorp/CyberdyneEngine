# Spec Delta

## MODIFIED Requirements

### Requirement: Cloud shadows
Clouds SHALL cast shadows onto the world through a **coarse world-scale shadow representation** — a
low-frequency field or map covering a large area at low resolution — consumed by terrain, foliage,
water, and illumination.

Cloud shadows SHALL NOT be produced through the virtual shadow page system, whose design assumes
shadow detail correlates with screen pixels; a cloud shadow's footprint is kilometres wide and its
detail is low-frequency.

Cloud shadow resolution and update rate SHALL be budget levers.

The field SHALL attenuate **direct sunlight only**. Sky light and ambient terms already derive from
the same cloud state and SHALL NOT be attenuated by it a second time, and the sky itself is not a
surface the field applies to. Where the field reports full sun — outside every cloud's shadow, under
a clear sky, or when cloud shadows are disabled — a surface SHALL be shaded exactly as it would be
without cloud shadows.

The field SHALL be derived from the same cloud reconstruction, at the same time, that the sky draws
and lights with, so that a shadow on the ground belongs to a cloud in the sky and drifts with the
wind that moves that cloud.

#### Scenario: A cloud shadow crosses a valley
- **WHEN** clouds drift over terrain
- **THEN** a coarse shadow field SHALL darken the affected area, sampled by surfaces and by
  illumination

#### Scenario: The right mechanism
- **WHEN** cloud shadows are implemented
- **THEN** they SHALL use the coarse field rather than allocating virtual shadow pages

#### Scenario: Only the sun is shadowed
- **WHEN** a surface lit by the sun and by the sky's ambient term lies under a cloud's shadow
- **THEN** its direct sunlight SHALL be attenuated by the field and its ambient term and the sky
  above it SHALL be unchanged

#### Scenario: No cloud costs nothing
- **WHEN** cloud shadows are disabled, or the sky holds no cloud
- **THEN** the frame SHALL be identical to the frame drawn without cloud shadows

#### Scenario: The shadow drifts with the wind
- **WHEN** the field is produced at two times under a layer wind
- **THEN** the shadow SHALL have moved by the distance that wind carries the cloud in between

#### Scenario: The sky and the ground agree
- **WHEN** the sky is composed looking at the sun from a point on the ground
- **THEN** the cloud transmittance the sky draws there and the field's value there SHALL agree
