## MODIFIED Requirements

### Requirement: The simulation clock
Gameplay simulation SHALL advance in **fixed simulation ticks** with a monotonically increasing tick
number, and systems MAY run at reduced rates relative to it — strategic reasoning at a few hertz,
distant agents lower still.

The clock itself, the exact rational tick rate, catch-up bounds, simulation epochs, and the commit
boundary at which a tick becomes authoritative are defined in `simulation-and-determinism`. This
capability consumes them.

Scheduled gameplay SHALL be expressed in **ticks**, not wall-clock time: "at tick 8842" is
reproducible; "in 3.0 seconds" is not.

A moment SHALL be identified by **epoch and tick**, since rollback moves the tick backwards.

Timers SHALL be provided per time domain, implemented so that many timers cost bounded work — a
bucketed or wheel structure rather than one heap entry and one callback per timer per tick.

Network and replay systems SHALL use the same tick numbering, so peers and recordings refer to the
same instants.

#### Scenario: Deterministic scheduling
- **WHEN** a delayed effect is scheduled
- **THEN** it SHALL fire at a specific tick, identically on every peer and in replay

#### Scenario: Many timers are cheap
- **WHEN** fifty thousand timers are pending
- **THEN** advancing a tick SHALL cost work proportional to the timers actually due

#### Scenario: The same tick is not the same moment
- **WHEN** a rollback returns to a tick already simulated
- **THEN** the two occurrences SHALL be distinguishable by epoch

### Requirement: Deterministic random streams
Randomness used by gameplay SHALL come from **named streams** derived from the session seed, not
from a global generator.

Stream derivation, counter-based sampling, and inspection are defined in
`simulation-and-determinism`; this capability requires their use.

Streams SHALL be independent, so that consuming randomness in one system does not perturb another's
sequence — which is what makes a change in one feature alter unrelated outcomes in replay.

Stream state SHALL be part of session state where reproducibility requires it, and streams used for
presentation only SHALL be declared as such.

#### Scenario: Independent sequences
- **WHEN** a combat stream and a loot stream are both consumed
- **THEN** changing how much randomness combat uses SHALL NOT change loot outcomes

#### Scenario: Reproducible session
- **WHEN** a session is replayed from its seed and command stream
- **THEN** random outcomes SHALL match the original

#### Scenario: Sampling is parallel-safe
- **WHEN** many entities sample randomness concurrently
- **THEN** each SHALL derive its value from stable inputs, with no shared generator and no ordering
  dependency

### Requirement: Save and replay contracts
Gameplay state SHALL declare a persistence class — session transient, world persistent, profile
persistent, save game, or derived — and those declarations SHALL determine what a save captures.

A **replay** SHALL be reconstructible from: the session's initial state, its seed, its stream of
gameplay commands, recorded external results where determinism does not suffice, and periodic
checkpoints for seeking.

Replay playback SHALL be a control source producing commands, so playback exercises the same
simulation path as live play.

The mechanisms — the command log, external result records, snapshot kinds, checkpoints, seeking,
rollback, and the side-effect ledger — are defined in `replay-and-rollback`, and save encoding,
scopes, and migration in `save-and-persistence`. This capability declares what gameplay contributes
to them.

Command and gameplay state schemas SHALL be versioned, so a replay or save from an older build is
either migrated where supported or rejected clearly.

#### Scenario: Replay drives the game
- **WHEN** a replay is played
- **THEN** it SHALL be a control source issuing recorded commands, not a separate playback path

#### Scenario: Version mismatch is explicit
- **WHEN** a replay's command schema differs from the build's
- **THEN** it SHALL be migrated where supported or rejected with a diagnostic, never misinterpreted

#### Scenario: Non-reproducible results are recorded
- **WHEN** an authoritative outcome comes from a service or an inference
- **THEN** it SHALL be recorded as an external result and consumed from the record during replay
