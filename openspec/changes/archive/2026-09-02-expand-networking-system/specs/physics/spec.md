## MODIFIED Requirements

### Requirement: Determinism
Physics SHALL be deterministic for a fixed sequence of inputs on the same binary and platform:
the same initial state and the same per-tick inputs SHALL produce identical results.

Cross-platform determinism SHALL NOT be guaranteed by default, and this SHALL be documented,
since it depends on floating-point behaviour across architectures.

This limitation is **load-bearing for networking**: `Lockstep` mode (see
`networking-and-replication`) requires bit-identical simulation across every participating peer,
and therefore SHALL be restricted to peers sharing a platform, architecture, and binary build.
Supporting cross-platform lockstep would require a fixed-point or soft-float simulation path,
which the engine does not provide and which is recorded as deferred rather than planned.

The engine SHALL provide a determinism test mode that hashes world state per tick to detect
divergence.

#### Scenario: Replay reproduces a session
- **WHEN** a recorded input sequence is replayed on the same build and platform
- **THEN** the simulation SHALL reproduce the original result

#### Scenario: Divergence is detected
- **WHEN** determinism test mode runs and a hash mismatches at tick N
- **THEN** the tick number and the diverging body SHALL be reported

#### Scenario: Lockstep scope follows from this guarantee
- **WHEN** a lockstep session is formed
- **THEN** participants SHALL be verified to share platform, architecture, and build, because
  physics determinism is guaranteed only within that scope
