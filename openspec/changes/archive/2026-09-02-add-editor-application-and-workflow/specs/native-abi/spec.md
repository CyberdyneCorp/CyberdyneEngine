## ADDED Requirements

### Requirement: Rust SDK overlay
The engine SHALL provide a **Rust overlay over the C ABI** — the editor SDK — alongside the Swift
overlay, produced from the **same ABI description** so that the two overlays and the ABI cannot
drift.

The Rust overlay SHALL present safe, idiomatic types: generation-checked handles rather than raw
pointers, typed results rather than sentinel return codes, owned and borrowed string and buffer types
with explicit encoding and lifetime, and registered callbacks rather than arbitrary function
pointers.

`unsafe` code SHALL be confined to the overlay crate; consumers of the overlay SHALL require none.

The overlay SHALL be consumable by tools other than the editor, and SHALL not depend on any
interface toolkit.

Regeneration SHALL be deterministic, and a mismatch between the committed overlay and regeneration
SHALL fail continuous integration, as required by `developer-workflow-and-just`.

#### Scenario: Two overlays, one description
- **WHEN** a function is added to the C ABI
- **THEN** both the Swift and the Rust overlays SHALL be regenerated from the same description, and
  a stale overlay SHALL fail the build

#### Scenario: Safety stops at the overlay
- **WHEN** editor code calls the engine
- **THEN** it SHALL do so through safe Rust types, and no `unsafe` SHALL be required outside the
  overlay crate

#### Scenario: The overlay carries no interface dependency
- **WHEN** a command-line tool links the overlay
- **THEN** it SHALL not pull in any interface toolkit
