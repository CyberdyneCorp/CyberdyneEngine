# Capability Matrix

Every specified capability against every milestone of [the roadmap](../ROADMAP.md), with the
maturity tier it reaches there.

| | |
|:-:|---|
| *(blank)* | Not started at this milestone |
| **S** | **Seed** — interfaces, data model and invariants exist; dependents can be built against it |
| **W** | **Working** — the requirements a real project depends on are satisfied, tested, and diagnosable |
| **C** | **Complete** — every requirement satisfied, every scenario tested or exempted, gates in CI |
| ◇ | Deferred, with prerequisites verified from this milestone onward |

A capability seeds at the milestone **its first dependent needs it**, not the milestone at which it
becomes interesting. A capability may not reach Working before its prerequisites reach Seed, nor
Complete before they reach Working — see
[the dependency rules](../../openspec/specs/delivery-roadmap/spec.md).

The `Reqs` column is the requirement count in that capability's specification. It is a rough
indicator of size, not of effort: `denoising` has 6 requirements and is harder than
`thirdparty-dependencies` with 9.

| Capability | Reqs | M0 | M1 | M2 | M3 | M4 | M5 | M6 | M7 | M8 | M9 | M10 | M11 | Complete |
|---|---:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| **1 — Foundations** | | | | | | | | | | | | | | |
| [`engine-architecture`](../../openspec/specs/engine-architecture/spec.md) | 10 |  | S | W |  |  |  |  |  |  |  |  | **C** | M11 |
| [`core-type-system`](../../openspec/specs/core-type-system/spec.md) | 13 |  | W |  |  |  | **C** |  |  |  |  |  |  | M5 |
| [`core-memory-and-containers`](../../openspec/specs/core-memory-and-containers/spec.md) | 16 |  | W |  |  |  |  | **C** |  |  |  |  |  | M6 |
| [`core-math`](../../openspec/specs/core-math/spec.md) | 10 |  | W |  | **C** |  |  |  |  |  |  |  |  | M3 |
| [`core-jobs-and-concurrency`](../../openspec/specs/core-jobs-and-concurrency/spec.md) | 16 |  | W |  |  |  |  |  |  |  | **C** |  |  | M9 |
| [`core-assets-and-io`](../../openspec/specs/core-assets-and-io/spec.md) | 10 |  | S | W |  |  |  | **C** |  |  |  |  |  | M6 |
| [`core-platform-abstraction`](../../openspec/specs/core-platform-abstraction/spec.md) | 7 | S |  |  |  | W |  |  |  |  |  |  | **C** | M11 |
| **2 — World model** | | | | | | | | | | | | | | |
| [`ecs-core`](../../openspec/specs/ecs-core/spec.md) | 12 |  |  | W |  |  |  |  |  |  | **C** |  |  | M9 |
| [`scene-graph-and-nodes`](../../openspec/specs/scene-graph-and-nodes/spec.md) | 10 |  |  | W |  |  | **C** |  |  |  |  |  |  | M5 |
| [`serialization-and-prefabs`](../../openspec/specs/serialization-and-prefabs/spec.md) | 23 |  |  | W |  |  |  | **C** |  |  |  |  |  | M6 |
| [`gameplay-framework`](../../openspec/specs/gameplay-framework/spec.md) | 33 |  |  |  |  | S |  |  |  | W | **C** |  |  | M9 |
| [`gameplay-abilities-and-effects`](../../openspec/specs/gameplay-abilities-and-effects/spec.md) | 20 |  |  |  |  |  |  |  |  | W | **C** |  |  | M9 |
| [`visual-scripting`](../../openspec/specs/visual-scripting/spec.md) | 20 |  |  |  |  |  |  |  |  | W |  |  | **C** | M11 |
| [`sequencing-and-cinematics`](../../openspec/specs/sequencing-and-cinematics/spec.md) | 31 |  |  |  |  |  |  |  |  | W | **C** |  |  | M9 |
| [`input-and-actions`](../../openspec/specs/input-and-actions/spec.md) | 23 |  |  |  |  | W |  |  |  | **C** |  |  |  | M8 |
| [`camera-system`](../../openspec/specs/camera-system/spec.md) | 28 |  |  |  |  | S |  |  |  | W | **C** |  |  | M9 |
| [`simulation-and-determinism`](../../openspec/specs/simulation-and-determinism/spec.md) | 20 |  |  | S |  | W |  |  |  |  | **C** |  |  | M9 |
| [`replay-and-rollback`](../../openspec/specs/replay-and-rollback/spec.md) | 19 |  |  |  |  |  |  |  |  |  | W |  | **C** | M11 |
| [`save-and-persistence`](../../openspec/specs/save-and-persistence/spec.md) | 20 |  |  |  |  |  |  | W |  |  | **C** |  |  | M9 |
| [`world-partition-and-streaming`](../../openspec/specs/world-partition-and-streaming/spec.md) | 32 |  |  |  |  |  |  | W |  |  |  | **C** |  | M10 |
| [`environment-fields`](../../openspec/specs/environment-fields/spec.md) | 12 |  |  |  |  |  |  |  |  |  |  | W | **C** | M11 |
| [`procedural-content-generation`](../../openspec/specs/procedural-content-generation/spec.md) | 22 |  |  |  |  |  |  |  |  |  |  | W | **C** | M11 |
| [`weather-and-wind`](../../openspec/specs/weather-and-wind/spec.md) | 16 |  |  |  |  |  |  |  |  |  |  | W | **C** | M11 |
| [`atmosphere-sky-and-clouds`](../../openspec/specs/atmosphere-sky-and-clouds/spec.md) | 13 |  |  |  |  |  |  |  | S |  |  | W | **C** | M11 |
| [`terrain`](../../openspec/specs/terrain/spec.md) | 15 |  |  |  |  |  |  |  |  |  |  | W | **C** | M11 |
| [`foliage`](../../openspec/specs/foliage/spec.md) | 13 |  |  |  |  |  |  |  |  |  |  | W | **C** | M11 |
| [`water`](../../openspec/specs/water/spec.md) | 17 |  |  |  |  |  |  |  |  |  |  | W | **C** | M11 |
| **3 — Scripting** | | | | | | | | | | | | | | |
| [`native-abi`](../../openspec/specs/native-abi/spec.md) | 11 |  |  |  |  | W | **C** |  |  |  |  |  |  | M5 |
| [`swift-scripting`](../../openspec/specs/swift-scripting/spec.md) | 12 |  |  |  |  | W |  |  |  | **C** |  |  |  | M8 |
| **4 — Rendering** | | | | | | | | | | | | | | |
| [`rhi-and-render-graph`](../../openspec/specs/rhi-and-render-graph/spec.md) | 12 |  |  |  | W |  |  |  |  |  |  |  | **C** | M11 |
| [`rendering-architecture`](../../openspec/specs/rendering-architecture/spec.md) | 16 |  |  |  | W |  |  |  | **C** |  |  |  |  | M7 |
| [`rendering-culling-and-lod`](../../openspec/specs/rendering-culling-and-lod/spec.md) | 9 |  |  |  | S |  |  | W | **C** |  |  |  |  | M7 |
| [`virtual-geometry`](../../openspec/specs/virtual-geometry/spec.md) | 26 |  |  |  |  |  |  |  | W |  |  |  | **C** | M11 |
| [`virtual-texturing`](../../openspec/specs/virtual-texturing/spec.md) | 16 |  |  |  |  |  |  | W | **C** |  |  |  |  | M7 |
| [`virtual-shadows`](../../openspec/specs/virtual-shadows/spec.md) | 20 |  |  |  |  |  |  |  | W |  |  |  | **C** | M11 |
| [`residency`](../../openspec/specs/residency/spec.md) | 8 |  |  |  |  |  |  | W | **C** |  |  |  |  | M7 |
| [`rendering-forward-clustered`](../../openspec/specs/rendering-forward-clustered/spec.md) | 11 |  |  |  | W |  |  |  |  |  |  |  | **C** | M11 |
| [`shader-system`](../../openspec/specs/shader-system/spec.md) | 13 |  |  |  | W |  |  |  | **C** |  |  |  |  | M7 |
| [`rendering-materials-and-shading`](../../openspec/specs/rendering-materials-and-shading/spec.md) | 9 |  |  |  | W |  |  |  | **C** |  |  |  |  | M7 |
| [`material-compiler`](../../openspec/specs/material-compiler/spec.md) | 21 |  |  |  |  |  |  |  | W | **C** |  |  |  | M8 |
| [`rendering-lighting-and-shadows`](../../openspec/specs/rendering-lighting-and-shadows/spec.md) | 13 |  |  |  | S |  |  |  | W |  |  |  | **C** | M11 |
| [`rendering-global-illumination`](../../openspec/specs/rendering-global-illumination/spec.md) | 29 |  |  |  |  |  |  |  | W |  |  | **C** |  | M10 |
| [`denoising`](../../openspec/specs/denoising/spec.md) | 6 |  |  |  |  |  |  |  | W |  |  |  | **C** | M11 |
| [`ray-tracing-infrastructure`](../../openspec/specs/ray-tracing-infrastructure/spec.md) | 6 |  |  |  |  |  |  |  | W |  |  |  | **C** | M11 |
| [`rendering-post-processing`](../../openspec/specs/rendering-post-processing/spec.md) | 15 |  |  |  |  |  |  |  | W |  |  |  | **C** | M11 |
| [`temporal-rendering`](../../openspec/specs/temporal-rendering/spec.md) | 8 |  |  |  |  |  |  |  | W |  |  |  | **C** | M11 |
| [`rendering-geometry-and-resources`](../../openspec/specs/rendering-geometry-and-resources/spec.md) | 11 |  |  |  | W |  |  | **C** |  |  |  |  |  | M6 |
| [`rendering-2d`](../../openspec/specs/rendering-2d/spec.md) | 11 |  |  |  |  |  |  |  |  | W |  |  | **C** | M11 |
| [`vfx-system`](../../openspec/specs/vfx-system/spec.md) | 24 |  |  |  |  |  |  |  |  | W |  |  | **C** | M11 |
| **5 — Simulation** | | | | | | | | | | | | | | |
| [`physics`](../../openspec/specs/physics/spec.md) | 15 |  |  |  |  | W |  |  |  |  | **C** |  |  | M9 |
| [`animation-and-skinning`](../../openspec/specs/animation-and-skinning/spec.md) | 30 |  |  |  |  |  |  |  |  | W |  |  | **C** | M11 |
| [`ai-system`](../../openspec/specs/ai-system/spec.md) | 19 |  |  |  |  |  |  |  |  | W |  |  | **C** | M11 |
| [`navigation`](../../openspec/specs/navigation/spec.md) | 16 |  |  |  |  |  |  |  |  | W |  | **C** |  | M10 |
| [`ml-inference`](../../openspec/specs/ml-inference/spec.md) | 9 |  |  |  |  |  |  |  |  | S |  |  | **C** | M11 |
| [`audio`](../../openspec/specs/audio/spec.md) | 19 |  |  |  |  | S |  |  |  | **C** |  |  |  | M8 |
| **6 — Content and tooling** | | | | | | | | | | | | | | |
| [`text-and-fonts`](../../openspec/specs/text-and-fonts/spec.md) | 10 |  |  |  |  |  | S |  |  | **C** |  |  |  | M8 |
| [`ui-system`](../../openspec/specs/ui-system/spec.md) | 25 |  |  |  |  |  |  |  |  | W |  |  | **C** | M11 |
| [`asset-import-pipeline`](../../openspec/specs/asset-import-pipeline/spec.md) | 13 |  |  |  |  |  | W | **C** |  |  |  |  |  | M6 |
| [`editor-architecture`](../../openspec/specs/editor-architecture/spec.md) | 13 |  |  |  |  |  | W |  |  |  |  |  | **C** | M11 |
| [`editor-documents-and-transactions`](../../openspec/specs/editor-documents-and-transactions/spec.md) | 12 |  |  |  |  |  | W |  |  | **C** |  |  |  | M8 |
| [`editor-rust-application`](../../openspec/specs/editor-rust-application/spec.md) | 16 |  |  |  |  |  | W |  |  |  |  |  | **C** | M11 |
| [`editor-ui-ux`](../../openspec/specs/editor-ui-ux/spec.md) | 16 |  |  |  |  |  | W |  |  |  |  |  | **C** | M11 |
| [`editor-viewport-and-gizmos`](../../openspec/specs/editor-viewport-and-gizmos/spec.md) | 13 |  |  |  |  |  | W |  | **C** |  |  |  |  | M7 |
| [`editor-visual-language`](../../openspec/specs/editor-visual-language/spec.md) | 22 |  |  |  |  |  | W |  |  |  |  |  | **C** | M11 |
| [`live-editing`](../../openspec/specs/live-editing/spec.md) | 11 |  |  |  |  |  | W |  |  | **C** |  |  |  | M8 |
| [`project-and-plugins`](../../openspec/specs/project-and-plugins/spec.md) | 11 | S | W |  |  |  | **C** |  |  |  |  |  |  | M5 |
| [`build-and-packaging`](../../openspec/specs/build-and-packaging/spec.md) | 19 |  |  |  |  |  |  | W |  |  |  |  | **C** | M11 |
| **7 — Systems and process** | | | | | | | | | | | | | | |
| [`networking-and-replication`](../../openspec/specs/networking-and-replication/spec.md) | 25 |  |  |  |  |  |  |  |  |  | W |  | **C** | M11 |
| [`xr-support`](../../openspec/specs/xr-support/spec.md) | 8 |  |  |  | ◇ |  |  |  |  |  |  |  |  | deferred |
| [`build-system-and-platforms`](../../openspec/specs/build-system-and-platforms/spec.md) | 13 | S |  |  |  | W |  |  |  |  |  |  | **C** | M11 |
| [`developer-workflow-and-just`](../../openspec/specs/developer-workflow-and-just/spec.md) | 15 | S |  |  |  |  | W |  |  |  |  |  | **C** | M11 |
| [`testing-and-quality`](../../openspec/specs/testing-and-quality/spec.md) | 12 | S |  |  | W |  |  |  |  |  |  |  | **C** | M11 |
| [`diagnostics-profiling-and-crash`](../../openspec/specs/diagnostics-profiling-and-crash/spec.md) | 18 | S |  |  |  |  | W |  |  |  | **C** |  |  | M9 |
| [`thirdparty-dependencies`](../../openspec/specs/thirdparty-dependencies/spec.md) | 9 | S |  |  |  |  |  |  |  | W |  |  | **C** | M11 |
| [`delivery-roadmap`](../../openspec/specs/delivery-roadmap/spec.md) | 15 | W |  |  |  |  |  |  |  |  |  |  | **C** | M11 |

---

## Reading the matrix

**Foundations complete early, because everything encodes them.** `core-math` is Complete at M3 and
`core-type-system` at M5 — not because they are small, but because a convention or an identity rule
that changes after fifty consumers depend on it is not a change, it is a migration.

**The renderer is the longest arc.** `rhi-and-render-graph` seeds at M3 and completes at M11, and
that gap is entirely the second and third backends. Everything else about it is settled at M3,
which is the point: the abstraction is validated against one working implementation before a second
is attempted.

**Thirty-six capabilities reach Complete at M11, and that is the definition of 1.0.** Not "the engine is
finished" — the specifications will keep changing — but "no capability is in a state its
specification did not intend".

**Two capabilities never complete on this ladder.** `xr-support` is deferred by decision, with its
prerequisites checked from M3 onward. Nothing else is deferred wholesale; deferred *scope* inside
otherwise-complete capabilities is listed in [risks and deferrals](risks.md).

## Milestone load

| Milestone | Advanced | Reach Complete | Which |
|---|---:|---:|---|
| **M0** · Ground | 8 | 0 | — |
| **M1** · Substrate | 7 | 0 | — |
| **M2** · World | 6 | 0 | — |
| **M3** · First light | 11 | 1 | `core-math` |
| **M4** · Playable | 10 | 0 | — |
| **M5** · Authorable | 14 | 4 | `core-type-system`, `native-abi`, `project-and-plugins`, `scene-graph-and-nodes` |
| **M6** · Scale | 11 | 5 | `asset-import-pipeline`, `core-assets-and-io`, `core-memory-and-containers`, `rendering-geometry-and-resources`, `serialization-and-prefabs` |
| **M7** · Fidelity | 17 | 7 | `editor-viewport-and-gizmos`, `rendering-architecture`, `rendering-culling-and-lod`, `rendering-materials-and-shading`, `residency`, `shader-system`, `virtual-texturing` |
| **M8** · Game systems | 20 | 7 | `audio`, `editor-documents-and-transactions`, `input-and-actions`, `live-editing`, `material-compiler`, `swift-scripting`, `text-and-fonts` |
| **M9** · Integrity | 12 | 10 | `camera-system`, `core-jobs-and-concurrency`, `diagnostics-profiling-and-crash`, `ecs-core`, `gameplay-abilities-and-effects`, `gameplay-framework`, `physics`, `save-and-persistence`, `sequencing-and-cinematics`, `simulation-and-determinism` |
| **M10** · Worlds | 10 | 3 | `navigation`, `rendering-global-illumination`, `world-partition-and-streaming` |
| **M11** · Reach | 36 | 36 | everything remaining |

M11's load is large by construction: it is where every capability that has been Working for several
milestones is finished off, plus two graphics backends and the porting surface. It is the one
milestone that could reasonably be split, and the roadmap will split it through a change if the
work turns out to be separable along a real seam rather than an arbitrary one.

## The status record

The matrix above is the **plan**. Actual progress lives in [`status.yaml`](status.yaml), which
records for each capability its current tier, the milestone that last advanced it, and the change
that did so.

A change that implements or advances a capability updates `status.yaml` in the same commit.
`just roadmap-status` reports the record and fails when it disagrees with `openspec/specs/` — a
capability added, renamed or removed without a corresponding record entry is drift, and drift is a
build failure rather than a discovery.

As of M2 eighteen capabilities have left `—`. `core-assets-and-io`, `core-jobs-and-concurrency`,
`core-math`, `core-memory-and-containers`, `core-type-system`, `delivery-roadmap`, `ecs-core`,
`engine-architecture`, `project-and-plugins`, `scene-graph-and-nodes` and
`serialization-and-prefabs` are Working; `build-system-and-platforms`, `core-platform-abstraction`,
`developer-workflow-and-just`, `diagnostics-profiling-and-crash`, `simulation-and-determinism`,
`testing-and-quality` and `thirdparty-dependencies` are Seed. The remaining 57 have not started.
M2 advanced six of them, each recorded against `implement-m2-world`.

## Where M2's tiers are thin

The tiers above are the plan and the record agrees with it. What the plan does not say is where the
implementation is **thinner than the tier claims**, and that belongs here rather than in a commit
message. Every entry below was measured or reproduced while closing the milestone; where a number
appears, it is a number this tree produced.

Every entry was then re-checked at M2's gate against the tree as it stands, because a caveat
inherited from a mid-milestone draft is a caveat nobody has looked at. Four had moved and are
corrected in place: the identity claim (proved end to end at the gate, still not a committed test),
the bulk-copy figures (not reproducible; the ratio is smaller), the source-file count, and the shape
of the Debug flake (load-induced, not a property of any case). Three entries below are new, found by
attacking what the milestone exists to establish rather than by reading it: the state hash's
dependence on entity indices, `engine-architecture` reaching Working over seven null servers, and a
milestone gate that was never promoted when its milestone closed.

- **The state hash covers what was declared, and in the closing artefact that is four subjects out
  of seventeen.** `samples/02-headless-sim` prints `schema subjects declared=4 undeclared=13` and
  opens its own run with `[info] runtime: 13 component types have no reflected descriptor and are
  not in the state hash`. The thirteen are the ECS's `Parent`/`Children` and all twelve of the
  scene's built-in components; only 7 of the world's 11 archetypes reach the tree. The reason is
  structural rather than an oversight — those are registered by name through
  `ComponentRegistry::register_builtin` with no `reflect::TypeInfo` behind them, so there is nothing
  for `declare_reflected_components()` to read, and `simulation-and-determinism` forbids the
  fallback of hashing raw structure bytes. The design here is right: `WorldHashReport` counts every
  undeclared subject rather than quietly omitting it, and the sample prints the count. The
  consequence still has to be read plainly, and it was measured rather than inferred — a probe that
  declares one component, leaves a second undeclared, and writes to each in turn:

      subjects declared=1 undeclared=3  fields hashed=1
      after a DECLARED write     62de1379adc89c8d  changed=YES
      after an UNDECLARED write  62de1379adc89c8d  changed=NO

  So **a divergence in a node's name, its parent, its sibling order or its effective visibility does
  not change the state hash.** What the M2 gate proves reproducible is the four declared subjects —
  the sample's `Placement` and `Drift`, and `LocalTransform` declared field by field — not the
  world. Closing the gap is a reflected registration for `src/ecs/`'s two relationship components
  and `src/scene/`'s twelve, which is a change to `src/core/reflect/CMakeLists.txt` and
  `identity/manifest.toml` — neither of which those modules own this milestone, and both modules'
  READMEs record the seam.
- **The state hash depended on component *registration order*, and that was found at the gate rather
  than by a test.** `simulation-and-determinism` is explicit — "Registries whose contents affect
  simulation — systems, **types**, rules, providers — SHALL be finalised in a deterministic order
  derived from **stable identifiers**", and "WHEN plugins load in a different order THEN simulation
  results SHALL be unchanged". The walk folded `ComponentTypeId`s, which are the indices
  `ComponentRegistry` hands out in registration order, into the archetype key, into the Component
  node's id, and into the order an entity's components were visited. An adversarial probe built two
  worlds with the same entity, the same values and the same declared schema, registering the two
  component types in opposite orders:

      hash with Alpha registered first  8d3b6809aaa1992a
      hash with Beta  registered first  f9bf362042ec8ed7
      two worlds of identical content hash equal: NO

  Nothing in the tree would have caught it: `smoke.tick_loop` and `smoke.headless_sim` compare
  processes running the *same* binary, where registration order is fixed by the code. The case that
  would have broken in the field is **build-time feature slicing** — task 4.1.4, landed this same
  milestone — because dropping one module's components shifts every later id, so a sliced build
  could not compare hashes with a full one; a plugin registering a type would do the same, which is
  the scenario the requirement names.

  Fixed in `src/runtime/src/state_hash.cpp`: a component's identity is now its `reflect::TypeId`,
  or, for a built-in registered by name with no descriptor, an unseeded FNV-1a of that name — never
  `cy::hash_bytes`, which is seeded per process in development builds. The identities are **sorted
  before they are folded**, because `fold_hash` is order-dependent and the column order is ascending
  component id, so stable identities folded in column order still disagreed. An entity's components
  are walked in that order too. `integration.runtime_simulation`'s "the hash does not depend on the
  order components were registered" is the regression, and it asserts the premise — that the two
  worlds really did give the same type different numbers — before it asserts the conclusion. The
  artefact's hash changed as a result and `samples/02-headless-sim/README.md` records the new one.

  **What is still a sequence and not an identity:** a shared component's *value* is folded as its
  interned index, which is assigned in interning order. That is deterministic for one run of one
  program and is not a stable identity; `archetype_key()` says so at the line that does it. Nothing
  in M2 uses shared components in a hashed archetype, so it is recorded rather than fixed.
- **The same class is still open in two more places, and both are named rather than fixed.** The
  registration-order defect above was one instance of "a sequence number used where a stable
  identifier belongs"; the audit that found it turned up two more.
  `<cy/core/jobs/schedule.h>` levels a stage's systems into batches "with ties broken by
  **registration order**" (`src/ecs/include/cy/ecs/system.h`), and `CommandBuffer`'s merge key is
  "the registration order within the stage" (`src/ecs/src/system.cpp:99`) — both deterministic for
  one build and neither derived from the system's name, which is the stable identifier
  `simulation-and-determinism` asks a registry to be finalised by. They were left alone because
  changing which systems land in which batch is a far larger blast radius than a hash key, and
  because M2 has no plugins and no conditional system registration; they should be closed before
  either arrives. `StateProviderRegistry::finalize()` is the counter-example that shows the right
  shape: it insertion-sorts by name and says why.
- **The determinism firewall is unspellable, and nothing has adopted it.** `Classified<>` delivers
  exactly what design.md §5 demanded: a firewall crossing between two classified values does not
  compile — `Presentation<f32>::read(AuthoritativeContext)` has no overload, in either direction,
  and `test_classification.cpp` proves it with `static_assert` rather than with a runtime check. The
  probe is four translation units, each one line long, compiled against the real header:

      Presentation<f32>.read(AuthoritativeContext)   REJECTED  no matching function
      Authoritative<f32>.write(PresentationContext)  REJECTED  no matching function
      authoritative = presentation                   REJECTED  no matching operator=
      bypass_classification(), a plain global, a struct that never adopted the wrapper   ALL COMPILE

  The measurement that matters is the last line, and one more:
  **`grep -rl 'Classified<\|Authoritative<\|Presentation<' src/ samples/` returns
  `classification.h` and its own test, and nothing else.** No ECS component,
  no scene component and no field of the closing artefact is wrapped, so at M2 the firewall
  protects zero fields. The wrapper being opt-in per field is the design, and the header says so;
  what a reader must not take from "unspellable" is that the engine's state is currently behind it.
  M9's determinism lint inherits every unwrapped field, which is all of them.
- **`core-assets-and-io` is Working with two of its ten requirements unimplemented.** The cook path
  is real — `cy_cook` reads authoring documents and writes a `.cypak` addressed by identity, and
  `integration.cook_path` loads it back — and that is what the M2 tasks asked for. But the
  capability's **Streaming** and **Hot reload** requirements have no implementation at all: there is
  no residency budget, no partial-mip or LOD path, no file watcher, and no `reload` entry point
  anywhere under `src/core/assets/`. Streaming is deliberate and scheduled — design.md §7 puts it at
  M6 — but hot reload is named by the M2 row of `ROADMAP.md` and by task 3.2.13 and is simply
  absent. Read the tier as "the cook path is Working"; two of the ten requirements are still at
  none.
- **`core-type-system` is still a two-type demonstration, and M2 is what it was supposed to stop
  being.** The quadratic half of M1's caveat is closed: `TypeRegistry::find` and `find_field` now go
  through an open-addressed probe table. `integration.reflect_scaling` measures it against M1's
  linear scan kept in the test as a reference implementation, on the same corpus in the same
  process: over 3,200 types, 2.0 ns/op indexed against 693.6 scanning — 343x on this run, 761x on
  another machine-load — and a 256-field record decodes 11.2x faster. Growth over a hundred times
  more types took the scan 34x longer and the index 0.85x. The other half is
  not. `identity/manifest.toml` still holds exactly `cy::demo::Health` and `cy::demo::Placement` —
  **two live types, zero tombstones** — and the generator still emits one file. Every component M2
  introduced carries a **hand-written `reflect::TypeInfo`**: `src/ecs/tests/fixtures.h` says so and
  starts its ids at 9000 so a number there is obviously not one the manifest issued, and
  `samples/02-headless-sim/content.cpp` builds its `FieldInfo`s inline. So `just quality-identity`
  gates none of M2's data, and "overrides address stable identifiers" — the milestone's own
  headline for M1's identity work — is proved in `test_identity.cpp` against fabricated identifiers
  and a `SchemaRegistry` remap, never against a manifest identifier or a manifest tombstone.

  **The gate ran them together once, by hand, and they hold — which is the useful half of the
  finding and does not close it.** A copy of `identity/manifest.toml` was taken, the demo header
  renamed `cy::demo::Health::maximum` to `hit_points`, and `reflect_gen.py` run against the copy:
  without a declaration it refused (`1 declaration is recorded in the manifest but no longer
  declared in the tree`), with `--rename` it recorded `FieldId 1 unchanged`, and a second pass with
  `--tombstone` retired `FieldId 5` (`icon`) leaving `next_field_id = 6` so the number can never be
  reissued. The generated descriptor was then compiled into a probe that resolves a prefab override
  authored against those identifiers:

      descriptor: cy::demo::Health  fields=4   (FieldId 1 "hit_points", 5 absent)
      resolve: entities=1 overrides_applied=1 conflicts=1
      the renamed field's override was APPLIED                  ok
      the tombstoned override is RETAINED and marked conflicted ok
      the conflict is MissingField, not a silent rebind         ok
      FieldId 1 after resolution: 250.0 (authored 100.0, override 250.0)

  So the claim in `m2.toml`'s `documents` criterion — "a prefab override authored against a stable
  identifier survives a field rename with a tombstone" — is true. What is still missing is a
  **committed** test that says so: the probe was deleted with the rest of the gate's scaffolding,
  `test_identity.cpp` still uses identifiers 9302 and above, and M1's `identity-rename` criterion
  exercises the manifest half alone. Until a reflected component with a manifest identifier exists,
  the joined test has nowhere to live.
- **Cook-time flattening needs a fixup pass, and "activation is a bulk copy" is an abbreviation
  twice over.** This was the milestone's named risk and the spike answered the first half: a cooked
  block copies into a chunk as whole-column `memcpy`s — `World::copy_block_columns()` is one
  `memcpy` per column per run and nothing else — and then the key column and every entity-reference
  slot, which hold cook-time indices, must be rewritten. The specification already said so
  (`serialization-and-prefabs`: "Bulk copy with reference fixup"; `world-partition-and-streaming`:
  "allocate chunks, decompress, bulk copy, and fix up references"), so no roadmap change was owed
  and none was made. The second half is the number. The spike's headline of **3.7 ns/entity**
  excludes row reservation, which it measured separately at 236 µs for 102,000 rows; measured end to
  end through the public API — `World::instantiate()` of a 100,000-row two-column block, min of five,
  `-O2`, this host — activation costs **18.7-20.9 ns/entity**, and the advantage over creating the
  same entities one at a time and writing their components is **2.3-2.5x**, not an order of
  magnitude:

      bulk instantiate()           1867 µs    18.67 ns/entity   chunks=196
      per-entity create()+write    4429 µs    44.28 ns/entity   speedup 2.4x

  **The gate could not reproduce either figure, and the difference is the ratio rather than the
  noise.** An independent probe of the same shape — `World::instantiate()` of a 100,000-row
  `Position`+`Velocity` block against `create()`+two writes per entity, min of five worlds per run,
  `-O2` — measured, over four runs on this host, one of them under load:

      bulk instantiate()         25.7 - 28.1 ns/entity
      per-entity create()+write  43.2 - 46.9 ns/entity
      speedup                    1.67x - 1.80x

  The per-entity side agrees with the number above; the bulk side is a third slower and the
  advantage is **under 2x, not 2.4x**. Neither measurement is gated, so neither can be called the
  regression — what should be carried forward is the weaker of the two, because the argument the
  storage decision rests on is "bulk is the right shape", and a 1.7x margin makes that argument more
  worth defending with a real benchmark, not less. See the next entry.

  **That the copy is a copy was checked rather than read.** `memcpy` was interposed and counted
  around one `World::instantiate()` of 100,000 two-column rows into 196 chunks: **395 calls carrying
  2,402,048 bytes**, against 392 expected (196 chunks × 2 columns) and 2,400,000 bytes of payload.
  Nothing scales with the entity count. The same interposer around
  `EntityTemplate::spawn_many(world, 1000)` of a **cooked** 100-entity template — the path task 6.4
  actually names — counted **554 large copies for 100,000 spawned entities across 345 chunks**, and
  all 50,000 intra-template references resolved to an entity of their own instance. A per-entity
  copy path would have shown at least 100,000.

  The bulk path is the right one and the copy really is a copy; what should not be carried forward
  is the idea that M6's cell activation is free. Price it as a copy, plus a strided fixup pass, plus
  an entity-id allocation per row. Emitting the reference sites at cook time is what keeps the fixup
  cheap: asking the registry per row measured 4.7-5.2x slower in the spike.
- **The state hash is a function of the entity indices, and that is nowhere written down.** The
  walk seeds each Entity node with `entity.index()` (`hash.cpp`'s `seed_for`), so two worlds whose
  observable content is identical hash differently when the same values sit on different entity
  ids. That is defensible — an entity id *is* state as soon as anything holds an entity reference,
  and a divergence report has to name an entity — but the module's own "what is not hashed" list
  says only that chunk membership is excluded because it is allocator history, and entity-index
  allocation is allocator history of exactly the same kind. Measured at the gate, four routes to the
  same 500-entity content:

      registration order reversed                     hash equal
      rows moved between archetypes in reverse order  hash equal   (chunk packing is excluded)
      indices recycled in a different order           hash DIFFERS
      the same values on indices 64..563              hash DIFFERS

  The two "equal" lines are the guarantees the module claims, and both hold — the chunk-packing one
  is not covered by any committed test and was checked here for the first time. The two "differs"
  lines are the consequence to carry forward: M6's cell activation assigns ids from the world it
  activates into, so the same cell activated after different history hashes differently, and M9's
  replay must restore ids verbatim rather than merely restore values. `ecs-core`'s snapshot does
  restore them verbatim, which is why the M2 restore criterion passes.
- **`engine-architecture` is Working, and all seven of its servers are the null implementation.**
  `src/servers/` does not exist; `servers.h` is the registry, the selection chain and `NullServer`,
  and every one of `RenderServer`, `PhysicsServer`, `AudioServer`, `NavigationServer`, `TextServer`,
  `DisplayServer` and `InputServer` resolves to it because no backend registers before M3. The
  requirement's first scenario — "a `MeshRenderer` component becomes visible, holds a
  `RenderInstanceHandle` obtained from `RenderServer`, and pushes transform and visibility changes
  to it" — therefore has no implementation to exercise. What M2 does establish is the half that is
  cheap now and expensive later: the fallback chain, and that `Server` names no entity, node, world
  or script, so "the server SHALL never dereference an ECS entity or a scene node" is a property of
  the interface rather than a rule to remember. Read the tier as "the loop, the duality, the command
  queue and feature slicing are Working; the server split is an interface with one null behind it".
- **`milestone-m1` was still `joins-on-close` in `tools/roadmap/gates.toml` when M2 came to close.**
  `delivery-roadmap` is explicit that a milestone's criteria join the gate set when it closes, and
  the comment above `milestone-m0` in that file records what happened the last time the flip was
  forgotten: M0's gate was left at `joins-on-close` when M0 was archived, and M1 then landed
  static-analysis findings that turned `just roadmap-milestone m0` red. The same omission had been
  made for M1, and it matters more than the M0 case did: M2's own modules are compiled into and run
  by M1's `four-profiles` criterion, so an unflipped `milestone-m1` is a ledger nothing runs over
  code that changed underneath it. It is flipped to `green` in this change. The consequence is worth
  stating plainly rather than discovering in CI: `milestone-m1` runs `just roadmap-milestone m1`,
  which is a recipe of several minutes containing `four-profiles`, and `four-profiles` is the flaky
  criterion two entries below — so a pull request now has two independent exposures to that rate
  rather than one. The answer is to fix the flake, not to write an override.
- **The bulk-copy claim is a correctness test with a time budget, not a committed benchmark.**
  `integration.ecs_scale` instantiates 100,000 rows and reads them back through the runtime layout,
  inside the integration suite's per-case budget. That is a threshold a gross regression trips; it
  is **not** a ns/entity figure compared against `benchmarks/baseline.json`, because no benchmark
  runner exists for the ECS — only for the job system. `tools/roadmap/milestones/m2.toml` records
  this as a note and names `cy_add_benchmark(NAME ecs ...)` as the honest fix.
- **"Identical across restore-from-snapshot" is a round trip, not a re-run.** The gate captures a
  snapshot, ticks the world 128 further ticks so it demonstrably moved, restores, and checks the
  hash matches the settled one again — `smoke.headless_sim` asserts the divergence as well as the
  match, so it cannot pass on a restore that did nothing. What it does not do is *replay* from the
  restored state and reproduce the same trajectory: that needs the clock rewound into the same
  epoch, and `Simulation` exposes only `reset_epoch()`, which by design enters a new one. Rewinding
  is `replay-and-rollback`'s, at M9.
- **`scene-graph-and-nodes`' coherence check is five invariants, and "no orphaned entity" is not one
  of them.** `check_coherence()` covers the specification's five — entity alive, one node per
  entity, `Parent` matches the tree, `WorldTransform` consistent after propagation, effective flags
  consistent — and returns a report in every configuration rather than asserting, which is right,
  because `CY_ASSERT` is compiled out in Profile and Shipping. The orphan claim that appears in
  `ROADMAP.md`'s M2 exit criteria and in `gates.toml`'s `scene-coherence` description is carried by
  two separate scene cases ("unloading destroys exactly its entities", "a node reparented out of its
  scene is still destroyed with it") rather than by the invariant checker. The claim holds; the
  gate's own wording overstates where it is checked.
- **M2's own suites did not fit the test taxonomy, and the repair is a split rather than a fix.**
  `just test-all --profile debug` — an M1 *and* an M2 exit criterion, and the `profiles` permanent
  gate — was failing 8 runs in 10 at M2's close, and no agent's own report caught it because each
  ran the suite once. Two causes, both M2's own additions overrunning the taxonomy's
  one-millisecond unit budget at -O0. `unit.reflect_registry`'s growth case registers two hundred
  types and sweeps every earlier one after each — twenty thousand lookups, measured at 1.0-2.3 ms —
  and moved to `integration.reflect_scaling`. With that fixed the same measurement was 16 failures
  in 30 on `unit.scene`, whose cases each build a `World` and a `SceneTree` over it; the three files
  whose cases also walk the whole tree — `test_groups.cpp`, `test_scenes.cpp` and
  `test_visibility.cpp` — joined `test_coherence.cpp`, `test_transform.cpp` and `test_behaviour.cpp`
  in `integration.scene_scale`. **After both moves: 1 failure in 30**, on
  `test_hierarchy.cpp`'s first case, which pays the process's cold start (roughly 150 µs on top of
  its own 85 µs). That last three per cent is a property of a per-case wall-clock budget at -O0
  rather than of any code M2 wrote, and it is left standing and recorded rather than fixed by
  scaling the budget: weakening a gate to close a milestone is the wrong trade, and the harness
  already concedes exactly this argument for sanitizer builds, so whether it should concede it for
  Debug is a decision about the taxonomy and not about M2. **`four-profiles` is therefore still
  flaky, at roughly one run in thirty in the Debug configuration.**

  Re-measured at the gate, and the shape of it is worth knowing before anyone tries to fix it:
  `ctest -L unit` in the Debug tree failed **1 time in 30**, on `unit.scene`; the same suite run
  **alone** failed **0 times in 40**. The flake is therefore load-induced — seventeen suites running
  back to back — rather than a property of any case, which is why shrinking a case will not close it
  and why the honest fix is in the harness or the taxonomy. Note also what promoting `milestone-m1`
  costs: that gate runs M1's ledger, which contains `four-profiles`, so a pull request now has two
  independent exposures to this rate rather than one.
- **`build-system-and-platforms` is unchanged and still Linux-only in practice.** Windows and macOS
  have still never compiled. M2 added five modules and, by `just quality-layers`'s own
  count, the tree it walks is now **600 files** — so the first foreign build is a larger diff again,
  and every `three-platforms` criterion in every ledger is still reported as *not evaluated* rather
  than as passed.
- **The sanitizer gate is wider than M1's and still not what the specification asks for.** It now
  runs TSan and ASan+UBSan over the job suite and over M2's ECS suites — four commands where M1 had
  two, and the TSan run over `ecs_scheduling` is what found and closed the milestone's one real data
  race. `testing-and-quality` asks for the unit and integration suites under all three **at least
  nightly**, and that is still not true: `.github/workflows/ci.yml` has no `schedule:` trigger, and
  `--tests .` cannot be used because `just test-sanitize` exports `CY_TEST_BUDGET_SCALE=0` while
  `tests/unit/harness/test_assertions.cpp:60` asserts `budget_scale() > 0.0`, so the harness suite
  fails under every sanitizer run that includes it. M1's gate never included it, so nothing had met
  it; `m2.toml` records the defect against the criterion rather than working around it. Reproduced
  at the gate against the ordinary binary, so that the claim rests on output rather than on reading:

      $ CY_TEST_BUDGET_SCALE=0 ./build/gate/tests/unit/harness/cy_test_unit_harness
        values: CHECK_GT( 0, 0 )
        [doctest] test cases: 12 | 11 passed | 1 failed
      $ ./build/gate/tests/unit/harness/cy_test_unit_harness
        [doctest] test cases: 12 | 12 passed | 0 failed

## Where M1's tiers were thin, and what M2 closed

M1's list is kept rather than deleted, because a caveat that quietly disappears trains a reader to
skim. Each entry below is either **closed** — with what closed it — or still standing.

- **Closed. The reflection registry's linear scan.** M1 recorded `TypeRegistry::find` as a linear
  scan and `read_record` as quadratic in field count. M2's spec delta states the complexity contract
  and `src/core/reflect/probe_table.h` and `field_index.h` meet it: over 3,200 types, indexed lookup
  measured 1.6 ns/op against 1,249.6 ns/op scanning, and a 256-field record decodes 11.8x faster.
  Growth over 100x more types went from 88x to 0.53x.
- **Closed. The reflection generator's undeclared frontend.** The `clang` Python bindings and
  libclang are declared in `deps/host-tools.toml`, appear in `THIRD_PARTY.md`, and `just env-doctor`
  reports both — advisory rather than required, because the tree builds from committed metadata
  without them and only `just generate-check` fails.
- **Still standing, and narrowed. `core-math`'s SIMD claim covers the backends this build
  compiles, which is three of four.** `Backend` names Scalar, Sse, Avx2 and Neon. Scalar and Sse are
  always compiled on x86-64, Avx2 now is on a host that runs it (the entry below), and **Neon has
  still never been compiled by anyone**, because nothing in the tree cross-compiles for ARM and no
  CI leg is an ARM runner. The suite is written against `backend_compiled()` so it tests what the
  build contains rather than what the enum lists, which is the right shape; it does not make the
  fourth arm any more exercised than it was at M1.
- **Closed. AVX2 was compiled by nobody.** `src/core/math/CMakeLists.txt` now probes for `-mavx2`
  and for a host that executes it, and compiles `integration.math_simd_avx2` when both hold; the
  suite measured 48,274 assertions against the baseline's 24,557 and its objdump carries 139 AVX2
  instructions. The residual is honest and narrow: on a host or CI leg without AVX2 the 256-bit path
  is still not compiled, and nothing cross-compiles it.
- **Closed. `just test-sanitize` left the ordinary build tree instrumented.** The override now
  selects the sanitized tree's *parent*, `tools/ci/test_recipes.py` holds the rule, and this
  milestone's ledgers were run with `CY_BUILD_DIR` set throughout: `just roadmap-milestone m0` and
  `m1` were re-run after `m2` on the same tree and stayed green. Checked directly as well, after
  four sanitizer runs had gone through that tree — the ordinary binaries carry no instrumentation
  and the sanitized siblings do:

      build/gate/cy_test_unit_ecs                          __asan symbols: 0   __tsan: 0
      build/gate/sanitize-address-undefined/…unit_ecs      __asan symbols: 37
      build/gate/sanitize-thread/…ecs_scheduling           __tsan symbols: 32
- **Still standing. `core-jobs-and-concurrency`'s throughput thresholds are loose** — every metric
  has three to four and a half times the headroom it needs, so a regression to a quarter of today's
  speedup would still pass.
- **Still standing. `core-jobs-and-concurrency`'s non-blocking rule is enforced for declared
  blocking only.** An undeclared `read()` on a worker is caught only by the 250 ms watchdog, as a
  long task. This is the precedent design.md §5 named, and M2's classification answered it for
  classified state only — see the firewall entry above.
- **Still standing. `project-and-plugins` has never loaded a plugin**, because there is no `dlopen`
  and no `LoadLibrary` anywhere in the tree — re-checked at M2's gate over `src/`, `platform/`,
  `tools/` and `modules/`: zero matches.
- **Closed at M1 itself, and kept here so the thread is not lost. The ladder could not survive its
  own last rung.** M0's `m1-open` criterion matched `openspec/changes/*m1*/proposal.md`, and `*`
  does not match a path separator, so archiving the change would have turned the criterion red.
  Both that criterion and M1's `m2-open` use `**`; M2's `m3-open` was written the same way from the
  start, and it is the one criterion this milestone's ledger still fails — correctly, because the
  M3 change has not been opened yet.
- **Still standing. The quality gates are pinned to LLVM 22 and a clone without the pinned tooling
  cannot run the format or lint gates**, by design; `just env-doctor` prints the correction.
- **Superseded. `engine-architecture` was Seed on ordering alone.** The servers, the ECS/scene
  duality, the fixed-tick loop with its interpolation alpha and eight-tick cap, the deferred frame
  command queue and build-time feature slicing are this milestone's, and the capability is Working.
- **Superseded. `core-assets-and-io`'s `memcpy(dst, nullptr, 0)` class was "not exhausted".** UBSan
  now runs over the ECS, scene, serialization and determinism suites as well as the job suite and
  reports nothing; the class is not proved exhausted, but it is now looked for over four more
  modules than it was.
