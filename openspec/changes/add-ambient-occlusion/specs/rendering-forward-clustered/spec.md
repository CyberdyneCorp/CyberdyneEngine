## ADDED Requirements

### Requirement: A screen-space stage may declare its own passes
A screen-space stage whose producer needs more than one pass SHALL be declared by that producer at
the stage's position in the pass order, reading the prepass outputs and writing the stage's target,
with every barrier between its passes derived by the render graph.

#### Scenario: A producer declares the ambient occlusion stage
- **WHEN** ambient occlusion is enabled and a producer is attached to its stage
- **THEN** the producer's passes SHALL be declared after the depth prepass and before the opaque pass
- **AND** a producer that refuses SHALL fail the frame's declaration rather than leave the stage out
