# editor-rust-application Specification

## Purpose

Defines **CyberEditor as an application**: a native Rust desktop program that is a *client* of the
engine rather than a part of it, reaching it only through the stable C ABI and the live bridge
protocol.

Three decisions shape it. The editor runs the engine in a **separate process by default**, so a
runtime crash costs a restart rather than the session, and so a console is not a special case. Its
boundary is **enforced by the language**: no C++ type, no raw pointer used as identity, and `unsafe`
confined to an audited overlay. Its presentation is **MVVM with explicit services and a central
command registry**, under two rules that are stated as prohibitions because they are violated by
accident — a view model is never a second source of truth, and no view model depends on another
panel's view model.

This reverses the earlier position that the editor is an engine application built on CyberUI. That
argument was a good one; `design.md` of `add-editor-application-and-workflow` records what was given
up and what replaces it.

## Requirements

### Requirement: The editor is a Rust client of the engine
CyberEditor SHALL be a **native Rust desktop application**, and SHALL depend on the engine only
through Cyberdyne-owned stable boundaries: the stable C ABI (see `native-abi`) with a safe Rust
wrapper, and the live bridge protocol (see `live-editing`).

The editor SHALL NOT depend on C++ implementation types, standard-library containers, third-party
engine types, or direct ownership of runtime subsystem objects.

The editor SHALL NOT be compiled into the engine runtime, and a shipping game SHALL contain none of
it — which follows from it being a separate binary rather than from a compilation flag.

#### Scenario: The boundary is enforced by the language
- **WHEN** an editor feature needs engine data
- **THEN** it SHALL obtain it through the SDK or the protocol, and no C++ type SHALL appear in editor
  code

#### Scenario: Shipping excludes the editor
- **WHEN** a game is packaged
- **THEN** no editor binary or editor code SHALL be included

### Requirement: Engine hosting modes
The editor SHALL support three engine hosting modes:

| Mode | Engine location | Used for |
|---|---|---|
| `NoRuntime` | None | Project browsing, source assets, build configuration, source control, specification tooling |
| `Embedded` | In the editor process | Schema and reflection queries, asset metadata, lightweight previews |
| `Hosted` | A separate process or a remote device | Rendering, play mode, physics, streaming, profiling |

**Hosted SHALL be the production default.**

**Every editor feature SHALL remain capable of operating against a hosted runtime** unless it has a
documented reason to require in-process execution, and such reasons SHALL be enumerated rather than
accumulated.

A runtime failure SHALL NOT terminate the editor: the editor SHALL survive, surface the crash
artefact (see `diagnostics-profiling-and-crash`), and offer to restart the runtime or open the
reproduction.

#### Scenario: The editor survives a runtime crash
- **WHEN** the hosted runtime crashes
- **THEN** the editor SHALL remain running with its documents and journal intact, and SHALL present
  the crash artefact

#### Scenario: A console is not a special case
- **WHEN** the runtime is on a console
- **THEN** editor features SHALL work through the same hosted path used locally

### Requirement: Rust workspace structure
The editor SHALL be a **multi-crate workspace** rather than one crate, with dependencies flowing
toward shared foundations: application, core, sdk, protocol, documents, commands, ui, inspector,
assets, viewport, and domain editors.

Domain editors SHALL depend on shared editor services and SHALL NOT depend on one another.

Dependency direction SHALL be enforced by the workspace, and a cycle or an upward dependency SHALL be
a build error rather than a review comment.

#### Scenario: Panels do not depend on panels
- **WHEN** a sequence editor needs selection
- **THEN** it SHALL depend on the shared selection service, not on the hierarchy panel's crate

### Requirement: Safety and interoperation rules
`unsafe` code SHALL be confined to **narrow, audited interoperation and platform modules**, and SHALL
NOT appear in panels, view models, services, or domain logic.

Engine handles SHALL be represented as **generation-checked value types**, never raw pointers. A
structure holding a pointer to a runtime object SHALL NOT exist in editor code.

Errors crossing the boundary SHALL be converted to typed Rust results; failure SHALL NOT be signalled
by sentinel values reaching editor logic.

String and buffer contracts across the boundary SHALL be explicit — encoding, ownership, and lifetime
— and callbacks from the engine into editor code SHALL be constrained, registered, and re-entrancy
safe rather than arbitrary.

#### Scenario: No pointers as identity
- **WHEN** the editor refers to an entity or an asset
- **THEN** it SHALL hold a stable identifier, not an address

#### Scenario: Failures are typed
- **WHEN** an engine call fails
- **THEN** the editor SHALL receive a typed error with a reason, not a null or a code to interpret

### Requirement: Presentation architecture
The editor SHALL use an **MVVM-style presentation architecture augmented by explicit services and a
central command registry**:

| Layer | Owns |
|---|---|
| **Models and services** | Authoritative state: documents, project, workspace, runtime sessions, selection, transactions, assets, builds |
| **View models** | Presentation state and user intents: expansion, filtering, sorting, scroll position, formatted values, in-progress edits |
| **Views** | Rendering a view model and emitting intents |

Views SHALL contain no domain logic and SHALL NOT mutate documents, engine state, or services
directly.

Views and view models SHALL NOT access engine implementation objects; engine operations pass through
services backed by the SDK or the protocol.

#### Scenario: A widget cannot change the world
- **WHEN** a property field is edited
- **THEN** the view SHALL raise an intent, the view model SHALL invoke a command, and the change SHALL
  become a transaction — with no path from the widget to engine state

#### Scenario: The toolkit is replaceable
- **WHEN** the interface toolkit is changed
- **THEN** models, services, view models, and commands SHALL be unaffected

### Requirement: View models are not a second source of truth
A view model SHALL NOT hold authoritative document, project, or runtime state.

It MAY hold derived and presentation state — formatted values, expansion, filters, sort order,
selection presentation, scroll position, and in-progress edit buffers — and SHALL obtain authoritative
values from models and services.

An in-progress edit SHALL be explicitly distinguished from a committed value, so that an uncommitted
field is never mistaken for document state.

#### Scenario: Two panels cannot disagree
- **WHEN** the same value is shown in two panels
- **THEN** both SHALL derive it from the same model, and neither SHALL cache it authoritatively

#### Scenario: An abandoned edit changes nothing
- **WHEN** a field is edited and focus is lost without committing
- **THEN** the document SHALL be unchanged and no transaction SHALL exist

### Requirement: No peer dependencies between panels
A view model SHALL NOT depend on, hold a reference to, or call another panel's view model.

Cross-panel coordination SHALL occur through **shared services and state**: selection, documents,
commands, assets, runtime session, and notifications.

Panels SHALL communicate by observing shared state and by raising commands, never by direct
invocation of one another.

This is stated as a prohibition because it is violated by accident, for locally reasonable reasons,
and produces a dependency graph with no workable initialisation order.

#### Scenario: Selection reaches five panels through one service
- **WHEN** an entity is selected in the hierarchy
- **THEN** the inspector, viewport, properties, and status bar SHALL observe the selection service,
  and the hierarchy SHALL not notify them

#### Scenario: A new panel needs no wiring
- **WHEN** a plugin adds a panel that reacts to selection
- **THEN** it SHALL subscribe to the service, and no existing panel SHALL be modified

### Requirement: Commands are the single action surface
Every user-invokable action SHALL be registered in a **command registry** with an identifier, a label,
a category, an availability predicate, and an optional default binding.

Menus, toolbars, context menus, keyboard shortcuts, the command palette, automation, tests, and the
**agent interface** defined in `editor-agent-interface` SHALL all invoke commands. An action reachable only through a specific widget SHALL
be a defect.

Command availability SHALL be queryable, so that a disabled action can explain why it is unavailable.

Commands that modify project state SHALL execute through the transaction system defined in
`editor-documents-and-transactions`.

Command metadata SHALL be rich enough for **machine invocation**, not merely for rendering a menu
item: typed parameters with their meaning, a description written for a caller that cannot see the
interface, and a declared **effect class** — read, reversible mutation, irreversible mutation, or
external effect.

A registry that only has to satisfy a menu will not satisfy a caller that has never seen the menu,
and retrofitting metadata across an established registry is an entry-by-entry migration. This
therefore applies from the first command registered.

#### Scenario: A command is invocable without seeing the interface
- **WHEN** a caller has only the registry
- **THEN** it SHALL be able to determine what the command does, what its parameters mean, and what
  class of effect it has

#### Scenario: One action, six entry points
- **WHEN** an action exists
- **THEN** it SHALL be invocable from a menu, a shortcut, the palette, a script, and a test without
  additional implementation

#### Scenario: Unavailability is explainable
- **WHEN** a command is disabled
- **THEN** the reason SHALL be reportable rather than the control being merely greyed

### Requirement: Asynchronous operations
The interface thread SHALL NEVER block on asset import, cooking, shader compilation, world loading,
builds, network operations, project scans, or engine calls of unbounded duration.

Long operations SHALL be services returning **observable progress state** — status, progress,
cancellability, and errors — which view models expose and views render.

Every long operation SHALL be cancellable where the underlying work supports cancellation, and its
cancellation SHALL be surfaced.

#### Scenario: The editor stays interactive
- **WHEN** a large cook runs
- **THEN** the editor SHALL remain responsive and show progress

#### Scenario: Cancellation is offered
- **WHEN** an import is running
- **THEN** it SHALL be cancellable, and cancelling SHALL leave completed work valid

### Requirement: Change propagation
State changes SHALL propagate to view models through **change notifications, versions, or streams** —
not by polling engine or document state each interface frame.

A view model SHALL be able to determine cheaply whether its inputs changed, and SHALL rebuild only
what changed.

The editor SHALL NOT re-query reflected properties, asset listings, or runtime state at interface
frame rate when nothing has changed.

#### Scenario: An idle inspector costs nothing
- **WHEN** the selection and its values are unchanged
- **THEN** the inspector SHALL perform no engine queries

### Requirement: Editor state model
The editor SHALL maintain an explicit application state model — documents, selection, workspaces,
project state, runtime sessions, commands, and notifications — rather than distributing authoritative
state across widgets.

Editor state SHALL be inspectable and, where useful, serialisable, so that a session can be restored
and a defect reported with the state that produced it.

**Presentation state SHALL be separated from authoritative state** in persistence as well as in
memory: layout, expansion, filters, and scroll positions belong to workspace or user settings and
SHALL NOT dirty a document.

#### Scenario: Looking around dirties nothing
- **WHEN** a user expands sections and scrolls
- **THEN** no document SHALL become dirty

#### Scenario: A session restores
- **WHEN** the editor restarts
- **THEN** open documents, layout, and presentation state SHALL be restored

### Requirement: Editor SDK boundary
The engine SHALL expose an **editor SDK**: a Rust layer over the stable C ABI providing typed handles,
typed errors, reflection access, asset and document operations, runtime session control, and protocol
messaging.

The SDK SHALL be **generated or maintained against the ABI description**, so that the two cannot
drift, consistent with how the Swift overlay is produced.

Editor code SHALL use the SDK; it SHALL NOT call the C ABI directly outside the SDK crate.

The SDK SHALL be usable by tools other than the editor.

#### Scenario: One boundary, checked
- **WHEN** the ABI changes
- **THEN** the SDK SHALL be regenerated or updated and the mismatch SHALL be a build failure

#### Scenario: Tools reuse the SDK
- **WHEN** a command-line tool needs engine data
- **THEN** it SHALL use the same SDK

### Requirement: Editor plugin surface
Editor extensions SHALL register through **editor SDK abstractions**: panels, view models, commands,
menu contributions, property editors, asset editors, graph nodes, viewport tools and gizmos, search
providers, importers, and settings pages.

**Rust's native binary interface SHALL NOT be the plugin boundary.** Binary editor plugins SHALL cross
the engine's stable C ABI or a process protocol; a Rust plugin distributed as source MAY be compiled
with the editor.

The interface toolkit SHALL NOT appear in plugin-facing types. A plugin SHALL describe its interface
in editor abstractions rather than toolkit types.

Plugins SHALL mutate project state only through commands and transactions.

#### Scenario: The toolkit does not leak
- **WHEN** a plugin defines a panel
- **THEN** it SHALL use editor abstractions, and changing the toolkit SHALL not break it

#### Scenario: No unstable ABI as a contract
- **WHEN** a binary editor plugin is distributed
- **THEN** it SHALL cross the stable C ABI or a protocol, not Rust's native interface

### Requirement: Interface toolkit is an implementation detail
The Rust interface toolkit used to render the editor shell SHALL be an **implementation choice behind
editor abstractions**, selected on measurement, and SHALL NOT appear in editor extension interfaces,
protocol definitions, or engine-facing types.

The specification SHALL NOT mandate a particular toolkit.

Editor abstractions SHALL cover the surfaces plugins need — window, panel, dock, tab, menu, toolbar,
inspector row, tree, table, graph surface, timeline surface, dialog, notification — so that plugins
have no reason to reach through them.

#### Scenario: The choice can be revisited
- **WHEN** measurement favours a different toolkit
- **THEN** it SHALL be replaceable without changing editor services, view models, or plugin
  interfaces

### Requirement: Editor testability
Services, models, view models, and commands SHALL be testable **headlessly**: with no window, no
graphics device, and no interface toolkit.

Tests SHALL be able to drive the editor through commands and assert on model and view model state —
selection, property values, transaction history, filtering, and workspace state.

Where a test requires an engine, it SHALL use a hosted or embedded runtime through the SDK, or a test
double implementing the same interfaces.

#### Scenario: Selection behaviour is testable
- **WHEN** a test selects an entity and asserts the inspector's contents
- **THEN** it SHALL run with no window and no graphics device

#### Scenario: Undo is testable
- **WHEN** a test edits a property and undoes it
- **THEN** it SHALL assert on document state without any interface

### Requirement: Forbidden editor application patterns
The following SHALL NOT appear, and each SHALL be checkable:

- Editor code depending on C++ implementation types, standard-library containers, or engine internals
- Raw pointers used as editor-side identity
- `unsafe` code outside audited interoperation and platform modules
- A view model holding authoritative document or runtime state
- A view model depending on another panel's view model
- A view mutating documents, services, or engine state directly
- Blocking the interface thread on engine, build, asset, or network work
- Polling engine or document state every interface frame
- The interface toolkit's types appearing in plugin or protocol interfaces
- Rust's native binary interface treated as a stable plugin contract

#### Scenario: A proposal is checked
- **WHEN** a change would have the inspector panel call the hierarchy panel's view model
- **THEN** it SHALL be flagged against this requirement
