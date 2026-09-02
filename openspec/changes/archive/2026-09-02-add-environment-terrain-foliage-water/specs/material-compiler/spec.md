## ADDED Requirements

### Requirement: Environment-aware material inputs
Materials SHALL be able to request **environmental inputs** semantically, alongside geometry
attributes: world position, slope, altitude, curvature, and any declared **environment field** —
biome, moisture, wetness, snow depth, water distance, water depth, flow, temperature, and burn
state (see `environment-fields`).

Field access SHALL go through the field substrate's bindless GPU path, so a material samples a
field without per-draw binding and without the material system knowing which system produced it.

The compiler SHALL record field usage as a dependency, so that a material's cost report states
which fields it samples and a missing field is a cook-time diagnostic rather than a runtime
default.

Field-driven material logic SHALL compose with authored data: a painted value SHALL be able to
override a field-driven rule locally.

#### Scenario: Slope becomes rock without a painted mask
- **WHEN** a terrain material declares that slopes above a threshold are rock
- **THEN** the compiler SHALL bind the necessary inputs and the rule SHALL evaluate without an
  authored mask

#### Scenario: Wetness is not a terrain concept
- **WHEN** a rock prop sitting at a river's edge samples wetness
- **THEN** it SHALL read the same field a terrain material reads, with no terrain-specific
  material path

#### Scenario: Missing field is a diagnostic
- **WHEN** a material samples a field that the project does not declare
- **THEN** cooking SHALL fail naming the field and the material, rather than silently substituting
  a default
