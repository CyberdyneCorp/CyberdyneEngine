## MODIFIED Requirements

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
