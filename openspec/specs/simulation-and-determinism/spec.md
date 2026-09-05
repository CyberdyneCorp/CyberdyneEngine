# simulation-and-determinism Specification

## Purpose

Defines what makes a tick authoritative, and what "deterministic" means in this engine.

Nine capabilities made local determinism statements before this one existed — physics guarantees
same-platform only, lockstep is restricted because of it, AI derives its schedule from simulation
state rather than measured time, VFX and inference are firewalled, the scheduler has a deterministic
mode. Each was correct; together they were nine partial answers. **Determinism is now a declared
profile** — none, replay-stable, same-platform, cross-platform, lockstep — and each of those
statements is a consequence rather than a separate rule.

The **commit boundary** is the other load-bearing idea: the point after which a tick's state is
authoritative. Hashing, rollback capture, replay checkpointing, save snapshotting and network send
all key off it, so they describe the same state rather than each defining its own moment. And a
moment is an **epoch and a tick**, because rollback moves ticks backwards.

Determinism does not mean one thread. Work runs in any physical order and commits in a stable
logical one — with the specific rule that **worker identity may never appear in an ordering key**,
since with work stealing that is a function of timing.

The part that makes this a development capability rather than a constraint is the validator:
hierarchical hashing that narrows a divergence to a field, a chaos scheduler that perturbs execution
order to surface undeclared dependencies, and automatic capture of the window around the first
disagreement. Non-determinism becomes a bug you can find rather than a property you hope for.

## Requirements

### Requirement: The simulation clock
Authoritative simulation SHALL advance in **fixed ticks**, and the tick number SHALL be the
authoritative unit of simulation time. Wall-clock time SHALL NOT be authoritative.

Tick rate SHALL be expressed as an **exact rational** — a numerator and denominator — so that
accumulating a rounded step never drifts.

A frame MAY execute zero, one, or several ticks. The number of catch-up ticks per frame SHALL be
bounded by configuration, so a stalled process does not spiral.

**The fixed step SHALL NOT be lengthened to catch up.** Falling behind executes more ticks or
degrades presentation; it never changes simulation semantics.

#### Scenario: No drift
- **WHEN** a session runs for hours at 60 ticks per second
- **THEN** tick timing SHALL be derived from an exact rational rate rather than accumulated
  floating-point steps

#### Scenario: A long frame does not change physics
- **WHEN** a frame takes 50 milliseconds
- **THEN** the simulation SHALL execute additional fixed ticks up to the configured bound, and the
  step SHALL remain unchanged

### Requirement: Simulation epochs
A moment in simulation SHALL be identified by an **epoch and a tick**, not by a tick alone, because
rollback moves the tick backwards.

The **epoch** SHALL increment on disruptive resets: checkpoint restore, world reload, session
restart, and hot reload of gameplay code.

Caches, handles, histories, and logs carrying temporal assumptions SHALL be able to compare epochs
and detect that they are stale.

#### Scenario: The same tick twice
- **WHEN** a rollback returns to a tick already simulated
- **THEN** the resulting state SHALL be identified by a distinct simulation point, so logs and
  caches can distinguish them

#### Scenario: A reload invalidates history
- **WHEN** a checkpoint is restored
- **THEN** the epoch SHALL increment and temporal caches SHALL treat their contents as stale

### Requirement: The commit boundary
Each tick SHALL have a defined **commit boundary** at which its state becomes authoritative,
reached after: commands are ingested, systems execute, the task graph drains, per-worker structural
buffers merge deterministically, events commit, and the state version increments.

State SHALL NOT be considered authoritative before that point, and every consumer of authoritative
state — hashing, rollback capture, replay checkpointing, save snapshotting, and network send —
SHALL key off it rather than defining its own moment.

The tick pipeline SHALL define named phases so that ordering constraints and diagnostics share a
vocabulary; the task scheduler SHALL still derive actual dependencies from declared access.

#### Scenario: One moment, many consumers
- **WHEN** a tick completes
- **THEN** its hash, its snapshot, its replay record, and its network state SHALL describe the same
  state

#### Scenario: Nothing observes a partial tick
- **WHEN** a system queries authoritative state
- **THEN** it SHALL observe the last committed tick, never a partially updated one

### Requirement: Determinism profiles
A session SHALL declare a **determinism profile**, and the engine SHALL enforce and verify only what
that profile requires:

| Profile | Guarantee |
|---|---|
| `None` | No reproducibility guarantee |
| `ReplayStable` | Enough authoritative information is recorded that the session can be reconstructed |
| `SamePlatform` | The same binary, architecture, and inputs reproduce identical authoritative state |
| `CrossPlatform` | Different platforms converge to identical authoritative state |
| `Lockstep` | Peers reproduce identical state from commands alone |

The profile SHALL be validated at configuration time against the subsystems in use: a session
declaring `CrossPlatform` while using a subsystem that guarantees only same-platform determinism
SHALL be rejected with a diagnostic naming the subsystem.

Profiles SHALL compose with the constraints already declared elsewhere — physics determinism,
lockstep peer verification, and the exclusion of non-deterministic subsystems — rather than
duplicating them.

#### Scenario: A profile is a decision
- **WHEN** a project selects `ReplayStable`
- **THEN** it SHALL pay for command and external-result recording and SHALL NOT be required to meet
  lockstep ordering constraints

#### Scenario: An impossible profile is rejected
- **WHEN** a session declares `CrossPlatform` while relying on a subsystem that does not guarantee
  it
- **THEN** configuration validation SHALL fail naming that subsystem

### Requirement: Deterministic parallelism
Deterministic profiles SHALL NOT require single-threaded execution. Work MAY execute in any physical
order while authoritative results are committed in a **stable logical order**.

Ordering keys SHALL be built from stable logical identity — system, partition, local sequence — and
**SHALL NOT include worker or thread identity**, since with work stealing a worker's identity is a
function of timing.

Per-worker accumulation with ordered commit SHALL be the mechanism for structural changes, events,
and reductions, as defined in `core-jobs-and-concurrency`.

Global sorting SHALL NOT be the general mechanism for determinism; stable partition identity,
commutative operations, and deterministic reduction topologies SHALL be preferred, with sorting used
only where semantics require an order.

#### Scenario: Worker identity is not order
- **WHEN** work stealing assigns chunks differently between runs
- **THEN** committed results SHALL be identical

#### Scenario: Determinism is not serialisation
- **WHEN** a deterministic profile is active
- **THEN** parallel execution SHALL remain available, and throughput SHALL not collapse to one core

### Requirement: Stable iteration and tie-breaking
Queries SHALL declare their ordering requirement: **unspecified** where order does not affect
results, or **stable** where it does. Determinism SHALL NOT depend on allocator history or archetype
creation order by accident.

Every algorithm selecting among **equal candidates** SHALL declare a tie-break by stable identity:
agents with equal utility, spawn points with equal weight, path nodes with equal cost, commands with
equal priority.

Iteration over containers whose order is unspecified — hash maps in particular — SHALL NOT determine
authoritative results. Lookup is permitted; iteration as a decision order is not.

#### Scenario: Equal candidates resolve identically
- **WHEN** two targets score identically
- **THEN** the tie-break SHALL select the same one on every machine

#### Scenario: Order dependence is caught
- **WHEN** a system's result depends on unspecified iteration order
- **THEN** validation with perturbed ordering SHALL expose it

### Requirement: Floating-point policy
The `SamePlatform` profile SHALL require a **controlled floating-point environment** for
authoritative code: declared rounding mode, denormal handling, and contraction policy, with
fast-math transformations that alter results disallowed on authoritative paths.

The `CrossPlatform` and `Lockstep` profiles SHALL require **deterministic math types** for
authoritative computation, provided as an optional module — fixed-point scalars, vectors, angles,
and deterministic transcendental approximations.

The engine SHALL NOT claim that arbitrary floating-point code produces identical results across
architectures, compilers, or vector widths.

The deterministic math module SHALL NOT replace the engine's general math library. Rendering,
animation, and effects SHALL continue to use ordinary floating point.

Development builds under deterministic profiles SHALL detect writes of non-finite values to
authoritative fields and report them, since a propagated non-finite value destroys reproducibility.

#### Scenario: The claim matches the mechanism
- **WHEN** a project requires cross-platform lockstep
- **THEN** its authoritative movement and combat paths SHALL use deterministic math types, and this
  SHALL be a stated cost rather than an assumption

#### Scenario: Presentation keeps floats
- **WHEN** a lockstep session renders
- **THEN** rendering, animation, and effects SHALL use ordinary floating point with no determinism
  requirement

### Requirement: Random streams
Authoritative randomness SHALL come from **named streams** derived from the session seed, and no
authoritative system SHALL use a global or ambient generator.

Streams SHALL be **counter-based**: a value SHALL be derivable from seed, stream identity, tick,
entity, and sample index, so that sampling is parallel-safe, order-independent, and randomly
accessible without shared mutable state.

Streams SHALL be independent, so that consuming randomness in one system does not shift another's
sequence.

Stream identity SHALL be hierarchical and derived from stable identifiers, so that a stream for one
entity in one system at one tick is reproducible without a global sequence.

Streams used only for presentation SHALL be declared as such and SHALL NOT be required to be
reproducible.

#### Scenario: Parallel sampling is safe
- **WHEN** many entities sample randomness concurrently
- **THEN** each SHALL derive its value from its own inputs, with no shared generator state and no
  ordering dependency

#### Scenario: A new call does not shift the world
- **WHEN** a system begins consuming one more random value per tick
- **THEN** other systems' sequences SHALL be unaffected

### Requirement: Random stream inspection
Random draws in deterministic profiles SHALL be **traceable** in development builds: for a given
tick, stream, and entity, the sample index and resulting value SHALL be reportable.

Tracing SHALL be a diagnostic mode, not a shipping cost.

#### Scenario: A divergent draw is identifiable
- **WHEN** two runs diverge after a random decision
- **THEN** the trace SHALL show the stream, tick, entity, sample index, and value on each side

### Requirement: The determinism firewall
Authoritative simulation SHALL NOT read state produced by non-deterministic systems.
Presentation-only systems SHALL NOT feed back into authoritative state.

Data sources SHALL be classified: **deterministic**, **externally recorded**,
**non-deterministic**, or **presentation only**.

| Domain | Classification |
|---|---|
| Gameplay rules, economy, orders, authoritative movement, combat | Deterministic |
| Animation pose, camera, audio, VFX, GPU-produced data, illumination | Presentation only |
| Machine-learning inference, service responses, real time, secure random | Externally recorded |

Where a presentation system's outcome must influence gameplay, the outcome SHALL be captured as an
authoritative event or an external result rather than read directly.

Development builds SHOULD support **taint tracking**: a system declared deterministic that reads a
presentation-only or non-deterministic source SHALL be reported.

#### Scenario: Particles do not cause damage
- **WHEN** an explosion is simulated on the GPU for appearance
- **THEN** damage SHALL be computed by an authoritative system, and particle results SHALL NOT
  determine it

#### Scenario: A crossing is reported
- **WHEN** a deterministic system reads a camera or animation value
- **THEN** taint tracking SHALL report it as a firewall violation

### Requirement: State classification
Every field participating in simulation state SHALL carry a **state classification**, derived from
the field classification already defined in `serialization-and-prefabs` and extended for simulation:
authoritative, predicted, persistent, presentation, or derived.

Classification SHALL determine participation in: state hashing, rollback snapshots, replay
checkpoints, saves, and network replication — one declaration driving all five.

Derived state SHALL NOT be hashed, snapshotted, or saved; it SHALL be recomputed.

#### Scenario: One declaration, five behaviours
- **WHEN** a field is declared authoritative and persistent
- **THEN** it SHALL be hashed, snapshotted, saved, and replicated according to that declaration with
  no further configuration

#### Scenario: Caches are not state
- **WHEN** a spatial index or a cached transform exists
- **THEN** it SHALL be classified derived and reconstructed rather than captured

### Requirement: Generated state codecs
Capturing, restoring, and hashing authoritative state SHALL use **generated codecs** produced from
schema metadata, not a reflection walk per field per tick.

Codecs SHALL be generated per component and per state provider for each purpose that needs one:
rollback packing, checkpoint packing, save serialisation, and hashing.

Codecs SHALL operate on archetype storage in bulk where the layout permits, rather than per entity.

#### Scenario: Hot paths carry no reflection
- **WHEN** a rollback snapshot is captured
- **THEN** it SHALL use generated codecs over archetype columns, with no per-field reflection

### Requirement: State providers
Authoritative state that is not ECS component data — session state, rules, teams, participants,
random stream state, the world persistence index, and subsystem state — SHALL be exposed through
**state providers** that can capture and restore themselves.

A provider SHALL declare which mechanisms it participates in: rollback, replay checkpointing,
saving, and hashing. Participation SHALL be explicit rather than assumed.

Provider implementations SHALL be generated where the state is reflected.

#### Scenario: Session state is captured
- **WHEN** a checkpoint is taken
- **THEN** session, rule, team, participant, and random stream state SHALL be captured alongside
  entity data

#### Scenario: Participation is declared
- **WHEN** a provider is expensive to capture and is not needed for rollback
- **THEN** it SHALL declare that it does not participate in rollback

### Requirement: Hierarchical state hashing
The engine SHALL compute a **state hash** over authoritative state, and the hash SHALL be
**hierarchical**: world, subsystem, archetype, chunk, entity, component, field.

A divergence SHALL be narrowable by descending the hierarchy, so the result of a mismatch is a named
field on a named entity rather than a statement that two numbers differ.

Hashing SHALL cover declared authoritative fields only, and SHALL NOT hash raw memory, padding, or
derived data.

Hash frequency SHALL be configurable: every tick in validation builds, periodically in shipping
lockstep, and on demand.

Where practical, subtree hashes SHALL be maintained incrementally so that periodic hashing does not
cost a full traversal.

**Entity identity is part of the hash.** The hash SHALL fold an entity's identity into its node, so
that two worlds holding identical component values under different entity identifiers hash
differently.

This is deliberate and it has consequences that SHALL be written down rather than discovered. An
identifier is state the moment anything holds a reference to it, and a divergence report has to name
an entity, which it cannot do if identity is outside the hash. In exchange:

- a streaming cell activated after different world history receives different identifiers and
  therefore a different hash, even when its content is identical;
- a replay or snapshot SHALL restore identifiers **verbatim** rather than merely restoring values,
  because restoring equivalent content under fresh identifiers is a divergence by this definition.

What SHALL NOT affect the hash is genuine allocator history that no reference can observe: chunk
membership, packing order within a chunk, and component registration order.

#### Scenario: Identical content under different identifiers diverges
- **WHEN** two worlds hold the same component values on entities with different identifiers
- **THEN** their hashes SHALL differ, and the divergence report SHALL name the entity

#### Scenario: Packing is not state
- **WHEN** the same entities are packed differently across chunks with their identifiers unchanged
- **THEN** the hash SHALL be identical

#### Scenario: A snapshot restores identity, not merely value
- **WHEN** a snapshot is restored
- **THEN** entity identifiers SHALL be restored exactly, and the hash SHALL match the hash taken
  before the snapshot

#### Scenario: Narrowing to a field
- **WHEN** two runs disagree at a tick
- **THEN** descending the hash hierarchy SHALL identify the entity, component, and field that differ

#### Scenario: Raw memory is not the hash
- **WHEN** a component contains padding or a derived cache
- **THEN** they SHALL be excluded from the hash

### Requirement: The determinism validator
The engine SHALL provide a **validator** that runs a scenario more than once under deliberately
different execution conditions — worker counts, task ordering, chunk assignment, allocator layout —
with identical commands, and compares hashes per tick.

A **chaos scheduling mode** SHALL be available in which permitted execution order is randomised, so
that undeclared ordering dependencies surface rather than remaining latent.

On divergence, the validator SHALL **capture the window**: the last agreeing snapshot, the commands
in between, the random trace, the systems that wrote the differing state, and the divergent values.

#### Scenario: An ordering dependency is found
- **WHEN** a system's result depends on execution order
- **THEN** running under chaos scheduling SHALL produce differing hashes and identify the first
  diverging tick

#### Scenario: Divergence yields a reproduction
- **WHEN** the validator finds a divergence
- **THEN** it SHALL produce a small capture from which the window can be replayed

### Requirement: Determinism lint
Systems declared deterministic SHALL be checkable statically. The build SHALL be able to report, for
such systems: use of wall-clock time, use of an ambient random generator, iteration over containers
with unspecified order used as a decision order, reads of presentation-classified data, and use of
floating-point operations disallowed by the active profile.

Findings SHALL name the system and the source location, and SHALL be configurable as errors or
warnings.

#### Scenario: A wall-clock read is flagged
- **WHEN** a deterministic system reads the system clock
- **THEN** the lint SHALL report it with its location

### Requirement: Registration and initialisation order
Authoritative behaviour SHALL NOT depend on static initialisation order, plugin load order, or
filesystem enumeration order.

Registries whose contents affect simulation — systems, types, rules, providers — SHALL be finalised
in a deterministic order derived from stable identifiers before simulation begins.

#### Scenario: Load order does not change results
- **WHEN** plugins load in a different order
- **THEN** simulation results SHALL be unchanged

### Requirement: Simulation diagnostics
The engine SHALL report per tick, in validation builds: tick duration by phase, commit boundary
cost, hash cost, command counts, random samples drawn, deterministic sort and reduction counts, and
snapshot capture cost.

It SHALL identify **deterministic barriers** — points where ordering requirements serialise work —
so that the cost of a determinism profile is visible rather than assumed.

#### Scenario: The cost of determinism is visible
- **WHEN** a session runs under a strict profile
- **THEN** the profiler SHALL report which ordering requirements cost time

### Requirement: Simulation performance and testing
Determinism verification SHALL be part of continuous integration for every profile a project
declares: identical hashes across worker counts, across chaos-scheduled runs, and — for
`CrossPlatform` — across the platforms the project targets.

The engine SHALL maintain a **strategy-scale determinism benchmark**: eight participants, one
hundred thousand units, five thousand agent groups, sixty ticks per second, run for minutes, under
one, eight, and sixteen workers with randomised work stealing, requiring identical final hashes
under the `Lockstep` profile.

State hashing at shipping frequency SHALL be a small fraction of tick time, and its cost SHALL be
reported.

#### Scenario: The benchmark is the test
- **WHEN** determinism is assessed
- **THEN** the strategy-scale scenario SHALL be the reference, not a small synthetic case

#### Scenario: A regression fails the build
- **WHEN** a change introduces an ordering dependency
- **THEN** continuous integration SHALL fail with the first diverging tick and field

### Requirement: Forbidden determinism patterns
The following SHALL NOT appear in authoritative simulation, and each SHALL be checkable:

- Wall-clock time used as authoritative simulation time
- Authoritative behaviour depending on render frame rate
- A global or ambient mutable random generator
- Worker or thread identity in an ordering key
- Iteration order of an unordered container determining a gameplay decision
- Equal-candidate selection without a declared stable tie-break
- GPU-produced or presentation-classified state read by authoritative systems
- Raw structure memory hashed or serialised as canonical authoritative state
- A claim of cross-platform bitwise determinism for arbitrary floating-point code
- Lengthening the fixed step to catch up

#### Scenario: A proposal is checked
- **WHEN** a change would order work by worker identity
- **THEN** it SHALL be flagged against this requirement
