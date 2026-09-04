# Implement M2 — World: entities, nodes, and the commit boundary

## Why

M1 built the services. M2 builds the thing they serve: a world of entities that a designer authors
as a tree and the runtime processes as arrays. It is the last milestone before anything is visible
on screen, and the last one whose mistakes are cheap.

Two commitments land here that nothing later can add.

**The commit boundary is a property of every line of simulation code written after it.** Determinism
is not a subsystem — it is a discipline that every system obeys or silently breaks. The validator
that finds violations arrives at M9; by then there are eight milestones of code either written under
the constraint or not. Seeded random streams, the fixed tick's commit point, state classification
and hierarchical state hashing therefore land at M2 at Seed tier, so that M3 through M8 are written
under the rule rather than audited against it afterwards.

**Cook-time flattening is the reason the ECS exists.** A designer authors hierarchies; the runtime
gets flat data. If prefabs and scenes resolve at cook time into archetype-native blocks matching the
chunk layout M1 built, activating a streaming cell at M6 is a bulk copy and a shipping build carries
no prefab link at all. If they need runtime fixup, the prefab graph becomes load-bearing at runtime,
activation stops being a copy, and the storage argument collapses. That is why it is this
milestone's spike rather than an implementation detail discovered halfway through.

The third reason is quieter: **the node façade is where the engine's two audiences meet.** Component
data lives in packed per-archetype chunks; a `Node` is a named handle onto an entity and never
duplicates it. Designers get the tree, the runtime gets the arrays, and the coherence invariants
between the two are specified rather than assumed — because a façade that drifts from its storage is
worse than no façade at all.

## What Changes

Four workstreams, one milestone gate. `tasks.md` has the ordered plan; `design.md` records what the
specifications leave open and what M1's handoff makes possible.

- **The ECS core.** Entities, components, archetypes over M1's chunked storage, queries, systems and
  their access declarations, **structural change deferral**, resources and singletons, change
  detection and versioning, entity relationships, world serialization and snapshots, multiple
  worlds.
- **The node façade.** `Node` as a view onto an entity, hierarchy and naming, the transform model,
  node types and components, visibility and enablement, lifecycle callbacks, behaviours that bridge
  nodes and systems, groups and tags, scenes and worlds, and **the coherence invariants as tests**.
- **Serialization and prefabs.** Both serialization modes, reflection-driven from M1's registry,
  field classification, text and binary forms, entity references, prefabs and instance overrides
  addressing stable identifiers, variants, exposed parameters, schema and value migration, unknown
  data preserved, and **cook-time hierarchy flattening into archetype blocks**.
- **The loop and the determinism seeds.** Servers, the ECS/scene duality, the **fixed-tick loop with
  its interpolation alpha**, the deferred command queue, feature slicing — and from
  `simulation-and-determinism`: the simulation clock, epochs, **the commit boundary**, seeded random
  streams, state classification, and hierarchical state hashing.

**Closing artefact**: `samples/02-headless-sim` — loads a scene, ticks 10,000 fixed steps, prints a
hierarchical state hash, and reproduces it exactly on re-run and after snapshot restore.

## Capabilities

### Advanced Capabilities

`ecs-core`, `scene-graph-and-nodes`, `serialization-and-prefabs` and `engine-architecture` to
**Working**; `simulation-and-determinism` to **Seed**; `core-assets-and-io` to **Working** as cooked
assets arrive.

### Modified Capabilities

- `core-type-system` — **reflection lookup is not a linear scan.** M1 reached Working on a two-type
  demonstration, where `TypeRegistry::find` scanning linearly and `read_record` calling `find_field`
  per field per record was invisible. M2 brings hundreds of reflected types and real scene loads, at
  which point deserialisation being quadratic in field count stops being control plane and becomes
  the asset path. The requirement now states the complexity the registry must meet and that the
  reflected path a scene load takes is measured rather than assumed.

## Impact

- **New code**: `src/ecs/`, `src/scene/`, `src/core/serialize/`, the loop in `src/runtime/`, and
  `samples/02-headless-sim/`. First code above layer 0.
- **New permanent gates**: the state hash reproduces across runs and across snapshot restore; a
  cooked scene loads as archetype blocks; the coherence invariants hold; structural changes are
  observable only at flush points.
- **Carried forward from M1**: the reflection registry's complexity, the undeclared libclang binding
  absent from `deps/manifest.toml`, and AVX2 gated on a macro no build sets — each recorded in
  `capability-matrix.md` and each now with a consumer that will notice.
- **Risk**: concentrated in the flattening spike. If an authored hierarchy cannot lower to
  chunk-shaped blocks without runtime fixup, the storage decision — and M6's bulk-copy activation —
  needs rethinking, and that is a roadmap change rather than something the milestone absorbs.
