## ADDED Requirements

### Requirement: Soft directional shadows execute in the frame
When soft shadows are enabled, the frame's directional shadow SHALL be filtered by a blocker search
and a filter kernel whose radius is derived from the light's angular radius, the map's depth range
and its width, with no separate softness parameter.

#### Scenario: The penumbra widens with blocker distance
- **WHEN** two equal blockers cast shadows onto one floor, one resting on it and one hanging above it
- **THEN** the hanging blocker's shadow SHALL have a wider penumbra than the resting blocker's, in
  proportion to the distance between blocker and receiver

#### Scenario: No light leaks and no shadow speckles
- **WHEN** soft shadows are enabled
- **THEN** a receiver the light reaches past every blocker by more than the widest kernel SHALL be
  exactly lit, and a receiver deep in an umbra SHALL be exactly unlit

#### Scenario: Disabled is the frame before
- **WHEN** soft and contact shadows are disabled
- **THEN** the frame SHALL be byte-identical to the frame the shader drew before they existed

### Requirement: Contact shadows execute on the device
When contact shadows are enabled, the frame SHALL trace each selected pixel toward the directional
light through the prepass depth and SHALL darken the light's visibility where the trace finds an
occluder, as an addition to the shadow map's visibility and never a replacement for it.

#### Scenario: A coarse map's contact is supplied
- **WHEN** an object rests on a floor and the shadow map's texels are too coarse for the contact
- **THEN** floor pixels the geometry puts in contact shadow and the map leaves lit SHALL be darker
  with contact shadows enabled

#### Scenario: Nowhere else
- **WHEN** contact shadows are enabled
- **THEN** no pixel SHALL change whose trace found no occluder, and no pixel SHALL be darkened by the
  trace unless an object lies within the trace's reach toward the light

#### Scenario: An open plane is untouched
- **WHEN** a floor with nothing on it is traced at any light elevation
- **THEN** every pixel's contact visibility SHALL be exactly one
