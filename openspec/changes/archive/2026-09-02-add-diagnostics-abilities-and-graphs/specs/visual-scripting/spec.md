## ADDED Requirements

### Requirement: Shared infrastructure, domain-specific languages
Visual authoring SHALL be provided as **shared graph infrastructure** with **domain-specific
lowering**, not as one universal graph language.

The infrastructure SHALL own: node and pin models, typed connections, stable identity, serialization,
subgraphs, the editor canvas and its undo, diffing and merging, versioning and migration, and
debugging.

Existing domain graphs — materials, effects, animation, artificial intelligence, and camera rigs —
SHALL keep their **own intermediate representations and compilers**, because a material's algebra, a
particle kernel, a pose evaluation, and a behaviour program are different languages with different
type systems and execution models. Forcing them through one representation would make each worse.

This capability SHALL additionally provide **gameplay** and **ability** graph languages, lowering to
ECS systems and ability programs respectively.

Domain graphs SHALL adopt the shared infrastructure so that a project has one graph editing
experience, one diff format, and one debugging model.

#### Scenario: One editor, several languages
- **WHEN** a developer opens a material graph and a gameplay graph
- **THEN** both SHALL use the same canvas, identity model, diffing, and debugging, while compiling
  through different lowerings

#### Scenario: No universal representation
- **WHEN** a proposal would route material expressions and gameplay control flow through one
  intermediate representation
- **THEN** it SHALL be rejected against this requirement

### Requirement: Graphs are an authoring language
A graph SHALL be a **source representation** compiled to an executable program. It SHALL NOT be the
runtime object model.

There SHALL NOT be one interpreter instance, one graph object, or one node object per entity.

Per-entity graph state SHALL compile to **generated component data** with a declared layout, not to
hidden interpreter state.

A graph whose behaviour applies to many entities SHALL lower to a **system iterating chunks**, so that
a hundred thousand entities carrying one behaviour cost one system.

#### Scenario: A behaviour is a system
- **WHEN** a hundred thousand entities carry one graph behaviour
- **THEN** it SHALL execute as one generated system over archetypes

#### Scenario: State is data
- **WHEN** a graph holds state between events
- **THEN** that state SHALL be a generated component with a declared layout

### Requirement: Typed pins and connections
Pins SHALL carry **concrete types** — booleans, integers, floating-point values, vectors, entity
references, persistent references, asset handles, gameplay tags, identifiers, structures, arrays, and
optionals.

A universal variant type SHALL NOT be the default pin type. Type mismatches SHALL be **compile
errors**, reported with the node, the pin, the expected type, and the received type.

Implicit conversions SHALL be declared rather than inferred, so that a conversion is visible in the
graph.

#### Scenario: Errors are found before running
- **WHEN** an incompatible connection is made
- **THEN** it SHALL be rejected or reported at compile time, naming both pins and types

#### Scenario: Conversions are visible
- **WHEN** a value is converted between types
- **THEN** the conversion SHALL be an explicit element of the graph

### Requirement: Stable graph identity
Nodes and pins SHALL carry **stable identifiers** that survive editing, using the identity mechanism
in `core-type-system`.

Visual layout — positions, comments, collapsed state — SHALL be stored **separately from semantics**,
so that moving a node is not a semantic change.

Stable identity SHALL support: debugging and breakpoints, semantic diffing, three-way merging, hot
reload state migration, and node type version migration.

#### Scenario: Moving a node changes nothing
- **WHEN** a node is repositioned
- **THEN** the semantic representation SHALL be unchanged and the compiled program identical

#### Scenario: A breakpoint survives an edit
- **WHEN** a graph is edited around a node carrying a breakpoint
- **THEN** the breakpoint SHALL remain on that node

### Requirement: Events, not tick
The default execution model SHALL be **event-driven**: events, commands, queries, timers, and state
changes.

A per-frame or per-tick update SHALL be available and SHALL be **explicit**, declared rather than
implied, so that continuous evaluation is a deliberate choice.

Graph authoring guidance and the editor's node palette SHALL reflect this, so that the easy path is
the efficient one.

#### Scenario: The default is not a tick
- **WHEN** a developer creates a gameplay graph
- **THEN** its natural structure SHALL be responses to events rather than a per-frame update

#### Scenario: Continuous work is declared
- **WHEN** a graph genuinely needs per-tick evaluation
- **THEN** it SHALL declare it, and the cost SHALL be attributable

### Requirement: The graph intermediate representation
Graphs SHALL compile through a **typed intermediate representation** with basic blocks, explicit
control flow, typed values, field access, calls, event emission, command emission, queries, and
suspension points.

The compiler SHALL apply at minimum: type checking, control-flow validation, dead-node elimination,
constant folding, common subexpression elimination, branch simplification, query fusion, state layout
computation, asynchronous lowering, and access analysis.

Compilation SHALL produce, alongside the program: the **data access declaration** the task scheduler
needs, so that a graph system participates in dependency scheduling exactly as a handwritten one does.

The representation SHALL be inspectable, so a developer can see what a graph became.

#### Scenario: Scheduling comes from compilation
- **WHEN** a gameplay graph reads and writes components
- **THEN** the compiler SHALL emit its access declaration and the scheduler SHALL order it
  accordingly

#### Scenario: What did my graph become
- **WHEN** a developer inspects a compiled graph
- **THEN** the intermediate representation and the resulting program SHALL be viewable

### Requirement: Execution backends
The engine SHALL provide two execution backends from one intermediate representation:

| Backend | Purpose |
|---|---|
| **Bytecode** | Fast iteration, hot reload, stepping, sandboxed mod execution, portability |
| **Native** | Shipping performance, compiled ahead of time |

The bytecode virtual machine SHALL be a **typed register machine** with a shared program and separate
state. There SHALL NOT be one virtual machine instance per entity.

Selection SHALL be per graph and per build configuration, and a graph SHALL produce identical results
on either backend, which SHALL be verified.

#### Scenario: Iterate in bytecode, ship in native
- **WHEN** a developer edits a graph during play
- **THEN** the bytecode backend SHALL reload it immediately, and shipping SHALL use the compiled
  native path

#### Scenario: Backends agree
- **WHEN** a graph is executed on both backends
- **THEN** results SHALL be identical, and a divergence SHALL be a defect

### Requirement: Function and node metadata
Functions callable from graphs — engine, project, or plugin — SHALL declare metadata: their domain,
purity (pure, reads world, writes world, external, or asynchronous), determinism, thread safety, and
capability requirements.

The compiler SHALL use that metadata for validation, scheduling, optimisation, and capability
enforcement. A function without metadata SHALL NOT be callable from a graph.

Identifiers for types, fields, and functions SHALL be **resolved at compile time**. Runtime execution
SHALL NOT search by name.

#### Scenario: Purity enables optimisation
- **WHEN** a pure function's result is unused
- **THEN** the compiler SHALL eliminate the call

#### Scenario: No name lookup at runtime
- **WHEN** a compiled graph accesses a component field
- **THEN** it SHALL use a resolved binding rather than a name

### Requirement: Asynchronous graphs
Graphs SHALL support waiting: for ticks, for events, for animation markers, for asset availability,
for a path, or for a network response.

Waiting SHALL be lowered into an **explicit state machine with compact generated state**, and SHALL
not require a heap-allocated continuation per instance where that can be avoided.

References held across a suspension SHALL be **weak and revalidated on resume** — entity references,
persistent references, asset handles — never raw pointers, since the world may have changed.

Asynchronous graph state SHALL participate in snapshots so that rollback and save restore an
in-progress graph correctly.

#### Scenario: A door sequence survives a rollback
- **WHEN** a graph is waiting on an animation marker and a rollback occurs
- **THEN** its state SHALL be restored and resume correctly

#### Scenario: A destroyed target is handled
- **WHEN** a graph resumes and the entity it referenced no longer exists
- **THEN** the reference SHALL resolve as invalid rather than dereferencing freed data

### Requirement: Determinism auditing
Graphs SHALL be able to declare themselves **deterministic**, and the compiler SHALL **audit** them
against the rules in `simulation-and-determinism`: wall-clock reads, ambient randomness, iteration
over unordered containers as a decision order, reads of presentation-classified data, calls to
functions declared non-deterministic, and disallowed floating-point operations.

Violations SHALL be reported with the node responsible.

This audit SHALL be more thorough than review of handwritten code, because the compiler sees the
whole program — and that advantage SHALL be preserved as a design constraint on the language.

#### Scenario: A wall-clock read is rejected
- **WHEN** a deterministic graph calls a function that reads the system clock
- **THEN** compilation SHALL fail naming the node

#### Scenario: The compiler sees everything
- **WHEN** a deterministic graph reads a presentation-classified value indirectly
- **THEN** the audit SHALL still detect it

### Requirement: Capabilities
Every graph SHALL carry a **capability set** — gameplay, editor, asset mutation, file access,
network, and unsafe native — and the compiler SHALL reject calls outside it.

Graphs from untrusted sources, such as mods, SHALL be restricted to a limited capability set, enforced
at compile time and re-verified at load.

Capability restriction applies to graphs only. It SHALL NOT be described as sandboxing native code,
which `project-and-plugins` already states cannot be honestly promised in-process.

#### Scenario: A mod graph cannot write files
- **WHEN** a mod graph calls a file operation
- **THEN** compilation SHALL fail on the capability, and loading a graph claiming otherwise SHALL be
  refused

#### Scenario: The promise is bounded
- **WHEN** capability enforcement is described
- **THEN** it SHALL be described as applying to graphs, not as sandboxing native plugins

### Requirement: Semantic diff and merge
Graph source SHALL be stored in a **deterministic textual form** suitable for version control, with
stable identifiers and layout separated from semantics.

The engine SHALL provide a **semantic diff**: nodes added and removed, connections changed, defaults
changed, subgraph references changed, and layout changes marked as visual only.

**Three-way merge** SHALL operate on graph topology: non-overlapping changes merge; changes to the
same node or connection are reported as conflicts. Nothing SHALL be silently discarded.

Opaque binary source SHALL NOT be the only representation of a graph.

#### Scenario: Two developers edit one graph
- **WHEN** one adds a branch and another changes a default value elsewhere
- **THEN** the changes SHALL merge automatically

#### Scenario: Moving nodes does not conflict
- **WHEN** two developers rearrange a graph's layout differently
- **THEN** the layout difference SHALL not be a semantic conflict

### Requirement: Hot reload and state migration
Editing a graph in development SHALL recompile it and publish a new program generation, with
existing instances migrated according to a declared policy: preserve state, reset graph state,
restart the behaviour, or refuse the reload.

Generated state layout changes SHALL be migrated using the schema mechanism where safe, and reported
where not.

Hot reload of a graph SHALL increment the simulation epoch where the session's determinism profile
requires it, consistent with `simulation-and-determinism`.

#### Scenario: Editing a live behaviour
- **WHEN** a graph is edited while the game runs
- **THEN** instances SHALL migrate by the declared policy, and an unmigratable change SHALL be
  reported rather than silently resetting state

### Requirement: Node versioning and missing nodes
Node types SHALL carry stable identity and a version, and their owning module or plugin SHALL be
recorded.

A node type change SHALL be migratable: the owner SHALL supply a migration for parameters and
connections, applied when a graph authored against an older version is loaded.

A graph referencing a node type whose plugin is **absent** SHALL preserve that node's data opaquely
and display it as missing. **Loading SHALL NOT destroy graph content**, and re-enabling the plugin
SHALL restore the node.

#### Scenario: A disabled plugin does not destroy a graph
- **WHEN** a graph using a plugin's nodes is opened without that plugin
- **THEN** those nodes SHALL be preserved opaquely and shown as missing

#### Scenario: A node evolves
- **WHEN** a node type gains a parameter
- **THEN** existing graphs SHALL migrate through the owner's migration

### Requirement: Graph debugging
Debugging SHALL work against **compiled** graphs, not only interpreted ones: the compiler SHALL emit
a map from program locations to node and pin identity.

The debugger SHALL support: breakpoints, stepping, watching pin values, inspecting graph state,
viewing the executing entity or context, and tracing execution history.

For networked and predicted graphs, the debugger SHALL be able to show **client-predicted and
authoritative execution side by side**, with the divergence highlighted.

Execution SHALL be profileable per node — invocations, time, entities processed, query cost — and
presentable as a heat map over the graph.

#### Scenario: Debugging a shipping-style build
- **WHEN** a graph compiled natively misbehaves
- **THEN** breakpoints and value inspection SHALL work through the emitted debug map

#### Scenario: Prediction divergence is visible
- **WHEN** a predicted graph diverges from the authority
- **THEN** both executions SHALL be viewable with the differing point identified

### Requirement: Compiler-grade diagnostics
Graph errors SHALL be reported with the precision of a compiler: the node, the pin, the expected and
received types or values, and the path through the graph that produced the situation.

"The graph failed" SHALL NOT be an acceptable diagnostic.

Warnings SHALL be provided for likely defects: unreachable nodes, unused outputs, per-tick evaluation
where an event would serve, unbounded loops, and expensive calls in hot paths.

#### Scenario: An error names the pin
- **WHEN** a type error exists
- **THEN** the diagnostic SHALL name the node, the pin, the expected and received types, and the path
  that reached it

### Requirement: Graph testing
Graphs SHALL support **tests** expressed as given-when-then cases over inputs, world state, and
expected outputs, runnable headlessly in continuous integration.

The editor SHALL support executing a graph against a mocked context — a synthetic entity, tags,
attributes, and events — without launching the game.

Graph tests SHALL run in the same execution path as production, so a passing test means the compiled
program behaves correctly rather than an editor simulation of it.

#### Scenario: A graph has tests
- **WHEN** a gameplay graph is authored
- **THEN** its cases SHALL be runnable headlessly and in continuous integration

#### Scenario: Testing does not use a different path
- **WHEN** a graph test runs
- **THEN** it SHALL execute the compiled program, not an editor-only interpreter

### Requirement: Interoperability
Graphs SHALL coexist with native and Swift code within one project and one gameplay feature, using
the same ECS, commands, tags, scheduler, schema, and identity.

A graph system, a native system, and a Swift system SHALL be schedulable together by declared access,
with no ordering or data-access mechanism unique to graphs.

Graph-authored behaviour SHALL be replaceable by handwritten code without changing anything that
consumes it, and vice versa.

#### Scenario: Mixed authoring in one feature
- **WHEN** a feature contains native systems, Swift systems, and graph systems
- **THEN** all SHALL be scheduled by declared access and share one world

#### Scenario: Rewriting a graph in code changes nothing else
- **WHEN** a graph behaviour is reimplemented natively
- **THEN** its consumers SHALL be unaffected

### Requirement: Graph performance
Graph execution SHALL approach the performance of generated handwritten systems for common
operations: chunk iteration, component access, tag tests, and command emission.

The engine SHALL support **a hundred thousand behaviour-bearing entities** without one graph object,
one virtual machine instance, or one virtual update per entity.

Graph execution cost SHALL be attributable per graph and per node, so an expensive behaviour is
identifiable rather than a diffuse cost.

#### Scenario: Scale without instances
- **WHEN** a hundred thousand entities carry graph behaviours
- **THEN** execution SHALL be systems over shared programs, with per-entity cost limited to state

### Requirement: Forbidden visual scripting patterns
The following SHALL NOT appear, and each SHALL be checkable:

- A graph used as the authoritative runtime object model
- One interpreter, virtual machine, or graph object instance per entity
- String-based reflection or name lookup in graph execution
- A per-frame update as the default authoring idiom
- A deterministic graph reaching non-deterministic functionality without a compile error
- Visual layout changes producing semantic differences
- Loading a graph with missing plugin nodes destroying its content
- The graph editor owning the runtime execution engine
- A universal intermediate representation forced onto domain languages

#### Scenario: A proposal is checked
- **WHEN** a change would instantiate a virtual machine per entity
- **THEN** it SHALL be flagged against this requirement
