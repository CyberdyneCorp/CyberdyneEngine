## MODIFIED Requirements

### Requirement: Data interfaces
Graphs SHALL access engine data through typed **data interfaces**: declared, versioned sources
exposing readable fields and the GPU resources they bind.

The engine SHALL provide at least: scene depth, scene normals, the scene signed distance field,
the GPU scene, physics collision queries, terrain height and material, static and skeletal mesh
sampling, texture and curve sampling, camera state, **environment fields** (see
`environment-fields`), audio spectrum and level, ECS query results, and a generic structured
buffer.

Wind SHALL be read from the shared **wind field** rather than a VFX-owned wind model, so that
particles, foliage, cloth, and water agree about the wind in the same frame. Force fields local to
an effect remain VFX-owned; the ambient wind does not.

Data interfaces SHALL be **extensible**: modules and projects SHALL be able to register their own
without modifying the compiler.

Each data interface SHALL declare its cost class and whether it is available on the CPU path, so
the compiler can reject or warn about use in effects that require CPU simulation.

#### Scenario: Sampling the scene SDF
- **WHEN** a graph samples the scene signed distance field to detect collision
- **THEN** the compiler SHALL bind the SDF resources and generate the sampling code, with no
  bespoke compiler support for that specific case

#### Scenario: Custom data interface
- **WHEN** a project registers a data interface exposing its own simulation grid
- **THEN** graphs SHALL be able to read it as a first-class typed source with no engine change

#### Scenario: Unavailable on the CPU path
- **WHEN** an effect declares CPU simulation and uses a GPU-only data interface
- **THEN** cooking SHALL fail with a diagnostic naming the interface

#### Scenario: One wind, several systems
- **WHEN** smoke drifts past trees in the same frame
- **THEN** both SHALL have sampled the same wind field, and SHALL move consistently
