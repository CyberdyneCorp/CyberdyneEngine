# Design: identity, serialization, tasks, and memory

## 1. Identity must not be derived from names

The current specification defines `TypeId` as "a 64-bit hash of the fully qualified type name,
stable across builds and platforms". The second half is true and the first half is the problem: it
is stable across *builds*, and not across *edits*. Moving `MiningRobot` into a `Robots` namespace
changes its identity, and every scene, prefab, save, and network schema that referenced it silently
stops resolving.

That is exactly backwards, because the name is the part most likely to change and the identity is
the part that must not.

So identity is **assigned once and recorded**, never derived:

- On first registration a type or field receives an identifier — a name hash is a perfectly good
  *initial value* — which is then written to a committed **identity manifest**.
- Thereafter the manifest is authoritative. Renaming a type or field changes its name in the
  manifest and nothing else.
- Removing one leaves a **tombstone**. Identifiers are never recycled, because a recycled
  identifier is data corruption that looks like a successful load.
- A CI gate diffs the manifest and fails when an identifier changes for an existing entry, so
  the failure mode is a red build rather than a broken save six months later.

The manifest is a source-controlled artefact, which also makes identity changes reviewable — the
same argument as making the render pipeline configuration an asset.

## 2. Field identity was already required and never defined

`serialization-and-prefabs` states that overrides address "the prefab-local entity identifier, the
component type identifier, and the field identifier", and gives the reason: renaming a field must
not destroy a designer's work. `core-type-system` describes fields by name and byte offset and
defines no such identifier.

This is an inconsistency introduced by the world and authoring change, and it is worth being
explicit that it is being fixed rather than quietly patched. Field identity is introduced with the
same rules as type identity — assigned, recorded, tombstoned, gated — and the byte offset stays
where it belongs: a native-access convenience that is never serialized.

The consumers that immediately benefit are the ones that already assumed it existed: prefab
overrides, animation property tracks, and network replication schemas.

## 3. Two serialization modes, because one format cannot be both

| | Tagged | Cooked |
|---|---|---|
| Used for | Authoring, saves, overrides, replays | Runtime assets, ECS cells, GPU-ready data |
| Layout | Field-tagged records | Packed, layout shared with the runtime |
| Evolution | Skip unknown, migrate, round-trip | None — cooker and runtime share a build |
| Cost | Per-field overhead | Bulk copy |

The engine already depends on both: `world-partition-and-streaming` requires cells to be archetype
blocks copied into ECS chunks, which no tagged format can deliver, and
`serialization-and-prefabs` requires overrides to survive structural code change, which no packed
format can deliver.

Stating the split explicitly prevents the predictable drift where one is quietly used for the
other's job. Field-tagging two million transforms is the concrete failure this rules out.

## 4. Migration operates on values, not on old types

Migrating from version 2 to version 4 cannot construct a version-2 C++ object, because that type
does not exist any more. So migration reads and writes a **value-level record** — identifiers
mapped to encoded values — and native construction happens only after the record has reached the
current schema:

```
serialized record → value record → migration chain → current schema → native object
```

Three classes of migration are declared, because the distinction determines how much work a
developer must do:

| Class | Examples | Who writes it |
|---|---|---|
| Automatic | Rename, add with default, remove, safe numeric widening | Nobody — identity handles it |
| Generated | Enum remap, container change, wrapping in an optional | The generator, from a declaration |
| Custom | One field split into several, unit change, semantic change | A developer, against the value record |

The consequence that matters most: **migrations apply to prefab overrides and saves as well as to
assets**, since all three are tagged data addressed by the same identifiers. A field split that
migrates the asset but silently drops every override on it would be worse than not migrating.

## 5. Reflection is control plane; the hot path is generated code

Reflection describes types for the editor, serializer, migration, schema generation, and bindings.
It is not how a system iterates a million entities.

The rule is stated so it does not erode: **anything running per entity per frame uses typed
generated code**, and reflection appears at boundaries. `networking-and-replication` already
requires compiled schemas rather than per-field reflection per packet; this generalises that
instinct rather than leaving it as one subsystem's good behaviour.

## 6. Asynchrony is coroutines, not a fiber runtime

The job system can express dependencies between jobs and cannot express *waiting for something that
is not a job* — a file read, a decompression, a GPU fence. Every streaming path needs that, and
without it each grows its own callback chain.

C++20 coroutines are the answer rather than a custom fiber runtime: no separate stacks to manage,
no platform-specific context switching, and a straight-line reading order for code that is
genuinely a sequence of waits. The scheduler resumes continuations as ordinary tasks.

The rule that follows is absolute and worth stating as a requirement rather than a convention:
**a worker thread never blocks on I/O or on the GPU**. It submits and yields; completion schedules
the continuation.

A fiber runtime is recorded as a non-goal. If a future workload genuinely needs stackful
suspension, that is a change with its own proposal.

## 7. Deadlines and determinism pull against each other

Priority classes and deadline hints let the scheduler prefer a teleport's cell preparation over a
background prefetch, which is real value. They are also a form of time-dependent behaviour, and the
engine has already ruled that **simulation scheduling must not depend on measured time** — the AI
budget requirement exists precisely because that is how replay and lockstep break.

The resolution is the same one AI uses, applied consistently: deadlines and priorities may
influence **when work runs**, never **what the simulation computes**. In deterministic mode,
scheduling is a fixed topological order and deadline hints are ignored entirely. Work whose result
feeds simulation must complete before its consuming stage regardless of hints — a deadline is a
scheduling preference, not a correctness contract.

## 8. The task context is where tasks and memory meet

Every task receives a context carrying its worker index, its **scratch allocator**, and its
**cancellation token**. That single decision does most of the work of connecting the two systems:

- Temporary allocation inside a task lands in per-worker scratch by default, so the common case is
  contention-free and freed in bulk, without anyone remembering to choose an allocator.
- Cancellation is available where the work is, so a streaming task can check it at a natural
  boundary rather than being killed asynchronously.
- Task records themselves come from per-worker slabs, so scheduling a task does not call the
  general allocator.

Cancellation is **cooperative**. Nothing is forcibly terminated; a cancelled task observes its
token, releases what it holds, and returns. The alternative — terminating arbitrary code — cannot
be made safe in a language with destructors and manual resources.

## 9. Memory pressure is the missing half of the budget model

Every subsystem now holds a GPU-time allocation from the renderer budget arbiter. None holds a
memory allocation, and memory is the resource that fails hardest: an over-budget frame is a stutter,
an over-budget heap is a crash.

So memory gets the same treatment, with a different shape. Domains are declared, a **budget tree**
apportions them with hard and soft limits, and **pressure levels** — normal, elevated, critical —
are broadcast so that caches trim, prefetching backs off, and optional data is dropped *before* an
allocation fails. Every existing streaming and residency system already has an eviction policy;
pressure is the signal that tells them all to use it at once.

An allocation that fails is still specified to return null rather than throw, but by then the
system has already failed to do its job.

## 10. Deferred destruction generalises a rule the RHI already has

The RHI defers resource release until the frame's fence has signalled. The same problem exists for
anything a worker might still be reading — an asset page, a published snapshot, a command buffer.

**Frame epochs** generalise it: a resource is retired logically, and reclaimed once no in-flight
frame or task can still reference it. One mechanism, applied by the RHI, the asset system, the
world, and the task system, rather than four subtly different ones.

## 11. Order of implementation matters here more than usual

Two orderings are stated as requirements rather than left to judgement, because getting them
backwards wastes months:

- **Telemetry before allocator optimisation.** Choosing between allocators without per-domain
  attribution is guesswork, and the general-purpose allocator choice should be a benchmark result,
  not a preference.
- **Task profiling and critical-path reporting before tuning work stealing.** A frame's duration is
  its longest dependency chain, not the sum of its jobs; optimising throughput without knowing the
  critical path usually improves a number that does not matter.

## 12. Build order

| Step | Contents |
|---|---|
| 1 | Stable type, field, and component identity with the manifest and CI gate |
| 2 | Reflection generator and registry |
| 3 | Tagged serialization and the value-level migration model |
| 4 | Cooked serialization for ECS and assets |
| 5 | Memory domains, budget tree, telemetry |
| 6 | Frame, scratch, and pool allocators; ECS chunk allocator |
| 7 | Worker pool, task handles, work stealing, task profiling with critical path |
| 8 | Dependency scheduling from declared access |
| 9 | Coroutines, asynchronous I/O and GPU fence continuations, cancellation |
| 10 | Deterministic commit for commands and events |
| 11 | Pressure levels, retirement epochs, deadlines, budgets |

**Step 1 is the milestone that matters**, and it is first for a reason: every later step encodes
identity into data, and changing the identity model afterwards invalidates everything already
written with it.

## 13. Non-goals recorded

- **A fiber runtime.** Coroutines instead; stackful suspension is a future proposal, not a seam
  left open.
- **NUMA-aware allocation and worker placement.** Not required for the target platforms, and the
  allocator and worker abstractions are specified so as not to preclude it — worth naming so it is
  a decision rather than an oversight.
- **A bespoke general-purpose allocator.** The general heap is an integration decided by
  benchmark, not an engine-written malloc.
