## MODIFIED Requirements

### Requirement: Rollback and reconciliation primitives
Rollback SHALL be a reusable **mechanism** rather than a per-game implementation, and the mechanism
itself — the snapshot ring, restore, re-simulation from the command log, and the side-effect ledger
that prevents effects being realised twice — is defined in `replay-and-rollback`.

This capability owns the **networking policy** over that mechanism: when to roll back, the tolerance
for divergence between predicted and authoritative state, correction smoothing, and the response
when a correction predates the window.

It SHALL provide: an **input buffer** recording local commands with their simulation points,
**per-tick snapshots** over a bounded window, a **comparison** of predicted against authoritative
state for a tick, and a **replay** that restores the authoritative state and re-simulates buffered
commands.

Systems participating in rollback SHALL be **snapshot-restorable and deterministic**; systems that
are not — VFX, audio, non-pinned inference, adaptive controllers — SHALL be excluded from the
rollback set by declaration, and SHALL NOT be re-simulated during replay.

Corrections SHALL be **smoothed** for presentation over a configurable interval, so a rollback is
not visible as a snap.

The rollback window SHALL be bounded; an authoritative update older than the window SHALL cause a
full state resynchronisation rather than an unbounded replay.

#### Scenario: Misprediction is corrected
- **WHEN** authoritative state for tick N differs from the client's prediction beyond tolerance
- **THEN** the client SHALL restore tick N and replay buffered inputs to the present tick

#### Scenario: Non-deterministic systems are excluded
- **WHEN** a replay runs
- **THEN** VFX and audio SHALL NOT be re-simulated, so a rollback does not replay explosions or
  retrigger sounds

#### Scenario: Correction is not a snap
- **WHEN** a correction changes a character's position
- **THEN** the visual position SHALL converge over the smoothing interval while the simulation
  state is corrected immediately

#### Scenario: Beyond the window
- **WHEN** an authoritative update predates the rollback window
- **THEN** a full resynchronisation SHALL occur, reported as such

#### Scenario: Effects are not realised twice
- **WHEN** a tick that produced an effect is re-simulated
- **THEN** the side-effect ledger SHALL suppress it, and networking SHALL not implement its own
  suppression
