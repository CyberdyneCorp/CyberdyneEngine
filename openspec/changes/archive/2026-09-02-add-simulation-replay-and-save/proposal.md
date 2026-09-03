# Simulation integrity: determinism, replay, and save

## Why

Nine capabilities already make determinism promises, and no capability defines determinism.

Physics guarantees same-platform reproducibility and explicitly not cross-platform. Networking
restricts lockstep to peers sharing a build *because* of that. AI declares itself deterministic and
derives its schedule from simulation state rather than measured time. VFX and machine-learning
inference are firewalled from authoritative state. The job scheduler has a deterministic mode and
deterministic parallel primitives. Gameplay has a simulation clock, named random streams, and a
sentence describing what a replay is reconstructed from. The world has a persistence overlay that
"serves save games, dedicated server persistence, replays, and the editor's play-mode changes" —
four consumers, no encoding.

Each of those is correct locally. Together they are nine partial answers to one question, and the
parts that would make them a system are missing: what makes a tick authoritative, how parallel work
commits in a stable order, what a snapshot is, how a rollback avoids replaying an explosion sound,
how a save of a million-object world happens without loading the world, and how a divergence is
found rather than merely detected.

The organising rule, and the one that makes this a system rather than a file format:

> **A save is not a dump of runtime memory, and a replay is not a recording of what was drawn. Both
> are schema-aware representations of authoritative simulation state.**

And the one that keeps it honest:

> **Determinism is a profile, deliberately chosen, not a property claimed of the whole engine.**

## What changes

Three capabilities, as recommended, rather than one file that nobody can maintain.

**`simulation-and-determinism`** — the simulation clock with exact rational tick rates and a tick
number as the authoritative unit of time; **simulation epochs**, because a tick can rewind and an
epoch cannot; the **commit boundary** that makes a tick authoritative and gives every other system a
defined point to snapshot, hash, record, and save; five **determinism profiles** so that a
third-person action game is not forced to pay for lockstep and a strategy game is not left hoping;
deterministic parallelism with a stable mutation key and the explicit rule that a worker's identity
never determines order; stable iteration, tie-breaking, and reduction; the floating-point policy and
an optional deterministic math module for the cases that genuinely need it; counter-based random
streams derived from seed, stream, tick, and entity; the **determinism firewall** generalised from
VFX to every presentation system; hierarchical state hashing that narrows to a field rather than
reporting that hashes differ; a **chaos scheduler** and divergence capture; and a determinism lint.

**`replay-and-rollback`** — one command log serving replay, rollback, and lockstep, because a second
representation would drift; **external result records** for the things that are genuinely not
reproducible — a service response, an inference result, a real clock — recorded rather than re-run;
the replay container with compatibility manifest, chunked commands, and checkpoints; seeking by
checkpoint and fast-forward; **three snapshot kinds sharing schema and deliberately not sharing
encoding**; rollback restore and re-simulation; the **side-effect ledger**, without which a rollback
plays the explosion twice; speculative versus confirmed events; lockstep frames, hash exchange, and
resynchronisation; and the crash replay buffer that turns "can you reproduce it" into an attachment.

**`save-and-persistence`** — persistence scopes and traits; the save as the **world persistence
overlay plus scoped fragments**, not a second persistence model; persistent entity deltas with
tombstones for authored entities destroyed; dirty tracking, so an autosave does not scan ten million
entities; a journal with periodic compaction; **atomic writes with generations**, so a crash during
save leaves the previous save valid; background serialisation from a tick-consistent snapshot;
saving a world that is 95 % unloaded without loading it; the load pipeline that applies deltas
during cell activation; schema migration on value records; plugin-owned state; integrity and the
cloud boundary; and inspectors that answer *why is this field in my save*.

## Impact

- **New**: `simulation-and-determinism`, `replay-and-rollback`, `save-and-persistence`
- **Modified**: `gameplay-framework` (clock, random streams, and replay contracts delegate the
  mechanism), `core-jobs-and-concurrency` (chaos scheduling and profile awareness),
  `networking-and-replication` (rollback mechanism moves; networking keeps policy),
  `physics` (a declared determinism policy rather than one guarantee), `core-math` (floating-point
  policy per profile), `world-partition-and-streaming` (the overlay's encoding is defined
  elsewhere), `testing-and-quality` (golden replays, chaos, cross-platform verification),
  `thirdparty-dependencies`
- **Recommended next**: procedural content generation, then atmosphere and weather, then
  diagnostics — the foundations for all three now exist beneath them
