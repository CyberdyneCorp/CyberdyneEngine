# editor-documents-and-transactions Specification

## Purpose

Defines how the editor represents and mutates project state: authoring, preview and runtime worlds
kept distinct, **documents** as the unit of editing, and **transactions** as the only path by which
persistent state changes.

The decision that everything else rests on is that a transaction is a **semantic operation**, not a
snapshot — a typed delta addressing its target by the stable identities the type system guarantees.
Snapshots would be simpler and would foreclose everything, because one operation stream is five
products at once: undo, autosave as a journal, crash recovery, semantic diff and merge, and the
delta a running game consumes. Specifying it properly once is what makes the other four fall out.

Addressing by identity is also what makes an undo history survive an asset reload, a rename, and a
schema migration — the clearest payoff of having made type and field identity stable and recorded.

Two smaller rules do a lot of work. A **document is not a file**: a world backed by hundreds of
authoring chunks is one document with one history. And **view state is not content**: moving the
camera or expanding a tree node must never dirty an asset.

## Requirements

### Requirement: Authoring, preview, and runtime worlds are distinct
The editor SHALL maintain three kinds of world, and SHALL NOT collapse them:

| World | Contains |
|---|---|
| **Authoring** | The edited project: authoring-only data, prefab provenance, override state, unresolved references, selection metadata |
| **Preview** | Isolated worlds for asset editors — material, animation, VFX, UI previews — each with its own services |
| **Runtime** | The world play mode instantiates, produced *from* authoring data by a compilation step |

Authoring-only data SHALL NOT exist in a runtime world. Because a compilation step separates them,
this SHALL be a checkable property rather than a convention.

The editor's own interface SHALL run in its own world, as it already does.

#### Scenario: Editor data cannot leak
- **WHEN** a runtime world is instantiated from authoring data
- **THEN** authoring-only components and metadata SHALL be absent, and their presence SHALL be
  detectable as a defect

#### Scenario: Previews are isolated
- **WHEN** a material editor and an animation editor are both open
- **THEN** each SHALL have its own preview world, and neither SHALL affect the edited project

### Requirement: Documents are the unit of editing
Everything editable SHALL be presented as a **document**: world, scene, prefab, material, animation,
VFX graph, UI layout, behaviour graph, terrain, sequence, and any plugin-supplied kind.

A document SHALL expose: identity, its backing asset or assets, dirty state, save and reload, and
its transaction context.

**A document is not a file.** One document MAY be backed by many files — a world by its metadata,
authoring chunks, layers, and scene instances — and the editor SHALL present it as one coherent
authoring model.

Every editor SHALL use this document model rather than implementing its own save and history.

#### Scenario: A world is one document
- **WHEN** a designer opens a world backed by hundreds of authoring chunks
- **THEN** it SHALL be one document with one dirty state and one history

#### Scenario: A new editor inherits the model
- **WHEN** a plugin adds an asset editor
- **THEN** it SHALL obtain save, reload, dirty tracking, and undo from the document system rather
  than implementing them

### Requirement: Workspace and view state
The editor SHALL maintain a **workspace**: the set of open documents, their layout, and per-document
view state.

View state — camera position, expanded tree nodes, active tab, panel layout, filter settings — SHALL
be stored with the workspace or the user's settings, and SHALL NOT dirty an asset or enter its
history.

Workspace state SHALL be per user and SHALL NOT be committed as project content unless a project
deliberately shares a layout.

#### Scenario: Looking around does not dirty a scene
- **WHEN** a designer moves the viewport camera and expands tree nodes
- **THEN** the document SHALL remain clean and no history entry SHALL be created

#### Scenario: Layout survives a restart
- **WHEN** the editor restarts
- **THEN** open documents, layout, and view state SHALL be restored from the workspace

### Requirement: Transactions are the only path for persistent mutation
Every mutation of persistent project state SHALL be recorded as a **transaction** against a
document. Tools, panels, gizmos, importers, and plugins SHALL have no other write path.

A transaction SHALL carry a user-facing name, an ordered list of typed operations, and the document
it applies to.

A tool that mutates state outside a transaction SHALL be a defect, and development builds SHALL
detect and report it.

Widgets and tools SHALL NOT be the authoritative owner of project state; they read from and write
through documents.

Every transaction SHALL record the **actor** that produced it — the human user, or the agent,
session and stated intent defined in `editor-agent-interface`. Attribution SHALL be visible wherever
history is: the undo stack, the journal, and semantic diff.

This is not a security control and SHALL NOT be treated as one. It answers "who changed this, and
what were they trying to do" — a question every collaborator already has, and which an unattributed
history cannot answer at all.

#### Scenario: History names its authors
- **WHEN** a document's history is inspected
- **THEN** each transaction SHALL name the actor that produced it

#### Scenario: A plugin edit is undoable automatically
- **WHEN** a plugin modifies a document through the editor API
- **THEN** its changes SHALL be recorded as a transaction and SHALL be undoable without the plugin
  implementing undo

#### Scenario: Out-of-band mutation is caught
- **WHEN** development builds detect state changed outside a transaction
- **THEN** it SHALL be reported naming the document and the mutating code

### Requirement: Operations address stable identities
A transaction operation SHALL address its target by **stable identity** — document, persistent
entity or node identifier, component type identifier, and field identifier — and SHALL NOT use
pointers, array indices, or byte offsets.

Consequently a history SHALL remain valid across an asset reload, a document close and reopen within
a session, a rename, and a schema migration.

Operations SHALL be typed and enumerable, including at minimum: set field, add and remove component,
create, delete and reparent entity, instantiate prefab, modify override, change layer membership,
edit a graph, and change an asset reference.

#### Scenario: History survives a reload
- **WHEN** a document's underlying asset is reloaded after an external change
- **THEN** history entries addressing surviving objects SHALL remain valid

#### Scenario: History survives a rename
- **WHEN** a field is renamed and its identity is unchanged
- **THEN** existing history entries targeting it SHALL still apply

### Requirement: Deltas, not snapshots
Operations SHALL record **before and after values** for what they changed, not a snapshot of the
document.

Domain-specific operation payloads SHALL be supported where a generic field delta would be
inefficient: a terrain stroke records the affected tiles' deltas, a foliage paint records rule
deltas and instance exceptions, a graph edit records node and edge operations.

Undo SHALL apply the before values and redo the after values, and both SHALL be exact.

#### Scenario: A terrain stroke does not copy the landscape
- **WHEN** a designer sculpts terrain
- **THEN** the transaction SHALL record the affected tiles only

#### Scenario: Undo is exact
- **WHEN** an operation is undone
- **THEN** the document SHALL be byte-identical to its state before the operation

### Requirement: Interactive, nested, and coalesced transactions
The system SHALL support **interactive transactions** with explicit begin, update, and commit:
intermediate values drive live preview and only the committed result enters history.

**Nested transactions** SHALL collapse into the outermost transaction unless explicitly isolated, so
a tool calling another tool produces one history entry named for the user's intent.

**Coalescing** SHALL merge consecutive compatible operations within a time or interaction window —
typing a name, dragging a slider — into one entry.

An uncommitted transaction SHALL roll back automatically when its scope ends.

#### Scenario: A gizmo drag is one entry
- **WHEN** a user drags a transform gizmo through hundreds of intermediate positions
- **THEN** history SHALL contain one entry, committed on release

#### Scenario: Composite tools read naturally
- **WHEN** a tool duplicates an object, assigns a prefab, and sets a transform
- **THEN** history SHALL show one entry named for the tool's action

#### Scenario: Abandoned edit rolls back
- **WHEN** an interactive transaction is cancelled
- **THEN** the document SHALL return to its pre-transaction state and no entry SHALL be recorded

### Requirement: History scope and memory
Histories SHALL be **per document**, with a separate history for project-level changes, so undoing
in one document does not affect another.

History SHALL hold a configurable **memory budget**. When exceeded, the oldest entries SHALL be
discarded, and the discard SHALL be reported rather than silent.

Operation payloads SHALL be stored efficiently: small values inline, larger payloads compressed, and
bulk payloads referenced as content-addressed blocks rather than copied.

#### Scenario: Per-document undo
- **WHEN** two documents are open and the user undoes
- **THEN** only the focused document's history SHALL be affected

#### Scenario: Budget is enforced visibly
- **WHEN** history exceeds its budget
- **THEN** the oldest entries SHALL be dropped and the user SHALL be able to see that history was
  truncated

### Requirement: Transaction journal, autosave, and recovery
Each document SHALL maintain a **transaction journal**: committed operations since the last save,
persisted incrementally.

Autosave SHALL persist the journal rather than rewriting source assets, so an autosave never
overwrites a file the user has not saved.

After an abnormal termination, the editor SHALL offer recovery from the last saved revision plus its
journal, reporting how many transactions are recoverable.

The journal SHALL be the same operation stream used by diff, live editing, and any future
collaboration, rather than a separate representation.

#### Scenario: Crash recovery
- **WHEN** the editor is restarted after a crash with unsaved work
- **THEN** it SHALL offer to recover the journalled transactions and SHALL state how many there are

#### Scenario: Autosave does not overwrite sources
- **WHEN** autosave runs on a modified document
- **THEN** it SHALL write the journal, and the source asset SHALL remain as last saved

### Requirement: Semantic diff and merge
The system SHALL produce a **semantic diff** between two revisions of a document, or between a
document and its base, expressed as added, removed, and changed entities, components, fields, and
overrides — not as a text or binary difference.

The system SHALL support **three-way merge** at the same semantic level: non-overlapping changes to
different fields SHALL merge automatically; changes to the same field SHALL be reported as a
conflict for resolution.

Merge SHALL never silently discard a change. An unresolvable conflict SHALL be surfaced.

#### Scenario: Independent edits merge
- **WHEN** one developer changes a material and another changes a health value on the same prefab
- **THEN** both changes SHALL merge automatically

#### Scenario: A real conflict is surfaced
- **WHEN** two developers set the same field to different values
- **THEN** the merge SHALL report a conflict rather than choosing

### Requirement: Selection and property binding
Selection SHALL be a **service**, not per-panel state, holding typed selection sets — entities,
assets, components, graph nodes, and plugin-defined domains — addressed by stable identity.

The inspector SHALL bind to properties through an indirection resolving document, object, component,
and field, and SHALL NOT bind widgets to raw memory addresses.

**Multi-selection** SHALL be supported: common components are shown, differing values are presented
as mixed, and editing a mixed value SHALL produce one transaction affecting every selected object.

#### Scenario: Bulk edit is one entry
- **WHEN** five hundred entities are selected and a shared field is changed
- **THEN** one transaction SHALL be recorded affecting all of them

#### Scenario: Selection survives a reload
- **WHEN** a document is reloaded
- **THEN** selection SHALL be re-resolved by identity rather than lost or pointing at the wrong
  objects

### Requirement: Source control integration
Source control SHALL be accessed through a **provider interface** — status, history, diff, check
out, revert, submit, and lock where supported — with Git, Perforce, and a null provider as
implementations.

Editor code SHALL NOT embed the semantics of any one provider. Features a provider does not support
SHALL be reported as unavailable rather than emulated incorrectly.

Diff and history SHALL use the semantic diff above where the provider supplies revisions.

#### Scenario: Provider is replaceable
- **WHEN** a project switches source control systems
- **THEN** editor integration SHALL continue to work through the provider interface

#### Scenario: Unsupported operation
- **WHEN** a provider has no exclusive locking
- **THEN** locking SHALL be reported as unavailable rather than silently doing nothing
