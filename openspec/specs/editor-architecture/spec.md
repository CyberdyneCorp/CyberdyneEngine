# editor-architecture Specification

## Purpose

Defines the editor: an application built on the same runtime as games, its plugin architecture,
the inspector, undo/redo, play mode, viewport tooling, and the project and build workflow.

The editor is deliberately **not** a separate codebase: it uses the engine's ECS, scene system,
UI system, and renderer, so every improvement benefits both and the runtime is exercised by a
demanding consumer.

## Requirements

### Requirement: Editor is an engine application
The editor SHALL be built on the engine runtime, using the same ECS world, scene graph, UI
system, and renderer, compiled only when `CY_EDITOR` is enabled.

The edited scene SHALL live in its own `World` instance, separate from the editor's own UI world,
so editing state and edited content cannot interfere.

#### Scenario: Export build excludes the editor
- **WHEN** a game is built for shipping
- **THEN** no editor code SHALL be linked

#### Scenario: Two worlds
- **WHEN** the editor runs
- **THEN** the editor UI SHALL live in one world and the edited scene in another, each with its
  own systems and schedule

### Requirement: Editor mode and tool execution
Systems and behaviours SHALL declare whether they run in the editor: `Runtime` (default),
`Editor` (editor only), or `Always`.

Editor-executing code SHALL be able to query that it is in the editor and whether play mode is
active.

#### Scenario: Tool behaviour previews in the editor
- **WHEN** a behaviour is marked to run in the editor
- **THEN** it SHALL execute while editing so its effect is visible without entering play mode

#### Scenario: Gameplay does not run while editing
- **WHEN** a normal gameplay system exists in the edited world
- **THEN** it SHALL NOT execute until play mode begins

### Requirement: Play mode
Entering play mode SHALL: snapshot the edited world, instantiate the runtime world from the
current scene state, and run the full simulation schedule.

Exiting SHALL restore the snapshot exactly, so play-mode changes never leak into the edited scene.

Play mode SHALL support: pause, step one frame, and step one simulation tick; and **play in
place** (in the editor process) or **play standalone** (a separate process, closer to shipping
behaviour, connected to the editor's debugger).

#### Scenario: Play-mode changes are discarded
- **WHEN** play mode ends after entities were created and modified
- **THEN** the edited scene SHALL be exactly as it was before entering

#### Scenario: Standalone play
- **WHEN** standalone play is selected
- **THEN** the game SHALL launch as a separate process with the debugger attached, so
  editor-specific state cannot mask bugs

### Requirement: Undo/redo
All editor mutations SHALL go through an undo/redo system recording reversible operations, with
per-context histories (one per open scene, plus a global history for project-level changes).

Operations SHALL be composable into named transactions, mergeable for continuous edits (dragging
a gizmo produces one undo step, not hundreds).

#### Scenario: Gizmo drag is one step
- **WHEN** a user drags a transform gizmo
- **THEN** the continuous updates SHALL merge into one undo step committed on release

#### Scenario: Per-scene history
- **WHEN** two scenes are open and the user undoes
- **THEN** only the focused scene's history SHALL be affected

#### Scenario: Plugin edits are undoable
- **WHEN** a plugin modifies the scene through the editor API
- **THEN** the change SHALL be recorded in the appropriate history automatically

### Requirement: Inspector
The inspector SHALL build property editors from the type registry: enumerate a selection's
components and their reflected fields, group by category, and render an editor per field chosen
by type and attributes.

It SHALL support: multi-selection editing with mixed-value indication, per-property revert to
default or to prefab value, prefab override indication, drag-and-drop asset assignment, inline
sub-object editing, array and map editors with reordering, and search and filtering.

**Custom property drawers** SHALL be registrable per type or per attribute, from native code and
from Swift.

#### Scenario: New component needs no editor code
- **WHEN** a reflected component is added to an entity
- **THEN** the inspector SHALL render editors for all its fields without any inspector-specific
  code

#### Scenario: Multi-selection
- **WHEN** several entities are selected and share a component
- **THEN** the inspector SHALL show that component with differing values marked, and editing SHALL
  apply to all selected

#### Scenario: Custom drawer
- **WHEN** a type registers a custom drawer
- **THEN** it SHALL be used in place of the default, including inside arrays and nested structs

### Requirement: Scene editing viewports
The editor SHALL provide 3D and 2D viewports with:

- **Navigation** — orbit, pan, zoom, fly-through with configurable speed, focus on selection
- **Selection** — click, box, and paint selection; hierarchical selection; selection filters by
  type or layer
- **Gizmos** — translate, rotate, scale, and a combined transform gizmo, with world and local
  space, pivot and centre modes, and snapping (grid, angle, and vertex)
- **Component gizmos** — per-component editing handles (light range, collider shape, camera
  frustum, spline points), registrable by modules and Swift
- **View modes** — the renderer debug views, plus wireframe overlays and per-type visibility
  toggles
- **Grid and guides** — configurable grid, rulers, and alignment guides in 2D

#### Scenario: Vertex snapping
- **WHEN** vertex snapping is active and an object is dragged near another's vertex
- **THEN** it SHALL snap so the vertices coincide

#### Scenario: Custom component gizmo
- **WHEN** a module registers a gizmo for its component
- **THEN** its handles SHALL be drawn and draggable, and the resulting change SHALL be undoable

### Requirement: Scene hierarchy and asset browser
The **hierarchy** panel SHALL show the edited scene's node tree with: search and filtering,
drag-and-drop reparenting, multi-selection, visibility and lock toggles, prefab instance and
override indication, and creation from templates.

The **asset browser** SHALL show the project's assets with: folder navigation, search and type
filtering, thumbnails generated asynchronously, drag-and-drop into scene and inspector, rename
and move with reference preservation, and import settings access.

#### Scenario: Asset moved
- **WHEN** an asset is moved in the browser
- **THEN** its `.meta` SHALL move with it and all references SHALL continue to resolve by
  `AssetId`

#### Scenario: Thumbnail generation
- **WHEN** a folder of models is displayed
- **THEN** thumbnails SHALL be rendered asynchronously without blocking the UI, and cached

### Requirement: Plugin architecture
The editor SHALL be extensible by plugins written natively or in Swift, able to:

- add panels, menus, toolbars, and keyboard shortcuts
- add inspector property drawers and component gizmos
- add importers and asset post-processors
- add scene view tools that receive viewport input
- add build steps and platform targets
- react to editor events (selection changed, scene opened or saved, play mode changed)
- read and write the edited scene through an API that records undo operations

Plugins SHALL be loadable per project, hot-reloadable, and SHALL declare their required editor API
version.

#### Scenario: Swift editor plugin
- **WHEN** a project includes a Swift editor plugin
- **THEN** it SHALL load with the game module, register its panels and tools, and hot-reload on
  rebuild

#### Scenario: Plugin API version
- **WHEN** a plugin requires a newer editor API than is present
- **THEN** it SHALL be reported as incompatible rather than loaded and failing at random

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

### Requirement: Project management and settings
The editor SHALL provide: project creation from templates, project settings organised by
category with search and per-platform overrides, layer and tag configuration, input action
mapping, quality and rendering settings, and package and dependency management for engine modules
and Swift packages.

Settings SHALL be stored in text form suitable for version control, with user-specific
preferences stored separately from project settings.

#### Scenario: Settings in version control
- **WHEN** a project setting changes
- **THEN** the diff SHALL show only that setting, and user preferences SHALL not appear in the
  project file

### Requirement: Build and deployment
The editor SHALL provide a build pipeline: select a platform and configuration, cook assets,
compile the game module, package, and optionally deploy to a device.

Build profiles SHALL be saveable, and builds SHALL be runnable from the command line with the same
configuration, so CI and local builds match.

#### Scenario: CI matches local
- **WHEN** CI runs a build with a saved profile
- **THEN** it SHALL produce the same output as the editor with that profile

#### Scenario: Deploy to device
- **WHEN** a mobile target is selected with a connected device
- **THEN** the build SHALL install and launch on it with the debugger attached

### Requirement: Debugging and profiling tools
The editor SHALL provide: a log panel with filtering and source navigation, a remote and local
debugger (breakpoints, stepping, variable inspection for Swift and native), a live entity
inspector for the running game, a frame profiler (CPU stages, jobs, GPU passes) with a timeline,
memory profiling by allocator tag and asset category, and a render debugger (draw calls, render
graph structure, resource lifetimes).

Live editing SHALL be supported: property changes in the editor applied to the running game.

#### Scenario: Frame spike investigation
- **WHEN** a frame exceeds its budget
- **THEN** the profiler timeline SHALL show which stage, system, or GPU pass consumed the time

#### Scenario: Live tweak
- **WHEN** a value is changed in the inspector while the game runs
- **THEN** it SHALL apply immediately in the running game, with a clear indication that the change
  is not persisted unless applied

### Requirement: Collaboration-friendly behaviour
The editor SHALL avoid patterns hostile to team work: text-based scene and asset formats with
stable ordering, no binary-only project state, no global files rewritten on every operation, and
per-user state kept out of shared files.

The editor SHALL detect external file changes and offer to reload rather than silently
overwriting.

#### Scenario: Two developers, two scenes
- **WHEN** two developers edit different scenes and both commit
- **THEN** their changes SHALL merge without conflict

#### Scenario: External change
- **WHEN** a file changes on disk while open in the editor
- **THEN** the editor SHALL detect it and prompt, rather than overwriting on next save
