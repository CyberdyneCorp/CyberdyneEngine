## MODIFIED Requirements

### Requirement: Code generation
The build SHALL generate, as explicit build steps with declared inputs and outputs:

- **reflection metadata, serialization code, component registration, editor metadata, replication
  schema inputs, and binding descriptors** — all from annotated declarations, by the reflection
  generator (see `core-type-system`), which SHALL parse C++ with a real compiler frontend rather
  than a bespoke text scanner
- the **identity manifest** updates for newly declared types and fields, which SHALL be written to
  the source tree and committed, not to the build directory
- the C ABI headers and the machine-readable ABI description, from the ABI definition
- the Swift overlay, from the ABI description
- shader artefacts, from Slang sources
- the module registration table, from enabled modules
- version and build metadata (version, git hash, build configuration, timestamp)
- the default theme and editor icon atlases

Generated files SHALL be written to the build directory, except the Swift overlay and the identity
manifest, which SHALL be committed — the overlay so consumers need no generator, the manifest
because it is the authoritative record of persistent identity and its changes must be reviewable.

Generation SHALL be **deterministic**: identical inputs SHALL produce byte-identical outputs, so
generated artefacts do not churn in builds or defeat caches.

Continuous integration SHALL fail when the committed identity manifest would change in a way that
alters an existing identifier (see `core-type-system`).

#### Scenario: Incremental correctness
- **WHEN** an input to a generator changes
- **THEN** only the affected generated files and their dependents SHALL rebuild

#### Scenario: Generation does not churn
- **WHEN** the build runs twice without source changes
- **THEN** generated outputs SHALL be byte-identical and no rebuild SHALL be triggered

#### Scenario: Identity changes are gated
- **WHEN** a change would alter an existing identifier in the manifest
- **THEN** continuous integration SHALL fail, naming the entry
