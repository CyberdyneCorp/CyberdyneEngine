# ray-tracing-infrastructure Specification

## Purpose

Defines the renderer service that owns acceleration structures: bottom-level lifecycle and refit
policy, the top-level structure, the ray query interface, its own build budget, and a **geometry
adapter per geometry source**.

The adapters are the part that is genuinely engine-specific. A streaming cluster hierarchy cannot
feed an acceleration structure, so virtual geometry supplies a proxy at a declared detail level and
the resulting difference from the rasterised surface is documented rather than denied. Skinned
meshes refit from the GPU pose world. Procedural geometry supplies bounds and an intersection
shader.

This is specified ahead of the systems that consume it — reflections, shadows, global illumination
— so that they consume an interface rather than each inventing one. Ray tracing is capability
gated throughout, and disabling it on capable hardware is required to behave exactly like absent
hardware, so the fallback paths stay exercised.

## Requirements

### Requirement: Ray tracing infrastructure is a renderer service
Acceleration structure management SHALL be a renderer-level service, distinct from the RHI
primitives beneath it and from the lighting and global illumination features above it.

It SHALL own: bottom-level structure lifecycle and refit policy, the top-level structure, geometry
adapters per geometry source, the ray query interface, and its own budget.

Consumers — reflections, shadows, ambient occlusion, global illumination probe tracing, and
gameplay queries — SHALL use this service rather than managing acceleration structures
themselves.

#### Scenario: Consumers do not manage structures
- **WHEN** a reflection feature traces a ray
- **THEN** it SHALL use the ray query interface, and SHALL NOT build, refit, or own any
  acceleration structure

#### Scenario: One structure serves many consumers
- **WHEN** reflections, shadows, and GI all trace in one frame
- **THEN** they SHALL share one top-level structure

### Requirement: Geometry adapters
Every geometry source SHALL supply a **ray tracing adapter** producing the representation the
acceleration structure requires:

| Source | Adapter behaviour |
|---|---|
| Static mesh | A bottom-level structure built from a chosen LOD |
| Skinned mesh | A structure refit each frame from the GPU pose world |
| Virtual geometry | A **proxy representation** at a declared detail level, since a streaming cluster hierarchy cannot feed a structure directly |
| Terrain | A structure built from the collision or proxy heightfield representation |
| Mesh particles | Instanced references to the source mesh's structure |
| Procedural | Bounds plus an intersection shader |

The proxy detail level for virtual geometry SHALL be a cooker and runtime policy with a stated
memory and accuracy trade-off, because ray traced results will not match the rasterised surface
exactly.

#### Scenario: Virtual geometry traces against a proxy
- **WHEN** virtual geometry is ray traced
- **THEN** a proxy representation SHALL be used, and the difference from the rasterised surface
  SHALL be documented rather than presented as exact

#### Scenario: Skinned geometry is refit, not rebuilt
- **WHEN** a skinned mesh animates
- **THEN** its structure SHALL be refit from the current pose, with a full rebuild only when
  refit quality degrades beyond a threshold

### Requirement: Structure lifecycle and budget
Bottom-level structures SHALL be built asynchronously, cached, shared between instances of the
same geometry, and evicted when unreferenced.

Per frame, the service SHALL hold a **build and refit budget**: the number and cost of builds,
rebuilds, and refits SHALL be bounded, with work beyond the budget deferred and prioritised by
instance importance and screen coverage.

The top-level structure SHALL be rebuilt per frame from resident instances.

#### Scenario: Budget bounds a spike
- **WHEN** many new instances become resident in one frame
- **THEN** structure builds SHALL be spread across frames within the budget, and instances awaiting
  a structure SHALL be excluded from tracing rather than stalling the frame

#### Scenario: Shared structures
- **WHEN** a thousand instances reference one mesh
- **THEN** one bottom-level structure SHALL exist, referenced by a thousand top-level instances

### Requirement: Ray query interface
The service SHALL expose ray queries through both **inline** ray queries in existing shaders and,
where supported, **ray tracing pipelines** with separate hit and miss shaders.

Queries SHALL be able to request: closest hit, any hit, and a shadow query returning only
occlusion.

Hit results SHALL resolve to the same instance and material identifiers used by the GPU scene and
the GPU material table, so a hit can be shaded through the existing material path.

#### Scenario: A hit shades through the material system
- **WHEN** a ray hits a surface
- **THEN** it SHALL resolve to a GPU scene instance and a material table entry, and shading SHALL
  use the same material program as rasterisation

#### Scenario: Inline queries in a compute pass
- **WHEN** a screen-space effect needs an occlusion test beyond the screen
- **THEN** it SHALL issue an inline ray query without a separate ray tracing pipeline

### Requirement: Capability gating and fallback
Ray tracing SHALL be capability-gated. Every consumer SHALL have a non-ray-traced path, and no
content SHALL depend on ray tracing being present.

Where ray tracing is unavailable, the service SHALL report so and consumers SHALL select their
fallback without content changes.

#### Scenario: Device without ray tracing
- **WHEN** the device lacks ray tracing support
- **THEN** the service SHALL be inactive, no structures SHALL be built, and every consumer SHALL
  render its fallback

#### Scenario: Ray tracing disabled by profile
- **WHEN** a renderer profile disables ray tracing on capable hardware
- **THEN** behaviour SHALL match the unsupported case exactly, so the fallback path is exercised
  and testable

### Requirement: Ray tracing diagnostics
The service SHALL report: structure memory in use, builds, rebuilds and refits per frame against
budget, instances excluded for lack of a structure, top-level rebuild time, and traced ray counts
per consumer.

#### Scenario: Structure memory is attributable
- **WHEN** acceleration structure memory is high
- **THEN** the report SHALL attribute it to geometry sources and assets
