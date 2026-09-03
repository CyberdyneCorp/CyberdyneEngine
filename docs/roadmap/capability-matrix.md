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

As of M0 eight capabilities have left `—`: `delivery-roadmap` is Working, and
`build-system-and-platforms`, `core-platform-abstraction`, `developer-workflow-and-just`,
`diagnostics-profiling-and-crash`, `project-and-plugins`, `testing-and-quality` and
`thirdparty-dependencies` are Seed, each recorded against `implement-m0-ground`. The remaining 66
have not started.

The tiers above are the plan and the record agrees with it; what the plan did not say is what M0's
seeds are *thin* on, and that belongs here rather than in a commit message:

- **`build-system-and-platforms`** is Seed on Linux only in practice. The Windows and macOS paths
  are authored — the compiler matrix, the host files under `platform/desktop-sdl3/src/host/`, the
  presets — and have never been compiled. The CI matrix is what will first execute them.
- **`testing-and-quality`**'s sanitizer wiring works, but no CI job runs a suite under it, and
  LeakSanitizer reports the trace's process-lifetime thread rings, so only the unit set is clean
  under ASan today. See `risks.md` if this is still true at M1.
- **`project-and-plugins`** is Seed on the module half. There is no project manifest yet; the
  requirement that a project be described declaratively is M1's.
