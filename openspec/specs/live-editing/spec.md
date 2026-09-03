# live-editing Specification

## Purpose

Defines how an edit reaches a running world — in the editor's process, in a separate process, or on
a console across the room — without pretending that authoring state and simulation state are the
same thing.

Changes pass through a **live edit compiler** that produces a validated runtime delta. Every field
and component declares a **live edit policy** saying what its change means for a running world:
applied immediately, component reinitialised, entity recreated, asset reloaded, world restarted, or
not supported — and the editor reports which applies *before* the user acts.

What is preserved comes from the **field classification** introduced with the foundations work,
which until now had no consumer: authoring fields update, runtime state survives. Raising a
character's maximum health while it stands at 53 leaves it at 53, because the schema says which is
which rather than because the code was careful.

The same rigour applies to what is *not* promised. Data, asset and shader reload are specified fully;
native module reload is gated on type ownership and must be declared by the module, because a module
cannot unload while instances of its types are live. Pretending otherwise produces a feature that
works until it corrupts something.

All three play modes drive one **live bridge**, with in-process operation an optimisation of
transport rather than a separate architecture — which is why remote inspection of a console costs
nothing extra.

## Requirements

### Requirement: Live editing is a compilation step
Changes made in the editor SHALL reach a running world through a **live edit compiler** that
translates an authoring delta into a validated runtime delta.

The compiler SHALL NOT assume authoring state and runtime state are the same. It SHALL determine
what the change means for a running world, whether it can be applied, and what must be preserved.

Arbitrary mutation of a running world's objects from editor code SHALL NOT be a supported path.

#### Scenario: A change is translated, not copied
- **WHEN** a designer edits an authoring value
- **THEN** the live edit compiler SHALL produce a runtime delta, rather than the editor writing into
  runtime objects

#### Scenario: An unrepresentable change is refused
- **WHEN** an authoring change cannot be expressed as a runtime delta
- **THEN** it SHALL be reported with the reason and the required action, rather than partially
  applied

### Requirement: Live edit policy
Every component and field SHALL declare a **live edit policy** determining how a change reaches a
running world:

| Policy | Behaviour |
|---|---|
| `Immediate` | Applied to running instances directly |
| `ReinitializeComponent` | The component is torn down and rebuilt from new data |
| `RecreateEntity` | The entity is recreated, preserving identity |
| `ReloadAsset` | The referenced asset is reloaded and rebound |
| `RestartWorld` | Requires ending and restarting play |
| `Unsupported` | Cannot be live edited; the change applies on the next run |

Policy SHALL be derivable by default from the field's classification and the component's nature, and
overridable per field.

The editor SHALL report the applicable policy **before** the user acts, so that a change requiring a
restart is known in advance rather than discovered.

#### Scenario: Light intensity applies immediately
- **WHEN** a light's intensity is changed during play
- **THEN** it SHALL apply immediately to running instances

#### Scenario: A restart requirement is announced
- **WHEN** a change requires restarting the world
- **THEN** the editor SHALL say so before applying it

### Requirement: Runtime state is preserved
Live edits SHALL be governed by the **field classification** defined in `serialization-and-prefabs`:
`Authoring` fields are updated from the new data, `RuntimeState` and `PersistentState` are preserved,
and `Derived` fields are recomputed.

An authoring change SHALL NOT reset simulation state as a side effect.

Where a change cannot preserve runtime state — a component whose state has no counterpart in the new
shape — the loss SHALL be reported and, by configuration, require confirmation rather than occurring
silently.

#### Scenario: Health survives a prefab edit
- **WHEN** a designer raises a character's maximum health while it is at 53 of 100
- **THEN** the maximum SHALL update and the current value SHALL remain 53

#### Scenario: Unpreservable state is reported
- **WHEN** a component is restructured such that its runtime state cannot be carried over
- **THEN** the loss SHALL be reported rather than applied silently

### Requirement: Live asset reload
Assets SHALL be live-reloadable: recompiled or reimported content SHALL replace the resource behind
an existing **stable handle**, so holders need not re-resolve.

Replacement SHALL occur at a **safe epoch** using the retirement mechanism in
`core-memory-and-containers`: the previous resource is retired and released only when no in-flight
frame or task can still reference it.

Consumers SHALL NOT retain raw resource pointers across frames; they hold handles.

A failed reload SHALL keep the previous resource and report the failure.

#### Scenario: A texture reload is invisible
- **WHEN** a texture is reimported while the game runs
- **THEN** the new content SHALL appear without any holder re-resolving its handle

#### Scenario: The old resource is not freed early
- **WHEN** a resource is replaced while the GPU may still be reading it
- **THEN** it SHALL be retired and released only after the relevant epoch has passed

### Requirement: Shader and material live reload
Shader and material changes SHALL compile asynchronously on job workers, build their pipeline
states, be validated, and then be published.

A **failed compile SHALL keep the previous working pipeline** and report the error. A valid pipeline
SHALL NOT be replaced by a broken one.

Material parameter changes SHALL apply without recompilation, since parameters are runtime data;
only structural changes SHALL trigger compilation.

#### Scenario: Live shader iteration
- **WHEN** a shader is edited and saved
- **THEN** the change SHALL appear within a frame or two, with no restart and no stall

#### Scenario: A broken shader does not break the frame
- **WHEN** an edit does not compile
- **THEN** the last working pipeline SHALL continue rendering and the error SHALL be shown

### Requirement: Module hot reload is a declared capability
Native and script module reload SHALL be supported only where a module **declares** it, and SHALL
NOT be assumed.

Reload SHALL be gated on **type ownership** (see `core-type-system` and `project-and-plugins`): a
module may unload only when no live instances of the types it owns remain, or when it supplies a
declared migration for them.

The reload sequence SHALL be: quiesce the module's tasks, serialise migratable state, unregister its
types and callbacks, load the new binary, re-register, migrate state, resume.

A module that does not support reload SHALL require a restart, and the editor SHALL say so rather
than attempting it.

#### Scenario: Gameplay module reload
- **WHEN** a reloadable game module is rebuilt
- **THEN** its types SHALL be unregistered and re-registered, declared state migrated, and play
  SHALL continue

#### Scenario: Live instances block unload
- **WHEN** instances of a module's types exist and no migration is declared
- **THEN** reload SHALL be refused with a diagnostic naming the types, rather than leaving dangling
  instances

### Requirement: Play modes
The engine SHALL support three play modes:

| Mode | Purpose | Runtime location |
|---|---|---|
| `InEditor` | Fast iteration | A runtime world in the editor's hosted runtime process |
| `SeparateProcess` | Closer to shipping behaviour; isolates editor state from runtime behaviour | A second runtime process |
| `RemoteDevice` | The game runs on a console, phone, tablet, or another machine | A remote runtime |

Because the editor is a separate Rust application (see `editor-rust-application`), **no play mode
runs in the editor process**. `InEditor` denotes iteration speed and shared runtime state, not
co-location with the editor.

All three SHALL be driven through the **same live bridge interface**. Locality SHALL be an
optimisation of transport, not a different architecture, so that remote play requires no separate
implementation.

Play mode SHALL support pause, single frame step, and single simulation tick step in every mode where
the runtime permits.

Entering and leaving play SHALL leave the authoring document exactly as it was, as already required
by `editor-architecture`.

A runtime failure in any mode SHALL leave the editor running, as required by
`editor-rust-application`.

#### Scenario: The remote case is not an afterthought
- **WHEN** the game runs on a console
- **THEN** live editing, inspection, and profiling SHALL work through the same interface used for
  local play

#### Scenario: Standalone behaviour is honest
- **WHEN** separate-process play is used
- **THEN** editor-only state SHALL be absent from the runtime, so editor-specific behaviour cannot
  mask a defect

#### Scenario: A crash during play does not lose editor state
- **WHEN** the runtime crashes in any play mode
- **THEN** the editor SHALL remain running with its documents and transaction journal intact

### Requirement: Live bridge protocol
Communication between the editor and a running world SHALL use a **versioned message protocol**
covering at minimum: asset changed, entity and component deltas, prefab recompiled, layer state
change, console command, profiler request and response, selection highlight, camera synchronisation,
and runtime inspection queries.

The protocol SHALL be schema-versioned and SHALL verify compatibility on connection, so a mismatched
editor and runtime are rejected rather than misinterpreting messages.

Bulk data MAY use a shared-memory or streaming transport while control messages remain
message-based, and transport choice SHALL NOT change the protocol.

The protocol SHALL be usable by tools other than the editor.

#### Scenario: Mismatched versions are rejected
- **WHEN** an editor connects to a runtime built against an older protocol
- **THEN** the mismatch SHALL be detected at connection and reported

#### Scenario: A tool uses the bridge
- **WHEN** a command-line tool needs runtime state
- **THEN** it SHALL use the same protocol rather than a separate mechanism

### Requirement: Runtime inspection
A running world SHALL be inspectable through the bridge: entity and component listings, resource and
memory usage by domain, task and frame timing, GPU scene contents, world cell and residency state,
system schedules, and log output.

Inspection SHALL be **read-only by default**, with mutation requiring an explicit mode.

Inspection SHALL work identically for in-process, separate-process, and remote runtimes, since
debugging a console is where it is most needed.

#### Scenario: Console debugging
- **WHEN** a defect appears only on a console
- **THEN** the editor SHALL inspect that runtime's entities, memory, and timings remotely

#### Scenario: Inspection does not mutate
- **WHEN** a developer browses runtime state
- **THEN** nothing SHALL be modified unless the explicit mutation mode is enabled

### Requirement: Runtime tweaking is distinct from authoring
Changing a value in a running world SHALL be visibly distinct from editing the project, and SHALL
NOT dirty an authoring document.

Runtime changes SHALL be applied to the running world only, and their transient nature SHALL be
clear in the interface.

An explicit action SHALL promote a runtime value into the authoring document, recorded as a normal
transaction.

At the end of play, a **keep changes** flow SHALL compare final runtime state against what the
authoring data produced and offer the differences — restricted to fields classified `Authoring`.
Runtime state SHALL NOT be offered, because it was never authored.

#### Scenario: Debug tweak does not dirty the project
- **WHEN** a developer changes a value while playing
- **THEN** the authoring document SHALL remain clean

#### Scenario: Keeping a change
- **WHEN** an object was moved during play and play ends
- **THEN** its transform SHALL be offered for keeping, while its current health SHALL not

### Requirement: Live editing diagnostics
The system SHALL report: pending and applied live edits, the policy applied to each, edits refused
with their reason, state that could not be preserved, reload failures, protocol version and
connection state, and live edit latency.

When a live edit does not visibly take effect, the developer SHALL be able to determine whether it
was refused, deferred, applied to a different instance set, or overwritten by simulation.

#### Scenario: Why did my change not appear
- **WHEN** an edit appears to have no effect
- **THEN** the diagnostics SHALL state whether it was refused, deferred, or applied and then
  overwritten by running simulation
