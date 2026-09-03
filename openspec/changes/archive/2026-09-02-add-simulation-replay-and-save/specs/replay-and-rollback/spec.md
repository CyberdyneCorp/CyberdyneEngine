## ADDED Requirements

### Requirement: One command log
Replay, rollback, and lockstep SHALL share **one command log**: the gameplay command stream defined
in `gameplay-framework`, recorded with each command's simulation point, participant, and sequence.

A second representation of participant intent SHALL NOT exist, because two representations of the
same thing drift.

**Raw device input SHALL NOT be the replay representation.** A recording of key presses would bind a
replay to a binding set, an accessibility configuration, and a frame rate, none of which are part of
the simulation.

Commands SHALL carry a per-participant **sequence number** within a tick, so that ordering is
defined, duplicates are detectable, and gaps are reportable.

#### Scenario: One log, three consumers
- **WHEN** a session is recorded, rolled back, and played in lockstep
- **THEN** all three SHALL use the same recorded commands

#### Scenario: Rebinding does not invalidate a replay
- **WHEN** a player rebinds their controls
- **THEN** existing replays SHALL remain valid, because intent rather than input was recorded

### Requirement: External results
Authoritative outcomes that are **not reproducible by simulation** — a service response, a
matchmaking assignment, a machine-learning inference result, a secure random value, a real-world
date — SHALL be recorded as **external results** with their simulation point and type.

During replay and re-simulation, the recorded result SHALL be consumed and the external source SHALL
**NOT** be invoked.

An authoritative system that consumes an external source SHALL declare it, so that a session under
`ReplayStable` or stricter records what it must.

Failing to record a consumed external source SHALL be detectable: replay validation SHALL report an
external consumption with no corresponding record.

#### Scenario: Inference is recorded, not re-run
- **WHEN** an inference result influences authoritative gameplay
- **THEN** it SHALL be recorded, and replay SHALL use the recorded value rather than running the
  model again

#### Scenario: An unrecorded source is caught
- **WHEN** a system consumes an external source that was not recorded
- **THEN** validation SHALL report it rather than the replay silently diverging

### Requirement: Replay contents
A replay SHALL consist of: a header, a compatibility manifest, a session descriptor with its seed
and tick rate, an initial authoritative state, chunked command records, external result records,
periodic checkpoints, an index, and optional presentation tracks.

A replay SHALL be reconstructible from those alone. Presentation output — rendered frames, audio —
SHALL NOT be part of the authoritative reconstruction.

Command records SHALL be **chunked over tick ranges and compressed**, not written per command, so
that reading and seeking are efficient.

#### Scenario: Reconstruction needs no renderer
- **WHEN** a replay is reconstructed
- **THEN** it SHALL require only recorded authoritative data, with no rendering or audio output
  needed

### Requirement: Replay compatibility
A replay SHALL record what it depends on: engine and project build identity, gameplay and command
schema versions, the plugin lockfile hash, the content manifest hash, the determinism profile, and
the tick rate.

Playback SHALL classify a replay as: **reproducible** (identical build and content), **compatible**
(within a declared migration window), or **incompatible**.

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

### Requirement: Snapshot kinds
The engine SHALL define snapshot kinds that share schema and state classification and
**deliberately do not share encoding**:

| Kind | Optimised for | Encoding |
|---|---|---|
| `Rollback` | Restore latency, captured many times per second | Current layout, bulk copy, in memory |
| `ReplayCheckpoint` | Compact restore for seeking | Compressed current layout |
| `SaveCheckpoint` | Surviving code and schema change | Tagged, versioned, migratable (see `save-and-persistence`) |
| `DebugCapture` | Diagnosis | Rich, may include selected derived state and metadata |

**The save encoding SHALL NOT be used for rollback.** A format that tolerates schema evolution
cannot be fast enough to capture at simulation rate, and a format fast enough cannot tolerate schema
evolution.

Every kind SHALL capture the authoritative state defined by state classification, including random
stream state and state providers, so that a restore resumes exactly.

#### Scenario: Rollback is not a save
- **WHEN** a rollback snapshot is captured
- **THEN** it SHALL use the fast current-layout encoding, not the versioned save format

#### Scenario: Restore resumes exactly
- **WHEN** a snapshot is restored
- **THEN** random streams and provider state SHALL be restored with entity state

### Requirement: Checkpoint policy
Replays SHALL contain **periodic checkpoints**, so seeking does not require replaying from the
beginning.

Checkpoint interval SHALL be a **policy** rather than a constant, informed by: elapsed ticks, state
size, command volume, expected seeking behaviour, and storage budget.

The replay index SHALL allow locating the nearest checkpoint at or before a target tick without
scanning the file.

#### Scenario: Seeking is bounded
- **WHEN** a viewer seeks two hours into a long session
- **THEN** the nearest preceding checkpoint SHALL be restored and only the intervening commands
  replayed

### Requirement: Playback and seeking
Replay playback SHALL be a **control source** producing recorded commands, so playback exercises the
same simulation path as live play.

Seeking SHALL restore the nearest checkpoint and fast-forward by simulating intervening ticks, with
presentation output suppressed during fast-forward so it runs as fast as the processor allows.

Playback speed SHALL be selectable. Simulation SHALL always advance in whole fixed ticks; speed
SHALL affect which ticks are presented, never the step size.

**Backward simulation SHALL NOT be attempted.** Reverse playback SHALL seek an earlier checkpoint and
replay forward, optionally using an in-memory snapshot ring for short reverse windows.

#### Scenario: Fast-forward is headless
- **WHEN** seeking forward
- **THEN** rendering, audio, and effects SHALL be suppressed while ticks are simulated

#### Scenario: Speed does not change the step
- **WHEN** a replay plays at one tenth speed
- **THEN** ticks SHALL remain fixed and only presentation frequency SHALL change

### Requirement: Presentation tracks
A replay MAY carry **presentation tracks** — camera direction, annotations, director cuts, markers —
separate from authoritative data.

Removing or ignoring a presentation track SHALL NOT affect authoritative reconstruction.

By default cameras SHALL be reconstructed from recorded simulation state; an authored camera track
SHALL be able to override that for directed playback.

#### Scenario: A replay without a camera track still plays
- **WHEN** a presentation track is absent
- **THEN** the replay SHALL reconstruct and play normally

### Requirement: Rollback mechanism
The engine SHALL provide rollback as a **mechanism**: an in-memory ring of rollback snapshots over a
bounded window, restore to a simulation point, and re-simulation of the intervening ticks from the
command log.

Policy — when to roll back, tolerance for divergence, and correction smoothing — belongs to the
system requesting it, in particular `networking-and-replication`.

Systems participating in rollback SHALL be **snapshot-restorable and deterministic**, declared as
such. Systems that are not SHALL be excluded and SHALL NOT be re-simulated.

The window SHALL be bounded by a memory budget; a request older than the window SHALL be reported as
requiring full resynchronisation rather than triggering an unbounded replay.

Re-simulation SHALL be identical in path to normal simulation: the same systems, the same commit
boundary, the same commands.

#### Scenario: Restore and re-simulate
- **WHEN** a correction arrives for an earlier tick
- **THEN** the snapshot at that point SHALL be restored and the intervening ticks re-simulated from
  the recorded commands

#### Scenario: The window is a budget
- **WHEN** the configured rollback memory is exhausted
- **THEN** the window SHALL shorten and the limit SHALL be reported

### Requirement: The side-effect ledger
Presentation effects triggered by simulation — sounds, particles, camera shakes, interface
notifications, haptics — SHALL carry the **simulation point** that produced them, and the engine
SHALL maintain a **ledger** of effects already realised.

During re-simulation, an effect whose simulation point is already in the ledger SHALL be
**suppressed**, so that rolling back and replaying a tick does not play the explosion twice.

Effects that were realised on a timeline that re-simulation has invalidated SHALL be reconcilable:
cancelled, allowed to finish, or corrected, by declared policy per effect kind.

The ledger SHALL be bounded and pruned as the rollback window advances.

#### Scenario: The explosion plays once
- **WHEN** a tick that triggered an explosion is re-simulated after a rollback
- **THEN** the effect SHALL be suppressed rather than triggered again

#### Scenario: An invalidated effect is reconciled
- **WHEN** re-simulation determines an effect should not have occurred
- **THEN** the declared reconciliation policy SHALL apply rather than the effect simply continuing

### Requirement: Speculative and confirmed effects
Effects and outcomes SHALL declare whether they may be realised **speculatively** — on predicted
state — or only when **confirmed** by authority.

A muzzle flash may be speculative; an achievement, a currency change, or a persistent unlock SHALL
NOT be.

A speculative effect that is later invalidated SHALL be reconciled through the ledger; a confirmed
effect SHALL NOT be realised until the authority confirms it.

#### Scenario: An achievement waits
- **WHEN** a predicted action would unlock an achievement
- **THEN** the unlock SHALL wait for authoritative confirmation

#### Scenario: A flash does not
- **WHEN** a predicted shot is fired
- **THEN** its speculative effects MAY play immediately and be reconciled if the prediction was
  wrong

### Requirement: Lockstep frames
Under the `Lockstep` profile, simulation SHALL advance from **lockstep frames**: for each tick, the
set of participants' commands and a reference to a previously agreed state hash.

An **input delay** SHALL be configurable, so commands are issued for a tick some ticks ahead,
allowing distribution. Changing the delay SHALL be coordinated and deterministic.

A participant missing its deadline SHALL be handled by a **declared policy** — treat as no command,
repeat the previous command, predict, pause the session, substitute an agent, or remove the
participant — and the decision SHALL be authoritative and recorded, so replay reproduces it.

State hashes SHALL be exchanged periodically at a configured frequency; divergence SHALL be detected
rather than accumulating silently.

#### Scenario: A late participant is handled by policy
- **WHEN** a participant's commands do not arrive by the deadline
- **THEN** the declared policy SHALL apply, and the decision SHALL be recorded so replay reproduces
  it

#### Scenario: Divergence is detected, not endured
- **WHEN** two peers' hashes disagree
- **THEN** the disagreement SHALL be detected at the next exchange

### Requirement: Resynchronisation
When peers diverge, the engine SHALL support **resynchronisation** rather than only termination:
requesting an authoritative snapshot, restoring it, and resuming.

Resynchronisation SHALL increment the simulation epoch and SHALL be recorded, so replays and logs
show that the timeline was reset.

Policy — whether to resynchronise, disconnect, or continue — SHALL belong to the session's rules,
and the mechanism SHALL support all three.

#### Scenario: A strategy match survives a desync
- **WHEN** a peer diverges mid-match
- **THEN** it SHALL be able to receive an authoritative snapshot and resume rather than the match
  ending

### Requirement: Server-side recording
A dedicated server SHALL be able to record a replay **continuously and headlessly**, with no
renderer, audio, or interface, at a bounded and reportable cost.

Server recordings are the authoritative record: where a client's recording exists it SHALL be
identified as client-side and MAY additionally record the corrections it received, for debugging
prediction.

Recording SHALL be able to run for the length of a session without unbounded memory growth,
streaming completed chunks to storage.

#### Scenario: A match is recorded without a renderer
- **WHEN** a dedicated server records a match
- **THEN** no rendering or audio code SHALL be required, and the cost SHALL be reported

### Requirement: Replay privacy
Replay data SHALL separate **simulation-necessary** information from **personal** information.
Simulation requires opaque participant identifiers; names, chat, account identifiers, and voice are
not required to reconstruct a session.

Personal information SHALL be recorded only in explicitly declared, separately removable tracks, and
inclusion SHALL be an opt-in project decision.

Stripping a personal-information track SHALL leave the replay reconstructible.

#### Scenario: A shared replay carries no chat
- **WHEN** a replay is shared for debugging
- **THEN** it SHALL be possible to remove personal tracks and still reconstruct the session

### Requirement: The crash replay buffer
Development and, by configuration, shipping builds SHALL maintain a **rolling replay buffer** of a
configured recent duration: recent commands, external results, and periodic snapshots.

On a crash, assertion, or reported defect, the buffer SHALL be attachable to the report together
with build identity, simulation point, recent state hashes, and the divergence capture if one
exists.

The resulting artefact SHALL be loadable to reproduce the final seconds of the session.

#### Scenario: A bug report contains its reproduction
- **WHEN** a tester reports that units stopped moving
- **THEN** the report SHALL carry a replay slice from which a developer can reproduce the window

#### Scenario: The buffer is bounded
- **WHEN** a session runs for hours
- **THEN** the buffer SHALL retain its configured window at a bounded memory and storage cost

### Requirement: Replay and rollback validation
Replays SHALL be executable as **tests**: replaying a recording and comparing final and checkpoint
hashes against those recorded SHALL be a supported operation.

Continuous integration SHALL maintain **golden replays** whose expected hashes are committed;
intentional behaviour changes SHALL update them in the same change with a recorded justification.

Validation levels SHALL be selectable: none, checkpoint comparison, periodic hashing, or every tick.

**Replay fuzzing** — generating valid random command streams, recording, replaying, and comparing —
SHALL be supported, as SHALL fault injection for rollback: reordered, delayed, and duplicated
commands.

#### Scenario: A player session becomes a regression test
- **WHEN** a recorded session exposes a defect
- **THEN** the replay SHALL be addable to continuous integration as a test with expected hashes

#### Scenario: Rollback correctness under adversity
- **WHEN** commands arrive late, reordered, and duplicated
- **THEN** rollback SHALL converge to the authoritative result with no duplicated side effects

### Requirement: Replay and rollback performance
The engine SHALL report: rollback frequency, restore cost, ticks re-simulated, re-simulation cost,
snapshot capture cost and memory, ledger size, command log throughput, checkpoint size and interval,
and replay storage by category.

Rollback cost SHALL be reported as a distribution rather than an average, since the worst case
determines whether a game is playable.

Replay recording SHALL be a small, bounded cost during normal play, and its overhead SHALL be
reported so it is a decision rather than a surprise.

#### Scenario: The worst case is visible
- **WHEN** rollback performance is assessed
- **THEN** the distribution of restore and re-simulation cost SHALL be reported, not only the mean

### Requirement: Forbidden replay patterns
The following SHALL NOT appear, and each SHALL be checkable:

- Raw device input used as the canonical replay representation
- A second representation of participant intent maintained alongside the command log
- The save serialisation format used as the rollback snapshot format
- Re-invoking an external source during replay instead of consuming its recorded result
- Presentation effects replayed without ledger suppression during re-simulation
- Simulating backwards
- Rendering, audio, or effects required to reconstruct a replay
- Silent best-effort playback of an incompatible replay presented as faithful

#### Scenario: A proposal is checked
- **WHEN** a change would record raw input as the replay format
- **THEN** it SHALL be flagged against this requirement
