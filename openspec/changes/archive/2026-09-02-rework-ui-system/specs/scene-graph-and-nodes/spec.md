## MODIFIED Requirements

### Requirement: Node types and components
A node "type" SHALL be defined by a **component archetype template** plus an optional behaviour,
not by C++ inheritance. Creating a `MeshRenderer` node means creating an entity with
`LocalTransform`, `WorldTransform`, `MeshRenderer`, and `Visibility`.

The engine SHALL ship node templates for: spatial grouping, mesh rendering, skinned mesh,
instanced mesh, camera, lights (directional, point, spot, area), reflection probe, decal,
VFX effect, audio listener, audio emitter, rigid body, static body, character controller,
collider shapes, navigation region and agent, **UI host** (a node presenting a UI document in
screen, world, or surface space), and 2D sprite and tilemap.

Individual UI elements SHALL NOT be node templates: they live in the UI system's own storage (see
`ui-system`), and a UI host node is the scene's single point of attachment to a UI document.

Users SHALL be able to define their own templates without engine changes.

#### Scenario: Composition over inheritance
- **WHEN** a designer wants a light that also emits sound
- **THEN** they SHALL add both components to one entity, with no new class

#### Scenario: Template is data
- **WHEN** a project defines a custom node template
- **THEN** it SHALL be a data declaration listing components and defaults, registered at startup

#### Scenario: UI attaches at one point
- **WHEN** a world-space health bar is attached to a unit
- **THEN** the unit SHALL have a UI host node referencing a UI document, and the document's
  elements SHALL NOT appear as entities in the scene
