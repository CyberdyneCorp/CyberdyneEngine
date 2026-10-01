# physics Spec Delta

## MODIFIED Requirements

### Requirement: Determinism
Physics SHALL declare a **determinism policy** rather than one blanket guarantee, and a session's
determinism profile (see `simulation-and-determinism`) SHALL be validated against it:

| Policy | Meaning |
|---|---|
| `SamePlatformDeterministic` | The same binary, architecture, and inputs reproduce identical results |
| `ExternalAuthority` | Physics results are authoritative only where an authority produces them and replicates or records them |
| `NonAuthoritative` | Physics is presentation only and is excluded from authoritative state |

The default policy SHALL be `SamePlatformDeterministic`: the same initial state and the same
per-tick inputs SHALL produce identical results on the same binary and platform.

Cross-platform determinism SHALL NOT be guaranteed, and this SHALL be documented, since it depends
on floating-point behaviour across architectures. This holds even though the backend is built with
its cross-platform determinism switch on. That switch is enabled for its same-platform effect. A
cross-platform claim for physics SHALL require two things:

- a measured agreement of per-tick physics hashes across every cross-architecture continuous
  integration leg;
- its own change to this requirement.

This limitation is **load-bearing for networking**. Cross-platform `Lockstep` (see
`networking-and-replication`) requires bit-identical simulation across every participating peer.
Authoritative movement under it SHALL therefore use the deterministic math path defined in
`deterministic-math`, and physics SHALL be classified `NonAuthoritative` and used for debris,
ragdolls, and secondary effects outside the deterministic core. Same-platform lockstep MAY keep
physics authoritative within its verified compatibility scope.

A session declaring `CrossPlatform` or `Lockstep` while treating physics as authoritative SHALL be
rejected at configuration time.

The engine SHALL provide a determinism test mode that hashes world state per tick to detect
divergence.

#### Scenario: Replay reproduces a session
- **WHEN** a recorded input sequence is replayed on the same build and platform
- **THEN** the simulation SHALL reproduce the original result

#### Scenario: Divergence is detected
- **WHEN** determinism test mode runs and a hash mismatches at tick N
- **THEN** the tick number and the diverging body SHALL be reported

#### Scenario: Lockstep scope follows from this guarantee
- **WHEN** a same-platform lockstep session with authoritative physics is formed
- **THEN** participants SHALL be verified to share platform, architecture, and build, because
  physics determinism is guaranteed only within that scope

#### Scenario: Cross-platform lockstep excludes authoritative physics
- **WHEN** a session requires cross-platform determinism
- **THEN** physics SHALL be classified non-authoritative and authoritative movement SHALL use the
  deterministic math path, or the configuration SHALL be rejected

#### Scenario: A cross-platform measurement is not a claim
- **WHEN** the cross-architecture legs publish a physics scene's per-tick hash and the hashes agree
- **THEN** the physics policy SHALL remain `SamePlatformDeterministic` until a change to this
  requirement makes the claim
