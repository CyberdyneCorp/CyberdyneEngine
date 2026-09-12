# CyberFoliage — `src/foliage/`

Vegetation as a GPU-instance ecosystem, with entities as the exception rather than the rule.
M10 section 2 (task 2.4), and the `foliage` capability reaching **Working**.

A million trees cannot be a million entities. But "vegetation is never interactive" is not an
acceptable architecture either, so the bridge is **promotion**, and placement is **deterministic
from rules, a seed and a region** so that a forest is regenerated rather than serialised.

---

## Two targets, and the line between them is the renderer

| Target | What is in it |
| --- | --- |
| `cy::foliage` | Species, the sixteen-byte instance and its clusters, placement, exceptions, promotion, ground cover, wind, the interaction field, the budget, streaming, diagnostics. **Names no `rendering::` type.** |
| `cy::foliage-render` | `src/cook.cpp` alone: the `FoliageSurface` → `rendering::vg::SurfaceClass` mapping, publication as `rendering::vg::GeometryInstance`, and the arbiter adapter. |

`src/terrain/`'s `cy::terrain-cook` and `src/ml/`'s `cy::ml-cook` are the precedents. The split is
what makes the specification's own claim checkable: *"Foliage SHALL publish into the GPU scene as
instances … and SHALL be culled, shaded, and shadowed **by the same passes** as other geometry"* —
the only file a foliage-specific pass could hide in is `src/cook.cpp`, and `src/cook.cpp` produces
one engine-standard instance record and declares one priced ladder.

---

## The five decisions worth reading

### 1. Sixteen bytes, and identity is not one of them

`FoliageInstance` is 16 bytes and `static_assert`ed at that size. Position is **cluster-relative and
quantised** to three `u16`; rotation is a `u16` yaw plus two signed-byte tilts, not a quaternion;
and **identity is not stored at all** — it is derived from the cluster and the slot.

That derivation is **substream then draw**, and the M10 PCG spike is why. Of four identity schemes
measured against 7 877 hand-placed overrides rebound across twelve regenerations: a traversal
**counter** mis-bound 3 351 of them — 43% — on an ordinary *full* regeneration; a **rank** among
survivors mis-bound 292; a value **derived from stable identifiers** mis-bound **zero**. The spike
also found a defect in its own first derivation, and `instance.h` repeats the fix rather than the
defect: `fold_multiply(region + 1, slot + 1)` is plain `a * b` for small operands, and a product is
not injective.

A slot is stable because `ClusterBuilder::finish()` imposes a **canonical order over the stored
record** — species, then quantised z, x, y, then the rest. Two machines that accepted the same set in
different orders write byte-identical clusters. `test_instances.cpp` builds one cluster forwards and
one backwards and requires zero differences.

### 2. Placement is order-free, and this module owns no cache

`foliage` says placement rules are procedural programs and *"Foliage SHALL NOT maintain a separate
procedural execution or invalidation model."* `src/pcg/` does not exist yet — it is M10 section 4 —
so this module builds the half that is foliage's own and **refuses to build the half that is not**:

* the rules and their evaluation are here, because what a placement rule *reads* is foliage's
  knowledge;
* the execution and invalidation model is **not** here. There is no cache, no dirty set, no
  dependency graph and no traversal in `src/placement.cpp`. `generate_region()` is a pure function
  and reports, in `Provenance`, exactly which regions and which fields it actually read.

Three of the spike's four binding conditions constrain this file:

1. **Order-free conflict resolution.** `resolve_spacing()` compares a candidate against every *other
   candidate* within reach — including neighbours', which are **regenerated** rather than read out of
   a neighbour's result. A candidate's fate is a function of the candidate sets alone. The spike's
   `ordered` variant reproduced 2 of 12 trials at best.
2. **Invalidation is a fixed point, not a radius** — and the fixed point is PCG's. What this module
   owes it is the provenance record. The spike's own design note is that the cost of partial
   regeneration is dominated by long-range gathers (179 regions invalidated to find 1 changed) and
   that the answer is a record of what was read rather than a wider radius.
3. **An iterative operator declares convergence, not a budget.** *There is no iterative operator
   here*, deliberately: spacing is resolved in one order-free pass rather than by relaxation, so
   there is no sweep count to truncate. The spike's `budget2` axis reproduced 0 of 12 trials in all
   twelve of its configurations — the only axis with no survivor anywhere.

`test_placement.cpp` holds the property that matters: one region generated alone is bit-identical —
output *and* generated identity — to the same region generated inside a sweep over its neighbours,
and the sweep runs in the opposite order.

### 3. Placement reads what terrain produced, not an assumed heightfield

`PlacementSampler` goes through `terrain::TerrainQuery::sample()`, which answers for **any**
representation. So:

* a world with authored cliffs and arches places on the surface that is actually there —
  `test_placement.cpp` puts a `MeshSource` ledge over the hillside and requires every plant to be on
  it;
* a **hole is not a surface**, which is `terrain`'s own rule. A consumer that only checked `resolved`
  could plant a tree in a cave mouth; this one checks `resolved && !hole`, and the test punches a
  real hole and watches the count fall.

Field samples go through `sample_deterministic()` and never `sample()`. A forest whose density came
from the finest resident level would differ between a machine that had streamed the region and one
that had not.

### 4. Exceptions have two anchors, and an ambiguous match is an orphan

*"Only exceptions to procedural placement SHALL be stored … anchored by stable instance identity
**and** spatially … An exception that cannot be re-resolved SHALL become an orphaned exception:
retained, reported, and resolvable, and SHALL NOT be silently discarded."*

Identity alone is orphaned by a rule-graph change (the version participates in `cluster_identity()`,
so the change renames every cluster). Space alone mis-binds: two saplings a metre apart are the same
position to within any useful tolerance, and a wrong binding silently moves a *different* object,
which is worse than losing the exception because nothing reports it.

So `resolve()` tries identity, then a spatial match constrained by species **and** radius **and
uniqueness** — two equally close candidates are `Ambiguous`, which is an orphan an author can fix
rather than a coin toss nobody finds. Persistence goes through `world::PersistenceOverlay::
record_blob()` under `kOverlayChannelFoliage`; authored exceptions are cooked and are not written to
a save.

### 5. Regional state is read, never stored

*"Regional state SHALL be read from fields rather than stored per instance, so that a burned forest
costs a field region rather than a million instance updates."* `regional_state_at()` is a function of
the field store and a position, and there is no member of `FoliageInstance` that could hold a state.
`test_fields.cpp` measures the **absence**: a fire publishes one field, the state reports `Burning`,
and the cluster's instance bytes are compared with a `memcmp` before and after.

Foliage **produces** exactly one field — `vegetation`, the ecosystem state — through
`FieldRegistry::claim()`, which refuses a second producer and names both. Being macro-resident
everywhere is what lets an unloaded region's ecosystem evolve, which is the *"a grown forest appears
grown"* scenario; declaring a `potential` link and a recovery rate is what makes a burned forest
recover rather than be repainted.

---

## What is measured, and what is not

| Claim | How it is checked |
| --- | --- |
| Tens of bytes per instance | `static_assert(sizeof == 16)` plus `ClusterBuildReport::bytes_per_instance()` |
| Clusters culled before instances | `CullResult::instances_rejected_in_bulk` against `instances_visible` |
| A partial regeneration reproduces the full one | 9 regions, both orders, output **and** identity compared |
| Storage proportional to change | `ExceptionStore::bytes()` against `FoliageCluster::bytes()` |
| Zero mis-bound overrides | `ResolutionReport::bound_by_identity` = examined, orphans = 0 |
| No per-instance CPU wind work | `WindPrepareReport::clusters_prepared` vs `instances_covered` |
| Tiny ground-cover storage | `GrassReport::blades_per_stored_byte()` |
| Deterministic contributor drop | Two registration orders, identical admitted set |
| Ground cover reduced before trees | `FoliageBudget::steps()`, in `kReductionOrder` |
| Cover survives budget pressure | `fraction_for()` = 1 for `GameplayImportance::Cover` |

### Mutations run

Every claim above was broken and watched to go red, then restored. A test that has never failed
proves nothing, and the spike's own warning — a determinism test that passed on the very defect it
was written for — is the reason this list exists.

| Mutation | What went red |
| --- | --- |
| A candidate may only be rejected by an EARLIER one (the spike's `ordered` variant) | the shuffled-array case: a different survivor set |
| Identity becomes `(cluster + 1) * (slot + 1)` (the spike's own first defect) | 3 969 of 4 096 identities collide |
| The canonical order returns `false` (insertion order) | forwards and backwards build different clusters |
| The halo is zero rings | six of nine regions differ from the whole-world resolution |
| `evaluate_candidate` ignores `has_surface` | plants stand on the hole |
| Promotion does not set `kPromoted` | the instance is drawn and simulated at once |
| `representable()` never refuses | a mid-fall tree is demoted and loses its state |
| `register_producer()` swallows the claim refusal | a second producer of `vegetation` succeeds |
| Contributors are admitted in arrival order | a priority-0 walker displaces a priority-3 one |
| `kReductionOrder` cuts instance counts first | trees are removed before ground cover |
| Wind is prepared per instance | 500 field samples for 10 clusters |
| An ambiguous spatial match binds to the nearest | an exception silently moves a different tree |
| The regional state ignores the burn field | a burning forest reports `Normal` |

One of them says something worth recording: `has_surface` is `resolved && !hole`, and **for the
heightfield source the `!hole` clause alone changes nothing** — `terrain` already reports a hole with
`resolved == false`. It is kept because a representation MAY report both (the requirement's own
wording), and because a consumer that checked only `resolved` is the mistake the clause names; but
the guarantee today is terrain's, not this module's.

### Declared gaps

* **No `.slang` module.** `evaluate_response()` (wind) and `GrassField::expand_patch()` are the CPU
  statements of algorithms a shader runs, written and measured here for the reason
  `environment::sample_field_image()` exists — an algorithm that lives only in a shader is an
  algorithm nothing measures. Neither has been executed on a device, and no shader in the stdlib
  imports them. `environment-fields` left the same gap for the GPU field sampler; it is owed by
  whichever renderer-facing row first evaluates foliage deformation in a shader.
* **PCG owns the cache.** There is no derivation-key cache and no dirty set here, by design (§2).
  Task 4.3 is where `Provenance` is consumed.
* **No `BudgetSubsystem::Foliage`.** Foliage declares a priced composite ladder under
  `BudgetSubsystem::Geometry`; adding an eighth subsystem would be a change to
  `rendering-architecture` made from inside a foliage module. See `budget.h`.
* **Authoring is the data model, not an editor panel.** `PlacementDiagnostic::dominant_exclusion()`
  answers *"why is nothing growing here"* and `generate_region()` is the preview path — the editor
  surface that calls them is `editor-*`'s, not this module's.
