## MODIFIED Requirements

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
