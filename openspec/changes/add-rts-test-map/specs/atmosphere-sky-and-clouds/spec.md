## ADDED Requirements

### Requirement: Sky irradiance is reused while its inputs hold
The ambient irradiance a frame takes from the sky SHALL be integrated again only when what it
depends on changes — the atmosphere's parameters, the eye's position in the planet's frame or the
sun's direction — and SHALL otherwise be the previous frame's value, exactly. A frame SHALL report
whether it reused the irradiance.

#### Scenario: A still sky is not integrated again
- **WHEN** two frames are assembled with the same atmosphere, the same eye height and the same sun
- **THEN** the second SHALL reuse the first's irradiance, bit for bit, and say so

#### Scenario: A moving input integrates
- **WHEN** the eye climbs or the sun moves between two frames
- **THEN** the second SHALL integrate the irradiance again, and its value SHALL be the one a fresh
  frame integrates
