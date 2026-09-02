## MODIFIED Requirements

### Requirement: Pipelines are pluggable
The concrete rendering pipeline SHALL be a replaceable component implementing a
`RenderPipeline` interface that receives the snapshot and view list and populates the render
graph.

The engine SHALL ship: **Forward+** (clustered, the default desktop pipeline), **Visibility
Buffer** (deferred material evaluation, the pipeline in which virtual geometry realises its full
benefit — see `virtual-geometry`), **Mobile** (reduced feature set, tile-friendly), and **Null**.
Projects SHALL be able to supply their own.

Pipelines SHALL differ in their strengths, and the documentation SHALL state them rather than
presenting one as strictly better: Forward+ handles transparency, MSAA, and varied shading models
directly; the visibility buffer handles very high geometric density and many materials, at the cost
of a more constrained transparency and MSAA story.

Content SHALL work under any shipped pipeline; a pipeline choice SHALL affect performance
characteristics and available features, not whether a scene renders.

#### Scenario: Pipeline selection
- **WHEN** the configuration selects a pipeline and the device supports it
- **THEN** it SHALL be used; otherwise the engine SHALL fall back with a diagnostic

#### Scenario: Custom pipeline
- **WHEN** a project registers a custom pipeline
- **THEN** it SHALL receive the same snapshot and graph builder as the built-in ones, with no
  engine modification

#### Scenario: Content is portable across pipelines
- **WHEN** a scene authored under Forward+ is rendered under the visibility buffer pipeline
- **THEN** it SHALL render correctly, with documented differences in transparency and
  anti-aliasing behaviour rather than missing content
