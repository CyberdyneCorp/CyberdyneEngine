# `src/ecs/` — layer 1

The authoritative runtime storage: entities are generational ids, component data lives in packed
per-archetype chunks, and behaviour runs as scheduled systems over queries.

**What belongs here**: the world and its entity table; component registration and the five storage
kinds; archetypes and their chunks; queries and their cached archetype lists; systems, stages and
the access declarations they are scheduled from; deferred structural change; resources; change
detection; entity relationships; world snapshots and the world byte stream; multiple worlds.

**What does not belong here**: the `Node` façade (that is `src/scene/`), any server, any knowledge
of scripting. Not every entity has a node, and the ECS must not assume one does.

**Governed by**: `ecs-core`. Reached Working at M2.

## The two rules the module is built around

**There is no allocator here** (`design.md` §1). Every chunk comes from
`<cy/core/memory/chunk_storage.h>`, whose `ChunkAllocator` is under M1's budget tree. An ECS that
allocated outside it would be invisible to the memory-pressure system that has to evict it at M6 —
which is exactly the subsystem M6's residency policy holds allocations from. What this layer adds is
the meaning M1 deliberately does not have: a `ChunkLayout`'s columns are an archetype's component
set, a `ColumnSpec` is a component type's size and alignment, and the key is the `Entity`.

**Structural change deferral is correctness, not optimisation** (`design.md` §2). Creating and
destroying entities and adding and removing components are deferred to stage flush points, and every
structural entry point on `World` *refuses* while a query is iterating — a returned
`ErrorCode::Unavailable` and a counter, never an assertion, because `CY_ASSERT` is compiled out of
Profile and Shipping and a rule that only holds in two configurations is not a rule. The supported
way to make a structural change from inside a system is a `CommandBuffer`, which hands back a usable
placeholder id immediately and is applied at the stage's flush point in
`(system, thread, record)` order.

## Reading order

| File | What it settles |
|---|---|
| `entity.h` | The id: a 32-bit index and a 32-bit generation, and the two reserved generations. |
| `component.h` | The five kinds, the registry, and `ComponentMask`. |
| `buffer.h` | A buffer component: a header in the chunk, inline elements, a heap spill. |
| `sparse_store.h` | The side table a sparse component lives in. |
| `archetype.h` | Archetypes over M1's chunks, and why they are held by pointer. |
| `world.h` | Everything an entity's lifetime touches, and the deferral rule. |
| `query.h` | Matching, the cached archetype list, change and shared filtering. |
| `command_buffer.h` | Deferred structural change and the placeholder. |
| `resource.h` | Named typed singletons that participate in conflict detection. |
| `system.h` | Stages, and the binding onto `<cy/core/jobs/schedule.h>`. |
| `relationships.h` | `Parent` and `Children`, maintained by the world. |
| `snapshot.h` | In-memory snapshots, and the world byte stream. |
| `diagnostics.h` | The counters, on the M0 trace. |

## Three things a later milestone should know

**A query is its own access declaration.** M2 is the first real consumer of M1's conflict checker,
and the model expresses what a real system needs on one condition: that the query and the
declaration are the same object. A system that writes down its access separately from the query it
runs can drift, and nothing catches the drift, because a declaration is only ever checked against
other declarations. `QueryDesc` therefore *is* the declaration — every term records itself into a
`jobs::AccessSet` as it is added — and `SystemDesc::access` is normally `query.desc().access()`. The
one shape the model cannot express is a system whose access depends on its input; none exist yet,
and one would have to declare the union.

**Entity reference sites are declared, not discovered.** A component that holds an `Entity` says
where, in byte offsets, when it is registered. Serialization remaps references by walking those
offsets — a strided pass over known columns — rather than asking reflection per row what a field
means. This is the M2 spike's second proposal expressed one level up: the spike measured the
reflection-driven alternative at 4.7–5.2× and named it as the thing that would turn a cooked cell's
activation into the walk the archetype layout exists to avoid.

**`Parent` and `Children` are built-in components, not reflected ones.** They are maintained by the
world rather than authored, never appear in a prefab, and carry entity references rather than data,
so they have no manifest identifier and are keyed by name; a serialized world names them rather than
numbering them (`ComponentRegistry::register_builtin`). This is a *seam*, not a category: the
reflection generator's annotated-header list lives in `src/core/reflect/CMakeLists.txt` and the
identifiers come from `identity/manifest.toml`, neither of which this module owned at M2. When
`src/ecs/`'s own headers are wired into the generator, both take manifest identifiers and move to
`register_reflected` with no change to anything that consumes them.

Being unreflected had one consequence that could not wait for that, and M3's task 1.2 closed it:
the state hash covered only reflected components, so **a divergence in an entity's parent produced
the same hash as no divergence at all**. `state_schema.h` declares both to
`determinism::StateSchema` with an explicit field list — `Parent` by the parent's entity *index*,
which is what `runtime::hash_world` already treats as an entity's identity, and `Children` with no
hashed fields, because `ecs-core` leaves the buffer's order unspecified and hashing it would hash
operation history. `integration.state_hash_coverage` is the claim, run. The same applies to the test
fixtures in `tests/fixtures.h`, whose hand-written `TypeInfo` descriptors carry identifiers from a
9000-range that the manifest never issued and that never leave a test's own registry.
