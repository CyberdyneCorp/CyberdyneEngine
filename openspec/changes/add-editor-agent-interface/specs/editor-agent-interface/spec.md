## ADDED Requirements

### Requirement: Agents are editor clients, not a privileged peer
The editor SHALL expose an **agent interface**: a connection over which an autonomous agent reads
project state, invokes editor actions, and observes the result.

An agent SHALL have **no capability a human user does not have**, and SHALL reach every capability
through the same mechanisms — the command registry, the transaction system, the selection service,
and the engine's own renderer. There SHALL be no agent-only mutation path, no agent-only query path,
and no bypass of a check a human interaction is subject to.

The interface SHALL be optional at build time and absent from a shipped runtime.

#### Scenario: An agent cannot do what a person cannot
- **WHEN** an agent attempts an operation
- **THEN** it SHALL succeed exactly when the same operation invoked from the command palette would
  succeed, and fail with the same reason when it would not

#### Scenario: The interface is removable
- **WHEN** the editor is built without the agent interface
- **THEN** no agent transport SHALL be compiled, linked, or listening

### Requirement: The protocol is MCP, behind an engine-owned interface
The agent interface SHALL speak the **Model Context Protocol**: editor actions are exposed as MCP
**tools**, project state as MCP **resources**, and long-running work through the protocol's progress
and cancellation facilities.

The protocol SHALL sit behind an engine-owned interface in the manner of every other integration, so
that the wire format is replaceable without touching the projection above it. No MCP type SHALL
appear in the editor's command, document, or view-model layers.

#### Scenario: The protocol is an implementation detail
- **WHEN** the protocol version changes or a second transport is added
- **THEN** the command registry, the transaction system and the view models SHALL be unaffected

### Requirement: Tools are a projection of the command registry
Every registered command SHALL be exposed as a tool, automatically, with its identifier,
description, typed parameters and availability predicate taken from the command's own metadata.

The projection SHALL NOT be a hand-maintained list. A command added for a menu SHALL be an agent
tool without further work, a renamed command SHALL NOT leave a stale tool behind, and a removed
command SHALL disappear.

Where a command is unsuitable for agent invocation, it SHALL be **excluded by a declared property on
the command**, and the exclusion SHALL state a reason — never by omission from a list.

#### Scenario: A new action needs no agent work
- **WHEN** a contributor registers a command
- **THEN** it SHALL be invocable by an agent with no additional registration

#### Scenario: Exclusion is deliberate and explained
- **WHEN** a command is not exposed
- **THEN** the command SHALL carry the exclusion and its reason, and the reason SHALL be reportable

### Requirement: Resources are the read surface
The agent interface SHALL expose as resources, at minimum: the scene hierarchy; an entity's
components and properties as the inspector presents them; the current selection; the asset browser's
contents and an asset's metadata; open documents and their dirty state; diagnostics — errors,
warnings and validation results; and the play and build state.

Resource content SHALL come from the same services the editor's own panels read, so that an agent
and a panel cannot disagree about what is true.

Reads SHALL NOT mutate. A read SHALL NOT dirty a document, alter selection, or produce a transaction.

#### Scenario: The agent and the inspector agree
- **WHEN** an agent reads an entity's properties while the inspector displays them
- **THEN** the values SHALL be identical, because both read the same service

#### Scenario: Observation is free of side effects
- **WHEN** an agent reads the scene hierarchy
- **THEN** no document SHALL become dirty and no selection SHALL change

### Requirement: The agent sees what the human sees
The agent interface SHALL be able to return a **rendered image of a viewport**, produced by the
engine's own renderer through the same path that produces the human's viewport.

There SHALL be no separate agent rendering path and no simplified representation substituted for the
image — for the same reason `editor-viewport-and-gizmos` forbids a second renderer: what the agent
evaluates must be what the project will look like.

A request SHALL be able to state its camera, its resolution, and whether overlays, gizmos and
selection outlines are included. An image intended to represent the shipping frame SHALL exclude
them, and every returned image SHALL state which it was.

The interface SHALL also be able to return a **depth, normal, or debug visualisation** view where the
renderer already produces one, because "why is this dark" is answerable from a buffer and not from a
colour image.

#### Scenario: An agent can evaluate its own edit
- **WHEN** an agent places a light and requests the viewport
- **THEN** it SHALL receive the engine-rendered result, and the image SHALL state whether overlays
  were included

#### Scenario: A capture is honest about overlays
- **WHEN** an agent requests an image representing the shipping frame
- **THEN** gizmos, selection outlines and overlays SHALL be excluded

### Requirement: Picking and spatial queries are engine-side
An agent SHALL be able to ask **what is at a point or within a region of a viewport**, and the answer
SHALL come from the engine's own picking, so that what an agent identifies is what was rendered —
including instanced content, foliage, terrain and skinned meshes.

An agent SHALL be able to query spatial relationships the editor itself can answer: bounds, distance,
containment, what a ray intersects, and what occupies a volume.

Results SHALL be expressed in the stable identities the rest of the editor uses, and SHALL survive
streaming and reload.

#### Scenario: The agent picks what was drawn
- **WHEN** an agent picks a point over a virtualised or instanced mesh
- **THEN** the correct instance SHALL be identified, by stable identity

### Requirement: Manipulation uses the manipulation path
An agent's translate, rotate and scale SHALL execute through the same manipulation implementation a
gizmo drag uses, and SHALL inherit its guarantees from `editor-viewport-and-gizmos`: operation on
state captured at the start rather than accumulated per step, exactly one transaction per
manipulation, cancellability, and identical treatment of pivot, space, snapping and constraints.

An agent SHALL be able to express a manipulation in the terms a human does — a delta or an absolute
value, in world, local, parent or view space, about a stated pivot, with snapping on or off.

A manipulation that would be refused interactively — a locked object, a constrained axis — SHALL be
refused identically, with the same reason.

#### Scenario: An agent's move is a human's move
- **WHEN** an agent translates an object and a human translates it identically with the gizmo
- **THEN** the resulting transform SHALL be identical, and each SHALL produce one undoable transaction

#### Scenario: Locked means locked
- **WHEN** an agent attempts to move a locked object
- **THEN** it SHALL be refused with the reason the interactive path would give

### Requirement: Every agent mutation is a transaction
Every change an agent makes to persistent state SHALL execute through the transaction system in
`editor-documents-and-transactions`. There SHALL be no direct write path.

Consequently, and without any additional mechanism: an agent's edits are undoable and redoable, they
appear in the journal, they survive crash recovery, they participate in semantic diff and merge, and
they interleave with human edits in one history.

An agent SHALL be able to group a sequence of operations into **one transaction** so that a
multi-step edit undoes as a unit, and SHALL be able to abandon a transaction it has begun.

#### Scenario: One undo reverses an agent's work
- **WHEN** an agent performs a grouped multi-step edit
- **THEN** a single undo SHALL reverse all of it

#### Scenario: Interleaved histories stay coherent
- **WHEN** a human edits between two agent edits
- **THEN** undo SHALL walk the combined history in order, and no edit SHALL be lost or reordered

### Requirement: Transactions carry attribution
Every transaction SHALL record the **actor** that produced it. For an agent, that SHALL include the
agent's identity, the session, and the **stated intent** the operation was performed toward.

Attribution SHALL be visible where history is visible — the undo stack, the journal, and semantic
diff — so that a reviewer reading a change can tell what a person did from what an agent did, and
why.

Attribution SHALL NOT be a security control. It answers "who changed this and what were they trying
to do", which is a question every collaborator has and no history currently answers.

#### Scenario: A reviewer can tell the difference
- **WHEN** a project's history is inspected
- **THEN** each change SHALL name its actor, and an agent's changes SHALL carry the intent they were
  made toward

### Requirement: Availability, refusal and error are explainable
A tool invocation that cannot proceed SHALL return the **reason**, drawn from the command's own
availability predicate — the same reason the interface would give a human for a greyed control.

An error SHALL state what went wrong and what would make it succeed. An agent that receives "invalid
argument" learns nothing; one that receives "the property is read-only because the object is a prefab
instance; override it first or edit the prefab" can act.

Validation failures SHALL be reported as structured results the agent can reason about, not as prose
it must parse.

#### Scenario: A refusal teaches
- **WHEN** a command is unavailable
- **THEN** the agent SHALL receive the specific reason and, where one exists, the operation that
  would make it available

### Requirement: The editor stays usable while an agent works
An agent connection SHALL NOT take exclusive control of the editor. The human SHALL be able to
navigate, select, edit and undo throughout.

Where a human action and an agent action conflict, **the human action SHALL win**, and the agent
SHALL be told its operation was superseded and why.

The human SHALL be able to see that an agent is connected, see what it is currently doing, pause it,
and revoke its access mid-session, without restarting the editor or losing work.

#### Scenario: The human is never locked out
- **WHEN** an agent is performing a long operation
- **THEN** the editor SHALL remain responsive and the human SHALL be able to interrupt it

#### Scenario: Revocation is immediate and clean
- **WHEN** access is revoked mid-operation
- **THEN** the in-flight transaction SHALL be abandoned rather than half-applied

### Requirement: Connections declare a scope
An agent connection SHALL declare the **scope** it operates within: which documents, which
directories, and which effect classes it may invoke. Operations outside that scope SHALL be refused
with the scope as the reason.

Every command SHALL carry an **effect class** — read, reversible mutation, irreversible mutation, or
external effect — and the class SHALL be part of the tool's description, so an agent can reason about
consequence before invoking rather than discovering it afterwards.

Scope SHALL default to the narrowest useful setting rather than to full access.

#### Scenario: Scope is enforced, not advisory
- **WHEN** an agent invokes a command outside its declared scope
- **THEN** it SHALL be refused, and the refusal SHALL name the scope that excluded it

#### Scenario: Consequence is visible before invocation
- **WHEN** an agent lists the available tools
- **THEN** each SHALL state its effect class

### Requirement: Irreversible operations require confirmation
An operation that cannot be undone — deleting an asset from disk, overwriting source content,
publishing, or invoking an external service — SHALL require **explicit human confirmation** when
invoked by an agent, unless the connection has been granted that effect class deliberately and for a
stated duration.

The confirmation SHALL state what will happen and what will be lost, per `editor-ui-ux`.

Reversible edits SHALL NOT require confirmation. Undo is the confirmation, and prompting for
ordinary work trains the human to approve without reading.

#### Scenario: A destructive act is not silent
- **WHEN** an agent attempts to delete an asset from disk
- **THEN** the human SHALL be asked, and the prompt SHALL state what is lost

#### Scenario: Ordinary edits are not gated
- **WHEN** an agent moves an object
- **THEN** no confirmation SHALL be required, because undo already covers it

### Requirement: An agent session is reproducible
An agent session SHALL be recorded as the sequence of commands it invoked, with their arguments and
their transaction identifiers.

The recording SHALL be replayable against the same starting state to produce the same result, for the
same reason `replay-and-rollback` makes one command log serve replay, rollback and lockstep: a
session that can be replayed can be reviewed, bisected, tested and reported as a defect.

A session recording SHALL be exportable and SHALL carry the project revision it began from.

#### Scenario: A bad session can be reproduced
- **WHEN** an agent produces an unwanted result
- **THEN** the session SHALL replay from the recorded starting revision and reproduce it

### Requirement: Agents hold a budget
An agent connection SHALL NOT be able to starve the editor. The interface SHALL bound the rate of
invocations, the cost of viewport renders, and the number of concurrent operations, and SHALL report
those limits to the agent rather than failing opaquely.

A viewport render requested by an agent SHALL be scheduled against the same frame budget the editor's
own viewport holds, and SHALL degrade rather than stall the interface.

#### Scenario: A loop cannot freeze the editor
- **WHEN** an agent issues requests as fast as it can
- **THEN** the editor SHALL remain interactive and the agent SHALL be told it is being throttled

### Requirement: Discovery is generated, and speaks the engine's vocabulary
Tool and resource descriptions SHALL be generated from command metadata and the reflection data that
already drives the generated inspector, so that a description cannot drift from the operation it
describes.

Descriptions SHALL use the engine's own vocabulary as `editor-visual-language` defines it — node and
entity rather than actor, graph rather than blueprint — and MAY carry the familiar term as an alias
so that an agent trained on another engine still finds the operation.

A description SHALL be written for a caller that cannot see the interface: it SHALL state what the
operation does and what its parameters mean, not where the menu item lives.

#### Scenario: A description cannot go stale
- **WHEN** a command's parameters change
- **THEN** its tool description SHALL change with it, without a separate edit

### Requirement: Agent activity is on the trace
Agent connections, invocations, refusals, transactions and viewport renders SHALL be recorded on the
engine's single trace with their privacy classification, per
`diagnostics-profiling-and-crash`.

An agent's contribution to editor frame cost SHALL be attributable, so that "the editor is slow" can
be answered with which agent and which operation.

#### Scenario: Agent cost is attributable
- **WHEN** the editor is profiled during an agent session
- **THEN** agent-induced work SHALL be distinguishable from the human's

### Requirement: Forbidden agent interface patterns
The following SHALL NOT appear, and each SHALL be checkable:

- A mutation path reachable by an agent that does not produce a transaction
- A hand-maintained list of exposed tools
- An agent-only capability, or a check an agent bypasses
- A second renderer, or a substituted representation, used to answer a viewport request
- A tool description written by hand beside the command it describes
- A manipulation implemented separately from the interactive one
- An operation whose effect class is undeclared
- An irreversible operation performed without confirmation or a deliberate grant
- A transaction with no recorded actor
- An agent connection that can render the editor unresponsive
- Another engine's vocabulary in a tool description
- A read that dirties a document or changes selection

#### Scenario: A proposal is checked
- **WHEN** a contributor adds an agent tool that writes a property directly for speed
- **THEN** it SHALL be flagged against this requirement, because undo, the journal, merge and
  attribution all silently stop working for that property
