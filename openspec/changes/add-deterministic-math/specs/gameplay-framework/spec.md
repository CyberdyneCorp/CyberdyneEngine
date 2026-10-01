# gameplay-framework Spec Delta

## ADDED Requirements

### Requirement: Authoritative quantities under cross-platform profiles
Under the `CrossPlatform` and `Lockstep` profiles, authoritative gameplay quantities SHALL use the
deterministic math types of `deterministic-math`. This covers positions, velocities, headings,
distances, timers, and magnitudes that participate in the state hash.

Command payloads SHALL carry those quantities as deterministic types. A command type whose payload
declares a floating-point field SHALL be refused at registration under those profiles, with the type
named. A floating-point value produced by the issuing peer, such as a picked position, SHALL be
converted when the command is created. The command log then carries the converted value.

Authoritative movement SHALL be computed by a deterministic kinematic step over deterministic
transforms. The scene's floating-point transform SHALL be derived for presentation from the
authoritative transform once per tick.

Subsystems whose authoritative state remains in floating point SHALL declare `SamePlatform`, so that
a `Lockstep` session using them is refused naming them rather than desynchronising.

#### Scenario: A move order across architectures
- **WHEN** a player on x86-64 orders units to a picked point in a `Lockstep` session with a peer on
  arm64
- **THEN** the command SHALL carry the destination as a deterministic value, and both peers' state
  hashes SHALL agree on every tick of the resulting movement

#### Scenario: A float payload is refused
- **WHEN** a project registers a command type with an `f32` payload field under the `Lockstep`
  profile
- **THEN** registration SHALL fail naming the command type and the field
