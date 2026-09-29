# editor-viewport-and-gizmos Spec Delta

## ADDED Requirements

### Requirement: Physics debug layers are requested from the engine
The editor SHALL let each viewport request the engine's physics debug layers — colliders, contacts,
joints, sleep state, velocities, centres of mass and broad-phase bounds — by the engine's own flag
bits, carried with the viewport's other intent. The engine SHALL draw the requested layers from its
physics world into the frame it presents, and SHALL draw none that were not requested. Each layer
SHALL be a command that states what it shows and how to read it, and toggling one SHALL change no
document.

#### Scenario: A requested layer is drawn where the body is simulated
- **WHEN** a viewport requests the collider layer while a world plays
- **THEN** the engine SHALL draw each collider at its simulated placement through the frame's own view

#### Scenario: An unrequested layer is not drawn
- **WHEN** a viewport requests no physics layer
- **THEN** the engine SHALL ask its physics world for nothing and draw nothing

### Requirement: The selected joint is drawn by the engine
While authoring, the engine SHALL draw the selected entity's authored joint in the viewport — both
anchors, the line to the joined body, the joint axis and its limits — with the same drawing it uses
for a simulated constraint, placed where the two bodies are authored.

#### Scenario: A hinge is drawn at its anchor with its axis
- **WHEN** an entity with an authored hinge is selected
- **THEN** the engine SHALL draw its anchor in the body's unscaled frame and its axis from that anchor
