# Spec Delta

## ADDED Requirements

### Requirement: Scene creation actions
The Scene menu, hierarchy panel, and command interface SHALL provide actions to create a Plane, directional light, point light, spot light, and camera in the active world. A created entity SHALL be selected, transformable, undoable, and persisted by the normal scene save operation.

#### Scenario: Create and reload a Plane
- **WHEN** an author creates a Plane, saves, and reopens the world
- **THEN** the Plane retains its mesh reference and transform

#### Scenario: Create and reload a light
- **WHEN** an author creates a light, changes its transform and intensity, saves, and reopens the world
- **THEN** the same type, transform, intensity, and enabled state are restored
