## MODIFIED Requirements

### Requirement: Ray-traced effects
Where the device supports ray tracing, the engine SHALL optionally use it for: reflections
(replacing or complementing SSR beyond the screen), soft shadows, ambient occlusion, and dynamic
GI probe tracing.

Ray-traced features SHALL consume the **ray tracing infrastructure** (see
`ray-tracing-infrastructure`) for acceleration structures, geometry adapters, and ray queries.
They SHALL NOT build, refit, or own acceleration structures themselves.

Ray-traced features SHALL be capability-gated and each SHALL have a non-ray-traced fallback, so
no content depends on their presence. Traced ray counts SHALL be a budget allocation held by the
renderer budget arbiter.

#### Scenario: Ray-traced reflection beyond the screen
- **WHEN** ray tracing is available and SSR confidence is low
- **THEN** a traced ray SHALL supply the reflection instead of falling back to a probe

#### Scenario: Acceleration structure maintenance
- **WHEN** dynamic geometry moves
- **THEN** the ray tracing infrastructure SHALL refit its bottom-level structures and rebuild the
  top-level structure within its declared budget, and GI SHALL not manage that itself

#### Scenario: Virtual geometry is traced as a proxy
- **WHEN** a traced ray hits virtual geometry
- **THEN** it SHALL intersect the proxy representation, and the documented difference from the
  rasterised surface SHALL apply
