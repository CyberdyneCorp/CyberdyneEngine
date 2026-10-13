## MODIFIED Requirements

### Requirement: Emissive surfaces as illumination
Emissive materials SHALL contribute to indirect illumination through the surface cache, so an
emissive surface lights its surroundings without an explicitly placed light.

Emissive surfaces SHALL be **classified by radiant power and area**:

- below a configured threshold: contributing through the cache only, and excluded from importance
  sampling
- above it: **promoted to a light**, sampled deterministically by the direct lighting path

Small, extremely bright emissive surfaces SHALL NOT be left to stochastic sampling, since that is
the primary source of fireflies. Energy SHALL be redistributed by classification rather than
suppressed by clamping.

A promoted emitter's emission SHALL leave the cache, so its energy is counted once, and the direct
lighting path SHALL integrate it over its area rather than as a point. The engine SHALL report where
every emitter's radiant power went — cache, light, or demoted back to the cache by a light budget.

#### Scenario: Neon sign lights the room
- **WHEN** an emissive sign is present with no light placed
- **THEN** nearby surfaces SHALL receive indirect illumination from it

#### Scenario: Tiny bright emitter is promoted
- **WHEN** a small surface has very high emissive power
- **THEN** it SHALL be promoted to a light and sampled deterministically, rather than producing
  sparse bright samples

#### Scenario: Energy is not silently clamped
- **WHEN** a bright emitter is handled
- **THEN** the mechanism SHALL be classification and importance sampling, and any clamping applied
  SHALL be reported as energy loss

#### Scenario: An emitter lights a room within a stated error of the path tracer
- **WHEN** a glowing panel is the only source in a closed room
- **THEN** the irradiance the surface cache holds at the room's surfaces SHALL agree with the offline
  path tracer over the same cards within a stated error

#### Scenario: A promoted emitter produces no fireflies
- **WHEN** a tiny bright emitter lights a floor, once promoted and once left to the cache
- **THEN** the promoted emitter's per-receiver ratio to the path tracer SHALL stay within a stated
  variance bound, and the cache-only one SHALL exceed it

### Requirement: GI volumes
Artists SHALL be able to place **GI volumes** overriding, within their bounds: quality level,
probe density, update rate, indirect diffuse and specular intensity, tier preference, and the
far-field transition distance.

Volumes SHALL have priority and blend distance, resolving overlap by the same rules as
post-process volumes.

Volumes SHALL be an override, not a requirement: a scene SHALL be correctly lit with none placed.

Resolution SHALL be per parameter and per sample: a volume contributes only the parameters it sets,
fading in over its blend distance, with a higher priority dominating where volumes overlap.

#### Scenario: Important interior
- **WHEN** a designer marks an interior as high quality
- **THEN** probe density and update rate SHALL increase there within the GI allocation

#### Scenario: No volumes needed
- **WHEN** a scene contains no GI volumes
- **THEN** it SHALL be lit correctly using global settings

#### Scenario: Overlapping volumes resolve by priority
- **WHEN** two volumes setting one parameter overlap
- **THEN** the higher priority SHALL dominate whatever order they were placed in, and SHALL blend
  over the lower one across its blend distance

#### Scenario: A volume does not reach past its blend distance
- **WHEN** a sample lies further from every volume than its blend distance
- **THEN** it SHALL resolve exactly as a scene with no volume placed
