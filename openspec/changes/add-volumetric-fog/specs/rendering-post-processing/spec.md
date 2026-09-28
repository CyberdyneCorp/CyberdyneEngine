## ADDED Requirements

### Requirement: Volumetric fog executes on the device
When volumetric fog is enabled, the frame SHALL fill a froxel volume, built with the engine's froxel
depth distribution for the frame's own camera, by marching each froxel column front to back through
the participating medium: a height fog stated as a meteorological visibility and a scale height, and
authored box, sphere, cylinder and cone volumes, each with an extinction, an albedo, an emission and
a Henyey-Greenstein anisotropy. At fixed sub-steps of every slice the march SHALL sample the medium
and the directional shadow map, SHALL light the medium with the same sun and the same ambient
radiance the frame's surfaces are lit with, and SHALL integrate each sub-step analytically. Every
opaque surface SHALL then be multiplied by the volume's transmittance between it and the eye and
SHALL receive its in-scattered light.

Where the frame applies aerial perspective from the atmosphere's froxel table, the fog SHALL be
marched with the air that table describes, per channel, and written into a table of the same layout,
so that one lookup applies both media.

#### Scenario: Light shafts between occluders
- **WHEN** a homogeneous fog lit by the sun alone surrounds an occluder whose shadow crosses open air
- **THEN** froxels the geometry puts in the occluder's shadow SHALL scatter less than a tenth of the
  sunlight a fully lit froxel of the same medium scatters, and froxels in the open SHALL scatter more
  than nine tenths of it

#### Scenario: The volume is the single-scattering integral
- **WHEN** a medium that varies along every ray is lit by the sun and the sky without shadow
- **THEN** each froxel's transmittance and in-scattering SHALL match the single-scattering equation
  integrated by brute force over the same ray within one per cent

#### Scenario: An empty medium changes nothing
- **WHEN** volumetric fog is enabled over a medium with no extinction and no emission
- **THEN** the volume SHALL hold a transmittance of exactly one and an in-scattering of exactly zero,
  and the frame SHALL be byte-identical to the frame with fog disabled

#### Scenario: Disabled is the frame before
- **WHEN** volumetric fog is disabled
- **THEN** no fog pass SHALL be declared, and the frame SHALL be byte-identical to the frame the
  shader drew before volumetric fog existed

#### Scenario: Fog and air are one integral
- **WHEN** the fog is marched with the atmosphere's froxel table
- **THEN** an empty fog SHALL reproduce the table, a switched-off table SHALL leave the fog exactly as
  it was, and together the two SHALL transmit the product of what each transmits
