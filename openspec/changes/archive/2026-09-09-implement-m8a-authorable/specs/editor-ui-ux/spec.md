## ADDED Requirements

### Requirement: Primitive creation
The editor SHALL create primitive shapes — at minimum a **box, sphere, cylinder, plane and capsule** —
as entities in the open world, with editable parameters and without an asset having been imported.

Creation SHALL go through the same transaction path as every other mutation: one transaction per
primitive, undo removes it, and redo restores it with the same identity.

A created primitive SHALL be a normal entity. It SHALL NOT be a special node type, a preview, or an
object the rest of the editor treats differently — it is a mesh instance whose mesh the engine
generated rather than imported, and every later system SHALL be unable to tell the difference.

#### Scenario: A primitive is an edit like any other
- **WHEN** a sphere is created and then undone
- **THEN** the world SHALL be exactly as it was, and redo SHALL restore the sphere with the same
  entity identity

#### Scenario: Nothing downstream knows it was generated
- **WHEN** a generated primitive and an imported mesh of the same shape are both in a world
- **THEN** selection, the inspector, the gizmo, saving, cooking and physics SHALL treat them
  identically
