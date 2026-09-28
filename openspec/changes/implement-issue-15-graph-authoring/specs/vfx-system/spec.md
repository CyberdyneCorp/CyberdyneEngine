# vfx-system Spec Delta

## ADDED Requirements

### Requirement: VFX systems are authored through the shared graph editor
The editor SHALL author VFX systems containing emitters, spawn/update/event/render stage graphs, and reusable module graph assets on its shared graph canvas. Its node palette SHALL come from the compiler's registered node catalogue and SHALL include typed data-interface and renderer definitions. Unsupported renderer kinds SHALL have a named reason.

#### Scenario: Compiler registry gains a node
- **WHEN** a compatible backend registers an additional VFX node
- **THEN** the editor SHALL offer it without a hand-maintained Rust node entry

#### Scenario: A stage references a saved module
- **WHEN** an emitter references a separately saved module graph asset
- **THEN** the cook SHALL resolve it through the project, validate its stage and typed inputs against the VFX node registry, compile its nodes through the existing compiler, include its content (transitively, not its name or path) in the cook identity, and declare it as a build-graph dependency of the system

#### Scenario: A module cannot be used
- **WHEN** a referenced module is missing, part of a cycle, or incompatible with its stage
- **THEN** the cook SHALL fail with a diagnostic naming the module and the referencing emitter rather than omitting it

#### Scenario: Two-emitter asset survives cooking
- **WHEN** an author saves and reopens a system with one CPU and one GPU emitter
- **THEN** both stage graphs SHALL cook and render with their authored settings

### Requirement: VFX changes use the engine compiler and runtime
Graph edits SHALL compile through `cy::vfx-compiler`; user parameter changes SHALL update a running instance without recompiling. Compiler diagnostics SHALL retain their text and offending node identity. The editor SHALL expose the compiler's attribute layout and generated source on demand.

#### Scenario: Invalid graph
- **WHEN** a VFX stage contains a compiler error
- **THEN** the offending node SHALL display the compiler diagnostic

#### Scenario: Parameter and graph edits
- **WHEN** an author changes a user parameter and then changes a graph node
- **THEN** only the graph change SHALL trigger compilation

### Requirement: Authored parameters reach scene instances and Swift gameplay
The editor SHALL author typed system parameters shared by emitters and typed emitter-local
parameters. It SHALL expose their defaults and instance overrides in the Inspector, persist scene
effect bindings and overrides through save/reopen and cook, and apply a changed override to only the
selected running instance without recompilation. The engine SHALL expose parameter set and get for
scene effect instances through its append-only C ABI and generated Swift overlay.

#### Scenario: Two scene instances use different values
- **WHEN** a scene places two instances of the same cooked VFX system and overrides an exposed
  parameter on one instance
- **THEN** only that instance SHALL use the override after save/reopen and runtime load; the other
  instance SHALL retain the system or emitter default

#### Scenario: Swift changes a live emitter parameter
- **WHEN** Swift gameplay sets an exposed emitter-local parameter on one effect instance
- **THEN** that instance SHALL update without recompiling, and a typed read through the same ABI
  SHALL return the new value while another instance remains unchanged

#### Scenario: Invalid integer parameter value
- **WHEN** an author enters a fractional value or a value outside the signed 32-bit range for an
  integer parameter default or scene instance override
- **THEN** the editor SHALL refuse the value and preserve the previous authored value

### Requirement: VFX preview and debugging are engine-backed
The editor SHALL provide play, pause, restart, scrub, and time-scale controls for an engine VFX preview. It SHALL report bounded per-emitter particle counts, budget/degradation state, event traffic, and one-particle attribute readback. VFX edits SHALL support undo/redo and equivalent MCP commands.

#### Scenario: Preview inspection
- **WHEN** an effect is playing in the editor
- **THEN** its preview and debug values SHALL come from the engine runtime
