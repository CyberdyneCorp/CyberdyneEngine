## MODIFIED Requirements

### Requirement: Sky and atmosphere
Sky and atmospheric models are defined in `atmosphere-sky-and-clouds`: the physically based
participating atmosphere, its precomputed tables, celestial bodies and time of day, cloud
representation and rendering, and aerial perspective.

This capability defines what illumination **consumes** from them: a filtered radiance map for
specular image-based lighting, irradiance for ambient diffuse, sun and celestial transmittance, cloud
shadowing, and aerial perspective applied consistently with volumetric media.

Sky-derived illumination SHALL update when the sky changes, **incrementally where it changes
continuously** — a moving sun, thickening cloud cover — and under the illumination budget rather than
triggering unbounded recomputation.

A change in atmospheric or cloud state SHALL invalidate only the illumination that depends on it:
sky-lit surfaces and the affected regions of the radiance and surface caches.

Volumetric clouds SHALL contribute shadowing through the coarse cloud shadow field, not by
participating in shadow page allocation.

#### Scenario: Time of day
- **WHEN** the sun rotates continuously
- **THEN** the atmosphere's tables SHALL update incrementally and the radiance and irradiance
  illumination consumes SHALL follow without a visible step

#### Scenario: Aerial perspective
- **WHEN** the physical atmosphere is enabled
- **THEN** distant geometry SHALL receive scattering consistent with the sky, not a separately tuned
  fog

#### Scenario: Cloud cover changes the light
- **WHEN** cloud coverage thickens
- **THEN** sky irradiance SHALL fall, and the illumination affected SHALL be updated incrementally
  within budget
