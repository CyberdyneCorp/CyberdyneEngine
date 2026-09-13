## MODIFIED Requirements

### Requirement: Integrity and confidentiality
Save chunks SHALL carry **content hashes**, and the manifest SHALL carry the hashes of its chunks,
so corruption is detected rather than loaded.

Where confidentiality is required, established authenticated encryption SHALL be used, applied after
compression. **Encryption SHALL NOT be described as integrity**, and bespoke cryptography SHALL NOT
be written.

Corruption of a non-essential chunk MAY be recoverable by policy — falling back to a generation or
recovering unaffected scopes — but the engine SHALL NEVER invent authoritative state to fill a gap.

A malformed save SHALL fail diagnostically and SHALL NEVER crash the process.

**Confidentiality is a dependency adoption and SHALL be scoped as one.** It is not a coding task
inside this capability: `thirdparty-dependencies` names this engine's cryptography library and
requires an adoption to go through the OpenSpec change flow with the evaluation recorded, and an
authenticated-encryption path is incomplete without a key-management decision — where a key comes
from, who holds it, what happens to a save whose key is gone. Until that change lands, this capability
SHALL record confidentiality as **deferred with a named re-entry point** rather than as outstanding
work inside itself, and the integrity half SHALL stand on its own.

A save that is **not** encrypted SHALL NOT be described or reported as protected, and the absence of
confidentiality SHALL be visible in the manifest rather than inferred from its absence.

#### Scenario: Corruption is detected
- **WHEN** a chunk fails its hash
- **THEN** the load SHALL report which chunk failed rather than proceeding

#### Scenario: Nothing is invented
- **WHEN** state cannot be recovered
- **THEN** the load SHALL fail or fall back, and SHALL NOT substitute plausible values

#### Scenario: Confidentiality is deferred explicitly
- **WHEN** this capability is assessed while no authenticated-encryption dependency has been adopted
- **THEN** confidentiality SHALL be reported as a recorded deferral naming the change that re-enters
  it, and SHALL NOT be counted as an unmet requirement of the save container

#### Scenario: An unencrypted save says so
- **WHEN** a save is written without confidentiality
- **THEN** the manifest SHALL record that it is unencrypted, so no reader infers protection from
  silence

### Requirement: The load pipeline
Loading SHALL proceed: read the manifest, validate compatibility, load profile and session scopes,
construct the session, load the persistent world index, begin world streaming, **apply persistent
deltas as cells activate**, restore participant and control state, and resume simulation.

The entire world SHALL NOT be instantiated before the player sees anything; loading SHALL use the
same streaming path as normal play.

Delta application SHALL occur during cell activation, so a region's authored content and its
persistent changes are combined once rather than instantiated and then corrected.

**The pipeline SHALL be assembled in the engine, not in each project.** Today the only ordered
assembly of these steps in the tree is inside a sample, and the only translation between the world's
persistence overlay and the save overlay is about ninety lines in that same sample — so every project
that loads a save re-derives the order in which a world comes back, and a mistake in that order is a
mistake nothing in the engine can detect. An engine module above this one SHALL own the sequence and
the translation, and a project SHALL invoke it rather than reproduce it.

#### Scenario: Loading streams
- **WHEN** a save in a large world is loaded
- **THEN** initial regions SHALL stream in and play SHALL begin without instantiating the whole
  world

#### Scenario: Deltas apply on activation
- **WHEN** a region activates during load
- **THEN** its authored content and persistent deltas SHALL be combined during activation

#### Scenario: A project does not re-derive the order
- **WHEN** a project loads a save
- **THEN** the ordered pipeline and the overlay translation SHALL come from the engine, and removing
  the project's own code SHALL NOT remove the ability to load

### Requirement: Forbidden save patterns
The following SHALL NOT appear, and each SHALL be checkable:

- Raw runtime memory serialised as a save format
- Runtime entity indices, pointers, or archetype positions used as persistent identity
- Derived caches saved rather than reconstructed
- A save requiring the whole world to be resident
- Save writes performed destructively in place
- One monolithic save blob mixing profile, campaign, and session state
- Runtime migration of cooked content
- Bespoke cryptography, or encryption presented as integrity
- A boolean returned as the result of a failed load
- Inventing authoritative state to replace unrecoverable data

**"Checkable" SHALL mean a check that runs.** Each pattern SHALL be detected by a test, a lint or a
criterion that can go red, and a pattern whose only enforcement is review SHALL be recorded as
unchecked rather than counted as checkable. Where a pattern cannot be detected mechanically, the
capability SHALL say which one, why, and what is done instead — an exemption with a reason, not a
silent absence.

#### Scenario: A proposal is checked
- **WHEN** a change would serialise a component structure by memory copy into a save
- **THEN** it SHALL be flagged against this requirement

#### Scenario: Every pattern names its check
- **WHEN** this requirement is assessed
- **THEN** each of the ten patterns SHALL name the check that detects it, or be recorded as an
  exemption with its reason

#### Scenario: A check that cannot fail is not a check
- **WHEN** a pattern's check is run against a deliberate violation
- **THEN** it SHALL go red, and the demonstration SHALL be recorded

## ADDED Requirements

### Requirement: The large-world save benchmark exists and is committed
*Save performance and testing* requires the engine to **maintain** a large-world save benchmark, and
there is none: the benchmark tree carries entries for other subsystems and no save entry at all, so
the numbers this requirement speaks about have never been measured in this repository.

The engine SHALL maintain a committed save benchmark over a large world, with committed thresholds
and declared tolerances, in the same harness every other benchmark uses. It SHALL measure at least
the write of a large overlay, the load of it, and the incremental save of a small change against a
large world — because a save system whose full write is fast and whose incremental write is not is
the failure a player experiences.

A threshold SHALL be a number that can be exceeded. A benchmark that records timings without a
threshold SHALL NOT be counted as satisfying this requirement.

#### Scenario: The benchmark runs in the ordinary harness
- **WHEN** the benchmark suite runs
- **THEN** the save benchmark SHALL run with it, and SHALL fail when a committed threshold is
  exceeded

#### Scenario: The incremental case is separate from the full case
- **WHEN** a small change is saved against a large world
- **THEN** its cost SHALL be measured and thresholded separately from a full save
