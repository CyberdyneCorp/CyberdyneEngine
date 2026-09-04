# Tasks: M2 — World

Ordered. The flattening spike first, because three later milestones are priced on its outcome. Then
the ECS, because everything else is a view onto it; then the façade and serialization in parallel;
then the loop and the determinism seeds; then the artefact.

## 0. Spike — cook-time flattening

M2's named risk. Its only deliverable is a decision (`design.md` §4).

- [ ] 0.1 Cook a deliberately awkward prefab: nested variants, cross-references between instances,
      exposed parameters overridden at two levels
- [ ] 0.2 Measure whether the result memcpy's into an M1 chunk, or needs a fixup pass walking
      references. State which, with the numbers.
- [ ] 0.3 Confirm the flattened block's layout matches `<cy/core/memory/chunk_storage.h>` rather
      than merely resembling it
- [ ] 0.4 If fixup is required, propose the roadmap change **before** section 1 proceeds — a
      narrower flattening guarantee or a changed reference model, not a quiet acceptance

## 1. Carried forward from M1

Do these first: the ECS is the consumer that makes each of them matter.

- [ ] 1.1 Reflection lookup is no longer linear, and record decode is linear in the record rather
      than quadratic in field count (the spec delta in this change)
- [ ] 1.2 Measure the reflected path a scene load takes, against a representative type set; record
      the numbers
- [ ] 1.3 Declare the generator's libclang binding in `deps/manifest.toml` and `THIRD_PARTY.md`, and
      check it in `env-doctor` — it is currently an undeclared machine-local dependency
- [ ] 1.4 Either compile and test the AVX2 path or remove it; it is gated on `__AVX2__`, which no
      build sets, so it has never been compiled by anyone

## 2. ECS core — `ecs-core` → Working

- [ ] 2.1 Entities: identity, generation, liveness
- [ ] 2.2 Components: registration through M1's reflection, storage layout
- [ ] 2.3 Archetypes over M1's chunked storage — **no allocator of its own** (`design.md` §1)
- [ ] 2.4 Queries: matching, iteration, filtering
- [ ] 2.5 Systems and access declarations, over M1's `<cy/core/jobs/access.h>` — the first real
      consumers of the conflict checker
- [ ] 2.6 **Structural change deferral** (`design.md` §2), with a test that mutates mid-iteration
      and observes the deferral
- [ ] 2.7 Resources and singletons
- [ ] 2.8 Change detection and versioning
- [ ] 2.9 Entity relationships
- [ ] 2.10 World serialization and snapshots
- [ ] 2.11 Multiple worlds
- [ ] 2.12 ECS diagnostics on the M0 trace

## 3. The façade and serialization

### 3.1 Nodes — `scene-graph-and-nodes` → Working

- [ ] 3.1.1 `Node` as a view onto an entity — **no duplicated data, no shadow copy, no sync step**
- [ ] 3.1.2 Hierarchy and naming
- [ ] 3.1.3 The transform model
- [ ] 3.1.4 Node types and components
- [ ] 3.1.5 Visibility and enablement
- [ ] 3.1.6 Node lifecycle callbacks
- [ ] 3.1.7 Behaviours bridging nodes and systems
- [ ] 3.1.8 Groups and tags
- [ ] 3.1.9 Scenes and worlds
- [ ] 3.1.10 **The coherence invariants as tests** (`design.md` §3), not as prose

### 3.2 Serialization and prefabs — `serialization-and-prefabs` → Working

- [ ] 3.2.1 Prefab, scene and world as distinct asset kinds
- [ ] 3.2.2 Two serialization modes, one reflection-driven traversal (`design.md` §6)
- [ ] 3.2.3 Field classification; text and binary forms; authoring file granularity
- [ ] 3.2.4 Entity references
- [ ] 3.2.5 Prefabs, instance overrides, and variants
- [ ] 3.2.6 **Overrides address stable identifiers** from M1's manifest — the first data whose
      correctness depends on an identifier never being reused
- [ ] 3.2.7 Exposed prefab parameters; apply and extract; diff and override provenance
- [ ] 3.2.8 Dependency cycles are rejected
- [ ] 3.2.9 Schema versioning, value migration, and unknown data preserved
- [ ] 3.2.10 Scene and prefab cooking; scene instances and cook modes
- [ ] 3.2.11 **Hierarchy flattening** into archetype blocks, per the spike's outcome
- [ ] 3.2.12 Entity templates and batch spawning; live prefab update
- [ ] 3.2.13 `core-assets-and-io` → Working: cooked assets, the cook path, hot reload

## 4. The loop and the determinism seeds

### 4.1 `engine-architecture` → Working

- [ ] 4.1.1 Servers and the ECS/scene duality
- [ ] 4.1.2 **The fixed-tick loop**: N simulation ticks then one variable render, with the
      interpolation alpha. Max 8 ticks per frame.
- [ ] 4.1.3 The deferred command queue
- [ ] 4.1.4 Build-time feature slicing

### 4.2 `simulation-and-determinism` → Seed — **invariant, M2**

- [ ] 4.2.1 The simulation clock; every system reads time from it, never a wall clock
- [ ] 4.2.2 Simulation epochs
- [ ] 4.2.3 **The commit boundary** — one point per tick where deferred change is published
- [ ] 4.2.4 Seeded random streams; every draw comes from a named stream
- [ ] 4.2.5 State classification — **make an illegal read unspellable, not merely refused**
      (`design.md` §5; M1's "declared blocking only" trap is the precedent)
- [ ] 4.2.6 Hierarchical state hashing
- [ ] 4.2.7 Stable iteration and tie-breaking

## 5. The artefact

- [ ] 5.1 `samples/02-headless-sim` — loads a scene, ticks 10,000 fixed steps, prints a hierarchical
      state hash
- [ ] 5.2 `just run-sample headless-sim`
- [ ] 5.3 Smoke test asserting the hash reproduces across runs and after snapshot restore

## 6. Closing the milestone

- [ ] 6.1 The state hash is identical across runs, across process restarts, and across
      restore-from-snapshot
- [ ] 6.2 A scene round-trips text → binary → text with no semantic change
- [ ] 6.3 A prefab override survives a field rename with a tombstone
- [ ] 6.4 Cooked scenes load as archetype blocks; a bulk-copy activation is benchmarked
- [ ] 6.5 The coherence invariant tests pass: no node duplicates component data, no orphaned entity
- [ ] 6.6 Structural changes are observable only at flush points, proven by a test that tries
      otherwise
- [ ] 6.7 All four profiles build clean and `just test-all` is green in each
- [ ] 6.8 Sanitizers green over the new suites, with intentional lifetime allocations declared
- [ ] 6.9 `just roadmap-milestone m2` exits zero, and `m1` and `m0` still do
- [ ] 6.10 Update `docs/roadmap/status.yaml` and `capability-matrix.md`; record what is thinner than
      the tasks claim
- [ ] 6.11 `openspec validate --specs --strict` passes; archive this change
- [ ] 6.12 Open the M3 change — render graph scheduling with async compute is its named spike
