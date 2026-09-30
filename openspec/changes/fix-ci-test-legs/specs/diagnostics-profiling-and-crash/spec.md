## MODIFIED Requirements

### Requirement: Crash artefacts
On a crash, fatal assertion, or unrecoverable fault, the engine SHALL write a **crash artefact**
containing: build identity, platform and system information, process and thread state, the
exception or signal, a stack trace with module identities and offsets, the module list, the trace
tail, breadcrumbs, the log tail, health state, and a reference to any reproduction artefact.

The artefact SHALL be **self-contained and symbol-independent**: it SHALL be usable on a machine with
no symbols, and symbolication SHALL occur later against the symbols archived by the build system.

Crash capture SHALL NOT require the editor, a network connection, or a debugger to be present.

The capture path SHALL be defensive: it SHALL assume the process is damaged and SHALL avoid
allocation, locks, and subsystem re-entry where possible.

#### Scenario: A player's crash is diagnosable
- **WHEN** a shipping build crashes on a player's machine
- **THEN** the artefact SHALL identify the build and carry offsets that symbolicate against archived
  symbols

#### Scenario: No editor required
- **WHEN** a dedicated server crashes
- **THEN** the artefact SHALL be written without any tool attached

#### Scenario: The stack trace is the faulting thread's on every desktop platform
- **WHEN** a process faults on Linux or macOS and the handler runs on its alternate signal stack
- **THEN** the artefact's backtrace SHALL list the interrupted thread's frames, each with its module
  and offset, rather than stating that no backtrace is available
