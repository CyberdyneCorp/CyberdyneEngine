# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Navigation baking editor
The editor SHALL open `Domain::NavigationBaking` as a form editor. From it, an author SHALL be able to:
- select a navigation world;
- set its agent profile: radius, height, max slope and step height;
- set its build settings: cell size, tile size, and the Recast or engine-rasteriser back end;
- start a bake that shows per-tile progress and the engine's diagnostics;
- toggle the world's overlay;
- add NavMeshSurface, NavObstacle, NavArea and NavLink components;
- run a test path.

The engine SHALL perform the bake. The editor SHALL NOT voxelise, build tiles or hash geometry.

The panel SHALL expose accessible names for all of its controls. When no world is open, it SHALL show an explained empty state.

#### Scenario: Bake from the panel
- **WHEN** an author opens the Navigation panel on a world with a surface and presses Bake
- **THEN** the editor SHALL request the bake from the engine and show its progress
- **AND** on completion it SHALL show the diagnostics and the bake identity

#### Scenario: Accessible empty state
- **WHEN** no world is open
- **THEN** the panel SHALL state that no world is open and SHALL remain reachable through accessibility

### Requirement: Navigation commands have undo and MCP parity
Every navigation authoring action SHALL be a command in the shared command registry: settings, overlay toggles, bake, and component add, edit and remove. Each command SHALL record exactly one document transaction per gesture. An asynchronous bake SHALL record its transaction when the engine reports completion. The recorded bake identity SHALL let undo restore the previous saved bake and its overlay. The same commands SHALL be the MCP tools. Status, path, flow-field and point-pick queries SHALL be read-only commands.

#### Scenario: Agent and human agree
- **WHEN** an MCP client performs a settings edit, an obstacle add and a bake, then undoes and redoes each one
- **THEN** the document and the history SHALL match the result of the same actions performed from the panel
- **AND** undo SHALL restore the prior settings, components and bake identity

#### Scenario: Undoing the first bake unbakes the engine
- **WHEN** the first bake of a world is undone
- **THEN** the engine SHALL drop the world's mesh, and path, flow-field and pick queries SHALL be refused as unbaked until the bake is redone

#### Scenario: Failed bake records nothing
- **WHEN** the engine reports a failed bake
- **THEN** no transaction SHALL be recorded, and `navigation.bake.status` SHALL report the failure and its diagnostics

### Requirement: Stale bake is shown to the author
The editor SHALL display the engine-reported stale state of each navigation world's saved bake in the Navigation panel and through `navigation.bake.status`.

#### Scenario: Stale after a geometry edit
- **WHEN** a mesh inside a baked surface is moved
- **THEN** the panel and `navigation.bake.status` SHALL report the bake as stale until it is rebaked or the edit is undone
