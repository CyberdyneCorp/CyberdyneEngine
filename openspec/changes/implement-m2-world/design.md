# Design: M2 — World

## The M1 handoff

M2 is the first milestone written substantially *on top of* engine code rather than beside it.

| | |
|---|---|
| Chunked storage | `<cy/core/memory/chunk_storage.h>` — allocator, layout and iteration, deliberately with no knowledge of components or archetypes. M2's ECS is its first consumer, and this is the split that keeps `ecs-core` about entities rather than about memory. |
| Reflection | `<cy/core/reflect/…>` — the registry, the generator, and the committed identity manifest with its gate. Working, but on **two demonstration types**; see the caveat below. |
| Values | `Var`, generational handles, asset ids distinct from handles, events, `Callable`, interning. |
| Jobs | `<cy/core/jobs/access.h>` — Read/Write/Exclude declarations and the conflict checker, exercised so far only by synthetic systems. M2 brings the first real ones. |
| Math | Conventions as executable tests. A scalar SIMD reference that is always compiled. |
| Assets | Seed: identity, virtual filesystem, package read path, async load. Cooking is this milestone's. |
| Fallible calls | `cy::Expected<T, Error>`, `cy::fail(...)`. `CY_ASSERT` compiled out in Profile and Shipping. |

**The caveat that will bite first.** `TypeRegistry::find` is a linear scan and `read_record` calls
`find_field` once per field per record, so decoding is quadratic in field count. With two reflected
types nothing noticed. M2 reflects hundreds and loads real scenes through that path. Fix it before
the first scene load, not after — the spec delta in this change states the complexity contract.

## 1 — Archetypes are the storage; M1 owns the chunks

**Decision.** `ecs-core` implements entities, archetype identity, the component-type registry, query
matching and iteration **on top of** M1's chunk allocator, and does not implement memory management
of its own.

The temptation is to write a chunk allocator inside the ECS because the ECS knows its access pattern
best. That is exactly the argument that produces two allocators, two budget accountings, and a
subsystem the memory-pressure system cannot see. M1's budget tree and pressure levels are what M6's
residency policy holds allocations from; an ECS that allocates outside them is invisible to the
system that will later have to evict.

## 2 — Structural change deferral is not an optimisation

`ecs-core` requires structural changes — creating and destroying entities, adding and removing
components — to be deferred to stage flush points.

**Decision.** Deferral lands with the first structural operation, not after profiling shows it
matters. It is a *correctness* property: a system iterating a chunk while another system moves an
entity out of it is iterating freed memory, and the scheduler parallelises systems by construction
from M1's access declarations. Immediate structural mutation and parallel systems cannot coexist.

Write the test that tries to mutate structurally mid-iteration and observes the deferral, rather
than one that merely checks the result is eventually right.

## 3 — The node façade never owns data

`scene-graph-and-nodes` is explicit: a `Node` is a named handle onto an entity and never duplicates
component data.

**Decision.** `Node` holds an entity id and a world reference. Every property it exposes reads or
writes through the ECS. There is no shadow copy, no dirty flag pair, no "sync the node to the
entity" step — because the moment one exists, the two representations can disagree, and every bug
that follows is a debugging session about which one was right.

The coherence invariants in the specification become tests in this milestone rather than prose: a
node's transform *is* the entity's transform, destroying an entity invalidates its nodes, and no
traversal order produces a state a direct ECS query would not.

## 4 — Cook-time flattening — the spike

The specification's claim is that prefabs, scenes and worlds resolve at cook time into archetype
blocks matching the runtime's chunk layout, so activating a streaming cell is a bulk copy and a
shipping build carries no prefab link.

**This is M2's named risk and it runs first**, because three later milestones are priced on it: M6's
cell activation, M6's save overlay, and the whole argument for archetype storage.

The spike cooks a deliberately awkward prefab — nested variants, cross-references between instances,
exposed parameters overridden at two levels — and measures whether the result is a block that can be
memcpy'd into a chunk, or a block that needs a fixup pass walking references. If it needs fixup, say
so plainly: the honest outcomes are a narrower flattening guarantee, or a change to the reference
model, and both are roadmap changes rather than things this milestone absorbs quietly.

## 5 — The commit boundary lands now, the validator does not

`simulation-and-determinism` reaches **Seed**: the simulation clock, epochs, the commit boundary,
seeded random streams, state classification, and hierarchical state hashing. The determinism
profiles, the validator, the lint and the divergence capture are M9.

**Decision.** Seed here means *the shape is right and the hooks exist*, not that anything is
validated. Specifically: every system reads simulation time from the clock rather than a wall clock;
every random draw comes from a named seeded stream; state that participates in the hash is declared
rather than discovered; and the tick has one commit point where deferred changes are published.

**A warning from M1 that applies directly here.** M1's "workers never block" rule is enforced for
*declared* blocking; an undeclared `read()` on a worker is caught only by a 250 ms watchdog. The
same trap is waiting for state classification: a checkpoint that *refuses* an illegal read at a
boundary will not see a system that simply reads a global. Classification has to make the illegal
read **unspellable**, not merely refused — otherwise M9 inherits a validator that reports clean on
code that is not.

## 6 — Two serialization modes, one reflection source

`serialization-and-prefabs` requires a text form for authoring and source control and a binary form
for cooked data, both reflection-driven.

**Decision.** One traversal, two writers. The text form is the authoring artefact — diffable,
mergeable, and the thing a designer's tool edits. The binary form is a cooked derivation of it,
never authored directly, and never the source of truth for anything under version control.

Overrides address **stable identifiers** from M1's manifest, not names and not paths. This is where
M1's identity work stops being theoretical: a prefab override is the first data whose correctness
depends on an identifier having been assigned once and never reused.

## 7 — What M2 deliberately does not do

- **No renderer.** M3. Nothing in M2 draws; the closing artefact is headless by design.
- **No streaming, no cells, no residency.** M6. M2 cooks scenes; loading them under a budget is later.
- **No determinism profiles, no validator, no replay.** M9. The hooks land here; the enforcement does not.
- **No ABI, no Swift, no editor.** M4 and M5.
- **No physics, no animation, no audio.** M4 and M8. A component is data; nothing simulates it yet.
