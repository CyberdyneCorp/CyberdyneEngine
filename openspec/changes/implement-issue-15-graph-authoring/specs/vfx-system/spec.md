# vfx-system Spec Delta

## ADDED Requirements

### Requirement: VFX systems are authored through the shared graph editor
The editor SHALL author VFX systems containing emitters, spawn/update/event/render stage graphs, and reusable module graph assets on its shared graph canvas. Its node palette SHALL come from the compiler's registered node catalogue and SHALL include typed data-interface and renderer definitions. Unsupported renderer kinds SHALL have a named reason.

#### Scenario: Compiler registry gains a node
- **WHEN** a compatible backend registers an additional VFX node
- **THEN** the editor SHALL offer it without a hand-maintained Rust node entry

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

### Requirement: VFX preview and debugging are engine-backed
The editor SHALL provide play, pause, restart, scrub, and time-scale controls for an engine VFX preview. It SHALL report bounded per-emitter particle counts, budget/degradation state, event traffic, and one-particle attribute readback. VFX edits SHALL support undo/redo and equivalent MCP commands.

#### Scenario: Preview inspection
- **WHEN** an effect is playing in the editor
- **THEN** its preview and debug values SHALL come from the engine runtime
