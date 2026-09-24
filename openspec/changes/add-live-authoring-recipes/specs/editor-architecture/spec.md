## ADDED Requirements

### Requirement: A new project is authorable
A project created from any built-in template SHALL carry the engine's component type manifest, so
that an entity created in its world has the engine's `Transform` and can be moved by the gizmo and
by transform commands.

A world file that declares no component types SHALL be opened with the project's type manifest,
the same as a world that does not exist yet.

#### Scenario: A primitive in an empty project can be moved
- **WHEN** a project is created from the empty template and a primitive is created in its world
- **THEN** the primitive SHALL have a `Transform`, and saving SHALL write that type into the world

#### Scenario: Creating over a project is refused
- **WHEN** a project is created in a directory that already holds a project manifest
- **THEN** creation SHALL be refused by name and the existing manifest SHALL be unchanged
