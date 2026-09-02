# Foundations: identity, serialization, tasks, and memory

## Why

These four systems were specified early, before the systems built on them existed. Fifteen
capabilities later, three of those early decisions no longer hold, and one of them is a defect
that would corrupt content the first time somebody moved a class into a namespace.

**1. `TypeId` is specified as a hash of the fully qualified type name.** That makes serialized
identity a function of C++ source organisation. Renaming a struct, moving it into a namespace, or
reorganising a module silently changes the identity of every serialized instance of it — scenes,
prefabs, saves, and network schemas all break, and nothing reports why. The rule the engine
actually needs is the opposite: **type identity must be independent of the source name**, because
names are the thing most likely to change.

**2. `FieldId` does not exist, and two capabilities already depend on it.**
`serialization-and-prefabs` requires that overrides address "the prefab-local entity identifier,
the component type identifier, and the field identifier" so that renaming a field does not destroy
a designer's work. `core-type-system` provides no such identifier — fields are described by name
and byte offset. That is an inconsistency this change introduced and must close.

**3. Serialization is specified as one thing with two forms.** Authoring data needs to be tolerant:
version-tagged, skip-unknown, migratable, round-trippable. Cooked runtime data needs to be the
opposite: tightly packed, no per-field tags, layout shared between cooker and runtime. Trying to
satisfy both with one format gives the worst of each — and the world partition work already
depends on cells being bulk-copied archetype blocks, which no tagged format can be.

**4. The job system cannot express asynchronous work.** It has jobs, dependencies, and work
stealing, and no way to say "wait for this file read" or "wait for this GPU fence" without blocking
a worker. Every streaming, loading, and readback path in the engine needs exactly that, and each
would otherwise invent its own callback chain.

**5. Memory has tags but no budgets.** Every subsystem now holds a *GPU time* allocation from the
renderer budget arbiter, and nothing holds a *memory* allocation. Streaming, geometry, texture,
audio, and world caches will each grow until the platform runs out, and the first symptom will be
a crash on the console with the least RAM.

## What changes

**`core-type-system`** — type identity becomes explicitly assigned and recorded, not derived from
the name; **field identity** is introduced with the same properties; both are recorded in a
committed **identity manifest** with tombstones for removed entries, and a CI gate rejects
accidental identity changes. Attributes become strongly typed rather than a string dictionary. The
**reflection generator** is specified: what it consumes, what it emits, and the rule that
reflection is control-plane infrastructure while hot paths use generated typed code.

**`serialization-and-prefabs`** — the two serialization modes become an explicit split: **tagged**
for authoring, saves, overrides, and anything that must survive schema change; **cooked** for
runtime assets and ECS chunks, packed with no per-field tags. Migration is respecified to operate
on a **value-level record** rather than an old C++ type that no longer exists, with migration
classes declared, and migration applying equally to prefab overrides and save data. Unknown fields
are preserved on round trip in tagged data.

**`core-jobs-and-concurrency`** — C++20 coroutines as the asynchronous model; asynchronous I/O and
GPU fences resume through continuations instead of blocking a worker; cooperative cancellation;
priority classes with anti-starvation fairness and optional deadline hints; a **task context**
carrying the worker's scratch allocator and cancellation token; task records from per-worker slabs;
deterministic parallel reduction helpers; and critical-path reporting, because a frame's cost is
its longest dependency chain rather than the sum of its jobs.

**`core-memory-and-containers`** — explicit **memory domains** and a **budget tree** with hard and
soft limits; **memory pressure levels** broadcast to subsystems so caches trim before the platform
runs out; generalised **retirement queues and frame epochs** for deferred destruction; the
allocator scope extended to domains; virtual address reservation for large caches; and the
requirement that telemetry exists before allocator optimisation.

## Impact

- **Modified**: `core-type-system`, `serialization-and-prefabs`, `core-jobs-and-concurrency`,
  `core-memory-and-containers`, `animation-and-skinning` (property tracks bind by identity),
  `networking-and-replication` (schemas bind by field identity, so a rename is not drift),
  `rhi-and-render-graph` (GPU memory reports into the budget tree),
  `build-system-and-platforms` (the reflection generator as a build step),
  `thirdparty-dependencies`
- **One defect corrected**: name-derived type identity
- **One inconsistency closed**: field identity required by prefab overrides but never defined
- **Not in scope**: NUMA-aware allocation and a fiber runtime, both recorded as seams
