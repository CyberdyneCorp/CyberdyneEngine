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

As of M1 fourteen capabilities have left `—`. `core-jobs-and-concurrency`, `core-math`,
`core-memory-and-containers`, `core-type-system`, `delivery-roadmap` and `project-and-plugins` are
Working; `build-system-and-platforms`, `core-assets-and-io`, `core-platform-abstraction`,
`developer-workflow-and-just`, `diagnostics-profiling-and-crash`, `engine-architecture`,
`testing-and-quality` and `thirdparty-dependencies` are Seed. The remaining 61 have not started.
M1 advanced seven of them, each recorded against `implement-m1-substrate`.

## Where M1's tiers are thin

The tiers above are the plan and the record agrees with it. What the plan did not say is where the
implementation is **thinner than the tier claims**, and that belongs here rather than in a commit
message. Every entry below was measured or reproduced while closing the milestone; where a number
appears, it is a number this tree produced.

- **The quality gates are pinned to one LLVM, and until M1's close they were not.** Two versions of
  these tools do not agree, which makes an unpinned gate a gate whose verdict is a property of the
  machine that ran it. clang-format 18.1.8 and 22.1.8 disagree about three
  sites in this tree — `struct sigaction action {}` against `action{}`, and `IntrusiveNode T::*Member`
  against `T::* Member` — and the disagreement has **no fixed point**: each version rewrites the
  other's output back, so no spelling of those lines satisfies both. clang-tidy is worse: 18.1.3
  reports two `bugprone-multi-level-implicit-pointer-conversion` findings in `src/core/values/src/var.cpp`
  that 22.1.8 does not report at all, and over `src/core/math/` the same tree measured 404 findings
  under 22 against 41 under 18. Until M1's close, `just quality-lint` ran whatever was on PATH and
  continuous integration installed Ubuntu's 18.1.3, so the gate meant a different thing in each
  place. It is now `llvm_pin` in the justfile, `just env-doctor` refuses a different major, and the
  workflow installs the pinned version — `just ci-check` fails if those two disagree. **A clone
  without the pinned tooling cannot run the format or lint gates**, by design: `pip install
  clang-format==<pin> clang-tidy==<pin>` is the correction the doctor prints.
- **The ladder could not survive its own last rung.** M0's `m1-open` criterion matched
  `openspec/changes/*m1*/proposal.md`. `openspec archive` moves a closed change under
  `openspec/changes/archive/`, and `*` does not match a path separator — so archiving
  `implement-m1-substrate`, which is M1's task 6.11 and the final step of closing it, would have
  turned that criterion red, taking `just roadmap-milestone m0` with it and then M1's own `m0-green`
  criterion. The question the criterion asks is whether the next milestone was opened deliberately,
  and a change that was opened and then closed answers it more strongly than one still open; both
  M0's `m1-open` and M1's `m2-open` now match either, and each records why.
- **`core-type-system`** is Working on a **two-type demonstration**. `identity/manifest.toml` holds
  exactly `cy::demo::Health` and `cy::demo::Placement`; no engine type is reflected, and the only
  consumer is `samples/01-headless-host`. The identity model, the manifest, the tombstones and the
  generator are real and proved — renaming `Health::icon` with no tombstone fails both
  `just quality-identity` and the build, naming the field, its `FieldId 5` and the two commands that
  resolve it, and tombstoning it retires the number and lets the tree through — but none of it has
  been exercised at the scale M2 will bring. `TypeRegistry::find` is a linear scan, and `read_record`
  calls `find_field` once per field per record, so deserialisation is quadratic in the field count.
  That is control plane today and asset-load path tomorrow.
- **The reflection generator's frontend is an undeclared dependency.** It needs the `clang` Python
  bindings pinned at 18.1.8 (`tools/gen/reflect/parse.py`) plus a libclang 18 shared library, looked
  for at a hard-coded list of paths. Neither is in `deps/manifest.toml`, neither appears in
  `THIRD_PARTY.md`, and `just env-doctor` does not check for either. A clone without them configures
  and builds, silently compiling the committed metadata and never regenerating it. Note also that it
  pins LLVM **18** while the quality gates pin **22**: the frontend has to match what it parses, but
  nothing in the tree says so. `implement-m2-world` carries the spec delta that makes build-time
  tooling a manifest entry; the entries themselves are M2 work.
- **`core-math`**'s SIMD claim covers the backends this build compiles, which is **two of four**.
  `SIMD: every compiled backend is bit-identical to the reference on the primitives` compares Scalar
  against Scalar and, here, against SSE — and passes, 24,557 assertions over the integration suite.
  NEON is compiled only on the two arm64 legs of the CI matrix. **AVX2 is compiled by nobody**:
  `CY_MATH_HAS_AVX2` is gated on `__AVX2__`, no build in this repository sets `-mavx2`, and no CI job
  does either, so the 256-bit path in `simd.h` has never been compiled, let alone compared against
  the reference. The conventions are executable tests and they hold: a perspective projection maps
  near to exactly 1.0 and far to within 1e-6 of 0, the infinite-far form maps 1e6 metres to 5e-09,
  and a look-at down −Z is the identity quaternion and the identity `Mat4`.
- **`core-jobs-and-concurrency`** is Working, and its **throughput thresholds are loose**. Measured
  on this 24-core host in the `profile` configuration: parallel efficiency 0.3625 against a floor of
  0.0809, `parallel_for` efficiency 0.6770 against 0.2090, dispatch overhead 0.0640 against a ceiling
  of 0.2072. Every metric has three to four and a half times the headroom it needs, so a regression
  to a quarter of today's speedup would still pass. The gate is real — raising a floor makes it fail
  — but it will not notice a gradual loss.
- **`core-jobs-and-concurrency`**'s non-blocking rule is enforced for **declared** blocking only.
  `begin_blocking_region` refuses on a worker and counts the violation, which is what the
  specification asks and what `test_blocking.cpp` proves. A worker that simply calls `read()`,
  `mutex::lock()` or a GPU fence declares nothing and is caught only by the watchdog as a *long
  task*, after `long_task_threshold_ns` — 250 ms by default. A 100 ms disk stall on a worker is
  invisible.
- **The sanitizer gate runs one suite.** The `sanitize` job runs TSan and then ASan+UBSan over
  `--tests jobs`, where `testing-and-quality` asks for the unit and integration suites at least
  nightly. Leak detection is on and M1 declared the trace's process-lifetime rings at their
  allocation site, so a leak reported there is a defect rather than M0's standing false positive.
  The nightly run the requirement describes does not exist, because this workflow has no schedule.
- **`core-assets-and-io`** is Seed as intended, and its serialisation is where the milestone's first
  real undefined behaviour was found: `VariantKey::parse` called `memcpy(dst, nullptr, 0)` for the
  any-key on every package load. It is fixed and carries a regression test, but assume the class is
  not exhausted — the tree holds two dozen `memcpy(dst, view.data(), view.size())` calls whose
  sources can be empty, and only the one that UBSan happened to reach is guarded.
- **`project-and-plugins`** is Working against the M1 row's scope — "layering enforced, modules and
  dependencies, layered typed configuration" — and **not against the capability's name**. The
  manifest, the graph, the cycle and undeclared-dependency rejections, the layered configuration and
  the target-graph layer check are all real and proved by `tools/project/selftest.py` and by the
  `project_graph` fixtures, but the only project that exercises them end to end is
  `tools/project/fixtures/valid/`. Of the plugin half of the specification, the manifest reaches the
  declaration: `tools/project/graph.py` gives a plugin a stable identifier, rejects a duplicate one,
  and rejects an engine-API range the engine falls outside — each with a fixture. Everything past the
  declaration is absent. There is no `dlopen` and no `LoadLibrary` anywhere in the tree, so per-project
  enablement, phased registration, failure containment and hot reload have no mechanism, and **no
  plugin has ever been loaded because nothing can load one.**
- **`engine-architecture`** is Seed on ordering alone. Startup and shutdown order is deterministic
  across 100 processes and asserted as such; the servers, the ECS/scene duality and the fixed-tick
  loop are M2's.
- **`build-system-and-platforms`** is unchanged from M0 and still **Linux-only in practice**.
  Windows and macOS have still never compiled — every claim about them is the CI matrix's to make,
  and it has not made one. M1 added roughly 190 more source files, so the first foreign build will be
  a larger diff than it would have been. One smaller consequence is visible locally: the `release`
  profile's link-time optimisation makes GCC drop zstd's own `-Wa,--noexecstack`, warning four times
  per build, so a fetched dependency loses an assembler hardening flag in the one configuration that
  ships.

- **The sanitizer gate had two defects that hid each other, and one of them made the ledger break
  the milestone it was checking.** `just test-sanitize` read `CY_BUILD_DIR` as the tree to build
  *in*, so a run with the override set — which is what `just roadmap-milestone` propagates to every
  criterion — configured the ordinary build tree with `-D CY_SANITIZE=address,undefined` and left it
  that way. Everything downstream then ran against an instrumented tree: `just quality-lint` failed
  on findings a non-sanitized compile database does not produce and on a missing
  `sanitizer/asan_interface.h`, and `just run-sample empty` failed on LeakSanitizer reports from
  inside SDL3's X11 backend. The symptom was `just roadmap-milestone m0` going red on the tree
  `just roadmap-milestone m1` had just proved green — the regression the ledger exists to catch,
  produced by the ledger.

  Correcting that exposed the second: without an override the recipe builds in
  `build/sanitize-address,undefined`, and SDL3 probes the linker with
  `-Wl,--version-script=<path>.sym`. `-Wl,` splits its argument on commas, the linker is handed a
  path truncated at `sanitize-address`, and SDL3 concludes that Linux does not support version
  scripts and fails the configure. **Continuous integration sets no `CY_BUILD_DIR`, so
  `just test-sanitize --sanitizer address,undefined` — a permanent gate since M1 and an M1 exit
  criterion — could never have passed there.** It passed on every developer machine only because
  each had set an override to a comma-free directory, which is the first defect. The override now
  selects the sanitized tree's *parent* and the sanitizer's name is reduced to letters, digits and
  hyphens; `tools/ci/test_recipes.py` holds both rules, fails against either old behaviour, and runs
  inside `just ci-check`, which is a permanent gate.

Carried from M0 and now closed: the trace's process-lifetime rings are declared to LeakSanitizer at
the allocation site (`src/core/diagnostics/src/lifetime.h`), so `just test-sanitize` defaults to leak
detection on. Closed at M1's gate rather than during it: `just quality-lint` is green over all 182
translation units, `just quality-format-check` over all 367 files, and every one of M1's seven new
permanent gates — identity, reflection, project-graph, sanitizers, job-throughput, profiles,
workflows — is declared in `tools/roadmap/gates.toml` and run by a job in `.github/workflows/ci.yml`,
which `just ci-check` now proves rather than assumes.
