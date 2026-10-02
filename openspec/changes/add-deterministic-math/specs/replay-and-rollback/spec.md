# replay-and-rollback Spec Delta

## MODIFIED Requirements

### Requirement: Replay compatibility
A replay SHALL record what it depends on:

- engine and project build identity;
- gameplay and command schema versions;
- the plugin lockfile hash;
- the content manifest hash;
- the determinism profile;
- the tick rate.

A replay recorded under `CrossPlatform` or `Lockstep` SHALL also record the deterministic math
kernel version and the session's simulation identity.

Playback SHALL classify a replay as one of:

- **reproducible**: identical build and content;
- **compatible**: within a declared migration window;
- **incompatible**.

For a replay recorded under `CrossPlatform` or `Lockstep`, reproducible SHALL mean an identical
simulation identity and kernel version rather than an identical binary. A replay recorded on one
architecture SHALL then be reproducible on another.

An incompatible replay SHALL be **rejected with a reason**, distinguishing it from a corrupt one. A
replay SHALL NOT be played back in a way that silently produces different results while claiming
fidelity.

Best-effort playback outside compatibility windows MAY exist as an explicit tool mode, labelled as
such.

#### Scenario: Corruption and incompatibility are different
- **WHEN** a replay cannot be played
- **THEN** the reason SHALL distinguish a build or content mismatch from a damaged file

#### Scenario: The window is declared
- **WHEN** a project ships an update
- **THEN** its declared replay compatibility window SHALL determine which existing replays remain
  playable

#### Scenario: A cross-platform replay on another architecture
- **WHEN** a `Lockstep` replay recorded on arm64 is played on x86-64 with the same simulation
  identity and kernel version
- **THEN** it SHALL be classified reproducible and SHALL reproduce the recorded state hash at every
  tick

#### Scenario: A kernel change is a named incompatibility
- **WHEN** a `Lockstep` replay recorded with one deterministic math kernel version is played by a
  build with another
- **THEN** it SHALL be classified incompatible with the kernel version named as the reason
