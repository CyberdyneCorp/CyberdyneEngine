# networking-and-replication Spec Delta

## MODIFIED Requirements

### Requirement: Lockstep requirements and limits
`Lockstep` mode SHALL require all of the following:

- a fixed simulation timestep;
- deterministic physics;
- deterministic AI scheduling;
- deterministic animation root motion;
- seeded random state that is part of simulation state;
- deterministic ECS system ordering;
- exclusion of every subsystem declared non-deterministic (VFX, non-pinned ML inference, adaptive
  budget controllers).

A lockstep session SHALL declare a **determinism profile**, and the profile SHALL decide its
compatibility scope:

- **Same-platform lockstep** (profile `SamePlatform`) SHALL declare its scope as platform,
  architecture, and binary build identifier. The engine SHALL verify that every participant matches
  before the session starts. This form remains available to projects whose authoritative subsystems
  compute in floating point.
- **Cross-platform lockstep** (profile `Lockstep`) SHALL be supported only for sessions that pass
  configuration under `simulation-and-determinism`. Every authoritative computation uses the
  deterministic math types of `deterministic-math`, and physics is non-authoritative unless a
  physics policy claims cross-platform determinism. Its scope SHALL be a **simulation identity**:
  the project source revision, the deterministic math kernel version, the random mixer version, and
  the content manifest hash, together with the replication schema set and the profile. Platform,
  architecture, and binary build SHALL NOT be compared.

Lockstep sessions SHALL exchange periodic **state hashes** so divergence is detected within a
bounded number of ticks. A divergence SHALL be reported with the diverging tick and, where
determinable, the diverging system.

#### Scenario: Mismatched participants are rejected
- **WHEN** a peer attempts to join a same-platform lockstep session with a different build or
  platform
- **THEN** it SHALL be rejected at join time with the mismatch named

#### Scenario: A different architecture joins a cross-platform session
- **WHEN** an arm64 peer joins a `Lockstep`-profile session hosted on x86-64, with the same
  simulation identity
- **THEN** it SHALL be admitted, and a peer whose deterministic math kernel version differs SHALL be
  refused naming the simulation identity

#### Scenario: Divergence is caught quickly
- **WHEN** two peers diverge
- **THEN** the periodic hash comparison SHALL detect it within the configured interval and report
  the tick, rather than the match drifting silently

#### Scenario: The limitation is documented, not implied
- **WHEN** a team plans a cross-platform lockstep title
- **THEN** the documentation SHALL state which subsystems must use deterministic math types, which
  architectures the claim has been measured on, and that authoritative rigid-body physics is not
  part of the claim
