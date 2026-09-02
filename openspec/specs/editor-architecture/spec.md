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

The editor SHALL maintain the world separation defined in `editor-documents-and-transactions`: its
own interface world, the **authoring** world holding the edited project, isolated **preview** worlds
for asset editors, and the **runtime** world that play mode instantiates from authoring data.

Authoring-only data SHALL NOT reach a runtime world, and because a compilation step separates them
this SHALL be checkable rather than conventional.

#### Scenario: Export build excludes the editor
- **WHEN** a game is built for shipping
- **THEN** no editor code SHALL be linked

#### Scenario: Two worlds
- **WHEN** the editor runs
- **THEN** the editor UI SHALL live in one world and the edited content in another, each with its
  own systems and schedule

#### Scenario: Previews do not touch the project
- **WHEN** an asset editor previews content
- **THEN** it SHALL use an isolated preview world, and the edited project SHALL be unaffected

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
Entering play mode SHALL instantiate a runtime world from the current authoring state and run the
full simulation schedule. Leaving play SHALL restore the authoring state exactly, so play-mode
changes never leak into the edited project.

The three play modes — **in-editor**, **separate process**, and **remote device** — and the live
bridge that drives all of them are defined in `live-editing`. This capability requires that the
editor expose them and that they behave identically from the user's point of view.

Play mode SHALL support pause, step one frame, and step one simulation tick.

Values changed while playing SHALL be runtime tweaks rather than authoring edits, and a **keep
changes** flow SHALL offer eligible differences for promotion into the project (see `live-editing`).

#### Scenario: Play-mode changes are discarded
- **WHEN** play mode ends after entities were created and modified
- **THEN** the edited content SHALL be exactly as it was before entering, except where the user
  explicitly kept a change

#### Scenario: Standalone play
- **WHEN** standalone play is selected
- **THEN** the game SHALL launch as a separate process with the debugger attached, so
  editor-specific state cannot mask bugs

#### Scenario: Remote play is the same experience
- **WHEN** play is targeted at a connected device
- **THEN** inspection, live editing, and profiling SHALL behave as they do in-process

### Requirement: Undo/redo
All editor mutations SHALL be recorded as **transactions** against documents, as defined in
`editor-documents-and-transactions`. This capability does not define the transaction model; it
requires that the editor use it exclusively.

Histories SHALL be per document, with a separate history for project-level changes.

Operations SHALL be composable into named transactions and mergeable for continuous edits, so that
dragging a gizmo produces one undo step rather than hundreds.

Undo entries SHALL remain valid across asset reloads and schema migrations, because operations
address stable identities rather than pointers or offsets.

#### Scenario: Gizmo drag is one step
- **WHEN** a user drags a transform gizmo
- **THEN** the continuous updates SHALL merge into one undo step committed on release

#### Scenario: Per-scene history
- **WHEN** two scenes are open and the user undoes
- **THEN** only the focused document's history SHALL be affected

#### Scenario: Plugin edits are undoable
- **WHEN** a plugin modifies the scene through the editor API
- **THEN** the change SHALL be recorded as a transaction automatically, without the plugin
  implementing undo

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

For a **world**, the hierarchy SHALL be a virtualised outliner over a **world metadata index** —
persistent identity, name, type, bounds, layer, cell, prefab provenance, and asset references —
rather than a materialised tree of every entity. It SHALL support queries by name, component,
layer, cell, prefab, tag, and asset, and SHALL NOT require the world to be loaded to search it.

Selecting an unloaded entity SHALL load its **authoring record** for inspection, and SHALL NOT
require activating its region.

The editor SHALL provide a **streaming debugger** view (see `world-partition-and-streaming`)
showing cell states, streaming sources, priorities, costs, layers, and why a given cell is in its
current state.

The **asset browser** SHALL show the project's assets with: folder navigation, search and type
filtering, thumbnails generated asynchronously, drag-and-drop into scene and inspector, rename
and move with reference preservation, and import settings access.

Prefab and scene assets SHALL support a **structural diff** view showing added, removed, and
changed entities, components, and fields, and the inspector SHALL show each value's provenance —
base, variant, or instance override — with unresolved override conflicts surfaced rather than
hidden.

#### Scenario: Asset moved
- **WHEN** an asset is moved in the browser
- **THEN** its `.meta` SHALL move with it and all references SHALL continue to resolve by
  `AssetId`

#### Scenario: Thumbnail generation
- **WHEN** a folder of models is displayed
- **THEN** thumbnails SHALL be rendered asynchronously without blocking the UI, and cached

#### Scenario: Millions of entities are searchable
- **WHEN** a designer searches a world containing millions of entities
- **THEN** the query SHALL run against the metadata index and results SHALL be virtualised, without
  loading entity component data

#### Scenario: Inspecting an unloaded entity
- **WHEN** a designer selects an entity in an unloaded region
- **THEN** its authoring record SHALL be loaded for inspection without streaming its region

#### Scenario: Overrides are legible
- **WHEN** a prefab instance is inspected
- **THEN** each value SHALL show whether it is inherited or overridden, and any override conflict
  SHALL be surfaced with its resolutions

### Requirement: Plugin architecture
The editor SHALL be extensible through the **extension points** defined in `project-and-plugins`,
which also owns plugin identity, manifests, lifecycle, dependency resolution, the binary boundary,
and hot reload eligibility. This capability defines the editor-specific extension points and their
behaviour.

Editor extension points SHALL include: panels, menus, toolbars, and keyboard shortcuts; inspector
property editors and component gizmos; importers and asset post-processors; viewport tools receiving
input; build steps and platform targets; commands; document kinds and asset editors; source control
providers; and editor event subscriptions.

Plugins SHALL read and write edited content only through the transaction API, so their changes are
undoable and journalled like any other.

Editor extension interfaces SHALL be versioned independently of the engine's release version, so a
plugin targets an interface rather than a patch release.

#### Scenario: Swift editor plugin
- **WHEN** a project includes a Swift editor plugin
- **THEN** it SHALL load with the game module, register its panels and tools, and reload on rebuild
  where it declares support for reload

#### Scenario: Plugin API version
- **WHEN** a plugin requires a newer editor extension interface than is present
- **THEN** it SHALL be reported as incompatible rather than loaded and failing at random

#### Scenario: Plugin edits go through transactions
- **WHEN** a plugin tool modifies the edited document
- **THEN** it SHALL do so through the transaction API, and no other write path SHALL exist

### Requirement: Specialised editors
The editor SHALL provide dedicated editors for: materials (including the node graph),
animation graphs and clips (a timeline with curve editing), the VFX graph (see `vfx-system`),
**terrain**, **foliage**, **water**, and **environment fields**, tilemaps, UI layout, audio buses
and mixing, navigation baking, lighting and lightmap baking, and localisation tables.

The environment tools SHALL include: terrain sculpting and material painting over a
**non-destructive modifier stack**, biome and field painting, river spline authoring with live flow
and shoreline preview, lake and ocean configuration, foliage painting and rule authoring with
regional preview, and road and spline tools.

Rule-driven tools — foliage placement, field-driven terrain materials — SHALL be able to show
**why** a result occurred, naming the input that drove or excluded it, since a procedural result
that cannot be explained cannot be corrected.

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

#### Scenario: A procedural result is explainable
- **WHEN** a designer asks why no trees appear in a region
- **THEN** the tool SHALL name the rule input responsible

#### Scenario: Sculpting is non-destructive
- **WHEN** a designer inserts an erosion pass beneath existing sculpting
- **THEN** the sculpting SHALL be preserved and reapplied above it

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
The editor SHALL provide build and deployment as a **client of the build service** defined in
`build-and-packaging`: it SHALL NOT invoke shell scripts and parse their output.

The editor SHALL: select a target, platform, and configuration; request compile, cook, package, and
deploy; display structured progress and diagnostics; and allow cancellation.

Build profiles SHALL be saveable, and builds SHALL be runnable from the command line against the
same service and the same profile, so continuous integration and local builds match by construction
rather than by convention.

Diagnostics SHALL be navigable: selecting one SHALL open what it refers to.

#### Scenario: CI matches local
- **WHEN** CI runs a build with a saved profile
- **THEN** it SHALL produce the same output as the editor with that profile, because both drive the
  same service with the same inputs

#### Scenario: Deploy to device
- **WHEN** a mobile target is selected with a connected device
- **THEN** the build SHALL install and launch on it with the debugger attached

#### Scenario: The editor stays usable
- **WHEN** a long cook is running
- **THEN** the editor SHALL remain interactive, showing progress and allowing cancellation

### Requirement: Debugging and profiling tools
The editor SHALL provide: a log panel with filtering and source navigation, a remote and local
debugger (breakpoints, stepping, variable inspection for Swift and native), a live entity
inspector for the running game, a frame profiler (CPU stages, jobs, GPU passes) with a timeline,
memory profiling by allocator tag and asset category, and a render debugger.

The **render debugger** SHALL show the render graph as it was built for a frame: passes and their
dependencies as a navigable graph, and per pass its GPU time, queue, resources read and written,
transient memory, and barrier wait time. It SHALL show the budget arbiter's allocations,
measurements, and adjustments for that frame.

A **shader and material inspector** SHALL show, for any material, each stage of lowering — graph,
material IR before and after optimisation, generated Slang, backend binary — with instruction
counts, occupancy estimates, resource bindings, cost attributed to graph nodes, and the reason
each permutation exists.

Profiler markers SHALL map cleanly into external GPU capture tools (RenderDoc, PIX, Xcode, Nsight,
Radeon GPU Profiler), and a capture SHALL be launchable from the editor.

These tools SHALL be built alongside the renderer rather than after it.

Live editing SHALL be supported: property changes in the editor applied to the running game.

#### Scenario: Frame spike investigation
- **WHEN** a frame exceeds its budget
- **THEN** the profiler timeline SHALL show which stage, system, or GPU pass consumed the time

#### Scenario: Live tweak
- **WHEN** a value is changed in the inspector while the game runs
- **THEN** it SHALL apply immediately in the running game, with a clear indication that the change
  is not persisted unless applied

#### Scenario: Quality drop is explained
- **WHEN** rendering quality drops during play
- **THEN** the render debugger SHALL show which allocation changed and what measurement caused it

#### Scenario: Expensive material is diagnosed
- **WHEN** a material is expensive
- **THEN** the inspector SHALL attribute the cost to specific graph nodes rather than reporting a
  total

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
