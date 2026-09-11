# M9 — Integrity: one command log, read five ways

## Why

**M8.c made the engine's frame visible; nothing yet makes its state defensible.** The determinism
firewall M8.c built refuses a write on *this* machine. The failure `simulation-and-determinism` and
`networking-and-replication` are actually about is between two machines that disagree, and no
capability in this tree currently detects that, localises it, or replays it.

**What is verified absent on the tree M8.c closes on**, rather than assumed:

- There is no `src/replay/` and no `src/networking/`. Both of this milestone's two Working rows have
  no directory.
- `replay-and-rollback` and `networking-and-replication` are both recorded at `none` in
  `docs/roadmap/status.yaml`. `CY_NETWORKING` is declared in `cmake/features.cmake`, defaults OFF
  and gates nothing — **which is exactly the shape M8.b's gate found in `CY_UI` and M8.c had to
  repair.** An option that removes nothing is the first thing this milestone must stop being true.
- `src/core/determinism/` carries the primitives — `hash.h`, `ordering.h`, `epoch.h`, `commit.h`,
  `random.h`, `classification.h`, `state_schema.h` — and **no profile, no validator and no lint**.
  `simulation-and-determinism` is at Seed and reaches Complete here; the three things it is missing
  are the three that make a profile a contract rather than a comment.
- `cy::gameplay::CommandStream` exists and carries a `Provenance`. **Nothing writes it to a log,
  nothing reads a log back, and there is no side-effect ledger** — so "one command log, read five
  ways" is today one command log read once, live.

**And one inherited certainty about the enforcement point.** M8.c decided, and proved by mutation,
that the determinism firewall lives at the ECS write path rather than at the command origin,
because `gameplay-framework` forbids provenance affecting validation or ordering. M9 must not
relitigate that: replication and rollback are *readers* of provenance, and a rollback that skipped a
command because of where it came from would break the same requirement. Rollback re-simulates from
inputs; the ledger is what stops a side effect being applied twice.

## What Changes

- **`simulation-and-determinism` to Complete** — determinism profiles declared and **rejected at
  configuration** when a subsystem cannot meet them, deterministic parallelism, stable iteration,
  the floating-point policy, generated state codecs, hierarchical hashing, and **the validator and
  the determinism lint** that turn a divergence into a named field on a named entity.
- **`replay-and-rollback` to Working** — one command log, external results, snapshot kinds,
  checkpoints, playback and seeking, presentation tracks, rollback, **the side-effect ledger**,
  lockstep, resynchronisation, and the crash replay buffer.
- **`networking-and-replication` to Working** — three network modes, the authority model,
  transports, replication schemas, component replication, baselines and deltas, spawning, RPCs,
  interest management, priority scheduling, bandwidth budgets, prediction and reconciliation, lag
  compensation, and the dedicated server.
- **`diagnostics-profiling-and-crash` to Complete** — rolling capture, crash artefacts, breadcrumbs,
  reproduction artefacts, remote and server diagnostics, telemetry export.
- **`save-and-persistence` to Complete** — integrity and confidentiality, storage backends,
  checkpoints.
- **`gameplay-framework` to Complete** — network integration, save and replay contracts, headless
  operation, performance contracts.
- **`CY_NETWORKING` stops gating nothing.** It flips to ON with `src/networking/` behind it, and the
  tree builds and passes with it OFF — both directions, because M8.b shipped an option tested in
  neither and M8.c's gate had to check every one of its own.

## Capabilities

### Advanced Capabilities

`replay-and-rollback` and `networking-and-replication` to **Working**;
`simulation-and-determinism`, `diagnostics-profiling-and-crash`, `save-and-persistence` and
`gameplay-framework` to **Complete**.

The full column is `docs/roadmap/capability-matrix.md`; the M9 row of `docs/ROADMAP.md` is the scope
statement this proposal implements. Six rows is the largest Complete load any milestone has carried,
and §4 of the design says which of them are genuinely small.

## Impact

- **New code**: `src/replay/` and `src/networking/` — two directories that do not exist today — plus
  the validator and the lint inside `src/core/determinism/`, and a new sample.
- **Existing code**: `cy::gameplay::CommandStream` gains a log seam it does not have; `src/save/`
  gains integrity and confidentiality over the container it already writes; `src/ecs/`'s `Snapshot`
  becomes one of several snapshot kinds rather than the only one.
- **Closing artefact**: `samples/09-multiplayer` — a four-player session over a simulated adverse
  network, recorded and replayed bit-exactly, with a deliberately injected divergence narrowed to
  one field on one entity. **A second sample rather than an extension**, and that is a deliberate
  departure from M8.c: the subject is what happens between four processes, and a slice that runs in
  one cannot demonstrate it. `samples/08-vertical-slice` stays the single-process artefact.
- **Risk**: cross-platform floating-point determinism, and it is the milestone's named spike.
  `design.md` §1 states what the spike must answer before the profile's definition is written down,
  and states plainly that **this machine can only run one platform**, so the honest outcome of the
  spike may be a profile that is verified on Linux and *declared unverified* elsewhere.
