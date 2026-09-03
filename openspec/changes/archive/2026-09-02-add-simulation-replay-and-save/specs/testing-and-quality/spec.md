## MODIFIED Requirements

### Requirement: Determinism tests
The engine SHALL verify that, for each determinism profile a project declares (see
`simulation-and-determinism`):

- running the same simulation twice produces identical state hashes per tick
- results are identical across **different worker counts** and under **chaos scheduling**, since
  both expose undeclared ordering dependencies
- re-simulation during network reconciliation reproduces the original result
- physics produces identical results for the same inputs within its declared policy
- for `CrossPlatform`, results agree across the platforms the project targets

State hashing SHALL be **hierarchical**, isolating the first diverging tick and narrowing to the
entity, component, and field that differ.

The suite SHALL maintain **golden replays**: recorded sessions with committed expected hashes,
replayed in continuous integration. An intentional behaviour change SHALL update them in the same
change with a recorded justification, so a determinism regression and a deliberate change are
distinguishable.

**Replay and save fuzzing** SHALL be included: generated command streams recorded and replayed with
hash comparison, and malformed saves — truncated chunks, corrupt hashes, unknown fields, older
schemas, missing plugins, duplicate identities — which SHALL fail diagnostically and SHALL NEVER
crash.

**Transactional save tests** SHALL simulate failure after each write phase and verify the previous
save remains valid.

#### Scenario: Non-determinism introduced
- **WHEN** a change makes system execution order depend on thread timing
- **THEN** the determinism test SHALL fail identifying the first diverging tick

#### Scenario: Hash granularity
- **WHEN** divergence is detected
- **THEN** the report SHALL narrow to the entity and component that differ, not merely report that
  hashes differ

#### Scenario: A player session becomes a test
- **WHEN** a recorded session exposes a defect
- **THEN** it SHALL be addable as a golden replay with expected hashes

#### Scenario: A malformed save never crashes
- **WHEN** a fuzzed save is loaded
- **THEN** it SHALL fail with a structured diagnostic
