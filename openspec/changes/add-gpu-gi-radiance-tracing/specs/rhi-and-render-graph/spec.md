## ADDED Requirements

### Requirement: Acceleration structures are an RHI resource
The RHI SHALL let a renderer create bottom- and top-level acceleration structures, record their
builds and in-place updates, and bind a top level to a shader's ray query, without naming a graphics
API above the backends. A build and a trace SHALL declare their access like any other resource so
the render graph derives the barriers, and a backend whose device cannot trace SHALL refuse by name.

#### Scenario: A backend without ray tracing refuses
- **WHEN** an acceleration structure is requested from a device that does not report ray tracing
- **THEN** the request SHALL fail as unsupported, naming the capability

#### Scenario: A build is ordered before the trace that reads it
- **WHEN** one pass builds a structure and a later pass traces it
- **THEN** the render graph SHALL derive a barrier from the build stage to the tracing stage
