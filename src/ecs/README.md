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
| `firewall.h` | **The determinism firewall's enforcement point.** Read it before adding a write path. |

## The determinism firewall is enforced here (M8.c section 1)

`vfx-system` and `ml-inference` each forbid their own subsystem from writing authoritative gameplay
state, and **neither specification says where that is enforced**. M8.c required one enforcement
point to be named and built, and the two candidates M8.b left were `gameplay-framework`'s command
origin and this module's write path. **The write path is the answer**, for three reasons stated in
full at the top of `firewall.h` and in one line each here:

1. A command is the simulation's *input*, not its write. The failure the rule is about — a VFX
   readback reaching into the world and poking a health value — never touches a `CommandStream`.
2. `gameplay-framework` forbids the check the other candidate would need: "Provenance SHALL NOT
   affect validation, ordering, or execution", and `sequencing-and-cinematics` requires that the
   simulation cannot distinguish a sequence-issued command from any other. Enforcing here leaves
   both intact, because the firewall never looks at a command.
3. `vfx-system` states the diagnostic in terms of *components*: "attempts to write replicated or
   physics-owned components from VFX-driven code paths". A component is an ECS concept.

**Every door into this world's storage consults `World::admit_write`**, and `firewall.h`
enumerates them: `World::get_mut`, `QueryChunk::write`, `World::set_sparse`/`remove_sparse`, the
structural entry points (`add`, `remove`, `set_shared`), the lifetime entry points (`create*`,
`destroy*`, `instantiate`), `World::set_parent`, `CommandBuffer::record`, and `Snapshot::restore`.
Two of those are the ones worth knowing about. `CommandBuffer::record` is checked at **record** time
rather than at flush time, because the flush runs at a stage boundary under the simulation's own
origin and would otherwise launder a VFX-driven structural change into an authoritative one; and
`Snapshot::restore` is checked at the call because `Snapshot` is a friend of `World` and writes
archetype rows directly, so it inherits nothing from a public entry point.

`World::get` and `QueryChunk::read` are deliberately NOT doors — they hand back const, and
`vfx-system` explicitly permits VFX to read gameplay state.

**Nothing changes until a caller opens a `WriteScope`.** The origin is a stack-scoped, thread-local
frame, because "what code path is executing" is a property of a call stack and two systems of one
stage run concurrently over one world. Everything that has not declared itself is
`WriteOrigin::Simulation` and is unrestricted; `Vfx`, `Inference` and `Presentation` are the three
that are not, and `PinnedInference` is unrestricted because `ml-inference` says a pinned session may
drive authoritative state.

**A component is guarded only when something declares it.** `declare_from_reflection` derives
`Replicated` from a field's `Replicated` attribute and `Authoritative` from an explicitly declared
authoritative `Persistence`; a field that declares neither derives nothing, and the count of
components it could not derive is reported rather than hidden. `PhysicsOwned` is not derivable and
is declared by hand.

`<cy/core/determinism/classification.h>` is the other half and not a competitor: it makes an
illegal *read* unspellable at compile time for state that adopted `Classified<>`. This is the
runtime half, and it exists because classification is opt-in per field while a component's storage
is reachable through those doors whether or not anybody adopted a wrapper.

**Two suites, and the second is here because the closing gate found the criterion unable to fail.**
`src/ecs/tests/test_firewall.cpp` is the doors, one case each, in the module that owns them —
`unit.ecs`. It exists because M8.c's gate mutated `WriteFirewall::admit` to return true
unconditionally and `unit.ecs` still passed: not one of its fourteen sources named the firewall,
while `tools/roadmap/milestones/m8c.toml`'s `m8c:firewall-ecs` criterion ran that suite and
described "every door … refusing a write whose origin may not make it". With the suite in place the
same mutation fails 8 of 54 cases and 34 of 1,048 assertions. The thread-locality half is
deliberately not in it: this tier starts no thread, and the integration suite below covers it with
eight.

**AND WHAT NEITHER SUITE CAN TELL YOU: NOTHING IN THE ENGINE OR IN THE ARTEFACT DECLARES COMPONENT
AUTHORITY YET.** `WriteFirewall` is armed from construction and `guards()` nothing until a caller
calls `declare` or `declare_from_reflection`, which this header says plainly — and the only callers
in the tree are three test files. A game built on this engine today therefore has a firewall that
refuses nothing, because it has been told nothing is authoritative. The mechanism is real,
enforced at one point, and proven by mutation; **its adoption is not, and the number that would say
so is `AuthorityDerivationReport::underived` beside `guarded_count()`, printed by nobody.** The
check this should become is a startup-time report — how many of a world's registered components are
guarded — asserted by the artefact rather than by a unit test with three components in it.

The second suite is `src/gameplay/tests/test_firewall.cpp` (it links `cy::runtime` for the state
digest, which is why it does not live here), and it is a negative control: four separate source mutations —
removing the enforcement point, removing the `get_mut` door, removing the deferred door, and
removing the development-build report — each turn it red, and the fourth is the interesting one
because the digest still matches (the firewall still refuses; only the report is gone). The results
are on `docs/design/images/m8c-determinism-firewall.png`, which is a **diagram** and says so, with
the mutation table's numbers taken from the runs themselves.

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
