# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: The lighting editor bakes the world as it was authored
The lighting and lightmap baking editor SHALL place and edit irradiance volumes, set each light's
bake mobility (static, stationary or movable) and each placed object's lightmap resolution and
whether it receives a lightmap, each as one undoable transaction through a registered command that
is also an agent tool. A bake requested without a named description SHALL first write the level's
description from the open world — every enabled light with its mobility, every placed mesh with its
world transform, the material it draws with and its resolution, and every irradiance volume, each
named by the identity the runtime world knows it by — and SHALL then bake that description with the
engine's one bake. The description SHALL be regenerated on every request and rewritten only when
its content changes, and a bake of an unchanged world SHALL NOT bake again.

#### Scenario: A light's mobility, as authored, is what the engine bakes
- **WHEN** an author sets a light to movable and bakes the world
- **THEN** the description SHALL name that light movable by its identity, the engine SHALL bake
  nothing of it, and undo SHALL restore the previous mobility in the next description

#### Scenario: An unchanged world is not baked again
- **WHEN** the same world is baked twice with no change between
- **THEN** the description SHALL not be rewritten and the second bake SHALL report that nothing was
  baked, leaving the cooked lightmap as it was

#### Scenario: The editor and the engine agree on the description
- **WHEN** the editor writes the description of a world with lights, meshes and a volume
- **THEN** the engine SHALL read back every light's identity and mobility, every instance's
  identity, resolution and whether it receives, and the volume's identity and grid
