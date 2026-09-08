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

| Capability | Reqs | M0 | M1 | M2 | M3 | M4 | M5 | M5.5 | M6 | M7 | M8.a | M8.b | M9 | M10 | M11 | Complete |
|---|---:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| **1 — Foundations** | | | | | | | |  | | | | | | | |
| [`engine-architecture`](../../openspec/specs/engine-architecture/spec.md) | 10 |  | S | W |  |  |  |  |  |  |  |   |  |  | **C** | M11 |
| [`core-type-system`](../../openspec/specs/core-type-system/spec.md) | 13 |  | W |  |  |  | **C** |  |  |  |  |   |  |  |  | M5 |
| [`core-memory-and-containers`](../../openspec/specs/core-memory-and-containers/spec.md) | 16 |  | W |  |  |  |  |  |       |       |     | **C** |  |  |  | M8.b |
| [`core-math`](../../openspec/specs/core-math/spec.md) | 10 |  | W |  | **C** |  |  |  |  |  |  |   |  |  |  | M3 |
| [`core-jobs-and-concurrency`](../../openspec/specs/core-jobs-and-concurrency/spec.md) | 16 |  | W |  |  |  |  |  |  |  |  |   | **C** |  |  | M9 |
| [`core-assets-and-io`](../../openspec/specs/core-assets-and-io/spec.md) | 10 |  | S | W |  |  |  |  |       |       |     | **C** |  |  |  | M8.b |
| [`core-platform-abstraction`](../../openspec/specs/core-platform-abstraction/spec.md) | 7 | S |  |  |  | W |  |  |  |  |  |   |  |  | **C** | M11 |
| **2 — World model** | | | | | | | |  | | | | | | | |
| [`ecs-core`](../../openspec/specs/ecs-core/spec.md) | 12 |  |  | W |  |  |  |  |  |  |  |   | **C** |  |  | M9 |
| [`scene-graph-and-nodes`](../../openspec/specs/scene-graph-and-nodes/spec.md) | 10 |  |  | W |  |  | **C** |  |  |  |  |   |  |  |  | M5 |
| [`serialization-and-prefabs`](../../openspec/specs/serialization-and-prefabs/spec.md) | 23 |  |  | W |  |  |  |  |       |  | **C** |   |  |  |  | M8.a |
| [`gameplay-framework`](../../openspec/specs/gameplay-framework/spec.md) | 33 |  |  |  |  | S |  |  |  |  | S | W | **C** |  |  | M9 |
| [`gameplay-abilities-and-effects`](../../openspec/specs/gameplay-abilities-and-effects/spec.md) | 20 |  |  |  |  |  |  |  |  |  |     | W | **C** |  |  | M9 |
| [`visual-scripting`](../../openspec/specs/visual-scripting/spec.md) | 20 |  |  |  |  |  |  |  |  |  |     | W |  |  | **C** | M11 |
| [`sequencing-and-cinematics`](../../openspec/specs/sequencing-and-cinematics/spec.md) | 31 |  |  |  |  |  |  |  |  |  |     | W | **C** |  |  | M9 |
| [`input-and-actions`](../../openspec/specs/input-and-actions/spec.md) | 23 |  |  |  |  | W |  |  |  |  |     | **C** |  |  |  | M8.b |
| [`camera-system`](../../openspec/specs/camera-system/spec.md) | 28 |  |  |  |  | S |  |  |  |  |     | W | **C** |  |  | M9 |
| [`simulation-and-determinism`](../../openspec/specs/simulation-and-determinism/spec.md) | 20 |  |  | S |  | W |  |  |  |  |  |   | **C** |  |  | M9 |
| [`replay-and-rollback`](../../openspec/specs/replay-and-rollback/spec.md) | 19 |  |  |  |  |  |  |  |  |  |  |   | W |  | **C** | M11 |
| [`save-and-persistence`](../../openspec/specs/save-and-persistence/spec.md) | 20 |  |  |  |  |  |  |  | W |  |  |   | **C** |  |  | M9 |
| [`world-partition-and-streaming`](../../openspec/specs/world-partition-and-streaming/spec.md) | 32 |  |  |  |  |  |  |  | W |  |  |   |  | **C** |  | M10 |
| [`environment-fields`](../../openspec/specs/environment-fields/spec.md) | 12 |  |  |  |  |  |  |  |  |  |  |   |  | W | **C** | M11 |
| [`procedural-content-generation`](../../openspec/specs/procedural-content-generation/spec.md) | 22 |  |  |  |  |  |  |  |  |  |  |   |  | W | **C** | M11 |
| [`weather-and-wind`](../../openspec/specs/weather-and-wind/spec.md) | 16 |  |  |  |  |  |  |  |  |  |  |   |  | W | **C** | M11 |
| [`atmosphere-sky-and-clouds`](../../openspec/specs/atmosphere-sky-and-clouds/spec.md) | 13 |  |  |  |  |  |  |  |  | S |  |   |  | W | **C** | M11 |
| [`terrain`](../../openspec/specs/terrain/spec.md) | 15 |  |  |  |  |  |  |  |  |  |  |   |  | W | **C** | M11 |
| [`foliage`](../../openspec/specs/foliage/spec.md) | 13 |  |  |  |  |  |  |  |  |  |  |   |  | W | **C** | M11 |
| [`water`](../../openspec/specs/water/spec.md) | 17 |  |  |  |  |  |  |  |  |  |  |   |  | W | **C** | M11 |
| **3 — Scripting** | | | | | | | |  | | | | | | | |
| [`native-abi`](../../openspec/specs/native-abi/spec.md) | 11 |  |  |  |  | W | **C** |  |  |  |  |   |  |  |  | M5 |
| [`swift-scripting`](../../openspec/specs/swift-scripting/spec.md) | 12 |  |  |  |  | W |  |  |  |  |     | **C** |  |  |  | M8.b |
| **4 — Rendering** | | | | | | | |  | | | | | | | |
| [`rhi-and-render-graph`](../../openspec/specs/rhi-and-render-graph/spec.md) | 12 |  |  |  | W |  |  |  |  |  |  |   |  |  | **C** | M11 |
| [`rendering-architecture`](../../openspec/specs/rendering-architecture/spec.md) | 16 |  |  |  | W |  |  |  |  |       |     | **C** |  |  |  | M8.b |
| [`rendering-culling-and-lod`](../../openspec/specs/rendering-culling-and-lod/spec.md) | 9 |  |  |  | S |  |  |  | W |       |     | **C** |  |  |  | M8.b |
| [`virtual-geometry`](../../openspec/specs/virtual-geometry/spec.md) | 26 |  |  |  |  |  |  |  |  | W |  |   |  |  | **C** | M11 |
| [`virtual-texturing`](../../openspec/specs/virtual-texturing/spec.md) | 16 |  |  |  |  |  |  |  | W | **C** |  |   |  |  |  | M7 |
| [`virtual-shadows`](../../openspec/specs/virtual-shadows/spec.md) | 20 |  |  |  |  |  |  |  |  | W |  |   |  |  | **C** | M11 |
| [`residency`](../../openspec/specs/residency/spec.md) | 8 |  |  |  |  |  |  |  | W | **C** |  |   |  |  |  | M7 |
| [`rendering-forward-clustered`](../../openspec/specs/rendering-forward-clustered/spec.md) | 11 |  |  |  | W |  |  |  |  |  |  |   |  |  | **C** | M11 |
| [`shader-system`](../../openspec/specs/shader-system/spec.md) | 13 |  |  |  | W |  |  |  |  |       |     | **C** |  |  |  | M8.b |
| [`rendering-materials-and-shading`](../../openspec/specs/rendering-materials-and-shading/spec.md) | 9 |  |  |  | W |  |  |  |  | **C** |  |   |  |  |  | M7 |
| [`material-compiler`](../../openspec/specs/material-compiler/spec.md) | 21 |  |  |  |  |  |  |  |  | W |     | **C** |  |  |  | M8.b |
| [`rendering-lighting-and-shadows`](../../openspec/specs/rendering-lighting-and-shadows/spec.md) | 13 |  |  |  | S |  |  |  |  | W |  |   |  |  | **C** | M11 |
| [`rendering-global-illumination`](../../openspec/specs/rendering-global-illumination/spec.md) | 29 |  |  |  |  |  |  |  |  | W |  |   |  | **C** |  | M10 |
| [`denoising`](../../openspec/specs/denoising/spec.md) | 6 |  |  |  |  |  |  |  |  | W |  |   |  |  | **C** | M11 |
| [`ray-tracing-infrastructure`](../../openspec/specs/ray-tracing-infrastructure/spec.md) | 6 |  |  |  |  |  |  |  |  | W |  |   |  |  | **C** | M11 |
| [`rendering-post-processing`](../../openspec/specs/rendering-post-processing/spec.md) | 15 |  |  |  |  |  |  |  |  | W |  |   |  |  | **C** | M11 |
| [`temporal-rendering`](../../openspec/specs/temporal-rendering/spec.md) | 8 |  |  |  |  |  |  |  |  | W |  |   |  |  | **C** | M11 |
| [`rendering-geometry-and-resources`](../../openspec/specs/rendering-geometry-and-resources/spec.md) | 11 |  |  |  | W |  |  |  |       |  |     | **C** |  |  |  | M8.b |
| [`rendering-2d`](../../openspec/specs/rendering-2d/spec.md) | 11 |  |  |  |  |  |  |  |  |  |     | W |  |  | **C** | M11 |
| [`vfx-system`](../../openspec/specs/vfx-system/spec.md) | 24 |  |  |  |  |  |  |  |  |  |     | W |  |  | **C** | M11 |
| **5 — Simulation** | | | | | | | |  | | | | | | | |
| [`physics`](../../openspec/specs/physics/spec.md) | 15 |  |  |  |  | W |  |  |  |  | W |   | **C** |  |  | M9 |
| [`animation-and-skinning`](../../openspec/specs/animation-and-skinning/spec.md) | 30 |  |  |  |  |  |  |  |  |  |     | W |  |  | **C** | M11 |
| [`ai-system`](../../openspec/specs/ai-system/spec.md) | 19 |  |  |  |  |  |  |  |  |  |     | W |  |  | **C** | M11 |
| [`navigation`](../../openspec/specs/navigation/spec.md) | 16 |  |  |  |  |  |  |  |  |  |     | W |  | **C** |  | M10 |
| [`ml-inference`](../../openspec/specs/ml-inference/spec.md) | 9 |  |  |  |  |  |  |  |  |  |     | S |  |  | **C** | M11 |
| [`audio`](../../openspec/specs/audio/spec.md) | 19 |  |  |  |  | S |  |  |  |  |     | **C** |  |  |  | M8.b |
| **6 — Content and tooling** | | | | | | | |  | | | | | | | |
| [`text-and-fonts`](../../openspec/specs/text-and-fonts/spec.md) | 10 |  |  |  |  |  | S |  |  |  |     | **C** |  |  |  | M8.b |
| [`ui-system`](../../openspec/specs/ui-system/spec.md) | 25 |  |  |  |  |  |  |  |  |  |     | W |  |  | **C** | M11 |
| [`asset-import-pipeline`](../../openspec/specs/asset-import-pipeline/spec.md) | 13 |  |  |  |  |  | W |  |       |  | W | **C** |  |  |  | M8.b |
| [`editor-agent-interface`](../../openspec/specs/editor-agent-interface/spec.md) | 18 |  |  |  |  |  | S | W |  |  |  |   |  |  | **C** | M11 |
| [`editor-architecture`](../../openspec/specs/editor-architecture/spec.md) | 13 |  |  |  |  |  | W |  |  |  |  |   |  |  | **C** | M11 |
| [`editor-documents-and-transactions`](../../openspec/specs/editor-documents-and-transactions/spec.md) | 12 |  |  |  |  |  | W |  |  |  | **C** |   |  |  |  | M8.a |
| [`editor-rust-application`](../../openspec/specs/editor-rust-application/spec.md) | 16 |  |  |  |  |  | W |  |  |  |  |   |  |  | **C** | M11 |
| [`editor-ui-ux`](../../openspec/specs/editor-ui-ux/spec.md) | 16 |  |  |  |  |  | S | W |  |  |  |   |  |  | **C** | M11 |
| [`editor-viewport-and-gizmos`](../../openspec/specs/editor-viewport-and-gizmos/spec.md) | 13 |  |  |  |  |  | W |  |  |       |     | **C** |  |  |  | M8.b |
| [`editor-visual-language`](../../openspec/specs/editor-visual-language/spec.md) | 22 |  |  |  |  |  | S | W |  |  |  |   |  |  | **C** | M11 |
| [`live-editing`](../../openspec/specs/live-editing/spec.md) | 11 |  |  |  |  |  | W |  |  |  |     | **C** |  |  |  | M8.b |
| [`project-and-plugins`](../../openspec/specs/project-and-plugins/spec.md) | 11 | S | W |  |  |  | **C** |  |  |  |  |   |  |  |  | M5 |
| [`build-and-packaging`](../../openspec/specs/build-and-packaging/spec.md) | 19 |  |  |  |  |  |  |  | W |  |  |   |  |  | **C** | M11 |
| **7 — Systems and process** | | | | | | | |  | | | | | | | |
| [`networking-and-replication`](../../openspec/specs/networking-and-replication/spec.md) | 25 |  |  |  |  |  |  |  |  |  |  |   | W |  | **C** | M11 |
| [`xr-support`](../../openspec/specs/xr-support/spec.md) | 8 |  |  |  | ◇ |  |  |  |  |  |  |   |  |  |  | deferred |
| [`build-system-and-platforms`](../../openspec/specs/build-system-and-platforms/spec.md) | 13 | S |  |  |  | W |  |  |  |  |  |   |  |  | **C** | M11 |
| [`developer-workflow-and-just`](../../openspec/specs/developer-workflow-and-just/spec.md) | 15 | S |  |  |  |  | W |  |  |  |  |   |  |  | **C** | M11 |
| [`testing-and-quality`](../../openspec/specs/testing-and-quality/spec.md) | 12 | S |  |  | W |  |  |  |  |  |  |   |  |  | **C** | M11 |
| [`diagnostics-profiling-and-crash`](../../openspec/specs/diagnostics-profiling-and-crash/spec.md) | 18 | S |  |  |  |  | W |  |  |  |  |   | **C** |  |  | M9 |
| [`thirdparty-dependencies`](../../openspec/specs/thirdparty-dependencies/spec.md) | 9 | S |  |  |  |  |  |  |  |  |     | W |  |  | **C** | M11 |
| [`delivery-roadmap`](../../openspec/specs/delivery-roadmap/spec.md) | 15 | W |  |  |  |  |  |  |  |  |  |   |  |  | **C** | M11 |

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
| **M3** · First light | 10 | 1 | `core-math` |
| **M4** · Playable | 10 | 0 | — |
| **M5** · Authorable | 16 | 4 | `core-type-system`, `native-abi`, `project-and-plugins`, `scene-graph-and-nodes` |
| **M5.5** · Operable | 3 | 0 | — |
| **M6** · Scale | 6 | 0 | — |
| **M7** · Fidelity | 13 | 3 | `rendering-materials-and-shading`, `residency`, `virtual-texturing` |
| **M8.a** · Authorable | 5 | 2 | `editor-documents-and-transactions`, `serialization-and-prefabs` |
| **M8.b** · Systems | 27 | 14 | `asset-import-pipeline`, `audio`, `core-assets-and-io`, `core-memory-and-containers`, `editor-viewport-and-gizmos`, `input-and-actions`, `live-editing`, `material-compiler`, `rendering-architecture`, `rendering-culling-and-lod`, `rendering-geometry-and-resources`, `shader-system`, `swift-scripting`, `text-and-fonts` |
| **M9** · Integrity | 12 | 10 | `camera-system`, `core-jobs-and-concurrency`, `diagnostics-profiling-and-crash`, `ecs-core`, `gameplay-abilities-and-effects`, `gameplay-framework`, `physics`, `save-and-persistence`, `sequencing-and-cinematics`, `simulation-and-determinism` |
| **M10** · Worlds | 10 | 3 | `navigation`, `rendering-global-illumination`, `world-partition-and-streaming` |
| **M11** · Reach | 38 | 38 | everything remaining |

M5.5's load is three, and small on purpose: it is a **repair milestone**. `implement-m5b-operable`
inserted it because M5 claimed `editor-ui-ux` at Working while closing on a script that could not
touch a window, and the three capabilities it advances are the ones a window and an agent make true.
It builds a large amount of code and moves few rows, which is what a repair looks like.

**M6's load is six, and the plan said eleven with five of them completing.** None of the five
Completes is in the record, and the M6 column above and the [status record](status.yaml) both say
six Workings instead. The reasoning per row is in
[where M6's tiers are thin](#where-m6s-tiers-are-thin) and in
`tools/roadmap/milestones/m6.toml` beside `[criterion.expect_tiers]`, and the five **C** cells moved
to the milestone at which their unmet prerequisite arrives: `core-assets-and-io` and
`core-memory-and-containers` to M7, `asset-import-pipeline`, `serialization-and-prefabs` and
`rendering-geometry-and-resources` to M8. That last one is M6 task 10.8, which asked the gate to
choose between scoping the requirement and moving the tier; the tier moved, because the requirement
it violates — skinning reads the GPU pose world rather than a second per-consumer upload — is the
sentence that stops a second pose upload being invented, and deleting it to fit a schedule is the
failure it exists to prevent.

**The Milestone load table above is now derived from the matrix rather than maintained beside it.**
M5's gate recorded that the two disagreed — the table said M5 advanced fourteen where the matrix
showed sixteen, and said M11 completes thirty-six where the matrix shows thirty-eight — and named it
"a gap worth a check rather than a paragraph". Every count and every name above was recomputed from
the columns of the matrix at M6's gate. It is still two documents that can drift; what has changed
is that they do not disagree today.

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

As of M7 fifty-seven capabilities have left `—`, and seven have reached Complete. **Every entry
below was read out of `just roadmap-status` at M7's gate rather than carried forward from the
previous edition of this list.**

- **Complete (7)**: `core-math`, `core-type-system`, `native-abi`,
  `rendering-materials-and-shading`, `residency`, `scene-graph-and-nodes`, `virtual-texturing`.
- **Working (37)**: `asset-import-pipeline`, `build-and-packaging`, `core-assets-and-io`,
  `core-jobs-and-concurrency`, `core-memory-and-containers`, `core-platform-abstraction`,
  `delivery-roadmap`, `denoising`, `ecs-core`, `editor-agent-interface`,
  `editor-documents-and-transactions`, `editor-rust-application`, `editor-ui-ux`,
  `editor-viewport-and-gizmos`, `editor-visual-language`, `engine-architecture`,
  `input-and-actions`, `material-compiler`, `physics`, `project-and-plugins`,
  `ray-tracing-infrastructure`, `rendering-architecture`, `rendering-culling-and-lod`,
  `rendering-forward-clustered`,
  `rendering-geometry-and-resources`, `rendering-global-illumination`,
  `rendering-lighting-and-shadows`, `rendering-post-processing`, `rhi-and-render-graph`,
  `save-and-persistence`, `serialization-and-prefabs`, `shader-system`, `swift-scripting`,
  `temporal-rendering`, `virtual-geometry`, `virtual-shadows`, `world-partition-and-streaming`.
- **Seed (13)**: `atmosphere-sky-and-clouds`, `audio`, `build-system-and-platforms`,
  `camera-system`, `developer-workflow-and-just`, `diagnostics-profiling-and-crash`,
  `editor-architecture`, `gameplay-framework`, `live-editing`, `simulation-and-determinism`,
  `testing-and-quality`, `text-and-fonts`, `thirdparty-dependencies`.

The remaining 19 have not started. **M7 advanced thirteen, and started nine of them**:
`material-compiler`, `virtual-geometry`, `virtual-shadows`, `temporal-rendering`,
`rendering-post-processing`, `rendering-global-illumination`, `denoising` and
`ray-tracing-infrastructure` went from `—` to Working and `atmosphere-sky-and-clouds` from `—` to
Seed; `rendering-lighting-and-shadows` went from Seed to Working; and
`rendering-materials-and-shading`, `residency` and `virtual-texturing` reached Complete — each
recorded against `implement-m7-fidelity` at milestone `M7`. **Six rows the plan had completing here
did not reach it**: `rendering-architecture`, `rendering-culling-and-lod`, `shader-system`,
`core-assets-and-io`, `core-memory-and-containers` and `editor-viewport-and-gizmos`. Every one of
the six received substantial implementation in this milestone — the budget arbiter and its
71-magnitude sweep, the GPU cull dispatch, the material compiler, `StreamingSystem`, four
attribution axes, and the engine's own frame inside the editor's viewport — and none of the six
closed the last requirement between it and Complete, so their rows still name the milestone that
last advanced their tier rather than this one. Where M7's tiers are
thinner than the tier they reached is [below](#where-m7s-tiers-are-thin).

The paragraph below is M6's, kept because it is the record of what that milestone moved.
**M6 advanced six, and started five of them**:
`build-and-packaging`, `residency`, `save-and-persistence`, `virtual-texturing` and
`world-partition-and-streaming` went from `—` to Working, and `rendering-culling-and-lod` from Seed
to Working, each recorded against `implement-m6-scale` at milestone `M6`. **Five rows the plan had
completing here did not move at all**, and two of those five — `core-assets-and-io` and
`core-memory-and-containers` — have no change under `src/core/` in this milestone whatsoever;
`serialization-and-prefabs`, `asset-import-pipeline` and `rendering-geometry-and-resources` all
received substantial implementation without reaching Complete, so their rows still name the
milestone that last advanced their tier rather than this one. Where M6's tiers are thinner than
Working suggests is [below](#where-m6s-tiers-are-thin).

The paragraph below is M5.5's, kept because it is the record of what that milestone moved.
**M5.5 advanced three, and started none**: `editor-ui-ux`,
`editor-visual-language` and `editor-agent-interface` moved from Seed to Working, each recorded
against `implement-m5b-operable` at milestone `M5B`. Nothing else moved — `editor-viewport-and-gizmos`
and `editor-rust-application` both received substantial implementation at M5.5 (the gizmo geometry
and its three states, the orientation widget, the cross-process viewport transport, the window, the
toolkit and the move to Rust 1.95) without changing tier, and their rows still name M5, because the
record's `milestone` field means *the milestone that last advanced the tier* and inflating it would
make the record say something it cannot support. Where M5.5's tiers are thinner than Working
suggests is [below](#where-m55s-tiers-are-thin). **M5 advanced thirteen** — the largest single jump in the record
so far, and ten of the thirteen are capabilities that had never started. Three reached Complete
(`core-type-system`, `native-abi`, `scene-graph-and-nodes`), four reached Working
(`editor-rust-application`, `editor-documents-and-transactions`, `editor-viewport-and-gizmos`,
`asset-import-pipeline`) and six reached Seed (`editor-architecture`, `editor-ui-ux`,
`editor-visual-language`, `editor-agent-interface`, `live-editing`, `text-and-fonts`), each recorded
against `implement-m5-authorable`.

**Four capabilities the plan marked for M5 did not reach the tier the plan gave them, and the record
says so rather than the plan.** The amendment, with the evidence for each, is written into
`tools/roadmap/milestones/m5.toml` beside `[criterion.expect_tiers]` — that file asked whoever closed
M5 to "either finish it or amend this table with the reason", and this is the amendment. In short:

- `live-editing` is **Seed**, not Working. The live bridge has only one side. `cy-runtime-stub` —
  the process the closing artefact kills — is a Rust binary in `cy-editor-testhost` whose own header
  says "What it is not: an engine. It holds no world, and it echoes what it is asked to apply. The
  real hosted runtime is a C++ binary over `cy::abi::Host`, and it arrives with `live-editing` at
  task 5.2." It did not arrive: nothing under `src/` or `tools/` binds a Unix domain socket, so no
  engine binary can accept a bridge connection and **the editor has never spoken to this engine**.
  Play modes have no implementation in any process; the per-field live edit policy is absent, by the
  importer's own statement. What is real — a module reloading while a loop runs with world state
  crossing it, runtime inspection from outside the owning process, an edited source re-cooking behind
  a stable handle, shader reload from M3 — is Seed's definition exactly.
- `editor-ui-ux` is **Seed**, not Working. This is not the gate's finding:
  `openspec/changes/implement-m5b-operable/`, committed to `main` before the gate ran, says "the row
  was wrong when it was written" and inserts M5.5 · Operable to deliver it. `cy-editor-interface`'s
  own header agrees: docking, workspaces, the palette, keyboard-first operation and the generated
  inspector exist "as models a test can drive, with no window, no graphics device and no interface
  toolkit."
- `editor-architecture` is **Seed**, not Working, and for the same reason as `live-editing`: its first
  requirement puts the authoring, preview and runtime worlds *in the runtime*, and all three are in
  the editor process because there is no runtime process to put them in. Play mode, the content
  browser, project settings, build and deployment, the specialised editors and the debugging and
  profiling tools have no implementation. Undo/redo and the inspector are Working-grade and are why
  this is Seed rather than nothing.
- `project-and-plugins` stays at **Working**, where M1 left it. Nothing in M5 touched it, and no task
  in `implement-m5-authorable/tasks.md` asked anything of it. Four of its eleven requirements —
  `Plugins`, `Plugin lifecycle`, `Plugin resolution and lockfile`, `Trust tiers for extensions` —
  have no implementation anywhere in the tree; `find src tools -iname '*plugin*'` returns one
  layercheck fixture.

**Three sources disagreed about what M5's scope was, and nothing checks them against each other.**
The M5 row of [ROADMAP.md](../ROADMAP.md#m5--authorable) lists nine capabilities;
`tools/roadmap/milestones/m5.toml`'s exit tiers list fourteen; the M5 column of the matrix above
lists sixteen, and the Milestone load table two sections up counts fourteen of them, so the matrix
does not agree with its own summary either. The matrix is the only one of the three that marks `editor-visual-language` at
Working — the proposal and the ledger both say Seed, and the cell is corrected above — and the only
one that marks `developer-workflow-and-just` and `diagnostics-profiling-and-crash` at Working at M5,
which no task and no criterion covers. Both stay at Seed. `just roadmap-status` compares the record
with `openspec/specs/` and nothing compares the three plan documents with the ledger; that is a gap
worth a check rather than a paragraph.

**The table above now has a column for M5.5, and this is where it came from.**
`implement-m5b-operable` inserted a milestone between M5 and M6 rather than renumbering, which keeps
every reference to M6 through M11 valid; M5's closing gate named the missing column and left it,
and M5.5's closing gate added it, together with the M5.5 row of [ROADMAP.md](../ROADMAP.md) and
`m5b` in `tools/roadmap/record.py`'s ladder — without which no capability row could name M5.5 as the
milestone that advanced it, because `record._entry` refuses a milestone that is not on the ladder.
Three cells were filled: `editor-ui-ux`'s **W** moved out of M5 into M5.5 and its M5 cell reads
**S**, which is the correction this whole milestone exists for; `editor-visual-language` reaches
**W** at M5.5; and `editor-agent-interface` reaches **W** at M5.5 rather than at M8, which is the
one place the plan changed rather than being corrected — the proposal argues that the agent loop is
most valuable *while* the engine is being built.

## Where M7's tiers are thin

The thirteen tiers M7 advanced are in [the status record](#the-status-record); the six the plan
expected to complete and that did not are argued under [milestone load](#milestone-load) and in
`tools/roadmap/milestones/m7.toml` beside `[criterion.expect_tiers]`. What follows is where the
implementation is **thinner than the tier it reached**, and what M7's own gate found. Everything
below was measured or reproduced at that gate on this tree: Linux 6.8, GCC 13.3.0 and Clang 18.1.3,
clang-format and clang-tidy at 22.1.8, Swift 6.3.3, Rust 1.95.0, an NVIDIA RTX 5060 with Vulkan
1.4.312, X11 on `:0`.

### The six the plan had completing, and what each is short of

Each of the six is stated in full in `tools/roadmap/milestones/m7.toml` and summarised in the
[M7 section of the roadmap](../ROADMAP.md#m7--fidelity). The shortest form:

| Row | Recorded | The requirement between it and Complete |
|---|---|---|
| `rendering-architecture` | Working | "Renderer budget arbiter". The arbiter is built and certified over 71 step magnitudes; `libcy_rendering_arbiter.a` is linked by its own two test binaries and by `samples/07-fidelity` and by nothing else, and the seven `SubsystemController`s live in that sample over a hard-coded cost table. No instance under `src/` reports a measured cost to it. |
| `rendering-culling-and-lod` | Working | "Occlusion culling". `GpuCullPass::upload` refuses `kGpuCullOcclusion` with `NotImplemented`; there is no hierarchical depth buffer on the device, and cluster-granular occlusion for virtual geometry has no implementation. `Hzb` and `TwoPassCull` are a CPU model, tested as one. |
| `shader-system` | Working | "Visual material editor". The compiler exists and `cy_material compile` prints every lowering stage; the editor has no material graph panel. |
| `core-assets-and-io` | Working | "Hot reload" (`AssetSystem::reload` refuses an asset served from a package) and the "Development file serving" scenario (`RemoteFileProvider` is an interface with no transport). |
| `core-memory-and-containers` | Working | "Memory diagnostics". All four missing axes were built and **nothing pushes one**: `MemoryAttributionScope` appears nowhere outside `src/core/memory/`. |
| `editor-viewport-and-gizmos` | Working | "The rendering responsibility split" (what you see is what ships) and "Selection and picking". See below — this is the row that moved furthest. |

### The engine has no assembled renderer, and that is the shape behind three of the six

This is worth stating once rather than three times. Of the modules under `src/rendering/`, the
following are linked by **nothing but their own test binaries** — checked in `build.ninja` rather
than in CMake source: `cy_rendering_forward` (the forward+clustered renderer itself, since M3),
`cy_rendering_material`, `cy_rendering_post`, `cy_rendering_shadows`, `cy_rendering_sky`,
`cy_rendering_temporal`, `cy_rendering_gpu_culling` and `cy_rendering_virtual_texturing`.
`samples/03-first-light` links `cy::core-math` and `cy::rendering-graph` and builds its render graph
by hand. `samples/07-fidelity` links `cy::rendering-virtual-geometry`, `cy::rendering-gi` (which
pulls in denoise and ray tracing) and `cy::rendering-arbiter`. Nothing anywhere assembles a frame
out of the renderer's own parts.

That does not make the modules wrong — every one of them is tested against a reference rather than
against a screenshot, which was the deliberate choice — but it is why `rendering-architecture`,
`rendering-culling-and-lod` and, one level down, the arbiter's degradation criterion are recorded as
they are. It is the single largest thing M8 will trip over.

### `editor-viewport-and-gizmos` at Working, and what a person can now see

**A person can now open the editor and see the engine's own three-dimensional world in the
viewport, with a translate gizmo on the selected object, and drag it.** That was not true at M6 and
it is the thing this milestone was asked for. `just run-editor-window` photographs it: a perspective
scene of five shaded boxes on a checkerboard ground with cast shadows, drawn by
`cy_editor_window_runtime` — which links `cy::sample-first-light`, `cy::servers-render` and
`cy::rhi-vulkan` — and delivered into the editor's viewport over an imported dma-buf with no copy
through the CPU. The engine publishes the gizmo's layout; the editor's drawn X arrow lands 6 px from
where the published layout maps to; a drag on it moves the object **in the engine's world** and the
engine re-centres its own gizmo from (365, 471) to (433, 530); `Ctrl+Z` returns it to within a pixel
and the inspector's `translation X` returns from 1.250 to 0.000.

What stops it being Complete, in the runtime's own words. `samples/05b-editor-window/runtime/session.h`:

> What this file does is the smallest honest thing that makes the interaction real: the runtime
> associates each identity the editor names with one of its own objects, in first-seen order …
> It is a stand-in for a shared world and it is named as one; a reader should not mistake it for one.

Three consequences, each observed rather than inferred:

* **The viewport is not the document.** Three entities created in the editor during the artefact
  (`Ctrl+Shift+N`, three journal records, the outliner going from 3 rows to 7) produced no change in
  the rendered image: the same five boxes before and after. `editor-viewport-and-gizmos` requires
  "the image SHALL be produced by the same render graph the game uses", and it is — but of a
  different scene.
* **Task 5b.2 is not done.** `.cyworld` is still read and written by nothing under `src/` or
  `tools/`; `grep -rl cyworld src/ tools/` returns two build-description headers that mean something
  else and one README.
* **Engine-side picking is unexercised.** The artefact's own counter reads `pick 0`: selection in
  the run is by clicking an outliner row, not by clicking the image.

### `denoising` at Working

There is exactly one denoiser and no second implementation, and the stochastic many-light path
refuses to enable without it (`validate_many_light`, "this path SHALL NOT be enabled without
denoising"). But of the five `SignalKind`s it declares — `IndirectDiffuse`, `IndirectSpecular`,
`RayTracedShadow`, `AmbientOcclusion`, `StochasticDirect` — only the first two have a producer that
routes through it: `src/rendering/gi/` is the only module in the tree that names `Denoiser`.

### `virtual-texturing` at Complete, and the ceiling it has

The device-side page table is the **flat** form. `PageTable::create()` refuses a texture whose
pyramid exceeds 65,536 entries, naming the limit, rather than allocating 128 MB of mostly-zero
words. A hash on the device is what a 512k-texel virtual space needs and it is not here. The CPU
side has had flat and sparse behind one lookup since M6; it is the shader-visible half that is flat.

### How the ledger was run, twice, and what it failed on

`just roadmap-milestone m7` evaluates **165 declarations, 68 of them duplicates of a criterion an
earlier milestone already declares, so 163 distinct checks run once each** — 134 inherited from M0
through M6 and 31 new here. The end-to-end run took 33 minutes and reported **three failures**, none
of them in a criterion M7 wrote:

| Failed | What it was | What was done |
|---|---|---|
| `m0:test` | `just test-all` — `unit.material_compiler`, and beside it `unit.editor_window_runtime` and `smoke.fidelity` | three over-budget cases moved to the integration suite; see below |
| `m1:four-profiles` | the same, in the **Debug** configuration, where four material cases are over the unit budget with nothing else running | the same move, which was necessary and is not sufficient — see the budget instrument below |
| `m5:sanitizers` | AddressSanitizer reported 15 leaked allocations, 10,102 bytes | `tests/lsan-suppressions.txt`; see below |

**The ledger was then run a second time, end to end, with those repairs in.** `m5:sanitizers` is
green. `m0:test` and `m1:four-profiles` are not, and what is left under them is two separate things:
`smoke.fidelity`, which is the milestone's own exit criterion and is the subject of the next section,
and the budget instrument, which is the first entry under the machinery below. Verdict:
**`M7 is not closed: 2 of 163 evaluated criteria failed`, `rc=1`.** All four profiles were also built
**from empty** and tested outside the ledger: debug, profile and release are `test-all` green
(56 unit, 102 integration, 13 or 14 smoke, 8 render); dev is green but for `smoke.fidelity`.

The **three-platform** entries (`m0:three-platforms`, `m5:editor-three-platforms`) are reported NOT
EVALUATED on a host with one operating system, which is not the same as passed.

### The milestone's own exit criterion fails three runs in ten, and the run says which

Neither ledger run showed this on its own — both reported `m0:test` and it took a `dev` tree
configured **from empty** to separate `smoke.fidelity` from the budget instrument sitting on top of
it. It is the failure that decides the milestone.

`samples/07-fidelity` is the artefact for *"the arbiter's allocations converge without oscillation
under a step load"*. Run twenty times on a quiet machine, from `build/gate/dev`, it **failed six**:

```
    GAP   no magnitude oscillates — 1 of 9 kept moving levers after the load returned to nominal
    fig   total frames over budget after the loop settled 31 to 37 over 6480 samples
```

The outcome is not noisy, it is **bimodal**, and the two modes are exactly the two branches of one
`if`. Within each branch it is deterministic: **six of the six runs that took the substitution
reported the gap, and fourteen of the fourteen that refused it did not.** What varies between runs is
not the control loop, which is fed seeded noise and reproduces exactly; it is which operating point
the machine hands it.

| Runs | Nominal headroom | Frames over budget after settling | Result |
|---|---|---|---|
| 14 of 20 | **3.00 ms** — the modelled geometry cost, 3.10 ms | 0 | pass |
| 6 of 20 | **1.03 to 1.12 ms** — the device's measured frame, about 5.0 ms | 31 to 37 | **GAP, exit 1** |

`samples/07-fidelity/spike.cpp` substitutes the frame the device just measured for the geometry
subsystem's authored cost — which is the artefact's best property, because six of the seven rows are
a model and that one is not — but only when the nominal state that results still keeps
`kNominalHeadroom = 0.92F` inside the allocatable budget. **The device frame on this machine is
itself bimodal**, 3.3 ms or about 5.2 ms depending on the GPU's power state when the frame act runs,
so the substitution is taken on some runs and refused on others. When it is taken, the shipped
control law does not settle: one of the nine magnitudes keeps moving levers after the load returns to
nominal, and about 35 frames of 6,480 are over budget with the loop no longer acting.

Three things follow, and the third is the one that matters.

1. **The harness rule works.** The run reports the gap and returns 1, so `smoke.fidelity` goes red
   rather than green-with-a-note. That is task 5b.5 doing exactly what it was written for, and
   the first thing it caught was this milestone's own artefact.
2. **The guard was already there and is not sufficient.** `spike.cpp`'s own comment records that an
   earlier draft landed "within four MICROseconds of the ceiling" and that `geometry_measured`
   flipped between runs; the ceiling was moved and the figure still flips — and now the *verdict*
   flips with it rather than only the number.
3. **The 71-magnitude sweep cannot see this, and the reason generalises.**
   `integration.render_arbiter_sweep` reports 0 of 71 oscillating on every run, because it sweeps the
   step LOAD over a fixed cost table. What differs here is the OPERATING POINT — the nominal cost the
   ladders start from, and therefore how coarse each lever's quantum is relative to the headroom.
   `design.md` §2.1 says "one step magnitude can make any law look stable" and swept magnitudes for
   that reason; one nominal cost can make any law look settled, and nothing sweeps that.

**Raising `kNominalHeadroom` until the substitution is refused would make the artefact green and
would be the thing this project has been burned by eight times.** It is not done. The two honest
repairs are to sweep the nominal cost the way the load is swept and find where the law stops
settling, or to establish that ~1.05 ms of headroom over a 5 ms geometry row is outside what the law
claims and say so in `rendering-architecture`. Both are `src/rendering/arbiter/`'s, and until one of
them lands **`rendering-architecture`'s Working tier is the right one and M7's exit criterion is not
met**.

### What the gate found in the checking machinery

* **A closed milestone's criterion selects its suites by substring, and M7 changed what that
  substring means.** `m5:sanitizers` reads `for t in import text assets abi_live_reload; do just
  test-sanitize --sanitizer address,undefined --tests "$t"`. `text` was written to select
  `unit.text`. M7 added `render.virtual_texturing_gpu`, and `virtual_texturing` contains `text`, so
  a **device** test entered a leak-checked sanitizer run for the first time in this project's
  history and the criterion went red. Every one of the 15 leaked allocations is inside
  `/lib/x86_64-linux-gnu/libdbus-1.so.3`, reached from the Vulkan ICD through `dbus_bus_register`;
  no frame is in engine code and there is no handle to release. `tests/lsan-suppressions.txt`
  carries the one entry with that stack written down, wired in by `just test-sanitize` with
  `print_suppressions=1` so the entry reports its count and byte total on every run. Verified: the
  criterion as written now exits 0, over 6, 4, 6 and 1 tests. **The general defect is the selector**
  — a substring is not a name, and a suite added three milestones later can silently join a closed
  milestone's gate.
* **The unit suite's per-case budget measures the CPU's clock rate as much as the test, and an IDLE
  machine is its worst case.** This is the finding worth carrying forward, and it is the opposite of
  what everyone including this page assumed an hour earlier. `cy::test::BudgetGuard` measures
  `CLOCK_THREAD_CPUTIME_ID`, which counts **seconds, not cycles** — so it reports work divided by
  whatever frequency the governor was running at. This host has `scaling_governor = powersave`, cores
  idling at 800 MHz. Same binary, `cy_test_unit_scene` in the Debug tree — an M2 suite that nothing
  in M7 touches — worst case per run:

  | | worst case per run, ms |
  |---|---|
  | idle machine, five runs | 0.716 · 0.734 · 0.877 · 0.939 · 0.846 |
  | four spinners beside it, five runs | 0.211 · 0.206 · 0.200 · (2 under the probe's floor) |
  | idle again, five runs | **1.049** · 0.922 · 0.702 · 0.904 · 0.727 |

  **A case doing 0.21 ms of work fails a 1.00 ms budget with nothing else running on the machine**,
  and the same case beside four busy cores measures a fifth of that, because the spinners keep the
  cores boosted. `cy_test_unit_material_compiler` behaves identically: 0.56 to 2.68 ms idle, 0.42 to
  0.61 ms boosted. This is the mechanism behind "one unit suite failed once" in every milestone
  report since M4, behind the four separate M7 agents who recorded `unit.material_compiler`, and
  behind the six suites the ledger's `four-profiles` reported red in the Debug tree minutes after the
  same tree ran clean. It is not contention, not memory reclaim and not the suites. Swapping wall
  clock for CPU time at M2 was still right — wall clock moved by ten — but the remaining factor is
  five, not zero, and the failure message said it was zero. It now says what was measured.
  **The repair is to normalise: measure a fixed reference workload in the same process and scale
  every budget by it, so the taxonomy's question ("what does this test cost?") stops being answered
  in seconds on an unknown clock.** Nothing else in the harness needs to change, and nothing short of
  it will make `four-profiles` reliable.

  **How far the reading drifts over one session, on one tree, with nothing else running.** The same
  Debug tree was surveyed twice, four hours apart, with `CY_TEST_BUDGET_SCALE=0.75` — which lists
  every case within a third of its budget:

  | | suites with a case over 0.75 ms | worst case |
  |---|---|---|
  | after the three moves, early | **none of 56** | under 0.60 ms |
  | four hours later, same binaries | **ten of 56**, six of them over 1.00 ms | 2.322 ms |

  The ten include `unit.scene` (M2), `unit.rhi` (M3), `unit.render_extract` (M3), `unit.save` (M6)
  and `unit.virtual_texturing` (M6) — five suites no part of M7 touches. `ctest -L unit` in that tree
  run three times in a row on an idle machine gave **5, 1 and 4 failures**. The set is not a property
  of the tests; it is a property of the hour.
* **Underneath that, three cases were genuinely too big for the suite they were in**, and the Debug
  configuration is where it showed. `CY_TEST_BUDGET_SCALE=0.5` over every unit binary names every
  case within a factor of two of its budget; in Debug it named `material_lowering`'s family cases
  (1.23 to 2.26 ms against 1.00, **over budget on an idle machine**), the cookie scroll case (0.87 to
  0.95 ms, five per cent of margin) and the overlay clipping case (0.81 to 0.98 ms, two per cent).
  All three are now integration suites — `integration.material_lowering`,
  `integration.render_lighting_reference` and `integration.editor_window_overlay` — each with the
  measurement written into the CMake declaration beside it, exactly as `material_ibl` has carried
  its own since M3. After the move **no unit case in the Debug configuration exceeds 0.60 ms and
  none in Development exceeds 0.40 ms**, where before the worst was 2.26 ms and three suites had
  under 15 per cent of margin. Every assertion is conserved: lighting 467+78 before and 460+85
  after, material 10,460+247 before and 233+10,227+247 after.
* **`just run-agent-authoring` reports four unsatisfied steps and exits 0.** M7 task 5b.5 made "a
  GAP is not a pass" structural in `samples/harness/`, with negative fixtures, and
  `samples/05b-editor-window/window.py` and `samples/07-fidelity/fidelity.py` are held to it. The
  M5.5 agent artefact was not migrated: its `Report.failed` reads `self.steps`, and `Report.gap`
  appends to `self.gaps`, so `return 1 if report.failed else 0` cannot see a gap. It is a permanent
  merge gate, and the ledger reports it as `ok` in 0.3 s: reproduced at M7's gate, the run prints
  `13 step(s) satisfied` then `4 step(s) NOT SATISFIED, each named above and in README.md` and
  returns 0. **It is left as it is deliberately and it is the one thing on this page a reader should
  argue with.** Migrating it to `samples/harness/` turns `m5b:agent-recipe` and `smoke.agent_authoring`
  — a green permanent gate and a closed milestone's criterion — red for four steps that name changes
  in the editor that do not exist, three of them outside M7's scope. Doing that at M7's close would
  make this milestone answer for M5.5's, so the finding is recorded here with the reproduction and
  the decision is the next change's. What is NOT acceptable is leaving it undecided a third time.
* **And the other rule has an unmigrated artefact too, which is the one M6's gate was actually
  about.** Of the five sample drivers under `samples/`, two report through `samples/harness/`
  (`05b-editor-window`, `07-fidelity`), one has its own `Report` with the gap defect above
  (`05b-agent-authoring`), one claims no figures at all (`05-editor-session`), and
  `06-open-world/openworld.py` still writes `worst tick {worst_us} us of CPU time, no hitch` as the
  detail of its frame-budget step and `worst tick … us` into the footer of the committed
  screenshot — the exact statistic, in the exact place, that task 5b.5b was written about. The rule
  is structural for anything that reports through the harness and it binds no sample that does not.
  Migrating this one is cheap, because the figures it needs are already computed
  (`run.cpp:794` prints `median`, `p99` and `worst` side by side) and only the choice of which to
  lead with would change.
* **One of those four gaps names a reason that is no longer true.** It reads "opening a document
  builds an empty one — `DocumentService::open` calls `Document::new` and there is no world loader",
  and `editor/crates/cy-editor-services/src/documents.rs:121` calls `worldfile::load`. The file's own
  header says a gap whose reason changed is a different gap.
* **"The four plan documents agree" does not catch an omission.**
  `plan._check_work_against_matrix` iterates the rows the roadmap's work table *names*; a capability
  the matrix advances at a milestone whose work table omits it is never compared. Eight of M7's
  nineteen planned rows were in the matrix and the proposal and not in the roadmap's M7 work table.
  **Quantified at the gate**, by running the reverse direction over the committed documents: 63
  matrix cells across seven milestone columns — M3 2, M4 2, M5 4, M8 15, M9 6, M10 3, M11 31 — are
  advanced by the matrix and named by neither the work table nor the prose of their milestone's
  section. **None of the 63 is M7's**, because M7's section names every row it advances in prose.
  Turning the check symmetrical is therefore a one-line change with 63 findings behind it, all of
  them in milestones that are closed or not yet written; it is a change to make at the start of a
  milestone and not at the end of one, and it belongs to whoever writes M8's plan.

## Where M6's tiers are thin

The six tiers M6 advanced are in [the status record](#the-status-record); the five the plan expected
to complete and that did not are argued under [milestone load](#milestone-load) and in
`tools/roadmap/milestones/m6.toml` beside `[criterion.expect_tiers]`. What follows is where the
implementation is **thinner than the tier it reached**, and what M6's own gate found. Everything
below was measured or reproduced at that gate on this tree: Linux 6.8, GCC 13.3.0 and Clang 18.1.3,
clang-format and clang-tidy at 22.1.8, Swift 6.3.3, Rust 1.95.0, an NVIDIA RTX 5060 with Vulkan
1.4.312. All four profiles were built **from empty** and `just test-all` was run in each.

**How the ledger was run, and why it was run three times.** `just roadmap-milestone m6` evaluates 132
criteria — 110 inherited from M0 through M5.5 and 22 new here — and the first end-to-end run reported
**five failures**. Four of them were criteria that had never executed at all (entry 11 below) and the
fifth was a test that had been over its budget in one profile since M4 (entry 8). Both were fixed;
the second run then went red on `just ci-check`, because repairing the ledger and the workflow made
the *third* copy of the same broken command — in two permanent gate declarations — visibly
inconsistent, which is that check doing its job. **The third run is green: `M6: 132 criteria pass`,
`rc=0`, with the two three-platform legs reported as NOT EVALUATED on a host with one operating
system.** A ledger that is only ever run once per milestone is a ledger whose own defects ship: two
of the findings below were found by running it rather than by reading it, and neither would have been
found by a run that stopped at the first failure.

All four profiles were also rebuilt and re-tested after those repairs, on a display with no keyboard
grab: **`just test-all` exits 0 in every one of the four**, unit 41/41, integration 85/85, smoke
13/13 in dev and debug and 12/12 in profile and release (the difference is `smoke.shader_slang`,
which `CY_SHADER_SLANG=DEVELOPMENT` correctly excludes from the two optimised configurations), render
5/5.

**One caveat about the timings.** An unrelated project on this machine was running
`cargo test --workspace --release` on up to four cores for part of the third run. It is recorded
because three "regressions" in this project's history were concurrent builds contending, and because
one entry below is a measurement.

### What the gate found

Numbered in the order they were found rather than by weight. **The two that matter most are
11 — four of M6's own criteria had never executed, and three of them were in a CI job too —
and 2, the milestone's named risk, which is fixed in the new build graph and still live in the
tool that actually cooks content.**

**0 · The one red test is the machine, and the mechanism is now named rather than guessed at.**
`smoke.editor_window` failed in all four profiles on `:0`, at `Ctrl+Shift+N never produced entity 1`,
and three earlier M6 agents recorded it as "an X server / XTEST fault on this machine". It is more
specific than that, and the difference matters because it decides whether anything in the repository
is at fault. On `:0`:

```
grab_keyboard(root) -> 1 (AlreadyGrabbed)      eight samples over sixteen seconds, all identical
xinput test-xi2 --root                          EVENT type 13 (RawKeyPress)  device: 3 (5)
                                                EVENT type  2 (KeyPress)     device: 5 (5)
a window that holds the input focus             events after the synthetic key: []
```

The X server **does** generate the XTEST events — they are visible on the XTEST slave keyboard at the
XInput2 level — and an **active keyboard grab held by another client** routes them to the grabber
instead of to the focused window. That grab is the fact, and it is measured. **Which client holds it
is an inference**, because the X protocol does not report a grab's owner: `ps` shows
`cinnamon-screensaver` and `cs-backup-locker --xid …` running for five and a half hours, which is
what a locked session looks like, and a lock screen holding the keyboard is the ordinary explanation.
Nothing here depends on that inference being right.

The proof that this is environmental and not the engine's is a second X server on the same machine.
A bare `Xephyr :77` has no grab (`grab_keyboard -> 0`), the same probe reports
`KeyPress KeyRelease ButtonPress ButtonRelease`, and on it:

| Suite, on `:77` | dev | debug | profile | release |
|---|---|---|---|---|
| `smoke` | 13/13 | 13/13 | 12/12 | 12/12 |

`smoke.editor_window` passes in **12.3 s** where it failed in 54 s on `:0`. Nothing in the repository
was changed between the two runs. The lesson for whoever runs this gate next is in the artefact's own
configure guard: `samples/05b-editor-window/CMakeLists.txt` declines to register the test when
`DISPLAY` is unset or python-xlib is absent, but a display whose keyboard is grabbed passes that guard
and cannot pass the test. **A test was left registered that no amount of correct code could pass.**

**1 · The milestone's own task 2.8 is still not done, and the artefact that would prove it exits
zero anyway.** Task 2.8 is *"a person opens a world, selects an object, drags a gizmo, undoes, and
saves — end to end, in the window"*. Run on `:77`, where every other act passes:

```
--- act 3: a gizmo drag, and the undo that takes it back ---
    GAP   the gizmo drag — the drag committed nothing. … A drag that still commits nothing means the
          pointer did not land on a gizmo handle, not that there is nothing to bind to
    1 step(s) NOT SATISFIED
```

Everything the milestone built for it **is** there: `types.cytypes` is the engine's own registry,
written by `cy::scene::serialization::write_authoring_schema` and compared byte for byte by
`cy_test_unit_scene_serialization`; the opened document's schema declares `Transform`;
`Viewport::pump` is called once per frame (M5.5's recorded one-line defect, closed);
`worlds/city.cyworld` is written on save, 31 lines, carrying the schema it was written against. What
is missing is the aim: `act_drag` presses `w` and drags from a **hard-coded (47 %, 38 %) of the
viewport**, hoping a gizmo handle is under it. `window.py` records a GAP and **returns 0**, so
`smoke.editor_window` is green and `just run-editor-window` is green while the milestone's headline
interaction is unproven. That is a criterion that does not cover its claim, and it is the single
thing M7 should close first on the editor side — by asking the viewport where it drew the handle
rather than by guessing at a coordinate.

**2 · The spike's headline correctness bug is fixed in the new graph and still live in the tool that
cooks.** `design.md` §1.3 found that `cy::import::import_derivation_key` contributes no compiler, no
flags and no library versions, so two builds of one importer computed the same key and one was served
the other's artefact. `tools/build/` fixed it *for itself* — `derivation_key` refuses a key without a
complete `ToolchainFingerprint`, which is generated from `deps/manifest.toml` and
`deps/host-tools.toml` at configure time. `tools/import/src/importer.cpp` is **unchanged**, and it is
the function `cy_import_cli` uses. Re-measured at this gate with the two binaries this gate had
already built:

```
dev     cy_import_cli  sha1 30264ce3…   -O2 -g -DCY_DEVELOPMENT
profile cy_import_cli  sha1 d72a293d…   -O2 -g -DNDEBUG
$ dev/cy_import_cli     … --cache shared    →  1 cache entry, key 271e229d…
$ profile/cy_import_cli … --cache shared    →  cache: 1 hit, 0 miss, 0 invalidated
```

Two different binaries, one key, a reported hit. The new fingerprint distinguishes them —
`704a618c…` against `f6ab6294…` — and nothing carries it into the importer's key.
`asset-import-pipeline` requires "one cache covering all derived data" and there are still two caches
and two key functions (`cy::assets::DerivedCache` and `cy::shader::CacheTier`; `design.md` §1.8).
**This is the one entry on this list that is a correctness bug rather than an absence**, and M7 owns
it before its material and geometry cooks make a wrong hit a wrong shipped artefact.

**3 · Two of the seven new modules are linked by nothing but their own tests.**
`cy::servers-render-culling` and `cy::servers-render-geometry` appear in exactly four
`DEPENDENCIES` lines, all of them in their own `tests/CMakeLists.txt`; no renderer consumes them and
`samples/06-open-world` does not link them. `cpu_reference_cull` is a reference with nothing yet to
check against it, and `SkinningDescriptor` describes a compute pass nobody dispatches. That is the
substance behind `rendering-culling-and-lod` at Working — the tier is carried by the layer-4 module
`cy::rendering-culling` that `cy::rendering-forward` has consumed since M3 — and behind
`rendering-geometry-and-resources` not completing.

**4 · Nothing in virtual texturing is on a GPU, and the ledger says so in its own comment.** The CPU
side is complete and measured: address spaces, page tables, tiles, the physical cache, the resident
mip tail, wait-free feedback, prefetch and runtime producers, exercised end to end by the artefact
— `smoke.open_world` asserts that no sample was ever missing while pages were produced under
traversal, and it passed in all four profiles at this gate. There is no feedback buffer written
by a shader and no page table sampled by one. `virtual-texturing` is recorded at **Working** because
its requirements are satisfied on the side that exists and the artefact exercises them; a reader who
wants a virtual texture on screen wants M7.

**5 · The world streams from a cost model, not from a file.** `src/world/` contains no file I/O —
`grep` for `fopen`, `ifstream` or `FileSystem` over `src/world/src/` returns nothing — and
`WorldStreaming::tick` charges `resident_io_bytes` from `CellPayload`'s declared size. The module's
own README says so ("No cooker and no asset loading yet"). The exit criterion *residency and
activation are separate* is nonetheless real and independent of that: 20 688 bytes resident, cell
state `Resident`, `ecs.entity_count() == 0`, `published_entities == 0`, and then `Activated` with
entities the moment a source that activates arrives.

**6 · Two overlay models, one requirement.** `cy::world::PersistenceOverlay` (keyed by `CellId`, raw
component bytes at a runtime component index) and `cy::save::Overlay` (keyed by `RegionKey`,
per-field records at a `reflect::TypeId`). The artefact converts between them and says so where it
does it. `src/save/README.md` carries the resolution — one `PersistentId` at layer 0, the world
holding a save overlay — and it is cheaper before a save format ships than after.

**7 · The editor reads a world format the engine can neither write nor read.** `types.cytypes` is the
engine's, gated and compared byte for byte. `worlds/*.cyworld` is not: it is
`cy_editor_services::worldfile`, a third authoring representation beside
`cy::scene::serialization`'s `cydoc` text form and `cy::world::CookedCell`. Nothing in `src/` or
`tools/` writes or reads a `.cyworld`. `src/scene/serialization/README.md` records it rather than
leaving it to be discovered. **The editor opens a world; it does not yet open the engine's world.**

**8 · A permanent-set criterion had been a coin flip in one profile since M4, and the first
end-to-end run of this ledger is what caught it.** `m1:four-profiles` went red in the **Debug**
configuration on one case: `unit.physics_server`'s *one thousand identical box colliders create one
shape*, at 1.573 ms of its own CPU against a 1.000 ms unit budget. Measured on an idle machine
afterwards: **22 of 40** runs of the case alone exceeded the budget, between 1.06 ms and 2.10 ms, and
about **1 run in 10** through `ctest`. `src/servers/physics/` has no change in this milestone — its
last commit is M4's — so a criterion that has been in the permanent set since M1, and that every
later ledger runs, has been failing about one Debug run in ten for two milestones with nobody
watching. That is the same shape as the flake `m2.toml` records and it is a different test.

**Fixed at this gate, the way the budget's own diagnostic says to.** The case moved to
`integration.physics_behaviour` as `src/servers/physics/tests/test_shape_sharing.cpp`, line for line,
because the requirement is about *one thousand* entities and cutting the count to fit a unit budget
would narrow the claim to fit the measurement. Fifteen consecutive Debug runs of each suite
afterwards: 0 failures.

**9 · The Shipping profile emits warnings that `-Werror` does not catch.** The link-time-optimised
build reports `-Walloc-size-larger-than=` twice, in `src/core/reflect/src/registry.cpp:131` and
`:135`, from `grow()` inlined into `add()`. `CY_WARNINGS_AS_ERRORS` is ON by default and these are
diagnosed during LTO, where it does not reach them. M1's code, found by building Shipping from empty.

**10 · Six milestone gates were green and no CI job ran one of them — closed here, and honestly
short of what the requirement asks.** `tools/ci/check_workflows.py` skipped any gate whose class was
not `permanent`, so `milestone-m0` through `milestone-m5b` sat at `state = "green"` with
`roadmap-milestone` appearing in `ci.yml` only inside a comment. That is M6 task 10.9 and it is
fixed: the coverage rule now reads milestone gates too, honouring the flat ledger — a green gate is
covered when a workflow evaluates the ledger of any milestone at or above its rung, so one job
covers all seven rather than seven jobs re-running `four-profiles` seven times — and its own negative
fixture restores the defect and fails. The new `milestone` job runs `just roadmap-milestone m5b`.
**It is not a merge gate.** It runs nightly, on `workflow_dispatch`, and on a push to `main`, because
one pass of that ledger took hours at this gate and a four-hour merge gate is a reason to write an
override rather than a reason to wait. Most of the ledger's work is blocking already through the
other jobs; what this adds that nothing else does is the artefact recipes, the tier records and the
ledger's own arithmetic — post-merge rather than pre-merge. Recorded so M7 can widen it deliberately.

**11 · FOUR OF M6's OWN TWENTY-TWO CRITERIA HAD NEVER RUN, AND THE LEDGER REPORTED THEM AS FAILURES
IN 0.0 SECONDS.** This is the finding the gate exists for, and it is the eighth milestone in a row to
produce one. `world-activation`, `save-generations`, `save-service` and `import-complete` were each
written as a single `ctest` regex:

```
run = "just test-integration -R \"world_(activation|streaming)\""
$ bash -c 'just test-integration -R "world_(activation|streaming)"'
bash: -c: line 1: syntax error near unexpected token `('
error: Recipe `test-integration` failed on line 19 with exit code 2
```

`just`'s `*args` is a **string**. A recipe body interpolates `{{args}}` textually and hands the line
to `bash -c`, so the quotes the caller wrote are consumed by the caller's own shell and the
parentheses arrive unquoted. No test was selected, no test was run, and the criterion failed
instantly — which is at least loud. **The same three commands were in `.github/workflows/ci.yml`'s
`scale` job**, where they would have failed on the first pull request that ran it, and where the
milestone's own streaming, save and import suites would have been covered by nothing until somebody
read the log. **And they were in `tools/roadmap/gates.toml` too**, as the `runs` of the *permanent*
gates `world-streaming` and `save` — a permanent merge gate whose declared command cannot be typed.
That third copy was found by the SECOND ledger run: repairing the ledger and the workflow made
`just ci-check` red, because its coverage rule compares a gate's `runs` against what the workflows
invoke and the two no longer matched. One defect, three files, and the check that caught the third
is the one M6 task 10.9 had just taught to look at milestone gates.

Each is now one invocation per suite joined by `&&`, which is the form `culling` and `geometry`
already used and the reason those two ran. All ten suites were then run singly, and all ten pass. The
deeper repair is in `just/test.just` — `test-unit`, `test-integration`, `test-smoke`, `test-render`
and `_ctest` all push `{{args}}` and then `${rest}` through unquoted word splitting, so **no argument
containing a shell metacharacter can reach `ctest` through any of them**. `set positional-arguments`
and `"$@"` is the idiomatic fix; it is a change to M0's shared workflow files and to `just _split`,
which is more than a milestone gate should do to a file other work is in flight over. It is written
into `m6.toml` beside the repaired criteria and is M7's.

### The adversarial pass, and what each of the eleven actually showed

`delivery-roadmap` requires the audit in full through M8, and these are M6's own invariants rather
than anything an accumulated gate inherits. Every probe was written for this gate and deleted after
it; the commands and figures are in the milestone report.

| # | Invariant | Verdict | What was actually done |
|---|---|---|---|
| a | Two different inputs cannot share a derivation key | **PASS** | 30 node declarations attacking the framing at every boundary a name meets a value — `("ab","c")` against `("a","bc")` as an option, as platform/profile, as an output list, as a producer version — produced 26 keys, and every shared key was one declaration written more than one way. **And the shipping importer's key is separately shown blind to the toolchain — entry 2 above.** |
| b | An immutable artefact cannot be mutated unnoticed | **PASS** | Artefacts are mode `0444`; a plain overwrite is `Permission denied`. After `chmod u+w` and an overwrite, `cy_build verify` reports `9 checked, 1 corrupt`, and a build that must read it reports `failed 1` rather than serving it |
| c | A patch applies atomically and rolls back when interrupted | **PASS** | `--crash-at` at all five stages, then **120 real `SIGKILL`s at uniformly random instants** across a 30 ms apply: 106 left the previous build whole, 14 completed, **0 torn** |
| d | A save generation is fully applied or absent under `kill -9` | **PASS** | **200 `SIGKILL`s at uniformly random instants** during real filesystem commits of 24 regions × 32 entities: 136 loaded a whole generation, 64 had nothing committed yet, **0 torn, 0 unreadable**. Each verification checks all 768 values agree on one generation number, which is what makes "half a generation" detectable |
| e | Residency and activation are separate | **PASS** | 20 688 bytes resident, `CellState::Resident`, `ecs.entity_count() == 0`, `published_entities == 0`; then `Activated` with entities, same cells, one added source |
| f | Every subsystem survives teardown mid-flight, under load | **PASS** | **18 of M6's 21 suites** — every one of the thirteen integration sets, including all five `*_teardown` sets, `save_kill_nine` and `world_activation`, plus the five stateful unit sets — run **20 consecutive times each** under eight spinning threads on a 24-core machine: **360 passes, 0 failures**, 92.9 s. M5.5's engine defect was one run in forty on an *idle* tree, so a single green run of a teardown suite is not evidence and this is the shape that would have caught it |
| g | A cold build and a cache-warm build are byte-identical | **PASS** | Cold, warm (`ran 0 cached 6`) and a second cold into a third store: every artefact byte-identical and one build id, `601c4937…`, across all three |
| h | A one-asset change invalidates exactly its dependents | **PASS** | One edit ran exactly `{import:rock, cook:north, package:base}` and cached the other three; a rewrite with identical content ran **0 of 6**; an edit the producer normalises away ran **1 of 6** — early cutoff, where deep input keys would have run three |
| i | Virtual-texture feedback cannot block a frame | **PASS** | Measured on the recording thread's OWN cpu clock, because the first attempt measured `steady_clock` with more threads than cores and reported 35 ms of *descheduling* as if it were a wait. Three attacks: 5 000 000 samples into a never-drained 64-slot bank (`recorded 64, dropped 4 999 936`, p50 **124 ns**, max 64 µs); 12 recorders against a flipper (30 181 683 calls, p50 761 ns, p99 3.7 µs, p99.9999 127 µs, max 171 µs); and 12 recorders with **no consumer at all** (`recorded 256, dropped 29 429 933`, max 147 µs). Full always drops and never waits, and the tail is bounded — a call that waited on the flipper would have an unbounded one |
| j | Every `CY_*` option M6 delivered is ON by default | **PASS** | M6 delivered no new option, which is the strongest form of it. M6's **20** test suites — seven `unit`, thirteen `integration` — and the closing artefact's `smoke.open_world` entry are all registered in all four default-configured trees, and `grep option\(CY_ ` over the seven new modules' `CMakeLists.txt` returns nothing |
| k | ufbx and xatlas are declared, and the gate would fail without them | **PASS** | Three mutations, each restored: the attribution deleted from `THIRD_PARTY.md`, the dependency deleted from `deps/manifest.toml`, and a crate deleted from `deps/rust-crates.toml` — all three fail the gate, the third naming `egui 0.36.1` and the recipe that fixes it. That third is M5.5's own 386-crate gap, now covered |

## Where M5.5's tiers are thin

The three tiers M5.5 advanced are in the record above. What follows is where the implementation is
**thinner than the tier it reached**, and what M5.5's own gate found. Every entry was measured or
reproduced at that gate on this tree: Linux 6.8, GCC 13.3.0 and Clang 18.1.3, clang-format and
clang-tidy at 22.1.8, Swift 6.3.3, Rust 1.95.0, and an NVIDIA RTX 5060 with Vulkan 1.4.312. All four
profiles were built from empty and `just test-all` was green in each; the editor's own gate — rustfmt,
clippy with `pedantic` as errors, and 62 suites of 730 tests — was green in all four; and all thirteen
static gates were run by hand and passed. `just roadmap-milestone m5b` evaluates 112 criteria, 101 of
them inherited from M0 through M5 and 11 new here; **110 pass and two are not evaluated on this host
because they are the three-platform legs.** An unrelated build on another project held two to three
cores for part of the run, which is recorded because two entries below are timings and one is a flake.

### What the gate found

**0 · An intermittent trap in the Jolt backend, and it is a defect in the engine.** M5.5's ledger
went red on `m4:physics-jolt` — `integration.physics_jolt (SIGTRAP)` — a criterion in the permanent
set that `just test-all` runs on every pull request on three platforms. It reproduces: **one run in
forty to fifty** of the test binary, caught under `gdb`, in

```
JPH::FixedSizeFreeList<JPH::JobSystem::Job>::~FixedSizeFreeList   FixedSizeFreeList.inl:17
  JPH_ASSERT(mNumFreeObjects.load(relaxed) == mNumPages * mPageSize)   // "everything is freed"
cy::physics::jolt::EngineJobSystem::~EngineJobSystem              src/backends/physics-jolt/src/jolt_jobs.cpp:27
cy::physics::jolt::JoltWorld::~JoltWorld                          src/backends/physics-jolt/src/jolt_server.cpp:224
cy::physics::jolt::JoltServer::destroy_world_slot                 src/backends/physics-jolt/src/jolt_server.cpp:1156
cy::physics::jolt::JoltServer::shutdown                           src/backends/physics-jolt/src/jolt_server.cpp:340
(anonymous namespace)::Fixture::~Fixture                          src/backends/physics-jolt/tests/test_jolt.cpp:50
```

`EngineJobSystem::QueueJob` hands a `JPH::JobSystem::Job` to the **engine's** job system with an
`AddRef`, and `EngineJobSystem::run` does `Execute()` then `Release()` on a worker thread.
`~EngineJobSystem` is `= default` and drains nothing, so a shutdown that overtakes an in-flight
`physics.jolt` task destroys the free list while a job is still allocated. The failure is a **race at
teardown**, not a load artefact: it was reproduced on an otherwise idle tree, in a bare loop over the
test binary. In the **Profile** configuration the same test aborts (`SIGABRT`) about once in sixty
instead of trapping, which is either the assertion compiled differently or the memory error behind
it — that was not pinned down, and it is the reason this is worth more than a retry.

**What it costs the gate, measured.** `just roadmap-milestone m5b` was run **four times end to end**
at this gate. **Run 1**: `m4:physics-jolt` red (`SIGTRAP`) and `m1:four-profiles` red on a unit case
that could not be named (finding 9). **Run 2**: 110 pass, 0 fail. **Run 3**: `m1:four-profiles` red
again, on `integration` this time, `rc=1`. **Run 4**: 110 pass, `rc=0` — which is the run that
satisfies task 5.8. Every ledger runs this test at least five times — once as its own criterion and
once per profile inside `four-profiles` — so a two-percent flake is a ten-percent red ledger, and
**two of the four runs here were red**. The recipe exits zero; it does not exit zero reliably, and
those are different claims. It is M4's to fix and not M5.5's, and it is
recorded here because M5.5's gate is what found it, because it is the first finding in six milestones
that is a defect in the engine rather than in a gate or a claim, and because a permanent merge gate
that is red one run in ten is a gate people learn to re-run.

**1 · The engine's renderer has still never appeared in the editor.** The milestone's headline —
"the viewport shows the engine's frame" — is, precisely, *the editor imports and composites another
process's `VkImage` with no copy through the CPU*, and that half is real: 20 consecutive imported
frames at 1920×1080 were compared byte for byte against an independently recomputed image and
**8,294,400 bytes matched exactly on every one of them**, with the comparison shown to fail on a
one-bit change to a single channel. But the process on the other end is `cy-viewport-publisher`, a
Vulkan fixture in the editor's own Cargo workspace that clears an image and blits a white bar. No
binary under `src/` speaks this wire — `grep -rn "dma_buf\|SCM_RIGHTS\|external_semaphore" src/ tools/
modules/` returns nothing. What is proven is the transport and the toolkit; what is not is that the
editor can show a world. That is `live-editing`'s, and it is why `live-editing` stays at **Seed**.

**2 · The transform gizmo is never drawn, and cannot be dragged.** Both halves, and they have
different causes. It is not drawn because `editor-viewport-and-gizmos` assigns "gizmo geometry, depth
handling, and drawing" to the **engine**, and nothing publishes a `GizmoLayout` — which is correct
architecture and means `docs/design/images/transform-gizmo.png`, normative since M3, is realised in
no pixel at M5.5. It cannot be dragged because `DocumentService::open` calls `Document::new` — a name
and an empty schema — so `TransformBinding::of_schema` finds no `Transform`. The behaviour is real
and tested: 22 gizmo cases pass in the shipping profile, including
`a_drag_out_and_back_restores_the_original_bits` (exact `f32::to_bits` equality),
`a_drag_that_ended_where_it_began_records_nothing` and
`a_manipulation_produces_exactly_one_history_entry` — against a fixture document a test can build and
the editor cannot open. The scene orientation widget, by contrast, **is** drawn and **is** draggable,
and `dragging_the_widget_orbits_the_camera_and_produces_no_transaction` holds.

**3 · The editor survives its runtime's death and does not say so.** Reproduced by hand against the
shipping window: with `cy-viewport-publisher` `SIGKILL`ed mid-session the editor stayed alive, held
the last complete frame (a coherent image, not garbage — viewport chroma 254 against charcoal
chrome), kept the six journal records it had, authored two more entities afterwards through
synthesised key presses — six records to eight — and closed in 0.11 s. The same session had already
been through a `SIGSTOP` on the publisher, which stages an unsatisfiable wait with the socket still
open: the editor stayed responsive through that too, three records to six.
**Nothing on screen changed.** `ViewportLink::begin_frame` computes the sentence — *"The runtime is
no longer running. This is the last frame it produced."* — and `panels::viewport` draws the condition
only in the `else` branch of `if let Some(texture) = panels.link.texture()`, which after a death is
never taken, because the texture is deliberately retained. The transport counters simply stop
advancing. This is the M5 failure shape exactly — a fallback that happens in silence — and the fix is
to draw the condition as an overlay whenever liveness is not `Live`, rather than only when there is
no image.

**4 · Viewport picking is dark while the screen is full of frames.** Clicking in the viewport answers
*"No frame has arrived yet, so there is nothing on screen to have clicked"* while the overlay a few
centimetres away reads `announced 1016 · skipped 0`. `cy_editor_viewport::Viewport::pump` is called
from tests and from nowhere else in the product, so `Viewport::pick` sees an empty stream. The
artefact's own README found this and proposed one line in `ViewportLink::begin_frame`; the gate
confirms the missing edge and adds that **the line is not sufficient** — `Viewport::pick` produces a
`PickRequest` for a runtime to answer, and no runtime answers one, so click-to-select needs the
engine as well as the pump.

**5 · Three hundred and eighty-six third-party crates entered the tree and no attribution gate can
see them.** `editor/Cargo.lock` names 403 packages, 386 of them from the registry
(`grep -c '^source = "registry' editor/Cargo.lock`), where M5's lockfile named none. `thirdparty-dependencies`
requires every dependency to be "recorded in a single machine-readable manifest containing: name,
version, commit hash, upstream URL, licence identifier, licence file path ... and a one-line
justification", and requires the editor's Rust dependencies to be "declared, pinned, licence-reviewed
... and justified". The **direct** dependencies are exemplary — every one is `=`-pinned in
`editor/Cargo.toml` with its reason beside it — but `deps/manifest.toml`, `deps/host-tools.toml` and
`THIRD_PARTY.md` contain none of the 386, and `just maintenance-deps-check` compares `THIRD_PARTY.md`
only against the two C/C++ manifests. It passed. `grep -ci "egui\|wgpu\|winit" THIRD_PARTY.md`
returns 0. The Rust toolchain itself, newly load-bearing at 1.95.0, is likewise absent from
`deps/host-tools.toml`. This is a gate that is green over a question it was never asked, and its
shape admits a check.

**6 · The editor's build now sits inside a 300-second CTest budget that continuous integration may
not make.** `integration.editor_session_selftest` and `smoke.agent_authoring` both run
`just build-editor`, and at M5 that built a workspace with no dependencies. Measured here from an
empty tree on 24 cores with a warm crates.io cache: **30.7 s** in `debug`, **67.8 s** in `dev` — the
profile continuous integration uses — and **86.2 s** in `profile`, against a `CY_TEST_TIMEOUT_smoke`
and `CY_TEST_TIMEOUT_integration` of 300 s. The `test` job in `.github/workflows/ci.yml` installs no
Rust toolchain (the `editor` job does, and they are different jobs), restores no Cargo registry or
target cache, and the repository pins Rust 1.95.0 in `rust-toolchain.toml` — so a hosted runner pays
a rustup download and 386 crate downloads on fewer cores inside that budget. Unverified in CI, and
the honest reading is that it is marginal rather than safe.

**7 · A public write path one file away from `content.rs` is invisible to the test that exists to
forbid it.** `document_content_has_no_public_mutator_that_does_not_take_a_write_token` scans exactly
`src/content.rs`. Adding `pub fn detach_roots(&mut self) { self.roots.clear(); }` there fails it
loudly, naming the file and the line — the check works. Adding the identical method in
`src/content/escape.rs`, a **child module of `content`** and therefore allowed by Rust's own privacy
rules to touch every private field, compiles and **the whole crate's 52 tests pass, including that
one and the audit**. The audit misses it too, because a direct field write moves neither `writes` nor
`transactional_writes`. Scanning every file that carries `impl DocumentContent` rather than one file
closes it.

**8 · Six milestone gates are green and no continuous-integration job runs any of them.**
`tools/roadmap/gates.toml` says a `class = "milestone"` gate at `state = "green"` "is permanent from
then on — a later milestone may not break an earlier milestone's gate", and `just ci-check` reports
"every permanent gate run" — but `tools/ci/check_workflows.py` skips any gate whose `class` is not
`"permanent"`, and `grep -n roadmap-milestone .github/workflows/ci.yml` finds only a comment. Most
individual checks are covered because M5.5's criteria mostly name permanent jobs, but the eight that
name `milestone-m5b` run nowhere, and **five of them need neither a display nor a device**:
`roadmap-tiers`, `agent-recipe`, `screenshot-window`, `screenshot-agent` and `m6-open`. The two that
need a display and the one that needs a device declare that with a reason, so a machine that cannot
judge them reports *not evaluated* rather than passing — which is right, and is why they are not the
complaint. The other five are simply unassigned, and any hosted runner could run all of them.

**9 · A ledger that truncates the evidence of its own failure.** The same run recorded
`m1:four-profiles` FAILED with `test-all: failed — unit` in the `debug` leg. Which unit case failed
is **not recoverable**: `criteria.py` keeps a criterion's last thirty-odd lines, `just test-all`
deliberately runs every suite after a failing one and prints its verdict at the end, so the ctest
failure summary was hundreds of lines above the tail that was kept. The suite has since been run
**433 more times in that same tree** — `ctest --repeat until-fail:400` plus eight full
`just test-unit --profile debug` runs — with no failure, so it is rare rather than latent, but the
honest position is that a failure was seen and could not be named. A ledger should keep the lines
that say *FAILED* as well as the lines that came last; that is a one-line change to `_run` and it
would have turned this paragraph into a bug report.

**10 · One criterion reconfigures the tree every other criterion is using.** `m1:job-throughput`
runs `just test-bench-jobs --profile profile`, and `test-bench-jobs` resolves its directory as
`${CY_BUILD_DIR:-build/${profile}}` — so under a ledger, which sets `CY_BUILD_DIR`, it reconfigures
the **shared** ledger tree from `Development` to `Profile` (interprocedural optimisation on) and the
next criterion reconfigures it back. Two full rebuilds of the engine and of Slang, per ledger run,
for nothing: `m1:job-throughput` took 450 s and 319 s in the two runs here and `m1:memory-budgets`
immediately after it took 281 s, against four seconds for its neighbours. `m4:sanitizers` shows the
correct shape in the same file — `d="${CY_BUILD_DIR:-build}/m4-sanitize"` — and this criterion should
do the same. It is also the hazard `just/build.just`'s own header warns about in capitals: one build
at a time per tree, because "a milestone ledger running while a developer built by hand" has cost
this project an hour twice.

**11 · One claim in a delivered README does not survive being looked at.**
`samples/05b-editor-window/README.md` reports "Notification toasts overlap each other ... each toast
is drawn partly over the one below it and the text of both is unreadable". `notify.rs` lays them out
in a vertical `Ui` with `add_space` between, and the artefact's own shots on this tree
(`shots/02-authored.png`, `shots/06-palette.png`) show them stacked and individually legible. What
they do is cover the **Console panel** they are anchored over, and their right-aligned frames are of
uneven width. That is a smaller thing than the README claims, and the README should be corrected
rather than left to be rediscovered as a phantom.

### `editor-ui-ux` at Working

The row M5.5 exists to repair, and it is earned: `samples/05b-editor-window` opens the window on a
project and drives it through **synthesised X11 input** — real key presses and pointer drags through
XTEST, delivered to the server rather than to the application — and observes the result in the
journal on disk, in the count of bands the outliner draws, and in the viewport's pixels. Docking,
tabbing, floating, named workspaces that persist and reset, the palette over M5's registry,
keyboard-first operation with chords, density modes, the generated inspector, the problems surface
and non-interrupting notifications are all present and all reachable from a keyboard.

Thinner than Working suggests: the inspector says *"The selection carries no described components"*
in every screenshot, because there is no world loader and nothing describes a type (finding 2's
cause); there is no per-row visibility toggle, because `NodeState` has no visibility field and a
toggle that did nothing would be worse than none; content thumbnails are flat tiles rather than
renders, which `editor-visual-language` requires; and there is **no complex-script text shaping** —
egui has no HarfBuzz, so right-to-left and Indic scripts are unhandled. That last one is a ceiling of
the toolkit choice rather than an omission, is named in `implement-m5b-operable/design.md` §1 as a
cost accepted with the decision, and cannot be lifted from inside this workspace. Accessibility is
present as a dependency — `eframe` is built with the `accesskit` feature, which was one of the two
properties that decided the toolkit — and **nothing tests that the tree it exposes is useful**, which
is honestly unverified rather than claimed.

### `editor-visual-language` at Working

Charcoal chrome around a viewport that carries the screen's colour; the semantic palette; X red,
Y green, Z blue in the orientation widget and in the axis language; selection as a thin outline that
does not glow; gizmos separated by **shape**, held by nine tests that compare the two gizmos' forms
directly; the orientation widget carrying three axes and no ring, box or arrow, with a test that
refuses a proposal to add one *by name*; chrome as overlay in the viewport; surfaces by luminance
step; tabular figures; and the engine's own vocabulary, enforced by a gate that rejects another
engine's product words in any registered command's prose. The identity is derived from the normative
sheet by a committed script rather than redrawn.

Thinner: the performance overlay sits bottom-left where `docs/design/editor-visual-language.md`'s
region table says top-left — a disagreement with the design document, not with the specification,
which asks only for a corner that does not obstruct content. Asset thumbnails are not renders —
the browser shows a typed placeholder and says `1 previews requested`, which is the degradation
the requirement asks for, but the render behind it needs the engine (finding 1). The
toolbar is text where the references use icons, and carries transform modes and undo/redo/save rather
than the six groups the composition requirement lists — thinner, not wrong: exactly one full-width
toolbar sits between the header and the viewport, which is what "Default workspace composition"
prescribes and what "no **second** full-width toolbar" permits. Selection has no viewport outline,
because outlines are the renderer's (finding 1). And task 4.4's consequence is outstanding: the two
reference images are not yet replaced and
`docs/design/editor-visual-language.md` does not yet point at
`editor-window-m5b.png` — the artefact's README records the verdicts and names the edit, and the
specification requires a reference that no longer reflects the product to be replaced rather than
left to decay.

### `editor-agent-interface` at Working

**This is the one tier claim in M5.5 that a reasonable reviewer could dispute, and the case both ways
is recorded here rather than settled quietly.**

For it: the interface is a real MCP server on the editor's standard input, exercised by a
dependency-free client that the editor has never seen. Its 53 tools were compared against the same
binary's own `--list-commands` in the same run and are **exactly** the human registry — no
agent-only tool, and a tool that is not a command is refused with a remedy. A source write lands on
disk, is a transaction, appears in the history as `[agent] by <actor> (session …) — <intent>`, and
`edit.undo` deletes the file while `edit.redo` restores it byte for byte. The narrow scope is the
default and its refusals teach: *"the scope \"read\" does not grant the effect class
reversible-mutation … What would help: grant reversible-mutation to this connection deliberately."* A
path-traversal write outside the project is refused and writes nothing. The transport is optional at
build time and proven absent — 13 build artefacts with the feature, 0 without — and on by default.

Against it: **the milestone's own headline loop closes on three of its six steps.** `compose` and
`write` and `play` work; `build`, `reload` and **`look`** do not. No `--agent-scope` grants the
`external-effect` class, so no agent connection can start a build and therefore none can reload; and
the code that claims frames from the viewport transport is `ViewportLink`, which lives in
`cy-editor-shell` with the window, while `--mcp` returns before a window is created — so an agent can
never observe a viewport **in any configuration the shipped binary offers**, not merely in the
absence of an engine. `design.md` §3 is explicit that observation is what makes the loop authoring
rather than data entry.

Four of the specification's eighteen requirements are therefore not satisfied through the entry point
an agent uses: *"The agent sees what the human sees"* and *"Picking and spatial queries are
engine-side"* have no frame and no runtime to answer them; *"Manipulation uses the manipulation
path"* is implemented and tested — `an_agents_move_is_a_humans_move` passes — but only against a
document a test builds, since `scene.translate` finds no `Transform` in one the editor opens; and
*"Agent activity is on the trace"* has **no implementation at all** — `grep -rn trace
editor/crates/cy-editor-agent/src/` returns nothing, and the editor is a Rust process reaching the
engine over the C ABI while the trace is a C++ subsystem, so nothing an agent does is on it and
"which agent made the editor slow" cannot be answered.

The tier is recorded as Working because the other fourteen are satisfied through that entry point,
and because Seed — "interfaces and invariants exist; dependents can be built against it" — would
understate a running server that edits a real project, is refused correctly, and is checked against
the registry it projects. It is recorded **with these two paragraphs** because the four that are
missing are the interesting ones, and because three of the four are asserted as named GAPs by
`samples/05b-agent-authoring` on every run, so the day any of them closes the artefact says so. The
fourth — the trace — is asserted by nothing, and should be.

### `editor-viewport-and-gizmos`, unchanged at Working and exercised for the first time

Its row still names M5, because M5.5 changed no tier — but most of what it now has was built here:
the cross-process transport, the gizmo geometry and its three states, snapping, the four pivot modes,
per-axis numeric entry and the orientation widget. What runs is strong: three process-boundary tests
in the shipping profile, zero torn and zero newer-than-announced frames, a Vulkan-validation-clean
import layout, and a bounded host wait that was staged against an unreachable timeline value
on every frame of a three-second loop: 1,448 frames ran, no wait was ever satisfied, the **worst**
wait was 2.135 ms against a 2 ms budget, ordinary submissions on the same queue kept completing, and
the device tore down cleanly — which is what "still closeable" means, because `vkDeviceWaitIdle` is
the call a wedged queue never returns from. What is not exercised is a drag against a real
world, and no gizmo has ever been drawn — findings 1, 2 and 4.


## Where M5's tiers are thin

The thirteen tiers M5 advanced are in the record above, and for four capabilities the record and the
plan disagree — that is stated there rather than here. What follows is where the implementation is
**thinner than the tier it did reach**. Every entry was measured or reproduced at M5's gate on this
tree: Linux 6.8, GCC 13.3.0 and Clang 18.1.3, clang-format and clang-tidy pinned at 22.1.8, Swift
6.3.3 through `swiftly`, Cargo 1.92.0, and an NVIDIA RTX 5060 with Vulkan 1.4.312. The machine was
not quiet — an unrelated benchmark held about five cores for part of the run — and that is recorded
because two of the entries below are about load-sensitive tests.

### What the gate found

- **A lost runtime could be dropped in silence, and the fourth profile is what caught it.** `just
  roadmap-milestone m5`'s `editor-profiles` criterion — the one that exists because two Cargo
  profiles compile `debug_assertions` out — failed in the `debug` profile after `editor` had passed
  in `dev`. The failing case is
  `cy_editor_services::runtime::tests::a_runtime_that_dies_becomes_a_notification_and_the_editor_returns_to_no_runtime`,
  which is the milestone's headline mechanism written as a test. Measured failure rate on this
  machine: 1 in 10 in the `dev` Cargo profile, 1 in 10 in `profiling`, 0 in 10 in the other two.

  **The defect, not the flake.** `Session::lose` marks the state `Lost` and *then* sends
  `SessionEvent::Lost`. `RuntimeSession::pump` polled the event queue, then dropped the session on
  `!state().is_connected()`. A pump landing between the two calls polls an empty queue, sees a state
  that is no longer connected, and drops the session — so the event is delivered to a receiver
  nothing will ever read again, and every later pump returns early because there is no session. The
  editor falls back to `NoRuntime` with no notification, which is the half of
  `editor-rust-application`'s requirement that is not "survive": *"the editor SHALL survive, surface
  the crash artefact ... and offer to restart the runtime"*. The offer was what went missing.

  **And a second, deterministic path to the same silence, which the race led to.**
  `Session::spawn_writer` calls `mark_lost` and sends **no event at all**, reasoning in a comment
  that "the reader thread will notice too and is the one that reports". It does not always: a peer
  that stops reading while its own write end stays open fails the write and never closes the stream,
  so the reader blocks forever and the state is the only record. On that path the notification was
  lost on every run, not one in ten.

  **Fixed at the pump**, which is where both paths meet: the state is now the authority and the event
  is the fast path, and `announced` keeps it to one notification when both arrive. The regression
  test is the writer-side case —
  `a_write_side_failure_is_surfaced_even_though_the_reader_never_posts_an_event` — because it fails
  deterministically rather than one run in ten: 3 of 3 against the unfixed `pump`, 0 of 30 against
  the fixed one, and the racy sibling is 0 of 30 in both `dev` and `profiling` afterwards.

### The one that shapes everything else

- **The editor has never spoken to this engine.** Every claim M5 makes about being a client is
  demonstrated against `cy-runtime-stub`, a Rust binary in `cy-editor-testhost` whose own header
  says: "What it is not: an engine. It holds no world, and it echoes what it is asked to apply. The
  real hosted runtime is a C++ binary over `cy::abi::Host`, and it arrives with `live-editing` at
  task 5.2." It did not arrive. Nothing under `src/` or `tools/` binds a Unix domain socket, so
  there is no engine binary a bridge can connect to. The same is true of the SDK's embedded mode:
  `cy-editor-sdk` reaches a `CyInterface` table by `dlopen`, and the only library it is ever pointed
  at in a test is `libcy_editor_testhost.so`, a Rust reimplementation of the ABI.

  **What that does and does not undermine.** It does not undermine the boundary: the editor binary
  links `libc`, `libgcc_s` and nothing else, carries zero `cy::` symbols, and reaches the ABI
  through `dlsym` on a string — which is what rule 6 asks for, verified rather than asserted. It
  does not undermine the crash claim either: killing a real process and surviving it is a property
  of the editor, and the process being a stub makes the test *harder* rather than easier, because a
  stub cannot lose state the editor could then blame it for. What it undermines is every claim of
  the form "the editor and the engine agree" — the ABI's two ends have never been in the same
  session, and the first time they are will be the first time anything checks that the Rust SDK's
  reading of `CyVar`, of a chunk, or of a component schema matches what `src/abi/src/interface.cpp`
  actually writes. The layout tests on each side compare against the same header, which is a
  narrower claim than it looks.

### `editor-documents-and-transactions` at Working

- **The transaction invariant covers a document's content and not its schema or its asset list.**
  `DocumentContent`'s only mutator takes a `WriteToken` whose constructor is `pub(crate)`, and a
  probe outside the crate cannot get one: forging it by struct literal is `E0451: field
  `transaction` of struct `WriteToken` is private`, calling `WriteToken::issue` is `E0624:
  associated function `issue` is private`, and `Document::record` with no transaction open returns
  "no transaction is open, and there is no other write path". That is as strong as the tier claims.
  `Document::schema_mut()` is not. Against a saved, clean document, this sequence —

  ```rust
  document.schema_mut().rename_field(transform, position, "translation")?;
  document.schema_mut().set_writable(transform, position, false)?;
  document.add_asset("worlds/probe.layer2.cyworld");
  ```

  leaves `is_dirty() == false`, `history().entries().len()` unchanged, nothing in the journal, and
  `Audit::of(&document).is_clean() == true`. A field rename and a read-only flag are persistent
  document state that a user would expect to undo; the asset list decides which files a world is
  stored across. None of the six mechanisms the invariant is supposed to buy — undo, autosave,
  recovery, diff, merge, live editing — sees any of it, and nothing points at it. The audit counts
  only content writes, so it cannot: `Audit::of_content` reads `DocumentContent::write_counts()`.
  The fix is small (route schema edits through an operation) and the cost of not doing it is exactly
  what the capability's own text warns about.

### `editor-rust-application` at Working, and `editor-ui-ux` at Seed

- **The command surface has six commands, and not one of them is an action the milestone is about.**
  `cy_editor_services::builtin::register` registers `scene.create-entity`, `scene.delete-entity`,
  `edit.select`, `edit.undo`, `edit.redo` and `file.save`. There is no transform command, no play
  command and no import command, so "commands are the single action surface" is satisfied by a
  surface that cannot drive the milestone's own closing artefact. The machinery for all three exists
  and is tested — `cy_editor_viewport::gizmo` produces exactly one transaction per drag,
  `cy_editor_viewport::play` models the play states, `tools/import/` cooks glTF — and nothing
  reaches any of it through a command. The artefact says so on every run, which is the right
  behaviour, but a green artefact and a green ledger both report a session that did not manipulate
  anything.

- **Two of the three hosting modes are reachable from the binary, and the third is an enum variant.**
  `cyberdyne-editor` parses `--host <socket>` and nothing else: `HostingMode::Embedded` — the engine
  in the editor's own process through a loaded library — is defined in the SDK, is documented, is the
  one mode for which `shares_the_editors_fate()` is true, and no code path in `cy-editor-app` or
  `cy-editor-services` can enter it. The SDK's `dlopen`/`dlsym` half is exercised only by tests, and
  the shipped binary does not even link `dlopen`: `nm -D --undefined-only` on it lists `dlsym` and no
  `dlopen`, because nothing outside the test targets calls `RuntimeLibrary::open`.

### `editor-viewport-and-gizmos` at Working

- **Picking is the strongest thing in the milestone and it has no consumer.** `pick_ray`, `pick_rect`
  and `pick_polygon` resolve against the `Span<const DrawItem>` that `collect_draws` produced, so
  the candidate set is by construction a subset of what was drawn — a frustum-culled, layer-excluded
  or invisible instance cannot be picked because it is not in the list, and an instance published by
  a producer the picker has never heard of is pickable with no code knowing what produced it. Twelve
  cases in `unit.render_server` hold that, including "what the renderer did not draw cannot be
  picked, whatever the reason". Nothing in the editor calls it: the editor's own `picking.rs` builds
  rays that are checked to agree with the engine's, and the engine-side resolve is reached by no
  editor code path, because there is no runtime to reach it through.
- **A hit is a bounding-volume hit, not a triangle hit**, and `picking.h` says so: a `GpuInstance`
  carries a world-space sphere and no local box, so overlapping volumes produce several candidates
  and cycling is the answer. The exact answer is a GPU identifier buffer written by the pass that
  shaded the pixel, and it waits on M6.

### `asset-import-pipeline` at Working

- **None of the libraries the roadmap named was integrated, and the tier is over engine-owned code.**
  The M5 row asks for "texture and model import via glTF and meshoptimizer"; `deps/manifest.toml`
  carries neither, nor cgltf, nor a BC7 or ASTC encoder, and records the decision in the file that
  would carry them. What that costs, in its own words: "The simplifier is less tuned than
  meshoptimizer's at aggressive ratios; the texture importer reads Targa and not PNG; the text
  server lays out Latin and refuses Arabic." Every gap is reported by the code that cannot close it
  rather than approximated, which is what makes it safe to carry — but a project cannot import a PNG
  and a cooked texture is uncompressed pixels.

### `live-editing` at Seed

- **`cy::abi::LiveReloadSession` has one test and no consumer.** `grep -rl live_reload.h` over
  `src tools bindings samples tests` returns exactly its own header, its own source and its own test.
  The engine's runtime does not use it, the sample does not use it, and the editor cannot: it is C++
  behind the ABI rather than in front of it.
- **"A Swift module reloads while the runtime is live" is two tests that do not meet.** The live half
  — a loop that never stops, a swap at a nameable tick, state crossing it, and a deferral when the
  world is iterating — is `integration.abi_live_reload`, over **C** modules; `test_live_reload.cpp`
  says why, and it is a real reason (the watcher looks for `<stem>_g<N>.so` and a *copy* is a
  legitimate new image for C and not for Swift). The Swift half —
  `integration.swift_reload`, over three images built with distinct `-module-name`s, ARC-managed
  state carried by name, and a schema downgrade refused — calls `Runtime::reload()` directly between
  `fixed_update` calls and never goes near the watcher. Both halves are excellent and the
  intersection is untested.

### `core-type-system`, `scene-graph-and-nodes` and `native-abi` at Complete

- **`identity/manifest.toml` holds nine live types and no tombstones.** M2's record called
  `core-type-system` "still a two-type demonstration"; M5 took it to nine by reflecting the scene's
  components, which is what the state hash needed and what the inspector reads. Nine is the whole
  engine's opted-in surface. Reflection is opt-in by design, so this is not a defect of the
  capability — but "Complete" for a registry whose corpus is nine types is a claim about the
  mechanism, not about coverage, and the first large opt-in wave is where a stable-identity rule
  gets tested for real.
- **The orphan invariant is still not in the coherence checker**, unchanged since M2:
  `check_coherence()` covers the specification's five invariants and the orphan claim is carried by
  two scene cases. The claim holds; `gates.toml`'s `scene-coherence` description still overstates
  where it is checked.
- **The two ends of the ABI have never met**, as above. The gate compares the header with a committed
  baseline and both overlays are generated from that header, which is a strong drift check and not a
  behavioural one.

### The ledger, which this milestone rewrote

- **The flattening is real and the "exactly once" is about declarations, not work.** One run of
  `just roadmap-milestone m5` merges 137 declarations from six ledgers into 101 distinct criteria,
  36 deduplicated, and `four-profiles` — declared by M1, M2, M3, M4 and M5 — is evaluated once, as
  `m1:four-profiles`. Under chaining the same run would have evaluated it five times. What the
  fingerprint cannot see is a criterion whose command *contains* another's: `just build-editor-check`
  runs five times in one m5 run (once as `m5:editor`, four times inside `m5:editor-profiles`),
  `just generate-check` twice (`m0:generated-code`, and again inside `m4:generated-code`'s compound
  command), and `just build-engine` and `just test-all` each run in `dev` twice — once standalone and
  once inside `four-profiles`. That is a much smaller residue than chaining left, and the file's
  "each distinct check run exactly once" should be read as "each distinct *declaration*".
- **A criterion's `ci_job` is checked to name a gate, not to be run by it.** `roadmap.py`'s
  `_check_criteria_are_gated` verifies the id exists in `gates.toml`; `check_workflows.py`'s
  `gate_coverage` verifies each permanent gate's `runs` list appears in a workflow. Nothing joins
  the two, and the join is where a criterion goes quiet. **Concretely: the sanitizer criteria of
  M3, M4 and M5 are run by no job on a pull request.** The `sanitizers` gate lists four commands —
  thread and address+undefined over `jobs`, thread over `ecs_scheduling`, address+undefined over
  `ecs` — which covers M1's two criteria and M2's `tsan-world` exactly, and M2's `asan-world`
  partially (it names five suites; the gate runs one of them). `m3:sanitizers-render`,
  `m4:sanitizers` and `m5:sanitizers` name that same gate and run eleven suites between them that
  no command in it mentions. The only job that reaches them is `sanitize-nightly`, which runs
  `--tests .`, is triggered by `schedule` and `workflow_dispatch` and not by `pull_request`, and
  appears in no gate's `runs` list — so `just ci-check` would not notice if it were deleted.
- **The ladder still holds, and it holds by a different mechanism than before.** Chaining enforced it
  by construction; the flat plan enforces it by `gates.permanent_milestones` returning every
  milestone gate at `state = "green"`. A milestone gate left at `joins-on-close` after its milestone
  closed would now silently remove that rung from every later ledger. `selftest.py` checks the
  containment property statically and checks that no ledger with a green gate lacks a TOML; there is
  no check that a *closed* milestone's gate was flipped. M2's record already names that exact
  omission happening once.

  **The ladder was tested rather than argued.** Two independent M0 regressions were introduced and
  `just roadmap-milestone m5` was run in full against them: `just quality-layers` made to exit 1
  through a `PATH` shim, and a scratch copy of the record with `delivery-roadmap` knocked from
  `working` back to `seed`, passed as `--record` — the repository's own record was never edited.
  Both failed the M5 run, reported under their own labels and once each:

  ```
  ==> m0:layering              just quality-layers
      FAILED           exit 1  (0.0 s)
      declared by M0, M1, M2, M3, M4, M5 — one failure, not one per milestone
  ==> m0:roadmap-tiers         the status record is at this milestone's exit tiers
      FAILED           1 capability tier(s) below this milestone's exit  (0.0 s)
        | delivery-roadmap: recorded 'seed', below this milestone's exit of 'working'
  M5 is not closed: 3 of 99 evaluated criteria failed.
  ```

  The third was the `physics_jolt` flake above. Both probes were removed and `just quality-layers`
  is green again. **The flattening did not weaken enforcement**; what it changed is where the
  enforcement lives, which is the paragraph before this one.

### Carried forward, and where each is written down

- **`CY_BUILD_EDITOR` is `OFF` and its description names something it does not gate.** It reads
  "Build the editor application (the Rust workspace, from M5)"; the Rust workspace is built by
  `just build-editor`, which never consults it, and `just build-all` calls that unconditionally.
  What the option actually gates is editor-layer C++ modules, of which there are none. So a
  configured tree writes `CY_BUILD_EDITOR 0` into `cy_features.h` in a build that has just built the
  editor — the same inverse lie `cmake/features.cmake` documents against `CY_SCRIPTING` and fixed
  there. Nothing is excluded by it, so this is a truth defect rather than a coverage one.
- **`CY_PROFILING` is delivered and off**, which `cmake/features.cmake` records at the site as the
  one finding M5's option audit could not fix from where it sat. It is the last delivered feature in
  the table that is off by default.
- **Load-sensitive assertions are now the single most common cause of a red ledger run on this
  machine, and there are at least six of them.** Three were reported by earlier workstreams —
  `integration.physics_jolt` (SIGTRAP), `integration.reflect_scaling` and `unit.physics_server` —
  and this gate added three more, all in one run, while an unrelated workload held about six cores
  and the load average sat above 30:

  | criterion | what failed | in isolation |
  |---|---|---|
  | `m1:asan-jobs` | `integration.jobs_schedule`, "independent systems run concurrently on different workers": `peak_concurrent` was 1, expected 2 | 3 of 3 passed |
  | `m1:four-profiles` | `smoke.editor_session` and one integration case, **Debug** tree | `just test-all --profile debug`: 34 / 70 / 10 / 5, all green |
  | `m1:four-profiles` | `integration.physics_jolt`, **Debug** tree, a later run | 5 of 5 passed |
  | `m5:editor-profiles` | `cy_editor_services::runtime::tests::a_runtime_that_dies…` | a **real defect**, fixed above — the one time this shape was not a flake |

  **`m1:four-profiles` failed in two of the four full ledger runs this gate made, on a different test
  each time, and neither reproduced.** That is a permanent merge gate — declared by M1, M2, M3, M4
  and M5, and now evaluated once rather than five times, which is the only reason the number is two
  and not ten.

  The distinction the last row draws matters, and it is why it is in the table: on a busy machine
  every one of these looks identical, and the only thing that separated them was rerunning each in
  isolation rather than assuming. `peak_concurrent >= 2` is not a budget — it is a scheduling
  assertion that an oversubscribed machine defeats with no defect present. A gate whose failures are
  usually noise is a gate that trains people to rerun rather than to look, which is exactly how the
  defect above could have been missed.
- **`samples/05-editor-session/README.md` says "Nothing here is a mock"** two lines before naming the
  runtime, and the runtime is the one thing there that is. `session.py`'s own docstring is accurate;
  the README is not.

## Where M4's tiers are thin

The eight tiers M4 advanced are the plan, and the record agrees with it. What the plan does not say
is where the implementation is **thinner than the tier claims**. Every entry below was measured or
reproduced at M4's gate on this tree: Linux 6.8, GCC 13.3.0 and Clang 18.1.3, clang-format and
clang-tidy pinned at 22.1.8, Swift 6.3.3 through `swiftly`, and an NVIDIA RTX 5060.

Eight entries are defects this gate found by attacking what the milestone exists to establish rather
than by reading it, and each is fixed in this change rather than recorded and left; a ninth looks
like one and is not, and is here so the next reader does not have to re-derive it. **Three of the
eight are permanent merge gates that were red while every milestone ledger was green.** Two of the
three broke the same way — this milestone flipped a default correctly, and a check that had been
reading that default rather than stating its own inputs went red in continuous integration — and the
third is a case M3's own record predicted would fail and named. That is the shape to carry into M5,
which flips more defaults than M4 did.

### What the gate found

- **The layering check did not police Jolt or miniaudio, and the ledger said it did.** `m4.toml`'s
  `layering` criterion reads "no SDL, Vulkan, Slang, Jolt or miniaudio type appears above the
  backend that owns it — **each proven by introducing one and requiring the check to reject it**".
  `tools/layercheck/layercheck.py` had rules for the first three and nothing for the last two: its
  `gpuapi` set is Vulkan, SPIR-V, glslang and Slang, and neither `Jolt/` nor `miniaudio.h` appeared
  anywhere in the file. The rule was still true in the tree, but for a different reason —
  `cy::dep::jolt` and `cy::dep::miniaudio` are `PRIVATE` dependencies, so neither library's include
  directory is inherited and neither include resolves outside its backend. That is a real
  structural guarantee and it is not the one the criterion describes; a `PUBLIC` dependency, or an
  include directory added by hand, turns it off with nothing to say so. Fixed here: a `thirdparty`
  check with a per-library root, the fixture
  `tools/layercheck/fixtures/thirdparty-above-backends/`, two control files in `legal/` so a rule
  that fired everywhere would fail, and a selftest case. The check now reports `clean — 1002 files,
  barriers, gpuapi, includes, sdl, targets, thirdparty`, the selftest is 10 cases rather than 9,
  and introducing `#include <Jolt/Physics/Body/Body.h>` into `src/servers/physics/src/body.cpp` and
  `#include "miniaudio.h"` into `src/servers/audio/src/server.cpp` produces two findings naming the
  interface each library sits beneath.

- **M4 broke the `sanitizers` gate in continuous integration three separate ways, and its own
  ledger carried a workaround for one of them.** `just test-sanitize` builds the `dev` profile;
  M4's task 1.4 turned `CY_SHADER_SLANG` on in that profile; so from that commit the sanitized
  configuration compiled Slang — 1 000 targets of third-party C++ including code generators the
  build itself runs — with whichever sanitizer was asked for. Each mode fails differently, and all
  three were reproduced from a clean tree at this gate:

      address       [985/2027] Generating fiddle/.fiddle.stamp
                    ==177600==ERROR: LeakSanitizer: detected memory leaks
                      #1 Slang::StringRepresentation::createWithCapacityAndLength  slang-string.h:317
                      #1 fiddle::Parser::parseFiddleNode()                         slang-fiddle-scrape.cpp:806
                    SUMMARY: AddressSanitizer: 818214 byte(s) leaked in 3617 allocation(s).
                    ninja: build stopped: subcommand failed.

      thread        [1/1068] Generating slang-cpp-prelude.h.cpp
                    FATAL: ThreadSanitizer: unexpected memory mapping 0x5c8442139000-0x5c844213e000
                    ninja: build stopped: subcommand failed.

      undefined     smoke.shader_slang ***Failed
                    _deps/slang-src/include/slang-com-ptr.h:193:27: runtime error: member call on
                    address 0x… which does not point to an object of type 'ISlangUnknown';
                    object has invalid vptr

  `sanitizers` has been a permanent merge gate since M1, and `.github/workflows/ci.yml`'s `sanitize`
  job runs `just test-sanitize --sanitizer thread --tests jobs` and
  `--sanitizer address,undefined --tests jobs` with no override — so the first two steps of that job
  have been failing at the **build** step on every pull request since the flip, before a single
  suite is reached, on code nothing here tests. Three agents observed the LeakSanitizer half
  independently and each recorded it as someone else's item; `m4.toml`'s `sanitizers` criterion then
  pre-configured its own tree with `-D CY_SHADER_SLANG=OFF` and passed. **That is the part worth
  carrying forward: a criterion that runs a different configuration from the gate it stands for
  cannot speak for it, and this one was green while its gate was red — and it was green over the one
  mode that had a workaround, which is why the other two survived to this gate.**

  Fixed at the root, in `just/test.just`, which now configures the sanitized tree with
  `-D CY_SHADER_SLANG=OFF`. **What that costs is stated rather than discovered**: `smoke.shader_slang`
  is not sanitized, here or in the nightly "every suite under every sanitizer". It cannot be — with
  the vptr check quietened the same suite fails again on Slang failing to `dlopen` its own downstream
  plugins in that tree (`failed to load dynamic library 'slang-glslang-2026.9.2'`, reported as an
  error while still emitting valid SPIR-V) — and `shader-system` calls the front end a tool-time
  dependency whose shipping path is the SPIR-V passthrough, so what a sanitized run is for is
  unaffected. Two smaller wrappers land with it and stay although Slang's absence makes both inert
  today, because each is one line, each is backed by a reproduction above, and both are properties of
  *any* dependency that runs a binary during its own build: the build runs with `detect_leaks=0`
  (detection is turned back on before `ctest`, so every suite is still leak-checked), and the build
  runs under `setarch -R` when the sanitizer is `thread` — the same fix the recipe already applied to
  the test run.

  Verified after the fix, in `build/gate-m4/`, over the ABI suites: `--sanitizer address`,
  `--sanitizer undefined`, `--sanitizer address,undefined` and `--sanitizer thread` each build and
  pass 4/4. The criterion now runs the same commands CI does, with nothing in front of them.

- **M4 also broke the `generated-code` gate, and it had been red for the whole milestone.**
  `just generate-test` runs `tools/gen/tests/run_tests.py`, twenty-one CMake fixtures that hold the
  feature and module machinery to its rules. One of them,
  `test_optional_backend_requires_its_subsystem`, configured with `-DCY_AUDIO_STEAM_AUDIO=ON` and
  required the configure to **fail**, because that option requires `CY_AUDIO` — and it was relying on
  `CY_AUDIO` defaulting off to make that true. M4 turned `CY_AUDIO` on, correctly, because it
  delivered the miniaudio backend. Measured here: **20/21 passed**, with the diagnostic `configure
  was expected to fail but succeeded` over a feature line that now reads `CY_AUDIO
  CY_AUDIO_STEAM_AUDIO`. `generated-code` is a permanent gate and `.github/workflows/ci.yml`'s
  `generated` job runs that command, so it has been failing on every pull request since the flip.
  **No milestone ledger runs it**: M0's `generated-code` criterion runs `just generate-check`, one of
  the three commands its own gate declares. Fixed twice over — the fixture now passes `-DCY_AUDIO=OFF`
  so that it asserts the rule rather than the default, because a test of a validation rule must state
  every input the rule reads; and `m4.toml` gains a `generated-code` criterion that runs all three of
  the gate's commands verbatim, so the next default that moves is caught by the recipe that closes the
  milestone rather than by the pull request after it.

- **The `profiles` gate was red in the Debug profile, on the very case M3's record named as the
  canary.**
  `integration.scene_scale`'s "many instances of a batched behaviour cost one system, not one call
  each" builds 500 nodes and, at `-O0`, spends more than its 1 000 ms integration budget doing it:

      src/scene/tests/test_behaviour.cpp:113: ERROR: over budget: … spent 1363.940 ms of CPU
      against a budget of 1000.000 ms … The clock is the case's own CPU time, so this is not a
      busy machine: it is work the test did.

  Measured three times alone at 1 021, 1 114 and 1 563 ms. M3's section below records it at **868 ms
  of its 1 000 ms budget** and calls it "the closest thing to a canary", beside the rule it was
  written to illustrate: *a case within 20% of its budget is still a case that will fail eventually.*
  It did. Because `four-profiles` is an exit criterion of M1, M2, M3 **and** M4, and each of those
  ledgers nests the ones below it, one over-budget case failed four ledgers at once — which is the
  multiplication `testing-and-quality`'s M4 delta describes, arriving in the milestone that wrote it
  down. Fixed the way the harness's own message and M3's own precedent say: the case is cheaper, at
  128 instances rather than 500, and every assertion it makes is unchanged because none of them was
  ever about the count — `0 < g_batch_calls < kInstances` is the property, and it fails just as hard
  at 128 if the dispatch degenerates to one call per instance. 33 of 33 cases pass in Debug
  afterwards, with the case at roughly a quarter of its budget.

  **And then the sweep, because one canary is not a survey.** `CY_TEST_BUDGET_SCALE=0.8` over every
  unit and integration binary — the setting that fails anything above 80% of its budget, which is
  M3's own "within 20%" rule expressed as a command — reports **0 of 96 over** in the Development
  profile and **0 of 96** in Debug when the suites are run alone. Two cases are worth naming rather
  than leaving for the next gate to find: `unit.physics_server`'s "one thousand identical box
  colliders create one shape" sits at **0.544 ms of its 1 ms** budget at `-O0` (54%, flagged by the
  0.4 sweep and by nothing tighter), and `integration.jobs_diagnostics` passes alone and tips over
  80% under `ctest -j4`, which makes it the one case left whose margin is contention rather than
  work.

  **A corollary, learned the expensive way at this gate: the milestone ledgers are a serial gate, and
  `CY_JOBS` is not only a build knob.** Running `just roadmap-milestone` for several milestones at
  once — each in its own `CY_BUILD_DIR`, which is what keeps their build trees from corrupting each
  other — does not keep their *tests* from competing; four concurrent ledgers on twenty-four cores
  produced `four-profiles` failures whose suites passed when re-run alone, plus one clang-tidy run
  killed by the OOM reaper. And within a single ledger, `CY_JOBS` is passed to `ninja` **and** to
  `ctest --parallel`, so raising it to shorten the build also raises the contention every per-case
  budget is measured under: at `CY_JOBS=14` a `four-profiles` unit suite failed once and passed on
  every re-run, at 5 and 6 none did. Nothing in either case is a defect in the tree, and both look
  exactly like one in a log. One ledger at a time, and a modest `CY_JOBS`; the nesting means the
  deepest ledger runs the whole ladder anyway.

  **The residual is real and it is `m2`'s, recorded there since M2 and now with a second
  measurement.** `m2.toml`'s own note says `four-profiles` is "flaky at roughly three per cent in the
  Debug configuration", on a case that pays the process cold start. At M4's gate `just
  roadmap-milestone m2` failed on `four-profiles` in two of three serial standalone runs and passed
  on the third with 26 criteria — and in each failure every suite named passed on re-run, twice at
  the same `ctest` parallelism on an idle machine. Three per cent per Debug run is optimistic once
  four profiles and four nested ledgers multiply it; the same recipe run inside `m3`'s and `m4`'s
  ledgers passed on the first attempt both times. It is not a defect in M4 and it is not fixed here,
  but "roughly three per cent" is the number to stop quoting.

- **`four-profiles` is green in continuous integration without ever building a Swift module.** The
  criterion says "all four profiles build clean and `just test-all` is green in each — **including
  the Swift suites and the artefact's smoke test, which build the game module at `swiftc -O` in
  Profile and Shipping**". That is true on this machine and was false in CI: the `profiles` job
  installed no Swift toolchain, and `bindings/swift/` and `samples/04-character/` do not register
  their suites without one, so three of the four profiles were judged with the milestone's artefact
  absent from the tree. `.github/workflows/ci.yml` says in as many words that the toolchain "is
  installed in this job and nowhere else"; the job it names is `playable`, which runs the `dev`
  profile only. Fixed here by installing Swift in the `profiles` job too. The `build` and `test`
  matrix jobs still have none, on any of their six legs, and that is deliberate — two of the six
  are ARM runners for which no Swift setup action exists — so `three-platforms` remains what it has
  always been: not evaluated.

  **The same absence had a second consequence, in the gate least likely to be checked for it.** `just
  quality-lint` runs clang-tidy against `compile_commands.json`, so a target that is never declared
  is a target that is never linted: in the `quality` job, `samples/04-character/host/`'s three
  translation units and `bindings/swift/tests/test_swift_reload.cpp` — 1 990 lines, including the
  sample host that M4's whole "no C++ gameplay" claim is about — were formatted by the format gate
  (a tree walk, so it sees them) and linted by nobody. Swift is installed in that job now too.

- **`CY_SCRIPTING` was `OFF`, gated nothing, and made the engine's own feature table say scripting
  was off in a binary that was running Swift.** M3's lesson was a delivered backend left behind an
  option nobody turned on. M4's is the mirror image and it took a different shape, which is why it
  survived seven agents: nothing is behind `CY_SCRIPTING` — `src/abi/` is compiled unconditionally
  and says why in its `CMakeLists.txt`, and the Swift halves are gated on the *toolchain being
  present* — so the option excluded nothing and the omission cost no coverage. What it cost is
  truth. `cy_features.h` carried `/* CY_SCRIPTING is disabled */` and the runtime table carried
  `X("CY_SCRIPTING", 0)` in the same build that loads `libCyGame_g0.so`, so `#if
  defined(CY_SCRIPTING)` was a lie in the direction `cmake/features.cmake`'s own header warns
  about. Fixed here by flipping the default and by cutting the coupling that made the flip
  dangerous: `just env-doctor` derived Swift's severity from this option's default, so `ON` would
  have turned "no Swift toolchain" into a hard failure — and `env-doctor` is the *first* criterion
  of every milestone ledger, including the four that are closed. The option no longer decides
  whether the build uses Swift, so it no longer decides that either; `just/env.just` says so at the
  site, and names `deps/host-tools.toml` as where a real Swift requirement (and the version pin
  `swift-scripting` asks for) belongs.

- **The ledger's warning about a machine with no Swift is wrong in three of its four cases, in the
  direction that matters less — but it is the kind of wrong that trains a reader to skim.**
  `m4.toml`'s header says such a machine's criteria "do not fail, they are absent: `just test-smoke
  -R character_sample` matches no test and ctest exits zero". It does not: `just/test.just` passes
  `--no-tests=error` for every kind but `unclassified` and `render`, and the measurement is `ctest
  --label-regex "^smoke$" --no-tests=error -R zzz_nonexistent` → **exit 8**. So `sample-artefact`,
  `no-cpp-gameplay` and `swift-reload` all fail loudly on a Swift-less machine. The one that really
  does pass having judged less is `swift-api`, whose regex `swift_` still matches the three suites
  that need no toolchain — the overlay's currency, the generator's selftest, and the engine linking
  no Swift runtime — while `integration.swift_package` and `integration.swift_reload` are simply
  not there. Corrected in place.

- **`platform/desktop-sdl3/README.md` said input arrives at M2 and that the event pump discards
  every non-window event "until then".** M4 landed `sdl3_input_source.{h,cpp}` in that directory
  and `integration.sdl3_input` drives it, and the README was not touched. A stale caveat is worse
  than no caveat, because it is the sentence a reader trusts instead of reading the directory.
  Corrected, including the layout table, which listed neither new file.

- **`ModuleImage::close()` calls `dlclose`, and the reload model says there is none. Both are
  right, and it took reading the caller to know that.** Recorded here because the next reader will
  grep for it too: the one call site is the path where `dlopen` succeeded and the entry symbol is
  absent, so no initialiser of that image ever ran, it registered nothing and interned no type
  metadata; the comment above it says exactly that. Nothing in the reload sequence unloads
  anything, and `grep -rn dlclose src/` finding a hit is not the defect it looks like.

### `native-abi` at Working

- **The gate is stronger than its own description, and that is worth recording because it is the
  one thing here with no way back.** Independently re-run at this gate against the live header: a
  reorder of two entries with **identical C signatures** (`component_get_f32` and
  `component_get_vec3`, both `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId, uint32_t, float*)`)
  is refused with four findings naming the slot and the member — a swap the compiler cannot see and
  that nothing else in the tree would have caught; `--update` **refuses to launder it**, which is
  what stops the escape hatch from being the way through; a removal produces 44 findings; an append
  is refused until `CY_ABI_MINOR` is bumped, then reported as compatible-but-stale, then accepted
  at `1.1.0`. An append also fails `just generate-swift --check` until the overlay is regenerated,
  so the header and the overlay cannot drift apart — though that failure surfaces as an unhandled
  Python traceback rather than as the tool's own diagnostic, which is a small blemish on an
  otherwise exemplary gate.
- **One of the eleven requirements is not started, by design**: `Rust SDK overlay` is M5's, and the
  baseline it will be generated from is the same `abi_baseline.json` the gate diffs.
- **Thirty-one entries is a small table, and the shape of what is missing is systematic.** There is
  no chunk entry, so `CyberdyneKit`'s system model has no source of chunks; no node entry, so
  `@Node(path)` resolves to nil; no `CyStage` or `CySeverity`, so two Swift enums are hand-copied
  from engine enums with nothing to check them against — and the second **was already wrong**, six
  enumerators against the engine's three, which sent every `Log.info` to the engine as an error on
  a green run until M4's own reload suite installed a sink and read the severity the engine
  received.

### `swift-scripting` at Working

- **The tree callbacks are declared and not driven.** `create`, `destroy`, `fixed_update`,
  `serialize` and `deserialize` are real; `onEnterTree`, `onReady`, `onEnable`, `onDisable`,
  `onUpdate` and `onExitTree` are in the model and wait on scene entries in the table.
- **There is no shipping configuration.** `swift-scripting` asks for two — development (dynamic,
  hot-reloadable) and shipping (optionally static, whole-module optimisation, no dynamic load).
  Only the first exists. The `-O` half is exercised, because the Swift configuration follows the
  engine profile, and M4's `profiles` CI job now exercises it there too.
- **The Swift toolchain version is not pinned**, which `swift-scripting` requires "per engine
  release and verified in CI". `deps/host-tools.toml` is where it belongs and holds no `swift`
  entry.
- **A Swift trap in game code is still fatal.** A thrown error is caught, logged with the behaviour
  and callback that produced it, and disables the instance; a trap has no catch on any platform.
- **`swift build` and `swift test` are exercised; Xcode and SourceKit-LSP are not**, and every
  measurement is Linux with Swift 6.3.3. The macOS and Windows loader paths, the module-name rule
  and `@_cdecl` export behaviour are **unverified**.

### `input-and-actions` at Working

- **Input assets are not cooked.** The capability requires actions, contexts, bindings, processors
  and triggers to be authored as assets and cooked into these tables, participating in the derived
  data cache and the identity manifest. M4 builds the tables in code.
- **`ActionStableId` is not the identity manifest's number.** The shape is right — an opaque value
  the declaration carries, never derived from the name — but nothing allocates it from
  `identity/manifest.toml`.
- **Interface routing is the focus-layer half only**; `ui-system` does not exist. **Accessibility**
  is the input layer's settings and not the platform's tree. **Performance is unmeasured**: the
  evaluation path allocates nothing and locks nothing, and no benchmark asserts the eight-user,
  thousand-action figure. **`ActionValueType::Pose` is declared and unfed.**

### `physics` at Working

- **Constraints exist as a vocabulary and in no backend.** `constraints.h`/`.cpp` define and
  validate the types; neither the reference implementation nor Jolt maps one. Both answer
  `Capabilities::constraints == false` and fail creation with a diagnostic naming why, which is the
  capability's own "Unsupported feature" scenario rather than a silent gap — and it is the shape the
  rest of the unimplemented surface takes too: **soft bodies, vehicles, ragdolls, buoyancy and water
  interaction, and heightfield and terrain collision** are declared in the capability model,
  unimplemented, and reported as unsupported. `physics` scopes ragdolls to `animation-and-skinning`,
  which is **M8**, not M6 — corrected at M6's gate (task 10.7); the row was written before M5.5 was
  inserted and before the M5 audit of `cmake/features.cmake` moved `CY_ANIMATION` from M6 to M8 for
  the same reason. The matrix above has `animation-and-skinning` at W in the M8 column.
- **There is no ECS bridge.** `src/physics/` at layer 4 — the module that would register the
  components in a world, create bodies from them, drive `PhysicsStepper` in the `Physics` stage and
  write `cy::scene::LocalTransform` back — does not exist. `samples/04-character` does that work in
  its host, in C++, which is why the sample's 1 123-line `game.cpp` is larger than its 667 lines of
  Swift.
- **Determinism is one platform and is not claimed to be more.** Re-measured here: two processes of
  `cy_sample_character --ticks 900` produce byte-identical reports over the reference backend, and
  two more do over `--jolt`. Cross-platform determinism is explicitly not claimed, and
  `validate_session()` rejects a configuration that assumes it.
- **The backend swap changes more than the ledger's note implies.** `m4.toml` records that 400
  ticks over each backend gave "the same 11 footsteps, 3 landings, 1 jump and 1 climbed step". At
  900 ticks the same run gives 22 footsteps / 11 landings / 9 steps on the reference backend and 30
  / 4 / 1 on Jolt. What is invariant is what the requirement actually asks for — the command log
  and the input frame hash are identical (`log=0e4d14a10f4582a9`, `frames=269545449d8dd9f3`) and no
  gameplay code changes — not the trajectory.

### `camera-system` and `audio` at Seed

- **Camera: absent, not stubbed** — framing and composition constraints, camera volumes, the
  strategy camera, the director camera, aim assistance, and screen/world projection. All are M8's.
  Collision and occlusion are *responses* applied from results the caller supplies.
- **Audio: absent, deliberately** — decoding and streaming, the sixteen effects beyond the four in
  `bus.h`, `AcousticsBackend`, HRTF, propagation, reflections, acoustic geometry, interactive
  music, and the middleware backends. The engine plays float PCM the asset system does not own yet.
- **Neither is stubbed, and both READMEs say why**: a stub that returns a plausible pose, or a
  plausible mix, is worse than an absent function a caller cannot call.

### `gameplay-framework` at Seed

- **Section 4.4 of the task list named six of the capability's thirty-three requirements, and
  those are what exists.** Teams are an integer with no relationship matrix; the session's phase is a `Name` standing in for a hierarchical tag, so `Unit.Robot` does
  not match `Unit.Robot.Harvester`; capabilities are a derived index rather than components; and
  events, spawning, time domains, indexes, features, rules assets and session-state fragments are
  unstarted.
- **The command stream is single-threaded in practice.** The structure is the one the requirement
  asks for — per-producer buffers, no central lock, a deterministic merge — but nothing records
  from several threads, so the claim is architectural rather than measured. The performance
  contracts (100 000 entities, 100 000 commands per second) are unmeasured.
- **The invariant itself holds, and it was re-attacked here rather than read.** Adding
  `cy::servers-input` to `src/gameplay/CMakeLists.txt` stops the build on `test_bypass.cpp:53:
  static assertion failed: src/gameplay/ can see an input header`; adding a `void* input` member to
  `GameplayContext` stops it on line 77. Both messages explain the rule rather than naming it.
  **What no check can cover is the translation step itself** — the host must see actions and
  commands at once, and `samples/04-character/host/game.cpp` is where that lives; it applies only
  what `CommandStream::commit()` returned, and a version of it that wrote the game's input
  component straight from the action state would pass every check in the tree and diverge only on
  replay. That is design.md §3's own argument, and it is why the bridge is one file rather than a
  capability every system has.

### `core-platform-abstraction` at Working

- **Two of seven requirements are unimplemented.** `Clipboard, dialogs, and system integration`
  asks for clipboard text and images, native file and message dialogs, cursors, IME, on-screen
  keyboards, orientation, keep-awake and tray items; `DisplayServer` has the `Feature` enumerators
  and no calls, and `has_feature()` answers `false` for each. `Accessibility hooks` asks for an
  accessibility tree the OS screen reader can read; there is none. The M4 row of the roadmap names
  "system integration" in this capability's scope, and that half did not land.
- **`Supported platforms` is one of the three.** Windows and macOS host code is written against the
  documented APIs, reviewed, and has never been compiled.

### The artefact, and what it does not do

- **It draws nothing, and that is M3's recorded gap rather than a choice made here.**
  `cy::rhi::Device` still exposes no way to obtain the graphics-API instance a window surface must
  be created against, so a host can create a window and a device and cannot join them. The sample
  produces the `cy::render::ViewDescription` a renderer would draw, every tick, and counts it — the
  camera half of "render view production feeding M3's renderer" is exercised and the renderer half
  is not.
- **Nothing reloads while it runs.** `m4.toml` records this and so does the sample's README, and it
  is the half of the roadmap's "a Swift module hot-reloads **while the sample runs**, preserving
  world state" that M4 does not satisfy. What is proved, and was re-run at this gate, is the
  mechanism: `integration.swift_reload` is 5 cases and 78 assertions in which a Swift behaviour's
  `health`, `ammo` and an ARC-managed `String` survive a rebuild with a changed layout, carried by
  name through the module's own serializer, and a module whose schema predates the saved blob is
  refused with the previous generation left live. The sample declares `hot_reload = true`, the
  loader supports it, and nothing calls `reload()`. Read the tier as "the loader reloads", not "the
  artefact hot-reloads" — which is word for word what M3's section below says about shader reload,
  one milestone earlier.
- **No mouse look, and the level is axis-aligned boxes.** `Look` is the arrow keys as a rate; the
  ramp is a flight of shallow steps because `LevelBox` has no orientation.

### Carried forward, and where each is written down

Four of M3's entries are unchanged at M4 and each is annotated in place in the section below rather
than restated here, because a debt copied forward twice is a debt nobody re-checks:

- **The seven servers are still seven nulls, and M4 added four more real servers outside the
  registry.** `InputServer`, `PhysicsServer`, `AudioServer` and `CameraServer` are constructed and
  stepped by hosts; `ServerRegistry::register_backend` is called by nothing outside its own tests and
  `Runtime::tick()` names no server kind. `CameraServer` is not one of the seven `ServerKind` values
  at all.
- **No component in the engine is covered by the identity gate.** Still `2 live types, 0 tombstones`,
  both `cy::demo::`, while M4 added components in five modules and a Swift module that registers its
  own across the ABI.
- **Windows and macOS have never compiled**, and M4 is the largest single addition to the porting
  surface so far: a Swift toolchain, two fetched libraries, `dlopen` and a shared-library loader.
- **No job in continuous integration runs `just roadmap-milestone`** for any milestone. That is now a
  recorded decision rather than an accident, and archiving this change without promoting
  `milestone-m4` out of `joins-on-close` turns `just roadmap-test` red on the next pull request.

## Where M3's tiers were thin, and what M4 closed

The nine tiers M3 advanced are the plan, and the record agrees with it. What the plan does not say
is where the implementation is **thinner than the tier claims**. Every entry below was measured or
reproduced at M3's gate on this tree; where a number appears, it is a number this tree produced, on
an NVIDIA RTX 5060 (driver 580.95.05, device API 1.4.312, loader 1.3.275) with the Khronos
validation layers and synchronisation validation on.

**Re-checked at M4's gate, entry by entry, against the tree as it stands.** Four had gone stale
and are corrected in place — the Slang default, the Vulkan default, "every device suite renders one
frame", and the source-file count — because a caveat inherited from a closed milestone is a caveat
nobody has looked at, and a stale one is worse than none: it is the sentence a reader trusts instead
of checking. Four are unchanged and each now says so with M4's numbers.

Six entries are defects the M3 gate found by attacking what the milestone exists to establish rather
than by reading it, and each was fixed in that change rather than recorded and left: a persistent
descriptor set the Vulkan backend recycled after two frames, a build-time barrier gate that only
fired when the render graph itself relinked, an XR seam check whose expected value came from the
code it was checking, a leak and a use-after-free that only AddressSanitizer could see, a data race
on the null backend's statistics under parallel recording, and the per-case budgets that made the
whole milestone ladder flaky.

- **The milestone's central invariant holds, and one of its two halves had a hole.** The passkey is
  the strong half and it is airtight: `rhi::GraphBarrierKey`'s constructor is private and
  `cy::rendering::GraphExecutor` is its only friend, so a barrier CALL outside the graph does not
  compile. Introduced deliberately in `src/rendering/forward/src/frame.cpp`:

      error: 'constexpr cy::rhi::GraphBarrierKey::GraphBarrierKey()' is private within this context

  The grep half — design.md §2's "a grep-level gate fails the build if a barrier call appears
  outside it" — was **not** true as written. It was an `add_custom_command(TARGET cy_rendering_graph
  POST_BUILD ...)`, so it ran only when the graph was relinked, and a barrier SYMBOL introduced in a
  module that does not relink the graph built clean:

      `using ProbeRecorder = rhi::BarrierRecorder;` in src/rendering/forward/src/frame.cpp
        just build-engine    exit 0        <- the gate never ran
        just quality-layers  1 violation   <- the permanent gate still caught it

  Fixed here: the check is `add_custom_target(... ALL)`, so it runs on every build of the default
  target. Re-introducing the same symbol now fails the build from
  `cy_rendering_graph_barrier_gate`, and removing it goes green again. The invariant was never
  unenforced — `layering` is a permanent gate and an M3 criterion — but the build-time claim was
  overstated by a milestone, which is exactly the kind of thing that is believed rather than
  re-checked.
- **The artefact tripped 24 Vulkan validation errors a frame from frame 3, and exited 0.** This is
  the milestone's own sample on the milestone's own device path, and no suite in the tree saw it,
  because **every device suite renders one frame**. `VulkanDevice::allocate_descriptor_set(layout,
  per_frame)` recorded the flag and then allocated from `frames_[frame_slot_].descriptor_pool`
  whichever value it had — and a frame pool is `vkResetDescriptorPool`'d the moment its slot comes
  round. So a set the caller asked to be persistent, which is what the sample's constants set is,
  was recycled after `frames_in_flight` frames and every later draw bound a `VkDescriptorSet` the
  driver had already destroyed:

      frames=1  validation_errors=0     frames=4   validation_errors=48
      frames=2  validation_errors=0     frames=6   validation_errors=96
      frames=3  validation_errors=24    frames=10  validation_errors=192

  — 24 a frame from the third onward, on a run whose last line is `exit 0 (clean)`.

  Fixed here with a pool that is never reset (`VulkanDevice::persistent_descriptor_pool_`), and the
  regression is `render.golden`'s "the frame survives more frames than the device holds in flight",
  which renders twice round the ring and one more and then compares against the **committed
  reference** — because a descriptor naming recycled memory can also be a frame that happens to look
  right. With the defect restored the case fails on `validation_errors == 0`; with the fix it passes
  and the sample is clean at 60 frames. The pool sizes were wrong in the same place and are also
  fixed: neither the frame pools nor the new one sized `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`, which is
  what `DescriptorKind::SampledTexture` allocates.

  **What to carry forward is the shape rather than the bug.** A one-frame test cannot see a
  frames-in-flight defect. **Closed at M4** (task 1.3): `tests/render/test_many_frames.cpp` is
  registered as `render.frames`, runs the device past the ring and asserts that the steady-state
  frame's pass and barrier counts fall below the first frame's and then stay constant — and it was
  proved to fail before it was believed, by seeding its steady-state baseline with the first frame's
  report and watching three assertions go red. M6's streaming still lives on the far side of that
  ring.
- **The XR late-latch check had no teeth, and the number it compared against came from the code
  under test.** `render.xr_prerequisites`' third case asserts that the view is not in the command
  stream, and the null backend records a push constant's offset and size but never its bytes — so
  the case's real assertion was on the push block's SIZE, compared against
  `cy::sample::first_light::kObjectPushBytes`, which the renderer publishes. Baking a
  `f32 view_projection[4][4]` into `ObjectPush` and moving that constant from 64 to 128 — the seam
  closing, exactly — left the suite at **3 cases, 67 assertions, SUCCESS**. The expected size is now
  the test's own literal with a `static_assert`, and the same probe fails the build. Two limits
  remain and are stated in the case: the log still cannot see payload bytes at all, and closing that
  is a change to `rhi::null::RecordedCommand`; and **late-latching is proved for orientation only**
  — the per-object translation is camera-relative and does travel in a push constant, so a late
  correction to the predicted *position* would still require re-recording every draw.
- **Three defects that only a sanitizer could see, in code every criterion was passing over.** The
  `sanitizers-render` criterion is new at this milestone and it was red the first time it ran, on
  three separate faults, none of which any functional test could detect:

      ASan  Direct leak, 256 bytes   NullDevice::create_buffer   unit.rhi
      ASan  heap-use-after-free      Compiler::build_dependencies  unit.render_forward
      TSan  data race, x4            NullCommandBuffer::draw     integration.render_graph_scale

  The leak: `NullDevice`'s destructor was `= default`, so a buffer still alive when the device went
  away kept its mapped host storage. The use-after-free: `push(readers, readers[i])` in the graph
  compiler's dependency build hands `Array::emplace_back` a reference into the array it is about to
  reallocate — silent without a sanitizer, because the freed bytes are usually still intact. The
  race: `draw()` and `dispatch()` did `++device_->mutable_statistics().draws` while several job
  workers recorded secondaries at once, which is task 2.2.5's whole point. All three are fixed —
  the device now tracks and releases host-backed buffers, the compiler copies before it pushes, and
  a command buffer counts into its own `RecordedCounts` that fold in at `execute_secondary()` and
  `absorb()` on the submitting thread. **The lesson is the gate rather than the bugs**: the RHI's
  own report recorded ThreadSanitizer runs over this suite as clean, and the leak check had never
  been run over it at all.
- **The milestone ladder was flaky, and three per-case budgets were why.** `four-profiles` is run by
  `milestone-m1`, `milestone-m2` and `milestone-m3`, and `m3`'s ledger nests all three — so one
  flaky suite is a dozen exposures per pull request. Measured at this gate as **2 failures in 20**
  `just test-all` runs across the four profiles, and every one was a case sitting on its budget
  rather than anything intermittent:

      test_brdf.cpp    'the L2 basis is orthonormal'    0.536-1.241 ms  vs 1 ms  dev
      test_brdf.cpp    'a uniform environment gives'    2.262 ms        vs 1 ms  debug -O0
      test_extract.cpp 'a static crowd is skipped'      0.873-1.032 ms  vs 1 ms  debug -O0
      test_scaling.cpp CHECK_GT(scan_ns, index_ns*1.5)  a ratio of two single timings

  The two spherical-harmonic cases moved to `integration.material_ibl`, where the taxonomy puts a
  case that integrates something; the crowd shrank from 100 static entities to 32, which is more
  than enough for the property it asserts; and the reflection ratio is now the best of three
  repetitions, because contention can only make a run slower. After the fixes: **0 failures in 20**
  rounds, and a sweep of every unit binary at `CY_TEST_BUDGET_SCALE=0.4` finds no case above 40% of
  its budget in any profile.

  **What is not fixed, and what the next author should know.** The budget is the case's own CPU
  time, which is why the earlier wall-clock flake is gone — but CPU time is not load-free either:
  the same case measured 0.87 ms alone and 1.03 ms inside `ctest -j24`, which is cache pressure, so
  a case within 20% of its budget is still a case that will fail eventually. And the harness's
  **stall ceiling is wall clock** (100x the budget), so a unit case descheduled for 100 ms fails
  even though it did no work; nothing was observed hitting it here, and it remains the one
  load-sensitive check left in the taxonomy. The closest thing to a canary is
  `integration.scene_scale`'s behaviour case, which spends **868 ms of its 1 000 ms** budget at -O0.
  **The canary died at M4**, which is recorded in the section above: the same case measured 1 021 to
  1 563 ms there and failed `four-profiles` in the Debug profile — and therefore M1's, M2's, M3's and
  M4's ledgers at once, which is this paragraph's own multiplication arriving. It is 128 instances
  now rather than 500. The rule the entry states is unchanged and is now evidence rather than
  prediction: **a case within 20% of its budget is a case that will fail eventually**, and the sweep
  that finds them is `CY_TEST_BUDGET_SCALE=0.4` over every binary in every profile.

- **`rhi-and-render-graph` is Working over one backend, and the frame does not reach a window.**
  Vulkan is the only backend with a device behind it; the null backend executes nothing by design.
  `DisplayServer::create_surface(GraphicsApi::Vulkan, ...)` exists and works, and
  `SwapchainDescription` takes the surface it produces — but `cy::rhi::Device` exposes no way to
  obtain the API instance a surface must be created against (`native_handle()` returns the
  `VkDevice`; the `VkInstance` stays inside `src/backends/rhi/vulkan/`). So a host can create a
  window and a device and cannot join them, and `samples/03-first-light` renders offscreen and
  writes a PPM with `--capture`. The milestone's artefact is therefore a **headless** lit scene, and
  `m3.toml` records it as a note. It is a small engine-owned accessor away, and it is the first
  thing M5's editor viewport will want.
- **Async compute is derived, and nothing in the artefact uses it.** The spike proved the model on
  the device — two queue families, a coalesced `qf2 -> qf0` ownership release, a cross-queue
  semaphore, 256/256 texels correct, zero validation errors — and `unit.render_graph`,
  `integration.render_graph_scale` and `smoke.vulkan_frame` keep it. But the sample declares
  `request_async_compute = false` and the frame it renders is one submit on one queue, so the
  path that runs on every pull request is the single-queue collapse of the same derivation. Read
  "async compute, per the spike's outcome" as *derived and tested*, not as *exercised by the
  artefact*.
- **Transient aliasing saves nothing on the milestone's own frame, and the 87.5% figure is a
  synthetic chain.** Task 7.3's claim is real and measured twice — `integration.render_graph_scale`
  derives 64.00 MiB unaliased against 8.00 MiB aliased for a sixteen-transient read-modify-write
  chain, and `smoke.vulkan_frame` asserts the device reserves exactly the plan's figure — but the
  artefact's two transients (the colour target and the depth target) overlap in time and therefore
  cannot share memory:

      $ just run-sample first-light --frames 4
        memory   transients=196608 B unaliased=196608 B aliasing=on
      $ just run-sample first-light --frames 4 --no-aliasing
        memory   transients=196608 B unaliased=196608 B aliasing=off

  `render.null_frame` asserts `transient_bytes <= transient_bytes_without_aliasing` and says the
  same thing in its comment, which is the honest shape. The saving is a property of a frame with a
  post chain, and M3 does not have one.
- **`engine-architecture`'s server split is still seven nulls, and M3 was the milestone that was
  supposed to register the first.** `design.md`'s handoff table says so outright: "All seven servers
  resolve to null because no backend has ever registered. M3 registers the first." It did not.
  `cy::render::RenderServer` exists at layer 2, is driven directly by handles, and has 84 unit cases
  over it — but `ServerRegistry::register_backend` is called by **nothing outside its own tests**,
  and `Runtime::tick()`'s render step is still the empty seam M2 left.
  `samples/03-first-light/main.cpp` drives the renderer from the host loop, and its header comment
  records the gap and calls the closure "a four-line adapter at layer 5 that this sample does not
  own". So the M2 caveat below
  stands unchanged at M3, and the requirement's first scenario — a `MeshRenderer` component holding
  a handle obtained from `RenderServer` — still has no path through the runtime. **Unchanged at M4,
  and now four servers larger.** M4 built `InputServer`, `PhysicsServer`, `AudioServer` and
  `CameraServer`; `ServerRegistry::register_backend` is still called by nothing outside its own
  tests, `Runtime::tick()` names no server kind at all, and `samples/04-character/host/game.cpp`
  constructs and steps all four by hand. `CameraServer` is not even one of the seven `ServerKind`
  values. Two milestones after the handoff table said "M3 registers the first", the servers are
  libraries a host assembles rather than backends the runtime resolves — which is a real
  architectural claim in `engine-architecture`, at Working, with no path through the runtime for
  any of them.
- **The frame is deterministic within a process and across processes, and the guarantee rests on a
  hash that cannot see payload bytes.** Two runs agree: three separate processes of the sample
  printed `plan hash=f31bdc099ded9851` on Vulkan and `74b615b87a605044` on the null backend, and
  `render.null_frame` compares the command-stream hash of two frames in one process. The sort is
  genuinely order-independent — a total order over `(key, stable_id, surface)` where `stable_id` is
  the entity's bits, never a slot or a pointer — and `sort_draws()` asserts the identities are
  unique in a development build. What the stream hash does **not** cover is any command's payload:
  push-constant bytes, viewport values and clear colours are hashed as their sizes. So "the same
  frame twice records the same stream" is a statement about structure, and the content half is
  carried by the golden images, which need a device.
- **The null backend records the same frame the device does — as structure, not as bytes.** Same
  scene, two backends, three processes each: `passes=4 culled=0 submits=1 barriers=6 batches=4
  transfers=0 draws=16 triangles=172` on both. The plan hashes differ and should — `plan_hash`
  covers placement offsets and those come from the device's own memory requirements — and the
  transient totals differ for the same reason (196 608 B on Vulkan against 165 888 B on the null
  device's synthetic alignments). `render.golden`'s fourth case is the committed form of this
  comparison. Claiming hash equality across backends would have been claiming something false.
- **`core-math` is Complete, and half of what makes it Complete cannot run in continuous
  integration.** The device-side conventions are real and they have teeth: `render.conventions` is
  10 cases and 95 assertions on the RTX 5060, the near plane samples back as **1.000000000** and the
  far plane as **0.000000000**, and a negative control — `depth_compare` flipped to `LessOrEqual` —
  fails 14 assertions across all three files rather than one. But `render.conventions` and
  `render.golden` are declared only when `CY_RENDERER_VULKAN` is on, the default build has it off,
  and no hosted runner has a device. So the gate that runs on every pull request covers
  `unit.math`'s 72 cases and 1 012 assertions of arithmetic; the half that meets a depth buffer is
  evaluated on a machine with a GPU and reported as *not evaluated* everywhere else. **Half of this
  is stale as written**: `CY_RENDERER_VULKAN` has defaulted **on** since M3's own closing commit, so
  the suites are declared in every default build. What has not changed is the part that matters —
  no hosted runner has a device, so they are still evaluated on one machine. That is what
  `requires = "gpu"` in `m3.toml` records, and it is the honest reading of the tier.
- **Camera-relative rendering is proved twice, and only one of the two exercises the renderer's own
  subtraction.** `render.conventions`' million-unit case builds its camera-relative vertices in the
  test and asserts the two images are bit-identical, with a control showing the world-space path
  loses the centimetre offsets — good evidence about the arithmetic, none about the renderer.
  `render.golden`'s second case is the one that matters: it runs `samples/03-first-light` with
  `--origin 1000000` through `Renderer::render()`'s own `f64` subtraction and compares against **the
  same committed reference file** as the near scene. Both need a device.
- **Shader hot reload is proved over the file watcher and not over a running frame.**
  `integration.shader_pipeline` edits a module, waits for the watcher's settle period, recompiles
  and asserts that exactly the pipeline states naming the rebuilt program are invalidated — which is
  task 7.4's claim and is a real one. What no test does is replace a shader while the sample is
  running and see the next frame change, because the sample has no reload path wired into its loop.
  Read the tier as "the pipeline reloads", not "the artefact hot-reloads". **Unchanged at M4, and
  repeated by it**: `integration.swift_reload` proves the loader reloads a Swift module across a
  layout change, and `samples/04-character` never calls `reload()`. Two milestones, two artefacts,
  the same sentence — which is worth naming as a pattern rather than recording twice, because M5's
  editor is the milestone whose whole value proposition is that iteration does not cost a restart.
- **Closed at M4, and it cost something elsewhere.** M3 recorded that `CY_SHADER_SLANG` was off by
  default, so "a Slang regression is caught by whoever builds with the option, not by CI". M4's task
  1.4 flipped it to `DEVELOPMENT` — on in Debug and Development, off in Profile and Shipping, which
  is what `shader-system`'s "a shipping build SHALL contain no Slang compiler" requires as a
  structure rather than as a habit — so `smoke.shader_slang` now runs in the profile every CI job
  builds. What runs in the two shipping profiles is still the SPIR-V passthrough, which is the
  shipping path rather than a stub. **The flip had a consequence nobody followed up**, and it is the
  `sanitizers` entry in M4's section above: `just test-sanitize` builds the `dev` profile, so it now
  compiles Slang's own code generator under LeakSanitizer.
- **M3's renderer components are registered by name, and the identity manifest still holds two demo
  types.** M2's carried-forward debt 1.2 asked for M3's renderer components to be *reflected* as
  they were written. They are not: `src/core/reflect/CMakeLists.txt`'s annotated-header list is
  still one demo header, `just quality-identity` still reports **2 live types, 0 tombstones**, and
  `src/rendering/scene/include/cy/rendering/scene/components.h` says why in the header — reflection
  cannot carry a `Transform` or a `Name` today, and inventing manifest identifiers for a component
  would be inventing an identity. The hash gap was closed the other way instead, with an explicit
  `StateSchema` (see the M2 section below), which is the right call and is not the same thing. The
  consequence stands: **no component in the engine is covered by the identity gate**, and every one
  of them is a rename M5's save files will not survive. **Unchanged at M4, and larger.**
  `just quality-identity` still reports **2 live types, 0 tombstones** — both of them
  `cy::demo::` — while M4 added physics, camera, audio, gameplay and input components and a Swift
  module that registers its own across the ABI. `input-and-actions` asks for `ActionStableId` to
  come from the manifest and it does not; the ABI's `world_register_component` allocates no manifest
  identifier either.
- **Still standing at M3 and larger** — restated with M3's numbers in the section above. The M2
  finding:

  **`build-system-and-platforms` is unchanged and still Linux-only in practice.** Windows and macOS
  have still never compiled. The tree `just quality-layers` walks is **1 002 files** at M4, up from
  816 at M3 and 600 at M2, so the first foreign build is a larger diff every time it is deferred,
  and every `three-platforms` criterion in every ledger is still reported as *not evaluated* rather
  than as passed. M3 added `volk`, VMA and a second build configuration; M4 added a Swift toolchain,
  Jolt, miniaudio, `dlopen` and a shared-library loader, and `samples/04-character/module.toml`
  names a library for Windows and macOS that neither has ever loaded.
- **The milestone gates are green and nothing in continuous integration runs them.**
  `tools/roadmap/gates.toml` declares `milestone-m0`, `-m1` and `-m2` as `green` and permanent, and
  `tools/ci/check_workflows.py`'s coverage check skips every gate whose class is not `permanent` —
  so no job in `.github/workflows/ci.yml` runs `just roadmap-milestone` for any milestone. The
  ladder is real (each ledger's first criterion is the previous milestone's whole set) but it is
  run by whoever closes a milestone, not by a pull request. That is a defensible trade — `m2`'s
  recipe is a working session and `m3`'s contains three `four-profiles` loops — but it should be a
  recorded decision rather than an accident of how the coverage check is written, and it is recorded
  here as the second. **Unchanged at M4.** `milestone-m3` is `green` and `milestone-m4` is
  `joins-on-close`; `just ci-check` still reports only that every *permanent* gate is run, so no job
  runs `just roadmap-milestone` for any milestone. What did change is that forgetting the promotion
  is no longer possible: `just roadmap-test` now reads `openspec/changes/archive/` and fails when a
  milestone whose change is archived still has a gate at `joins-on-close`, which is the check M2's
  block asked for, M3's block landed, and M4's close is the first to be held to.

## Where M2's tiers were thin, and what M3 closed

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

**Re-checked again at M3's gate, and each entry now opens with what M3 did to it.** M3 carried seven
of M2's debts as its own section 1, so most of this list moved; a caveat that is still here after a
milestone that was asked to close it is worth more than one nobody revisited. Verdicts below were
run rather than read — the command or the file that decides each is named.

- **Closed in the engine at M3, and still true of M2's own artefact.** `src/ecs/state_schema.h`,
  `src/scene/state_schema.h` and `src/rendering/scene/state_schema.h` declare explicit field lists
  for the ECS's two relationship components, the scene's twelve built-ins and M3's renderer
  components, and `integration.state_hash_coverage` is the regression: renaming a node, reparenting
  one, changing sibling order and changing visibility each change the hash, and the derived world
  transform is recomputed rather than hashed. The route taken is an explicit schema rather than
  reflection, for the reason the M3 section above records. What did **not** change is the closing
  artefact: `just run-sample headless-sim` still prints `subjects declared=4 undeclared=13`, because
  `samples/02-headless-sim` declares only its own components and declaring the other thirteen would
  change the hash `smoke.headless_sim` asserts. The mechanism is closed; the sample that advertises
  the number is not. The M2 finding, as it was written:

  **The state hash covers what was declared, and in the closing artefact that is four subjects out
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
- **Closed at M2, and the residual is unchanged at M3.** The fix and its regression stand; the
  shared-component caveat at the end of this entry is still true, and
  `src/runtime/src/state_hash.cpp` still says so at the line that does it. The M2 finding:

  **The state hash depended on component *registration order*, and that was found at the gate rather
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
- **Closed at M3 for the scheduler, still standing for the command buffer's fallback.** M3's task
  1.7 replaced the stage scheduler's tie-break with the system's **name**:
  `Schedule::assign_merge_keys()` insertion-sorts by name and says why, so a plugin registering a
  system conditionally no longer shifts every later system's key. `CommandBuffer`'s merge key is now
  set from that same rank — but a buffer used **outside** a schedule still falls back to its
  attachment order to the world, which `command_buffer.h` documents as "itself a registration
  order". That is the remainder. The M2 finding:

  **The same class is still open in two more places, and both are named rather than fixed.** The
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
- **Adopted at M3, at three fields.** The grep that returned only the header and its own test now
  returns `src/scene/include/cy/scene/components.h` (2),
  `src/rendering/scene/include/cy/rendering/scene/components.h` (1),
  `src/scene/src/node_transform.cpp` and `src/rendering/scene/src/state_schema.cpp`. Three wrapped
  fields against the tree's whole state is movement rather than closure, and the debt's own argument
  — cheapest at the moment a component is authored — means every component M3 wrote unwrapped is one
  M9's lint inherits. The M2 finding:

  **The determinism firewall is unspellable, and nothing has adopted it.** `Classified<>` delivers
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
- **Half closed at M3.** Hot reload is no longer at zero: `src/core/assets/watch.h` is a file
  watcher with a settle period, `asset_system.h` has a reload entry point and a change report handed
  to observers, and `integration.shader_pipeline` is its first consumer. Streaming is still absent
  and still scheduled for M6 — no residency budget, no partial-mip or LOD path. The M2 finding:

  **`core-assets-and-io` is Working with two of its ten requirements unimplemented.** The cook path
  is real — `cy_cook` reads authoring documents and writes a `.cypak` addressed by identity, and
  `integration.cook_path` loads it back — and that is what the M2 tasks asked for. But the
  capability's **Streaming** and **Hot reload** requirements have no implementation at all: there is
  no residency budget, no partial-mip or LOD path, no file watcher, and no `reload` entry point
  anywhere under `src/core/assets/`. Streaming is deliberate and scheduled — design.md §7 puts it at
  M6 — but hot reload is named by the M2 row of `ROADMAP.md` and by task 3.2.13 and is simply
  absent. Read the tier as "the cook path is Working"; two of the ten requirements are still at
  none.
- **Unchanged at M3, and now costlier.** `just quality-identity` still reports **2 live types, 0
  tombstones**, and M3 added nine modules of components that are registered by name — see the M3
  section above for why the renderer took the explicit-schema route instead. The M2 finding:

  **`core-type-system` is still a two-type demonstration, and M2 is what it was supposed to stop
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
- **Unchanged at M3, and now measurable.** The figures were not re-run; what changed is that
  `benchmarks/ecs/` exists (task 1.6), so the next person to argue about them has a runner. The M2
  finding:

  **Cook-time flattening needs a fixup pass, and "activation is a bulk copy" is an abbreviation
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
- **Still standing at M3, unchanged.** The M2 finding:

  **The state hash is a function of the entity indices, and that is nowhere written down.** The
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
- **Still standing at M3, and M3 was the milestone that was supposed to close it** — see the M3
  section above, where it is restated with what exists now. `ServerRegistry::register_backend` is
  still called by nothing outside its own tests. The M2 finding:

  **`engine-architecture` is Working, and all seven of its servers are the null implementation.**
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
- **Closed at M3, and the pattern is now a check rather than a reminder.** `milestone-m2` was
  flipped to `green` in this change and `milestone-m3` with it, so M3 is the first milestone that
  did not have to be reminded. `just roadmap-test` now reads `openspec/changes/archive/` and fails
  when a milestone whose change is archived still has a gate at `joins-on-close` — proved by setting
  `milestone-m2` back and watching the selftest go 57/58. The cost is real and is stated in
  `gates.toml`: a pull request that runs `milestone-m3` runs `four-profiles` three times over
  through the nested ladder. The M2 finding:

  **`milestone-m1` was still `joins-on-close` in `tools/roadmap/gates.toml` when M2 came to close.**
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
- **Closed at M3.** `benchmarks/ecs/bench_ecs.cpp` is declared with `cy_add_benchmark(NAME ecs ...)`
  and `just test-bench` compares six ECS metrics against `benchmarks/baseline.json`. The M2 finding:

  **The bulk-copy claim is a correctness test with a time budget, not a committed benchmark.**
  `integration.ecs_scale` instantiates 100,000 rows and reads them back through the runtime layout,
  inside the integration suite's per-case budget. That is a threshold a gross regression trips; it
  is **not** a ns/entity figure compared against `benchmarks/baseline.json`, because no benchmark
  runner exists for the ECS — only for the job system. `tools/roadmap/milestones/m2.toml` records
  this as a note and names `cy_add_benchmark(NAME ecs ...)` as the honest fix.
- **Still standing at M3, unchanged; it is M9's.** The M2 finding:

  **"Identical across restore-from-snapshot" is a round trip, not a re-run.** The gate captures a
  snapshot, ticks the world 128 further ticks so it demonstrably moved, restores, and checks the
  hash matches the settled one again — `smoke.headless_sim` asserts the divergence as well as the
  match, so it cannot pass on a restore that did nothing. What it does not do is *replay* from the
  restored state and reproduce the same trajectory: that needs the clock rewound into the same
  epoch, and `Simulation` exposes only `reset_epoch()`, which by design enters a new one. Rewinding
  is `replay-and-rollback`'s, at M9.
- **Still standing at M3, unchanged.** The M2 finding:

  **`scene-graph-and-nodes`' coherence check is five invariants, and "no orphaned entity" is not one
  of them.** `check_coherence()` covers the specification's five — entity alive, one node per
  entity, `Parent` matches the tree, `WorldTransform` consistent after propagation, effective flags
  consistent — and returns a report in every configuration rather than asserting, which is right,
  because `CY_ASSERT` is compiled out in Profile and Shipping. The orphan claim that appears in
  `ROADMAP.md`'s M2 exit criteria and in `gates.toml`'s `scene-coherence` description is carried by
  two separate scene cases ("unloading destroys exactly its entities", "a node reparented out of its
  scene is still destroyed with it") rather than by the invariant checker. The claim holds; the
  gate's own wording overstates where it is checked.
- **Closed at M3, and it took four more cases than anyone expected.** Task 1.1 replaced the
  harness's wall-clock budget with the case's own CPU time plus a stall ceiling
  (`tests/harness/src/budget.cpp`'s `cpu_now_ns()`), which removed the load-induced failures the
  entry below describes. It did not remove the flake: at M3's gate `just roadmap-milestone m0`
  failed on `unit.material` on an **idle** machine, and three more cases followed it across the four
  profiles. All four are listed with their measurements in the M3 section above, and after the fixes
  `four-profiles` ran **20 rounds out of 20** green. The M2 finding:

  **M2's own suites did not fit the test taxonomy, and the repair is a split rather than a fix.**
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
- **Closed at M3.** `.github/workflows/ci.yml` has a `schedule:` trigger at 03:41 UTC and a
  `sanitize-nightly` job that runs `--tests .` under TSan, ASan+UBSan and UBSan alone; the
  `CY_TEST_BUDGET_SCALE=0` conflict with `test_assertions.cpp` is fixed, and all three commands were
  run at 69 tests passing. The M2 finding:

  **The sanitizer gate is wider than M1's and still not what the specification asks for.** It now
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
