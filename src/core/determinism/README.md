# `src/core/determinism/` — the simulation's sense of time, chance, state and order

Layer 0, `cy::core-determinism`, headers `<cy/core/determinism/*.h>`, namespace `cy::determinism`.

Governed by `openspec/specs/simulation-and-determinism/spec.md`, which reaches **Seed** at M2.
design.md §5 says what Seed means, and it is worth restating before anything below is trusted:

> Seed here means *the shape is right and the hooks exist*, not that anything is validated.

**Nothing in this module validates anything.** The determinism profiles, the validator, the chaos
scheduler, the divergence capture and the determinism lint are M9's, and none of them is here. What
is here is the vocabulary everything from M3 to M8 will be written against, which is why it lands
before there is anything to validate: a classification retrofitted onto components that already
exist is a classification nobody applies.

| File | Task | What it owns |
|---|---|---|
| `epoch.h` | 4.2.2 | `Epoch`, `SimulationPoint`, `is_stale`, `EpochCounter` |
| `clock.h` | 4.2.1 | `TickRate` as an exact rational, `SimulationClock`, `TickMode` |
| `commit.h` | 4.2.3 | `TickPhase`, `CommitRecord`, `CommitObserver`, `CommitBoundary` |
| `random.h` | 4.2.4 | `StreamId`, `RandomStream`, `RandomSource`, `SampleCursor` |
| `classification.h` | 4.2.5 | `SimulationClass`, `Classified<>`, the firewall, `ExternalResult` |
| `state_schema.h` | 4.2.5/6 | `StateSchema` — what participates in the hash, **declared** |
| `hash.h` | 4.2.6 | `StateHashTree`, `Divergence`, `HashSchedule` |
| `provider.h` | 4.2.6 | `StateProvider`, `StateProviderRegistry` |
| `ordering.h` | 4.2.7 | `Ordering`, `select_best`, `sort_by_key` |

The walk that binds the hash to an `ecs::World` is **not here** — layer 0 cannot name an entity. It
is `src/runtime/state_hash.h`, at layer 5.

## The four claims worth arguing with

### 1. The illegal read is unspellable — for classified state, and only for it

design.md §5 sets the bar and names the trap: M1's "workers never block" is enforced for *declared*
blocking, and an undeclared `read()` is caught only by a watchdog, so M9 would inherit a validator
that reports clean on code that is not.

**What is genuinely unspellable.** A firewall crossing between two values held in `Classified<>`
does not compile. `read()` takes an `AccessContext<C>` witness and is constrained on
`may_read(C, source)`, so an authoritative system cannot name the value inside a `Presentation<f32>`
— there is no expression that yields it. The same closes the feedback direction: a presentation
context cannot write an authoritative field. And a value cannot be laundered by copying, because the
wrappers are distinct types with no converting constructor.

`tests/test_classification.cpp` proves it the only way a negative claim can be proved without
breaking the build: a `CanRead`/`CanWrite` concept, and `static_assert` that the expression is
ill-formed. A regression that made the read legal again fails the build.

**What is only refused, or not caught at all.**

* A system that reads a plain global, a raw `float`, or a member of a struct that never adopted
  `Classified<>`. Nothing here sees it. That is the determinism lint's, at M9.
* `bypass_classification()`, which every reflection-driven consumer needs. It is spelled to be ugly
  and greppable — `grep -rn bypass_classification src/` is the audit — and it is not enforced.
* `record_external()`, which is *supposed* to be spellable: the requirement asks for the crossing to
  be captured, not prevented.

So the honest statement is: **the crossing is unspellable for state that is classified, and
classification is opt-in per field.** Adopting the wrapper buys the guarantee; a component that has
not adopted it gets a comment.

### 2. `SimulationClock` cannot read a wall clock

It has no member that calls one and includes no header that offers one. Its only source of elapsed
time is `accumulate()`, which the host calls with a duration it measured. A system handed a
`const SimulationClock&` — which is all `runtime::Simulation` hands out — has no wall clock reachable
through it. `Runtime::tick()` is the one place in the engine where wall time enters the simulation's
sense of time, and it says so at the line.

The tick's *duration*, which the diagnostics requirement asks for, comes from a function pointer the
host supplies in `SimulationConfig::diagnostic_clock`. It is read in two places in
`src/runtime/src/simulation.cpp` and appears in no interface a system is handed; with no such
function every duration is reported as zero.

### 3. A random draw is a pure function, and that is what makes three properties free

There is no `next()`, no cursor inside a stream, no mutable state anywhere in `random.h`. A draw is
`(seed, stream, point, entity, index) -> u64`. Parallel sampling is safe because there is nothing to
share; sampling is order-independent because the answer does not depend on what came before; and
"a new call does not shift the world" is arithmetic rather than discipline — a system that begins
drawing one more value per tick changes nothing any other system computes, *because no other
system's inputs mention it*.

The cost is that the caller supplies the sample index. That is the point: an index from a hidden
counter is exactly the shared mutable state the requirement forbids.

The mixer deliberately does **not** call `cy::hash_bytes`, which is seeded per process in
development builds. A stream keyed by that would produce a different sequence on every run.
`kMixerVersion` is what a replay header records so a mismatch is a diagnostic rather than a
divergence.

### 4. The chunk level is in the hash hierarchy's enum and not in the walk

`simulation-and-determinism` names seven levels: world, subsystem, archetype, chunk, entity,
component, field. The walk implements six of them. Chunk assignment is allocator and insertion
history, and the same specification requires that determinism not depend on allocator history and
has its validator *deliberately perturb* chunk assignment between runs — so a hash with a chunk level
in it would differ between two runs that agree about every value a game can observe. The level stays
in the enum because an incremental scheme (a per-chunk subtree hash re-folded in a stable order) is
the shape M9 will want.

## Things thinner than they look

* **Nothing is incremental.** A hash is a full walk: O(entities log entities) for the per-archetype
  sort plus one `World::get` per (entity, component). The requirement says subtree hashes SHOULD be
  incremental "where practical"; the runtime measures and reports the cost so that the decision is
  taken against a number rather than an intuition.
* **A component with no declared schema is not hashed, and is counted.** Hashing its bytes is not
  available — raw structure memory as canonical authoritative state is on the forbidden list — so
  the honest alternative is `WorldHashReport::subjects_undeclared`. At M2 that number is non-zero in
  every real world: the ECS's `Parent`/`Children` and the scene's twelve built-ins are registered by
  name with no `TypeInfo`. Covering them needs `StateSchema::declare()` with an explicit field list.
* **`StateSchema` exists because reflection has no simulation class.** M1's attribute set has
  `PersistenceKind` and no enumerator for `Predicted` or `Presentation` — the two the firewall is
  actually about. `declare_reflected()` derives what it can; the rest is declared. Adding a
  `SimulationClass` attribute to the generator would collapse the two, and that is a change to
  `src/core/reflect/`, which this milestone did not own.
* **`CommitBoundary` notifies observers in registration order, not name order** — the one place in
  this module where that is right. An observer's *effects* are outside the simulation (a file
  written, a packet sent) and are not part of any hashed state, so ordering by name would buy
  nothing and would make "the save runs before the network send" inexpressible.
* **The provider registry's `capture`/`restore` are byte-oriented and unimplemented by the two
  built-in providers.** They declare `Checkpoint` and `Save` participation and implement `hash`
  only; the base class refuses the others rather than returning empty bytes. Checkpointing is M6's
  and rollback is M9's, and a provider that silently captured nothing would be worse than one that
  says it cannot.
* **`Predicted` is never hashed.** Two peers legitimately disagree about a prediction, and hashing
  it would make correct prediction look like divergence. It is rolled back and checkpointed, because
  reconciliation needs a value to rewind.
* **`ordering.h` cannot stop a system writing its own loop over a `HashMap`.** `Ordering` is a
  declaration a query carries so the scheduler and the validator can read it; enforcing it is the
  chaos scheduler's and the lint's. What `select_best` does enforce is that there is no overload
  without a tie-break.
