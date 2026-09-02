## MODIFIED Requirements

### Requirement: Specialised editors
The editor SHALL provide dedicated editors for: materials (including the node graph),
animation graphs and clips (a timeline with curve editing), the VFX graph (see `vfx-system`),
terrain and tilemaps, UI layout, audio buses and mixing, navigation baking, lighting and lightmap
baking, and localisation tables.

Each SHALL be a plugin using the same panel and undo infrastructure as user plugins, so the
extension API is exercised by the engine's own tooling.

#### Scenario: The engine dogfoods its plugin API
- **WHEN** a built-in editor is implemented
- **THEN** it SHALL use only the public plugin API, so any limitation is discovered internally
  first

#### Scenario: Graph editors share infrastructure
- **WHEN** the material graph and the VFX graph editors are implemented
- **THEN** they SHALL share the node-graph canvas, undo, and inspector infrastructure rather than
  each implementing its own
