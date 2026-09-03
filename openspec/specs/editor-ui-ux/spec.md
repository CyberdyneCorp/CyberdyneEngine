# editor-ui-ux Specification

## Purpose

Defines how CyberEditor **behaves for the person using it**: what is familiar, what is dense, what is
reachable, and how fast it has to be.

Familiarity is treated as a feature rather than a compromise — a Unity-compatible keymap ships, and a
departure from convention must earn itself. Density is preferred to decoration, because this is a
tool used for eight hours a day. Every action is a registered command, so menus, shortcuts, the
palette, scripts and tests are the same surface. Every edit is one transaction, so a drag is one
undo. Problems appear on the object that has them, not only in a console. And the editor declares
interaction performance targets it is measured against, so responsiveness is a requirement rather
than an aspiration.

## Requirements

### Requirement: Familiarity is a feature
The editor's default interaction model SHALL be **deliberately familiar** to users of existing
engines: docked panels, a hierarchy, an inspector, a project browser, a viewport with standard
navigation, a play bar, and a console.

Departures from established conventions SHALL be **deliberate and justified by a measurable
improvement**, not incidental.

The editor SHALL ship a **Unity-compatible keymap** as a selectable preset, and SHALL support
user-defined keymaps.

An expert user's first hour SHALL not be spent relearning where things are.

**Familiarity is of structure and interaction, not of identity.** The editor SHALL be familiar in
where things are and how they behave — panels, hierarchy, inspector, viewport navigation, transform
tools, the play bar — and SHALL be distinctly itself in how it looks and what it calls things. Its
visual language, iconography, branding and vocabulary are specified in `editor-visual-language` and
SHALL NOT be borrowed from the engines whose layout conventions it deliberately adopts.

An expert arriving from another engine should find the controls where they expect them and know
immediately that this is not that engine.

#### Scenario: Familiar layout, distinct identity
- **WHEN** an experienced user opens the editor for the first time
- **THEN** the arrangement and interaction SHALL match their expectations, and the iconography,
  colour language, terminology and branding SHALL be recognisably Cyberdyne's own

#### Scenario: A Unity user is productive quickly
- **WHEN** a user selects the Unity keymap
- **THEN** navigation, transform tools, and common commands SHALL match their expectations

#### Scenario: A departure is recorded
- **WHEN** the editor differs from established convention
- **THEN** the reason SHALL be documented rather than assumed

### Requirement: Docking and workspaces
The editor SHALL support **dockable, floating, tabbed, and multi-window panels**, with panels movable
between windows and across monitors.

The editor SHALL support named **workspaces** — saved layouts for tasks such as scene editing,
animation, materials, sequencing, and profiling — switchable without losing document state.

Workspace layout SHALL persist across sessions per project and per user, and SHALL be shareable.

A workspace SHALL be resettable to its default, and a broken layout SHALL never make the editor
unusable.

#### Scenario: Layouts follow the task
- **WHEN** a user switches from scene editing to animation
- **THEN** the panel arrangement SHALL change without closing documents

#### Scenario: Recovery is always available
- **WHEN** a layout is unusable
- **THEN** resetting to a default workspace SHALL restore a working editor

### Requirement: Density over decoration
The interface SHALL be optimised for **information density and low interaction cost** for
professional daily use rather than for visual impression.

The editor SHALL provide **compact and comfortable density modes**, and SHALL honour a user-selected
interface scale.

Panels SHALL prefer showing more relevant information over decorative spacing, and SHALL avoid
animations that delay interaction.

Frequently used values SHALL be visible without expanding, and frequently used actions SHALL be
reachable without traversing menus.

#### Scenario: A dense inspector fits
- **WHEN** an entity has many components
- **THEN** the inspector SHALL present them compactly with clear structure rather than requiring
  extensive scrolling for common values

#### Scenario: Animation never blocks input
- **WHEN** a panel opens
- **THEN** interaction SHALL be possible immediately

### Requirement: Command palette and search
The editor SHALL provide a **command palette** that searches commands, assets, entities, settings,
documentation, and recent items from one input.

Search SHALL be **fuzzy, ranked, and incremental**, and SHALL show what a result is and where it
comes from.

Results SHALL be actionable directly — opening an asset, invoking a command, focusing an entity, or
navigating to a setting.

Search SHALL never block the interface thread, and partial results SHALL be shown while indexing
continues.

#### Scenario: One input reaches everything
- **WHEN** a user types a term
- **THEN** matching commands, assets, entities, and settings SHALL be offered with their origin
  shown

### Requirement: Keyboard-first operation
Every frequent workflow SHALL be completable **without the mouse** where the operation is not
inherently spatial.

Keyboard bindings SHALL be user-configurable per command, SHALL support chords and contexts, and
SHALL report conflicts rather than silently overriding.

Focus order and keyboard navigation SHALL be defined for panels, lists, trees, tables, and dialogs.

#### Scenario: A conflict is reported
- **WHEN** a user assigns a binding already in use in the same context
- **THEN** the conflict SHALL be shown with both commands named

### Requirement: Inspector is generated from reflection
The inspector SHALL be **generated from the engine's reflection data** (see `reflection-system`)
rather than hand-written per type, so a new type is editable without editor code.

Reflection metadata SHALL drive presentation: ranges, units, tooltips, categories, ordering,
conditional visibility, read-only state, and validation.

Custom property editors and custom whole-type editors SHALL be registrable to override the generated
presentation, and SHALL be usable by plugins.

The inspector SHALL support **multi-selection editing**, showing mixed values explicitly and applying
an edit to all selected objects as one transaction.

#### Scenario: A new component is editable immediately
- **WHEN** a reflected component type is added
- **THEN** it SHALL appear in the inspector with appropriate controls and no editor code

#### Scenario: Mixed values are honest
- **WHEN** a multi-selection has differing values for a property
- **THEN** the field SHALL show a mixed state, and editing it SHALL set all selected objects in one
  undo step

### Requirement: Every edit is a transaction
Every user edit SHALL produce a **transaction** in the sense of `editor-documents-and-transactions`,
with undo, redo, coalescing of continuous manipulation, and a description.

Drag operations — sliders, gizmos, timeline scrubbing, curve editing — SHALL produce **one**
transaction on completion rather than one per frame, and SHALL be cancellable mid-drag with the
prior state restored.

Undo history SHALL be inspectable, and each entry SHALL describe what changed and to what.

#### Scenario: A drag is one undo
- **WHEN** a user drags a slider across many values
- **THEN** one undo SHALL restore the value from before the drag

#### Scenario: Escape cancels
- **WHEN** a user presses escape during a drag
- **THEN** the value SHALL return to its pre-drag state and no transaction SHALL be recorded

### Requirement: Errors, warnings, and validation surface where they belong
Problems SHALL be surfaced **at the object that has them** — in the hierarchy, the inspector row, the
asset entry, the viewport — not only in a console.

The editor SHALL provide a **problems view** aggregating validation results, import failures, cook
errors, missing references, and warnings, with navigation to the source of each.

Every problem SHALL state what is wrong, where, and what would fix it; a message with no actionable
content SHALL be treated as a defect.

The console SHALL support filtering by severity, category, source, and search, and SHALL cope with
high message rates without stalling the editor.

#### Scenario: A missing reference is visible
- **WHEN** an asset reference cannot be resolved
- **THEN** the inspector row, the hierarchy entry, and the problems view SHALL show it, with an
  offered resolution

### Requirement: Long operations are visible and cancellable
The editor SHALL present a **unified progress surface** for imports, cooks, shader compilation,
builds, world loads, and remote deployments, showing what is running, its progress, and its elapsed
time.

Operations SHALL be cancellable where the work supports it, and cancellation SHALL leave the project
in a valid state.

Failures SHALL leave a **retained artefact** — a log, the failing input, and the reason — reachable
from the progress surface.

#### Scenario: A build failure is diagnosable
- **WHEN** a cook fails
- **THEN** the progress surface SHALL retain the failing asset, the reason, and the log

### Requirement: Notifications do not interrupt
Non-blocking information SHALL be presented as **notifications that do not steal focus** or block
input, and SHALL be dismissible and reviewable afterwards.

Modal dialogs SHALL be reserved for decisions that genuinely cannot proceed without input, and their
number SHALL be minimised.

A notification SHALL offer an action where one is meaningful — opening the asset, viewing the log,
retrying, or undoing.

#### Scenario: A background failure does not interrupt typing
- **WHEN** an import fails while a user is editing a value
- **THEN** a notification SHALL appear without taking focus, and SHALL remain reviewable

### Requirement: Theming and accessibility
The editor SHALL ship **dark and light themes**, SHALL support user themes, and SHALL keep colour out
of the sole encoding of meaning — state SHALL also be conveyed by shape, icon, or text.

The editor SHALL support interface scaling, high-DPI displays, and configurable font sizes.

Colour choices SHALL meet legibility contrast targets in the shipped themes, and colour-blind-safe
palettes SHALL be selectable for status and category encoding.

#### Scenario: Status without colour
- **WHEN** an entry is in an error state
- **THEN** it SHALL be identifiable without relying on hue alone

### Requirement: The editor states its performance targets
Interface interaction SHALL remain responsive on a project of realistic size, and the editor SHALL
declare targets it is measured against:

| Interaction | Target |
|---|---|
| Interface frame time when idle | Negligible; no per-frame engine queries |
| Selection change to inspector populated | Under 16 ms for typical entities |
| Command palette first results | Under 50 ms |
| Panel dock or workspace switch | Under 100 ms |
| Project browser scroll on 100 000 assets | Smooth; virtualised |

Lists, trees, and tables SHALL be **virtualised**, and their cost SHALL scale with what is visible
rather than with what exists.

Regressions against these targets SHALL be detectable in continuous integration.

#### Scenario: A large project stays usable
- **WHEN** a project contains hundreds of thousands of assets
- **THEN** browsing, searching, and selecting SHALL remain responsive

### Requirement: Editing never silently loses work
The editor SHALL protect in-progress work: unsaved documents SHALL be recoverable after a crash or a
power loss, and the editor SHALL state clearly what was recovered.

Destructive operations SHALL be undoable or confirmed, and a confirmation SHALL say precisely what
will be lost.

An external change to a file open in the editor SHALL be detected and surfaced as a reconcilable
conflict rather than being silently overwritten in either direction.

#### Scenario: A crash costs minutes, not hours
- **WHEN** the editor terminates unexpectedly with unsaved work
- **THEN** the next launch SHALL offer recovery and SHALL state what it recovered

#### Scenario: External edits are reconciled
- **WHEN** a file open in the editor changes on disk
- **THEN** the conflict SHALL be surfaced with a choice, not resolved silently

### Requirement: Discoverability without documentation
The editor SHALL make capability discoverable in place: tooltips with meaning rather than restated
labels, units shown on numeric fields, valid ranges indicated, and inline explanation of
non-obvious settings.

Every setting SHALL be able to explain **what it affects** and **what its default is**, and SHALL
show whether it differs from the default.

Documentation links SHALL be reachable from the object in question rather than only from a menu.

#### Scenario: A modified setting is visible
- **WHEN** a setting differs from its default
- **THEN** it SHALL be marked and resettable in place

### Requirement: Editor-specific diagnostics
The editor SHALL be able to profile **itself** — interface frame time, view model rebuild cost,
engine call counts and latencies, protocol traffic, and background operation duration — using the
diagnostics facilities of `diagnostics-profiling-and-crash`.

A slow editor interaction SHALL be attributable to a panel, a service, or an engine call rather than
being reported as a whole-application stall.

#### Scenario: A slow panel is named
- **WHEN** the interface stutters
- **THEN** the responsible panel or service SHALL be identifiable from an editor profile

### Requirement: Forbidden editor interface patterns
The following SHALL NOT appear, and each SHALL be checkable:

- An action reachable only from one widget with no command registration
- A non-virtualised list, tree, or table over unbounded data
- A modal dialog used for information that could be a notification
- A per-frame transaction produced by a continuous drag
- Colour as the only encoding of state
- A message that states a failure without stating a cause or a remedy
- A document dirtied by presentation-only state such as expansion or scroll position
- A confirmation prompt that does not say what will be lost

#### Scenario: A review catches a per-frame undo
- **WHEN** a new gizmo records a transaction each frame of a drag
- **THEN** it SHALL be flagged against this requirement
