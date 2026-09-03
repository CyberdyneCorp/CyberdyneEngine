## MODIFIED Requirements

### Requirement: The plugin binary boundary is the engine's C ABI
Externally distributed **binary** plugins SHALL cross the existing stable C ABI defined in
`native-abi`. A second plugin ABI SHALL NOT be introduced.

The boundary SHALL expose no standard library types, no third-party types, and no engine internal
layouts. A plugin SHALL be discovered through a documented descriptor entry point taking the host's
API version.

**First-party modules built together with the engine** MAY use native C++ interfaces, since ABI
stability across versions is unnecessary when everything is compiled from one source tree.

Plugins distributed as **source** MAY use the C++ interfaces of their declared dependencies, and
SHALL be rebuilt with the engine.

The same rule applies to the editor, which is a separate Rust application: **Rust's native binary
interface SHALL NOT be a plugin boundary**, because it is not stable across compiler versions or
build configurations. A binary editor plugin SHALL cross the engine's C ABI or a process protocol; a
Rust editor plugin distributed as **source** MAY use the editor SDK crates directly and SHALL be
rebuilt with the editor.

The editor's interface toolkit SHALL NOT appear in any plugin-facing interface, in either form.

#### Scenario: One ABI, two audiences
- **WHEN** a binary plugin is distributed
- **THEN** it SHALL use the engine's existing C ABI, and no parallel plugin ABI SHALL exist to keep
  compatible

#### Scenario: No implementation detail in the boundary
- **WHEN** a binary plugin interface is defined
- **THEN** it SHALL contain no standard library or third-party types

#### Scenario: The Rust interface is not a contract
- **WHEN** an editor plugin is distributed as a compiled Rust library
- **THEN** it SHALL be rejected as a boundary, and the plugin SHALL cross the C ABI, a protocol, or
  be distributed as source
