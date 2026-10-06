# rendering-geometry-and-resources Spec Delta

## ADDED Requirements

### Requirement: Skinned meshes in the forward frame
The engine's forward frame SHALL draw skinned meshes from the skinning compute pass's output, in the
depth prepass, the directional shadow pass, the opaque and transparent passes and the selection mask,
without re-skinning per pass and without a pipeline of the caller's own.

The output's normal-tangent stream SHALL be bound in the encoding the skinning pass writes, the
engine's cooked `PackedNormalTangent`, through a vertex format the RHI declares for it on every
backend, rather than through a second encoding written for one consumer.

A scene's skinned instances SHALL be skinned by one compute pass per frame, with a dispatch per
instance into its own double-buffered output window, from one device pose buffer.

The depth prepass SHALL derive a skinned draw's per-object motion from the previous frame's skinned
positions, and SHALL treat a draw with no previous skinned frame as having its current positions as
its previous ones.

A frame with no skinned draw SHALL be the frame from before skinned draws existed, byte for byte.

#### Scenario: Skinned once, drawn three times
- **WHEN** a skinned mesh appears in the depth prepass, the shadow pass and the opaque pass
- **THEN** one dispatch SHALL skin it and all three passes SHALL draw its one output buffer

#### Scenario: The frame without skinned draws
- **WHEN** the frame is created with its skinned pipelines and draws no skinned instance
- **THEN** its output SHALL equal, byte for byte, the frame drawn before the skinned pipelines
  existed

#### Scenario: Motion of a moving limb
- **WHEN** a forearm bound to one bone turns between two frames while the upper arm, bound to
  another, does not
- **THEN** the forearm's pixels SHALL carry motion vectors and the upper arm's SHALL be exactly zero,
  and a limb that does not move between two frames SHALL have none

#### Scenario: One pass for several instances
- **WHEN** three skinned instances are drawn in one frame
- **THEN** one compute pass SHALL record three dispatches, one per instance

#### Scenario: A selected skinned mesh
- **WHEN** a skinned instance is marked for a selection outline
- **THEN** the selection mask SHALL cover it where the frame drew it, and SHALL move when it bends
