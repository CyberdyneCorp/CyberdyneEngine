# `src/pcg/` — CyberPCG

Deterministic, region-based procedural generation over typed spatial datasets. M10 section 4, and
`procedural-content-generation` reaching Working.

**This row's scope was decided by a measurement rather than by a design meeting.** M10's named risk
was region invalidation, and its spike (`~/cyberdyne-spikes/m10-pcg-spike/`, recorded in
`openspec/changes/implement-m10-worlds/design.md` §1) asked one question: does a dependency-driven
PARTIAL regeneration reproduce a FULL regeneration of the same seed, bit for bit, for output *and*
for generated identity? The answer was **yes — in 2 of 24 configurations, in all 12 of 12 trials**,
and only when four properties hold at once. Those four properties are why this module has the shape
it has, and each of them is a test in `tests/` before it is a sentence here.

## What is here

| file | what it carries |
|---|---|
| `include/cy/pcg/dataset.h` | `AttributeId` and `AttributeTable` (compiled identifiers, never resolved by name at execution), `PointSet` (structure of arrays, one reserve, no per-point allocation), `Raster`, `Digest` |
| `include/cy/pcg/identity.h` | `GeneratedId` and `derive_identity()` — seed, node, region, slot, through `substream` then `draw` |
| `include/cy/pcg/ops.h` | The vocabulary a graph and a program share: the operator set, `NeighbourAccess`, `IterationPolicy`, `ExecutionDomain`, `GenerationBudget`, `DeterminismLevel` |
| `include/cy/pcg/graph.h` | The authoring layer: `GraphNode`, `Graph`, subgraph instantiation with typed exposed parameters |
| `include/cy/pcg/program.h` | The typed IR, `compile()`, the four refusals, the optimisation passes and `CompileReport` |
| `include/cy/pcg/execute.h` | `RegionState`, `GenerationWorld`, `SpatialQuery`, and `Generator` — the stage pipeline, the fixed point and the budgeted step |
| `include/cy/pcg/invalidation.h` | `RegionSet`, `dilate()`, `InvalidationLedger` (why is this dirty) and `ReadLedger` (what did this region actually read) |
| `include/cy/pcg/cache.h` | `DerivationKey`, `FieldVersions`, `RegionCache` and the refusal a budgeted result gets |
| `include/cy/pcg/overrides.h` | `OverrideLayer`, orphans, `PersistentDelta` and `re_resolve()` |
| `include/cy/pcg/diagnostics.h` | `RegionProvenance` (why is this here / why is nothing here) and `GenerationProfile` |
| `include/cy/pcg/adapters.h` | `OutputAdapter`, `OutputRegistry`, `FieldOutputAdapter`, `RecordingAdapter` |
| `include/cy/pcg/foliage_adapter.h` | **The second target.** `FoliageOutputAdapter`, `TerrainStampAdapter`, `TerrainSpatialQuery` |

## The four conditions, and where each one lives

| condition | the spike's number without it | where it is enforced |
|---|---|---|
| **Order-free conflict resolution.** A node may read its neighbours' *candidates*, never their accepted output | `ordered` reproduces **2 of 12** at best | `compile()` refuses `NeighbourAccess::AcceptedOutput` by name. `eval_spacing()` reads candidate lists and could not use an accepted one if it had it |
| **Invalidation expanded to a FIXED POINT**, not a declared radius applied once | `static` reproduces **7 of 12** — the dangerous cell, which passes a casual test more often than not | `Generator::expand_after_sweep()`, the only closure this module offers |
| **An iterative operator declares CONVERGENCE, not a budget** | `budget2` reproduces **0 of 12, in all 12 of its configurations** — the only axis with no survivor anywhere | A budget is allowed and sets `Program::cacheable()` false; `RegionCache::store()` refuses it |
| **Identity DERIVED from stable identifiers**, never a traversal counter and never a rank among survivors | `counter` mis-binds **43%** of 7 877 overrides on an ordinary FULL regeneration; `rank` 3.7%; `derived` **0** | `compile()` refuses the other two by name. `PointSet` stores each point's original SLOT so a filtered set cannot invent a rank |

## The six decisions a reader should know before changing anything

**1. Evaluation is STAGE-MAJOR, not region-major, and that is the whole reason a partial
regeneration reproduces a full one.** A gather reads its neighbours' output *of the previous stage*,
which is complete for every region before the stage begins — in a full run and in a partial one
alike. Region-major evaluation would have made that false and would have produced exactly the spike's
`ordered` configuration.

**2. The authoring graph is unreachable from the evaluator.** `ops.h` holds the vocabulary the two
share; `program.h` names `Graph` by forward declaration only; `execute.h` includes `program.h`. So
"interpreted virtual-node graph traversal SHALL NOT appear in a hot execution path" is a fact about
the include graph rather than about a reviewer's attention.

**3. The declared reach is the conservative bound; the RECORDED reads are the narrow one.** A node's
declared reach is what the first invalidation dilates by, because a region never yet evaluated has no
record. Once a region has been evaluated, `ReadLedger` says what it actually touched and the next
dirty set is narrowed to that. This is design.md §1.3's answer to the long-range gather — which
invalidated 179 regions of 576 to find ONE — and `forest.smooth` in the suite declares a reach of two
regions while reading one, so the narrowing is measured rather than asserted.

**4. `cy::pcg` cannot name a representation, and `cy::pcg-adapters` is where that is allowed.** The
generator links `cy::environment` and `cy::world` and nothing else above the core: no `cy::foliage`,
no `cy::terrain`, no `cy::ecs`, no physics, no renderer. "A procedural result SHALL NOT be an entity
by default" is strongest as a module that cannot spell `Entity`, and that is what `cy::pcg` is. The
adapters target is weaker and says so: it links `cy::foliage`, which links `cy::ecs` for its own
promotion path, so there the claim is behavioural — one cluster per region, built in one call — and
`tests/test_adapters.cpp` measures it rather than asserting the link graph.

**5. A field is read through the DETERMINISTIC path, always.** A generated world is authoritative
state — a save, a replay and a peer all have to agree on it — and `environment::FieldStore::sample()`
walks levels finest-first, so its answer depends on what streamed. This module never asks for that
one.

**6. The platform participates in every derivation key, and that is a temporary honesty.** design.md
§1.5 declines to claim on this host that a region generated on one architecture reproduces on
another, so a cache populated on one target is a MISS on another rather than a silently-wrong hit.
When `pcg-regeneration-cross-platform` goes green in continuous integration, the honest change is to
**delete** `platform_tag()`'s contribution, not to add a second one.

## What is NOT here, and who owns it

These are gaps, not omissions by oversight. Each names the row that should close it.

- **GPU EXECUTION IS CLASSIFIED AND NOT DISPATCHED.** `classify_gpu()` decides which nodes are
  eligible and refuses them to a gameplay-deterministic generator, and `StageProfile::gpu_micros` is
  always zero and says so in its own comment. There is no compute path, no buffer and no shader. The
  criterion that would change this is `pcg-gpu-domain-agreement`, declared `where = "ci"` and
  reported **NOT EVALUATED** — because this host has one GPU vendor and "the GPU domain reproduces
  the CPU domain" measured against one driver is not that claim. **Owed by** whichever milestone
  brings a second vendor into continuous integration.
- **THE DERIVED DATA CACHE IS AN IN-PROCESS ONE.** `DerivationKey` is the complete key the
  specification asks for, and `RegionCache` honours it, but nothing binds it to
  `build-and-packaging`'s artefact store — so "continuous integration has generated a region, a
  developer fetches it rather than regenerating" is not yet true, and neither is distribution across
  remote workers. **Owed by** a `build-and-packaging` integration.
- **PARALLEL EXECUTION IS ANALYSED AND NOT SCHEDULED.** `classify_parallelism()` reports
  `PerRegion`, `Swept` or `Barrier` per stage and the evaluator runs them on the calling thread. A
  second scheduler inside PCG is the parallel mechanism this project keeps refusing to build;
  dispatching the `PerRegion` stages through `core-jobs-and-concurrency` is the correct next edit and
  it changes no result, because every `PerRegion` stage is already a pure function of its region.
- **THE EDITOR TOOLING IS DATA WITHOUT A VIEW.** `RegionProvenance`, `InvalidationLedger`,
  `ReadLedger` and `GenerationProfile` carry everything "PCG diagnostics" asks a graph editor,
  region debugger, seed inspector, dirty-set view and generation profiler to show. None of it is
  drawn. **Owed by** the editor rows.
- **NETWORKING OF GENERATED CONTENT IS THE VERSION CHECK AND NOT THE POLICY.**
  `Program::digest()` and the generator version are what a session would compare at connection, and
  `procedural-content-generation` also asks for replication of the result or of the deltas where
  generation is not deterministic. That policy belongs beside `networking-and-replication`'s own, and
  is not written here.
- **THE LARGE-WORLD BENCHMARK IS NOT COMMITTED.** "PCG performance" asks the engine to maintain a
  hundred-kilometre-square world with ten million trees as a standing benchmark. The suites here
  measure the *properties* at the scale a test budget allows — a million points through one reserve,
  64 regions regenerated bit-exactly — and `benchmarks/` has no PCG entry. **Owed by** M10's own
  section 7 artefact or by the next milestone's performance work.

## Reading order

`identity.h` first: it is short and it is what the rest is arranged around. Then `ops.h` for the
vocabulary, `program.h` for the refusals, and `execute.h` for the pipeline. `tests/fixtures.h`
describes the one graph every suite runs, and it carries the three kinds of edge the spike measured,
so it is the fastest way to see what the module actually does.
