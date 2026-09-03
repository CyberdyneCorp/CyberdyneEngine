# Design: simulation integrity

## 1. Determinism is a profile, not a promise

The engine currently makes nine local determinism statements that are each true and that nobody can
reconcile into a single answer to "is this engine deterministic". The honest answer is that the
question is malformed: a third-person action game wants replay fidelity and rollback, a strategy
game wants lockstep, and an editor preview wants neither.

So determinism becomes a **declared profile**:

| Profile | Guarantee | Cost |
|---|---|---|
| `None` | Nothing | Nothing |
| `ReplayStable` | Enough authoritative information is recorded to reconstruct the session | Command and external-result recording |
| `SamePlatform` | Identical binary, architecture, and inputs reproduce identical state | Ordering discipline, stable reductions |
| `CrossPlatform` | Different platforms converge | The above, plus deterministic math for authoritative paths |
| `Lockstep` | Peers simulate identically from commands alone | The above, plus periodic hash agreement |

Every existing statement then becomes a consequence rather than a separate rule: physics guarantees
`SamePlatform` and not `CrossPlatform`; lockstep is therefore restricted to matching builds unless a
deterministic-math path is used for authoritative movement; VFX and inference are excluded from
authoritative state entirely.

Stating this once means a project chooses a cost deliberately instead of discovering it in
multiplayer testing.

## 2. The commit boundary is the thing everything else keys off

A tick is not authoritative while systems are still running. It becomes authoritative at a defined
point: after the task graph drains, after per-worker structural buffers merge deterministically,
after events commit, and after state version increments.

Naming that point is what makes the rest possible. The state hash is taken there. A rollback
snapshot is captured there. A replay checkpoint is written there. A save's consistent snapshot is
taken there. Without it, each of those would define its own "moment" and they would disagree in
exactly the situations that matter.

## 3. Ticks rewind; epochs do not

Rollback moves the tick backwards, which means a tick number alone does not identify a moment: tick
8,122 before a rollback and tick 8,122 after are different states.

So a simulation point is `{epoch, tick}`. The epoch increments on disruptive resets — a checkpoint
restore, a world reload, a session restart, a hot reload of gameplay code — and caches, handles, and
logs that carry temporal assumptions can detect that they are stale rather than silently reusing
data from a timeline that no longer exists.

## 4. Deterministic parallelism, and the specific trap

Determinism does not mean one thread, and the mechanism was already specified in the job system:
per-worker accumulation with ordered commit. What this change adds is the rule that catches the
recurring mistake — **worker identity must never appear in an ordering key**. With work stealing, a
worker's identity is a function of timing, so ordering by worker produces a system that is
deterministic on the developer's machine and not on the build server's.

Ordering keys are built from stable logical identity: system, partition, local sequence. That is
cheap, and it is what makes chunk-parallel simulation reproducible without sorting the world.

Related, and equally cheap to state, expensive to discover: any algorithm with equal candidates
needs a declared tie-break. Two agents with the same utility score, two spawn points with the same
weight, two path nodes with the same cost. Without a stable tie-break, two machines pick differently
and diverge for reasons no log explains.

## 5. Floating point, stated rather than wished

Bit-identical floating-point behaviour across architectures, compilers, SIMD widths, and fast-math
settings is not something an engine can promise for arbitrary code, and pretending otherwise is how
cross-platform lockstep fails late.

So: `SamePlatform` requires a controlled floating-point environment — rounding mode, denormal
handling, contraction policy — and is achievable for ordinary float code. `CrossPlatform` requires
**deterministic math types** for the authoritative paths that need it, provided as an optional
module rather than by replacing the engine's math library.

The renderer keeps floats. Animation keeps floats. Particles certainly keep floats. A strategy
game's unit positions may not, and that is a per-subsystem decision with a stated cost.

## 6. The firewall generalises

VFX and machine-learning inference were already firewalled from authoritative state. The same
boundary applies to animation (except root motion, which is already required to be computed on a
deterministic path), audio, camera, GPU-produced data, and anything derived from measured time.

The enforcement idea worth building is **taint tracking**: sources are classified deterministic,
externally recorded, non-deterministic, or presentation-only, and a system marked deterministic that
reads presentation-only data is a reported defect rather than a subtle divergence six months later.
This is a diagnostic that does not exist in the engines being compared against, and it is only
possible because the classification already exists in the schema.

## 7. One command log, three consumers

Replay, rollback, and lockstep all need "what the participants did at tick N". Recording that three
ways would guarantee they drift.

The gameplay command stream is already the single semantic representation of intent, so it is the
log. Replay reads it back through a control source. Rollback re-applies it during re-simulation.
Lockstep exchanges it. No parallel representation exists.

Raw device input is deliberately **not** the replay representation: it would bind a recording to a
binding set, an accessibility configuration, and a frame rate, none of which are part of the
simulation.

## 8. What cannot be reproduced is recorded

Some authoritative outcomes genuinely are not reproducible: a matchmaking assignment, a service
response, an inference result, a secure random value, the real date. Re-running them during replay
would produce different answers, and forbidding them would be unrealistic.

So they are **external results**: recorded with their tick and consumed from the record during
replay, never re-invoked. This is what allows `ReplayStable` to be a useful profile for games that
are not bit-deterministic — the non-determinism is bounded and captured rather than eliminated.

## 9. Three snapshots, one schema, three encodings

This mirrors the tagged-versus-cooked decision made for serialization, for the same reason.

| Kind | Optimised for | Encoding |
|---|---|---|
| Rollback | Restore latency, many per second | Current layout, bulk copy, in memory |
| Replay checkpoint | Compact restore for seeking | Compressed current layout |
| Save checkpoint | Surviving code change | Tagged, versioned, migratable |
| Debug capture | Diagnosis | Rich, including selected derived state |

They share schema and state classification metadata and share nothing else. **Using the save
serializer as the rollback format is forbidden**, because a format that tolerates schema evolution
cannot be fast enough to run sixty times a second, and a format fast enough to do that cannot
tolerate schema evolution.

## 10. The side-effect ledger, or the explosion plays twice

The rollback failure everybody hits: tick 100 plays an explosion; a late input rolls back to tick
98; tick 100 runs again and plays the explosion again.

Presentation effects therefore carry the simulation point that produced them, and a ledger records
what has already been realised. On re-simulation, an effect whose point is already in the ledger is
suppressed. Effects are additionally classified **speculative** or **confirmed**: a muzzle flash may
be speculative, an achievement or a currency transaction may not.

This is a small mechanism that prevents a whole class of defects that are otherwise fixed one effect
at a time.

## 11. A save is the overlay, not a second model

The world already defines `authored cells + persistence overlay = current world`, and states that
one overlay serves saves, server persistence, replays, and editor play-mode changes. What it does
not define is how the overlay is encoded, journalled, made atomic, or made incremental.

So a save **is** that overlay plus scoped fragments — profile, campaign, session, participant — and
not a parallel persistence model. Two persistence models would be two sources of truth about a
destroyed bridge.

## 12. Do not scan the world to save it

A world with ten million persistent objects of which five per cent are resident must be savable
without loading the rest, and an autosave must not walk every entity looking for changes.

Two mechanisms make that true. **Dirty tracking**: writing a field classified persistent marks its
record, so a save inspects changes rather than the world. **Cell-scoped deltas**: persistence is
organised by region, so unloaded regions contribute their stored deltas directly without being
instantiated.

The alternative — the obvious implementation — is an autosave whose cost grows with world size
rather than with what the player did.

## 13. A save must never destroy the previous save

Writing in place means an interrupted save leaves a file that is neither the old state nor the new
one. On a player's machine that is lost progress.

So: write chunks, verify, write the manifest, switch atomically, and retain generations. A crash at
any point leaves the previous generation valid and loadable. This is the same discipline the patch
system uses, for the same reason.

## 14. Divergence must be findable, not merely detectable

"Hash mismatch at tick 122,883" is a detection, not a diagnosis, and a state hash over the whole
world gives nothing else.

**Hierarchical hashing** narrows: world, then subsystem, then archetype, then chunk, then entity,
then component, then field. Combined with a **chaos scheduler** that deliberately perturbs execution
order and worker counts, and automatic **divergence capture** of the window around the first
disagreement, the workflow becomes: run the validator, receive the field that differs and the
commands that led to it.

That is the difference between an engine that reports non-determinism and one in which
non-determinism is a solvable bug. It is also the most defensible claim in this change, because it
is a capability the reference engines do not offer rather than a re-implementation of one.

## 15. Build order

| Phase | Contents |
|---|---|
| 1 | Tick, epoch, rational rate, commit boundary |
| 2 | Random streams, counter-based derivation, stable tie-breaking |
| 3 | Deterministic structural commit, event merge, reductions |
| 4 | State classification, generated snapshot codecs, hierarchical hashing |
| 5 | Rollback snapshot ring; restore and re-simulate; the side-effect ledger |
| 6 | Command log; the replay container; playback as a control source |
| 7 | Replay checkpoints, seeking, external result records |
| 8 | Persistence traits, entity deltas, the world persistent store |
| 9 | Save container, background snapshot, dirty tracking, atomic generations |
| 10 | Migration, compatibility, save and replay inspectors |
| 11 | Determinism validator, chaos scheduler, divergence capture, lint |
| 12 | Lockstep profile, deterministic math subset, resynchronisation |
| 13 | Crash replay buffer, golden replays in CI, scale benchmarks |

**Phase 1 is the milestone that matters.** Every later phase keys off the commit boundary, and a
system built without one has no defined moment at which its state is a thing that can be captured.

## 16. Non-goals

- **Simulating backwards.** Reverse playback seeks an earlier checkpoint and replays forward.
- **Replay compatibility across arbitrary code change.** Compatibility windows are declared;
  outside them a replay is rejected with a reason rather than misinterpreted.
- **Cross-platform bitwise determinism for arbitrary float code.** `CrossPlatform` requires the
  deterministic math path for authoritative work, and that is stated rather than implied.
- **Cloud save transport.** The save system produces artefacts and metadata for comparison; sync,
  quota, and conflict resolution belong to a platform service.
