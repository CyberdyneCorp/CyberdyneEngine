# material-compiler Spec Delta

## ADDED Requirements

### Requirement: Materials author vertex expressions as nodes
The material editor SHALL provide a vertex stage with graph outputs for world-position offset, custom interpolated attributes, and displacement. Its catalogue SHALL provide time, object/world position, vertex colour, UVs, normals, noise, wind field sampling, and applicable surface-stage math nodes. Authors SHALL NOT need handwritten Slang for these effects.

#### Scenario: Sine sway
- **WHEN** a material uses time and vertex position to produce a sine world-position offset
- **THEN** the preview mesh and scene SHALL render the displaced geometry

### Requirement: Vertex results remain correct across passes and geometry sources
The compiler SHALL report the variants produced for each geometry source under vertex-stage rendering. The editor and cook SHALL refuse a vertex graph on an unsupported path with a named reason. Shadows and motion vectors SHALL use the same displacement as visible geometry, including previous-frame evaluation for motion.

#### Scenario: Unsupported geometry path
- **WHEN** a vertex graph targets virtual geometry without supported offset evaluation
- **THEN** editor validation and cook SHALL refuse it with the same named reason

#### Scenario: Displaced shadow and motion
- **WHEN** a material offsets moving geometry
- **THEN** visible geometry, its shadow, and motion vectors SHALL match a CPU-displaced reference
