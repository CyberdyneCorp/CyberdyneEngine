# CyberdyneEngine — Delivery Roadmap

The order in which [the specifications](../openspec/specs/README.md) become code.

> This document is a **view** of the [`delivery-roadmap`](../openspec/specs/delivery-roadmap/spec.md)
> capability, which is authoritative. Where the two disagree, the specification wins and this
> document is wrong.

**There are no dates here, and there will not be.** A date is a compound estimate that decays from
the moment it is written. Ordering is a design consequence that does not: the ABI must be versioned
from its first symbol whether that symbol appears next month or next year. The roadmap answers
*after what*, not *when*.

---

## The shape of it

Twenty rungs, four eras. Each rung ends in something that runs, committed to the repository and
exercised by continuous integration — so it becomes a regression gate for everything after it.

**Twenty and not twelve, because the ladder has been split four times and never renumbered.**
[`implement-m5b-operable`](../openspec/changes/implement-m5b-operable/proposal.md) **inserted
M5.5 · Operable between M5 and M6**: M5 claimed `editor-ui-ux` at Working and closed on a scripted
session with no window, and the correct repair was to give the window a milestone rather than make
the plan retroactively right.
[`split-m8-authorable-and-systems`](../openspec/changes/archive/2026-09-08-split-m8-authorable-and-systems/proposal.md)
**made M8 into M8.a, M8.b and M8.c**, because a milestone whose artefact cannot be reached without
its own risk spike succeeding contains two. And
[`implement-m11-reach`](../openspec/changes/implement-m11-reach/proposal.md) task 0.1 **made M11
into M11.a through M11.e** — applied by
[`implement-m11a-foundations`](../openspec/changes/implement-m11a-foundations/proposal.md), which
carries the rule — because sixty-five Complete cells behind one artefact is a gate that cannot name
what it is looking at. And
[`implement-m11d5-backends`](../openspec/changes/implement-m11d5-backends/proposal.md) **inserted
M11.d.5 · Backends between M11.d and M11.e**, because M11.d's spike established that neither Metal
nor D3D12 can be compiled on the host this project works on — Linux, one GPU vendor, no Apple
toolchain — and a rung cannot be judged on work no machine it runs on can build.

Inserting rather than renumbering keeps every reference to a later milestone valid, and the suffix
says plainly that the ladder gained an entry rather than always having had one. **M8 and M11 remain
the names of their groups**, and a sentence written before either split still means what it said.
M11.d.5 is the first identifier to carry both forms at once — a letter rung from M11's split and a
`.5` insertion from M5.5's — which is exactly what it is.

```mermaid
flowchart LR
    subgraph F["Foundation"]
        direction TB
        M0["M0 · Ground<br/><i>it builds, everywhere</i>"]
        M1["M1 · Substrate<br/><i>core services</i>"]
        M2["M2 · World<br/><i>entities and nodes</i>"]
        M0 --> M1 --> M2
    end
    subgraph P["First playable"]
        direction TB
        M3["M3 · First light<br/><i>a frame</i>"]
        M4["M4 · Playable<br/><i>Swift gameplay</i>"]
        M5["M5 · Authorable<br/><i>the editor</i>"]
        M5B["M5.5 · Operable<br/><i>a window, and an agent</i>"]
        M3 --> M4 --> M5 --> M5B
    end
    subgraph S["Production scale"]
        direction TB
        M6["M6 · Scale<br/><i>build graph, streaming</i>"]
        M7["M7 · Fidelity<br/><i>the modern renderer</i>"]
        M8A["M8.a · Authorable<br/><i>build a scene, press play</i>"]
        M8B["M8.b · Systems<br/><i>a vertical slice</i>"]
        M8C["M8.c · Spectacle<br/><i>particles, a cinematic</i>"]
        M6 --> M7 --> M8A --> M8B --> M8C
    end
    subgraph SH["Shipping"]
        direction TB
        M9["M9 · Integrity<br/><i>determinism, network</i>"]
        M10["M10 · Worlds<br/><i>environment, PCG</i>"]
        M11A["M11.a · Foundations<br/><i>the debts, the budget</i>"]
        M11B["M11.b · Authoring<br/><i>a real game</i>"]
        M11C["M11.c · Image<br/><i>a beauty shot</i>"]
        M11D["M11.d · Desktop<br/><i>native platform · packaging</i>"]
        M11D5["M11.d.5 · Backends<br/><i>Metal · D3D12</i>"]
        M11E["M11.e · Ship<br/><i>mobile · 1.0</i>"]
        M9 --> M10 --> M11A --> M11B --> M11C --> M11D --> M11D5 --> M11E
    end
    M2 --> M3
    M5B --> M6
    M8C --> M9

    classDef gate fill:#3b1f1f,stroke:#f87171,stroke-width:2px,color:#fee2e2
    class M11E gate
```

| Era | Milestones | What exists at the end |
|---|---|---|
| **Foundation** | M0 – M2 | A headless simulation that is deterministic and serializable. Nothing user-visible. |
| **First playable** | M3 – M5.5 | A game you can write in Swift and edit in the editor — in a window, or through an agent driving the same editor. |
| **Production scale** | M6 – M8.c | A world larger than memory, rendered at film detail, playable as a real game. |
| **Shipping** | M9 – M11.e | Multiplayer, open worlds, a game made in the editor, every platform, 1.0. |

Foundation is one unbroken sequence. M0, M1 and M2 produce nothing usable individually, and the
roadmap says so rather than pretending otherwise.

---

## Maturity tiers

Nearly every capability spans five or more milestones. "Implemented" is not a boolean for any of
them, so progress is recorded against four tiers:

| Tier | Meaning |
|:---:|---|
| **—** | Not started. |
| **S** — Seed | Interfaces, data model and invariants exist. Dependents can be built against it. Behaviour may be minimal, single-threaded, unoptimised, or one backend only. |
| **W** — Working | The requirements a real project depends on are satisfied. Used by the samples, covered by tests, diagnostics exist. |
| **C** — Complete | Every requirement satisfied, every scenario tested or exempted, gates in continuous integration. |

A capability seeds at the milestone **its first dependent needs it** — not the milestone at which
it becomes interesting. See [the capability matrix](roadmap/capability-matrix.md) for every
capability's path through the ladder.

---

## The invariants that cannot wait

This is the load-bearing part of the roadmap. Each of these is a property of every line of code
written *after* it, not a feature of a subsystem — so each lands at **Seed** tier, early, long
before its capability is anywhere near Complete.

| Invariant | Lands | Cost of landing it late |
|---|:---:|---|
| Stable type and field identity, manifest, and gate | M1 | Every serialized artefact made before the manifest is invalidated |
| Layering enforcement over the project graph | M1 | Cheap to prevent, unbounded to unwind |
| Access declarations and structural-change deferral | M1–M2 | Systems written without declarations cannot be parallelised without rewriting them |
| Commit boundary, seeded random streams, state-hash hooks | M2 | The M9 validator can find violations but never prevent them |
| Cook-time flattening to archetype blocks | M2 | A runtime prefab graph becomes load-bearing; the ECS loses its reason to exist |
| Barriers computed by the render graph | M3 | Hand-written barriers spread to every pass; removal is a renderer rewrite |
| Camera-relative rendering; coordinate, depth and unit conventions | M3 | Precision assumptions reach every shader and transform path |
| Append-only versioned ABI with its gate | M4 | The first published symbol starts the obligation |
| One validated command stream into the simulation | M4 | Replay, rollback and lockstep are this mechanism; a second input path defeats all three |
| Transactions as the only persistent write path | M5 | Undo, autosave, recovery, merge and live editing each break silently |
| Residency ≠ activation ≠ simulation rate ≠ detail | M6 | Collapsed once, collapsed in every consumer that follows |
| The determinism firewall around presentation-only systems | per subsystem | One gameplay read from a non-deterministic system is invisible until a desync months later |
| Graphs compile, never interpret | per consumer | The interpreter becomes the compatibility surface |
| Privacy classification on every diagnostic field | M0 | Unclassified fields accumulate faster than they can be audited |

---

# Foundation

## M0 — Ground

*It builds on three platforms and does nothing.*

**Entry**: an empty repository.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `build-system-and-platforms` | S | CMake, four configurations, feature options, compiler support, the three desktop platforms |
| `developer-workflow-and-just` | S | The `justfile`, `env-doctor`, build/test/quality/roadmap recipes, profiles consistent across toolchains |
| `thirdparty-dependencies` | S | Dependency manifest, vendoring policy, generated attribution, and the five dependencies the artefact needs: SDL3, doctest, Tracy, zstd, BLAKE3 |
| `testing-and-quality` | S | The taxonomy's directory layout, the unit, integration, smoke and benchmark harnesses over doctest, formatting, static analysis and sanitizer wiring |
| `core-platform-abstraction` | S | Platform services, `DisplayServer`, an SDL3 desktop backend and a headless one behind them, a window that opens and closes |
| `diagnostics-profiling-and-crash` | S | One trace with its identity and formatting, structured logging, assertions, a crash handler that writes a report, **privacy classification from the first field** |
| `project-and-plugins` | S | Module manifests and project-graph validation — enough to enforce layering from the first module. The project-level manifest is M1 |
| `delivery-roadmap` | W | The status record and `roadmap-status`, `roadmap-milestone` and the merge-gate set — deferred to M0 by `add-delivery-roadmap` §4 |

**Closing artefact**: `samples/00-empty` — an application that opens a window, runs an empty loop,
writes a trace, and exits cleanly.

**Exit criteria**

- `just env-doctor` diagnoses a clean machine and a broken one, on Linux, Windows and macOS
- `just build-all` and `just test-all` are green on all three in continuous integration
- `just run-sample empty` opens and closes a window
- A trace file is produced and is readable by `just diagnose-trace`
- Formatting, lint and static analysis gates are live
- The layering check fails on each of the three deliberately introduced violations: an upward
  link, an upward `#include`, and an SDL header above `platform/`

Recipe names are flat and hyphenated rather than `just` modules: bare `just` must list every
recipe, and `mod` is unstable in the version in use. See `implement-m0-ground/design.md` §10.

The whole list is executable as `just roadmap-milestone m0`.

**Risk spike**: the four-toolchain profile mapping (CMake, Cargo, Slang, engine tools). Prove one
profile name means one thing everywhere before writing the rest of the recipes.

*Resolved.* The mapping is data — `cmake/profiles.cmake` and the table in the `justfile` — and the
two are checked against each other at configure time rather than merely documented. All four names
were built and run under CMake, and the four Cargo profiles named in the table were configured and
built against a scratch crate, so the M5 column is known to be expressible rather than assumed.

---

## M1 — Substrate

*The services everything else is written against.*

**Entry**: M0 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `core-type-system` | W | Reflection registry, the generator, `Var`, generational handles, events, `Callable`, **stable field identity, the committed manifest and its CI gate** |
| `core-memory-and-containers` | W | Allocators, memory domains, the budget tree, pressure levels, containers, handle pools, chunk storage, frame epochs |
| `core-math` | W | Types, SIMD, **the coordinate, depth and unit conventions as executable tests**, BVH, frustum primitives, curves, RNG |
| `core-jobs-and-concurrency` | W | One job system, thread roles, **access-declaration-driven scheduling**, coroutines, cancellation, priorities, non-blocking workers |
| `core-assets-and-io` | S | Asset identity, the virtual filesystem, the package format, async loading, compression |
| `project-and-plugins` | W | Layering enforced, modules and dependencies, layered typed configuration |
| `engine-architecture` | S | Layers, the module system, deterministic startup and shutdown |

**Closing artefact**: `samples/01-headless-host` — loads a package from the virtual filesystem, runs
a parallel job graph over reflected data, prints its memory budget tree, and shuts down
deterministically.

**Exit criteria**

- The identity manifest gate fails a renamed field with no tombstone, and passes when tombstoned
- Reflection round-trip golden tests pass; generated reflection data is reproducible
- The job system's throughput benchmark meets its threshold; thread sanitizer is clean
- Startup and shutdown order is identical across 100 runs
- Memory budgets report; an over-budget domain raises pressure
- A layering violation between core modules fails the build

**Risk spike**: the reflection generator's incrementality. If regeneration is not fast and
reproducible, every later capability pays for it on every build.

---

## M2 — World

*Entities, nodes, scenes — and the determinism hooks everything after this assumes.*

**Entry**: M1 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `ecs-core` | W | Entities, components, archetypes and chunks, queries, systems, **structural change deferral**, change detection, snapshots, multiple worlds |
| `scene-graph-and-nodes` | W | The node façade, hierarchy, transforms, visibility, lifecycle, **the coherence invariants as tests** |
| `serialization-and-prefabs` | W | Both serialization modes, prefabs, overrides against stable identifiers, variants, migration, **cook-time hierarchy flattening to archetype blocks** |
| `engine-architecture` | W | Servers, the ECS/scene duality, **the fixed-tick loop with the interpolation alpha**, the deferred command queue, feature slicing |
| `simulation-and-determinism` | S | **The simulation clock, epochs, the commit boundary, seeded random streams, state classification, hierarchical state hashing** |
| `core-assets-and-io` | W | Cooked assets, streaming, hot reload |

**Closing artefact**: `samples/02-headless-sim` — loads a scene, ticks 10,000 fixed steps, prints a
hierarchical state hash, and reproduces the hash exactly on re-run and after snapshot/restore.

**Exit criteria**

- The state hash is identical across runs, across process restarts, and across restore-from-snapshot
- A scene round-trips text → binary → text with no semantic change
- A prefab override survives a field rename with a tombstone
- Cooked scenes load as archetype blocks; a bulk-copy activation is benchmarked
- The coherence invariant tests pass: no node duplicates component data, no orphaned entity
- Structural changes are observable only at flush points, proven by a test that tries otherwise

**Risk spike**: cook-time flattening. Whether an authored hierarchy really lowers to chunk-shaped
blocks without runtime fixup is the assumption the whole storage decision rests on.

**Corrections recorded at M2's close.** The row above is the plan; where the implementation came out
differently the difference is written down in
[the capability matrix](roadmap/capability-matrix.md#where-m2s-tiers-were-thin-and-what-m3-closed) rather than edited
away here. The four a reader of this row should know: the spike found that a cooked block needs a
**reference fixup pass after the copy**, which the specifications already allowed for and which
makes M6's activation a copy *plus* a pass rather than a copy; `core-assets-and-io` reaches Working
on the cook path with **hot reload not implemented at all**, so two of its ten requirements are
still at none; the state hash covers **only declared subjects**, which in the closing artefact is
four of seventeen; and `engine-architecture` reaches Working with **all seven servers still the null
implementation**, because the first backend is M3's — the loop, the ECS/scene duality, the deferred
command queue and feature slicing are what is Working here.

---

# First playable

## M3 — First light

*A frame, drawn by a graph nobody wrote a barrier for.*

**Entry**: M2 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `rhi-and-render-graph` | W | Explicit RHI, the **null backend**, Vulkan, **automatic barriers, aliasing and scheduling**, parallel recording, descriptors, capability model |
| `shader-system` | W | Slang → SPIR-V, permutations, reflection-driven binding, the shader library and cache, hot reload |
| `rendering-architecture` | W | The render server, the simulation-to-render snapshot, frame structure, the **GPU scene**, render targets, debug visualisation, deterministic submission |
| `rendering-geometry-and-resources` | W | Mesh representation, vertex compression, instancing, texture formats |
| `rendering-materials-and-shading` | W | The BRDF, shading models, IBL, the standard material, parameter storage |
| `rendering-forward-clustered` | W | The cluster grid, light assignment, depth prepass, sorting, pass order |
| `rendering-lighting-and-shadows` | S | Light types, physical units, the shadow atlas, cascades, filtering |
| `rendering-culling-and-lod` | S | Spatial indexing, frustum culling, LOD selection |
| `core-math` / `rendering-architecture` | — | **Camera-relative rendering and reversed-Z proven by test, not by convention** |

**Closing artefact**: `samples/03-first-light` — a lit, textured, shadowed scene with a moving
camera, guarded by golden images, rendering identically through the null backend in continuous
integration.

**Exit criteria**

- Golden-image tests pass on Vulkan; the null backend records the same graph
- A grep-level check finds no barrier call outside the render graph
- Transient aliasing reduces peak GPU memory measurably against a no-aliasing build
- Shader hot reload replaces a material's shader without a restart
- A scene one million units from the origin renders without visible precision loss
- Frame submission order is identical across runs
- The XR prerequisite checks — multi-view capable, runtime-driven timing, late-latch seam — pass

**Risk spike**: render graph scheduling with async compute. Get the barrier and aliasing model
right against a hard case before thirty passes depend on it.

---

## M4 — Playable

*A game, written in Swift, over a boundary that will not move.*

**Entry**: M3 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `native-abi` | W | The flat C interface, **the versioned append-only table and its CI gate**, handles, marshalling, `cy::Expected` across the boundary, module entry points, hot reload |
| `swift-scripting` | W | `CyberdyneKit`, the generated overlay, behaviours and systems, macros, ARC and concurrency rules, hot reload |
| `input-and-actions` | W | Users and devices, actions, mapping contexts, bindings, processors, triggers, **fixed-tick sampling and buffering** |
| `camera-system` | S | The four separated concepts, a camera stack, follow and orbit, the lens model, render view production |
| `physics` | W | `PhysicsServer`, Jolt behind it, components, fixed-step integration, collision events and filtering, queries, the character controller |
| `gameplay-framework` | S | Gameplay lifetime, the context, **one validated command stream**, control sources and bindings, the simulation clock, deterministic random streams |
| `audio` | S | The audio driver layer over miniaudio, the bus graph, playback, spatialisation |
| `core-platform-abstraction` | W | Input devices, fixed-step input handling, system integration |

**Closing artefact**: `samples/04-character` — a third-person character controller written entirely
in Swift: move, jump, collide with a level, hear footsteps, follow with a camera.

**Exit criteria**

- The sample contains no C++ gameplay code
- The ABI gate rejects a reordered or removed entry and accepts an appended one
- A Swift module hot-reloads while the sample runs, preserving world state
- The simulation reads input only through the command stream, proven by a test that bypasses it and fails
- Physics is deterministic across runs on one platform; the character controller test suite passes
- Swift API tests run in continuous integration

**Risk spike**: hot reload across the ABI with live Swift objects. If reload cannot preserve state,
the live-editing story in M5 changes shape.

> **The spike's answer was yes, with a condition, and one exit criterion above is half met.** State
> survives a reload — including a type whose layout changed and an ARC-managed `String` — but only by
> *serialize, migrate by name, recreate*: keeping instance pointers across a module swap reads
> correct values until the layout changes and then corrupts silently, and `dlclose` of a Swift image
> is unsafe whenever the Swift runtime outlives it, so a retired generation is never unloaded. What
> is not met is the words *while the sample runs*: the loader reloads and is tested doing it;
> `samples/04-character` declares `hot_reload = true` and never calls `reload()`. Both are recorded
> where the outcome belongs — `tools/roadmap/milestones/m4.toml` and
> [the capability matrix](roadmap/capability-matrix.md#where-m4s-tiers-are-thin) — rather than
> silently in a plan nobody re-reads.

---

## M5 — Authorable

*An editor that is a client, and a crash that costs a restart rather than a session.*

**Entry**: M4 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `editor-rust-application` | W | The Rust workspace, hosting modes, safety rules, MVVM with services and commands, the editor SDK generated from the ABI |
| `editor-documents-and-transactions` | W | Documents, **transactions as the only persistent write path**, deltas, nesting and coalescing, the journal, autosave and recovery |
| `editor-architecture` | W | The editor as an engine application, play mode, the inspector, hierarchy and asset browser, project settings |
| `editor-viewport-and-gizmos` | W | Viewport transport, navigation, **engine-side picking**, gizmos, snapping, view modes, degradation |
| `editor-ui-ux` | W | Docking and workspaces, density, the command palette, keyboard-first operation, the generated inspector, validation surfacing |
| `asset-import-pipeline` | W | The importer framework, texture and model import via glTF and meshoptimizer, the cook cache, dependency tracking |
| `live-editing` | W | Live edit as a compilation step, policies, asset and shader reload, play modes, the live bridge, runtime inspection |
| `text-and-fonts` | S | `TextServer` over HarfBuzz, ICU and FreeType; glyph atlases for viewport and overlay text |
| `project-and-plugins` | C | Plugins over the C ABI, extension points, lifecycle, resolution and lockfile, trust tiers |

**Closing artefact**: `samples/05-editor-session` — a scripted editor session: open a project,
import a glTF asset, place and manipulate it with gizmos, undo, save, enter play mode, and — with
the hosted runtime killed mid-session — recover without losing work.

**Exit criteria**

- Killing the runtime process leaves the editor running with an unsaved document intact
- Every persistent mutation in the session goes through a transaction, proven by an audit hook
- Undo/redo round-trips the full session; the journal replays after a simulated crash
- Picking returns the object the renderer actually drew, including instanced and skinned cases
- The generated inspector edits a reflected type with no per-type editor code
- Editor headless tests run in continuous integration on all three platforms

**Risk spike**: the live bridge — latency and state synchronisation over the out-of-process
boundary, including the console-shaped case where the runtime is not local.

*Measured, and the answer is that out of process is affordable.* Against a runtime holding 1,000
entities and applying every drag through the real `CyInterface` table, the process boundary costs
about 60 microseconds at the median — 0.001 ms in process against 0.059 ms over a Unix domain
socket. What dominates is the runtime's own frame: correctly frame-coupled, in process and out of
process are within 0.3–1.3 ms at p50 and 1.4–3.5 ms at p99, and the 8.6 ms median is paid
identically with no boundary at all. Two traps were found and are written down rather than
rediscovered: an editor running at exactly the runtime's tick rate **phase-locks** into a stable
full-frame stall, which a vsynced editor on a 60 Hz monitor walks straight into; and 1% packet loss
at TCP's minimum retransmit timeout costs a 7x worse tail than a tuned application retransmit on the
same link, which is the common case during a paced drag because nothing else is in flight to trigger
a fast retransmit. The viewport transport was measured separately at task 4.1 and is where the
decision actually bites: a queue rather than a mailbox puts the image the user is dragging against
half a second behind.

**What closed, and what did not.** The table above is the plan M5 was written against. The record of
what the milestone actually reached is [`status.yaml`](roadmap/status.yaml), and where the two differ
the record wins: `editor-architecture`, `editor-ui-ux` and `live-editing` reached **Seed** rather
than Working, and `project-and-plugins` did not move from Working — the evidence for each is in
[the capability matrix](roadmap/capability-matrix.md#where-m5s-tiers-are-thin) and in
`tools/roadmap/milestones/m5.toml` beside `[criterion.expect_tiers]`. Three capabilities the row
above does not name reached **Complete**: `core-type-system`, `scene-graph-and-nodes` and
`native-abi`. The single fact behind three of the four shortfalls is that the hosted runtime the
editor talks to is a test stub: no engine binary accepts a live-bridge connection, so play mode, the
runtime-owned worlds and the bridge's engine half are all absent. Correcting this row itself belongs
to [`implement-m5b-operable`](../openspec/changes/implement-m5b-operable/proposal.md), which inserts
**M5.5 · Operable** between M5 and M6 and gives its own reasoning for the `editor-ui-ux` correction.

*(**M10's record audit closed the divergence this paragraph describes.** Until M10 the correction
lived only here, in prose, while the matrix column went on claiming what the gate had refused —
which is how `m9:record-matches-plan-history` came to find nineteen such cells over four closed
columns. `editor-architecture` and `live-editing` now read **S** in the M5 column and
`project-and-plugins`' Complete cell has moved to M11, so the plan agrees with the record for all
three and the sentence above is a record of the gate's finding rather than of a live disagreement.
`developer-workflow-and-just`'s **W** also moved out of this column — to M6 — because at M5 the whole
Content category of the required recipe surface was `_not-implemented` stubs. Per-cell evidence:
[M10's record audit](roadmap/capability-matrix.md#m10s-record-audit-nineteen-cells-over-four-closed-milestones).)*

---

## M5.5 — Operable

*A window somebody can use, and the same editor an agent can drive.*

**Entry**: M5 green.

**Why this milestone exists at all.** M5 delivered docking, workspaces, the command palette,
keyboard-first operation and the generated inspector — *as models a test can drive, with no window,
no graphics device and no interface toolkit.* That satisfied `editor-ui-ux` to the letter and
satisfied `delivery-roadmap` not at all, which requires a milestone to end in an artefact exercising
its capabilities **through the entry points a user would use**. The row was wrong when it was
written. M5.5 is the repair, and it is an insertion rather than a renumbering so that every existing
reference to M6 through M11 stays valid.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `editor-ui-ux` | W | A window and an application shell; docking, floating, tabbing and named workspaces that persist and reset; the hierarchy, the generated inspector and the content browser; the command palette over M5's registry; keyboard-first operation with chords; density modes; validation surfacing; notifications |
| `editor-visual-language` | W | The identity lockup; charcoal chrome around a viewport that carries the screen's colour; the semantic palette; X red, Y green, Z blue everywhere; selection as a thin gold outline; gizmos separated by **shape**; the orientation widget that is not a manipulator; chrome as overlay; surfaces by luminance step; the engine's own vocabulary, enforced by a gate |
| `editor-agent-interface` | W | MCP behind an engine-owned interface, gated at build time; tools projected from the command registry with declared exclusions; resources for the hierarchy, an entity, the selection, assets and diagnostics; manipulation through the interactive path; **source authoring as a transaction with a computed effect class**; attribution; scope, confirmation and budget |
| `editor-viewport-and-gizmos` | W | Unchanged from M5 in tier and **exercised interactively for the first time**: the viewport presents another process's rendered image with no copy through the CPU, camera navigation, the transform gizmo's four modes and three states, snapping, pivots and per-axis numeric entry, and the scene orientation widget |
| `live-editing` | S | Unchanged. The cross-process viewport transport is new and measured; the runtime on the other end of it is still a fixture |

**The toolkit decision, taken here.** egui 0.36.1 + `egui_dock` 0.21.1 over wgpu 30.0.1, and the
minimum supported Rust version moves to 1.95 with it. Every candidate presented an engine-rendered
GPU image with no CPU copy; what decided it was **byte-exactness** — Dear ImGui's wgpu renderer puts
a gamma curve over the whole draw list, so "is the viewport the engine's image" would have had to be
a tolerance rather than `==` — and accessibility, which the Dear ImGui ecosystem structurally cannot
offer. The decision is kept reversible by a containment test: exactly one crate may name a toolkit,
that crate and the viewport transport may name a graphics API, and nothing at layer 2 or below may
name either.

**Closing artefact**: two, because the milestone has two audiences.
`samples/05b-editor-window` — the editor's window opened on a project and operated through
**synthesised X11 input**, so it cannot tell the driver from a hand on a keyboard: three entities
created from the keyboard, a selection by pointer, undo and redo, a command found by typing in the
palette, and a save, each observed through the journal, the outliner's drawn rows and the viewport's
pixels. `samples/05b-agent-authoring` — an agent over the Model Context Protocol composing a scene
in an empty project, writing a gameplay script that undo removes and redo restores byte for byte,
finding its own changes in the editor's history with actor, session and intent, and being refused
what its scope does not grant. The second needs no display and is therefore the half continuous
integration runs.

**Exit criteria**

- The editor opens on a project and a person can select and manipulate
- The viewport image is the engine's, proven by comparison against a direct render
- A gizmo drag returned to its origin restores exact values, in one transaction
- An agent composes a scene, writes and reloads a script, and observes the result
- An agent's source write is a transaction with an actor and an intent
- No agent capability exceeds a human one
- All four profiles build clean and `just test-all` is green in each

**Risk spike**: the interface toolkit, on the one criterion that dominates — can the viewport present
an engine-rendered GPU image without a copy through the CPU?

*Measured, and the answer changed what the milestone had to build.* All three candidates could, at
**below the measurement floor** — 0.44 ms with the image and 0.44 ms without, the same at 4K —
against ≈ 2.0 ms per frame quiet and ≈ 8.9 ms at 4K for the device→host→device path the deferral had
implicitly been costing. A second spike settled cross-process synchronisation: `wgpu_hal`'s
`Adapter::open_with_callback` hands over the extension list before `vkCreateDevice`, so pushing
`VK_KHR_external_semaphore_fd` and passing the result to `create_device_from_hal` is about thirty
lines, and timeline semaphores then export and import over `OPAQUE_FD`. Three findings are written
down rather than rediscovered: **three images minimum and four preferred**, because one wedges and
two cost a whole editor frame of latency; the editor's wait must be a **bounded host wait and never
a queue wait**, because an unsatisfiable value staged on the queue is an editor that renders nothing
and cannot be closed; and the runtime must **announce after `vkQueueSubmit`**, which is the only
reason a SIGKILLed runtime leaves the editor alive on its last complete frame.

**What closed, and what did not.** The record of what the milestone reached is
[`status.yaml`](roadmap/status.yaml); where this row and the record differ the record wins. Three
tiers advanced — `editor-ui-ux`, `editor-visual-language` and `editor-agent-interface`, each to
Working — and the evidence and the shortfalls are in
[the capability matrix](roadmap/capability-matrix.md#where-m55s-tiers-are-thin). Two of the seven
exit criteria above are **half met, and the artefacts report it on every run rather than narrowing
the claim**:

* *A person can select and manipulate.* Selecting works, through the outliner and the keyboard.
  **Manipulating does not**: `DocumentService::open` builds an empty schema because there is no
  world loader, so a gizmo has no `Transform` to bind to and a drag over the viewport commits
  nothing. The gizmo itself is real and its one-transaction rule, its three states, its snapping,
  its pivot and space modes and its per-axis numeric entry are held by tests against a document that
  declares a `Transform` — which a test can build and an editor cannot yet open.
* *An agent composes, writes and reloads a script, and observes the result.* Composing and writing
  close. **Reloading and observing do not**: no `--agent-scope` grants the `external-effect` class,
  so no agent connection can start a build; and the code that claims frames from the viewport
  transport lives in the window's crate, so `--mcp`, which runs without a window, has no host for it.

And the sentence "the viewport image is the engine's" is **stronger than what runs**. The comparison
itself is as strong as it can be made: twenty consecutive imported frames at 1920×1080, each of
8,294,400 bytes, matched an independently recomputed image **byte for byte** — `==`, not a tolerance,
which is the property the toolkit was chosen for — with the comparison shown to fail on a one-bit
change to a single channel. What the other end of it is, though, is `cy-viewport-publisher`: a Vulkan
fixture in the editor's own Cargo workspace. No binary under `src/` speaks this wire, so **the
engine's renderer has still never appeared in the editor**. Closing that is `live-editing`'s, which
is why it stays at Seed — and it is also why the transform gizmo, normative in
`docs/design/images/transform-gizmo.png` since M3, is drawn in no pixel at M5.5:
`editor-viewport-and-gizmos` assigns gizmo drawing to the engine, and the engine is not there.

`just roadmap-milestone m5b` evaluates 112 criteria — 101 inherited from M0 through M5, 11 new — and
**it exits zero without exiting zero reliably, for a reason that is not M5.5's**:
`integration.physics_jolt` traps at teardown about one run in forty, the ledger runs it at least five
times, and two of the four full runs at this gate went red on it. The defect is real, it is in
`src/backends/physics-jolt/`, and it is the first finding in six milestones that is a defect in the
engine rather than in a gate or a claim. It is written up with its stack in
[the capability matrix](roadmap/capability-matrix.md#where-m55s-tiers-are-thin).

The gate found two further defects worth carrying forward by name, both of them silences rather than
crashes, and both recorded with their fix in the capability matrix: **the editor survives its
runtime being killed and does not say so** — the sentence is computed and drawn only when there is
no image, which after a death is never — and **clicking in the viewport reports that no frame has
arrived while the overlay beside it counts a thousand**, because nothing in the product calls
`Viewport::pump`.

---

# Production scale

## M6 — Scale

*A world larger than memory, and a build that is a graph rather than a script.*

**Entry**: M5.5 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `build-and-packaging` | W | The derivation graph, derivation keys, explicit inputs, immutable artefacts, the derived data cache, the build service, precise invalidation, packages, chunk-level patching |
| `world-partition-and-streaming` | W | Partitioning, stable cell identity, spatial binding, **cells cooked in ECS-native form**, streaming sources, shapes and prediction, channels, priorities and deadlines, staged atomic activation, layers, HLOD, the persistence overlay |
| `residency` | W | **Shared policy with separate storage: importance, priority, deadlines, budgets, pressure, eviction, churn control** |
| `virtual-texturing` | W | Virtual address spaces, page tables, tiles, the physical cache, the resident mip tail, GPU feedback, prefetch, runtime producers |
| `save-and-persistence` | W | The overlay as the save, scopes and traits, persistent identity, dirty tracking, the journal, atomic generations, migration |
| `rendering-culling-and-lod` | W | GPU-driven culling, visibility ranges, HLOD, shadow caster culling |
| `asset-import-pipeline` | ~~C~~ **W** | ufbx, xatlas, mesh processing, cook profiles, packaging — see the correction below |
| `core-assets-and-io` | ~~C~~ — | Streaming under the residency policy — **not done**; see the correction below |

**Closing artefact**: `samples/06-open-world` — a multi-kilometre world traversed continuously at
speed: cells stream in and out, textures page, the game is saved, quit, reloaded, and resumes in the
same state. Then a content change is cooked, packaged, and shipped as a patch.

**Exit criteria**

- Continuous traversal holds the frame budget with no hitch above threshold, measured over a fixed route
- Residency and activation are provably separate: a test holds bytes resident with simulation off
- A cold build and a cache-warm build produce byte-identical artefacts
- A one-asset change invalidates only the derivations that depend on it
- A patch applies atomically and is rolled back cleanly when interrupted
- Save/load round-trips an unloaded region's state; save generations are atomic under kill -9
- Virtual texture feedback never blocks a frame; the mip tail guarantees a frame is never missing

**Risk spike**: the derivation key model. If keys are not precise, the cache is either wrong or
useless, and every later milestone builds on top of it.

**What closed here, and what did not.** M6's gate checked each planned tier against the code rather
than against this table, and **five capabilities the plan had reaching Complete did not**. Six rows
advanced, all to Working: `build-and-packaging`, `residency`, `save-and-persistence`,
`virtual-texturing` and `world-partition-and-streaming` from nothing, and
`rendering-culling-and-lod` from Seed. The five that did not, with the requirement that stops each:

- `asset-import-pipeline` — FBX through ufbx, xatlas, mesh processing and cook profiles are real and
  are what makes this the milestone someone can bring a model through. "Model import" also requires
  skeletons, animations and a prefab, "Texture import" requires BC7 and ASTC variants where the
  importer reads Targa alone, and "Virtual geometry cooking" is a whole requirement whose subject is
  M7's. **Complete moves to M8.b.**
- `core-assets-and-io` — the scope line above is "streaming under the residency policy" and nothing
  under `src/core/assets/` changed in this milestone at all. Its own header still reads "STREAMING IS
  M6. There is no residency budget driven by renderer feedback, no per-mip request and no priority
  derived from distance here." **Complete moves to M7.**
- `core-memory-and-containers` — also untouched here. "Memory diagnostics" requires attribution by
  domain, type, thread, world cell and asset; three of the five axes have no field to report into.
  **Complete moves to M7.**
- `serialization-and-prefabs` — the authoring schema landed and is what made `DocumentService::open`
  produce a schema instead of an empty one, which was M5.5's handover and this milestone's first job.
  "Apply and extract" has no implementation anywhere in the tree and no recorded exemption.
  **Complete moves to M8.b.**
- `rendering-geometry-and-resources` — "Skinning" reads bone matrices from the **GPU pose world**,
  which is `animation-and-skinning` at M8.b, and the matrix's own rule forbids Complete before a
  prerequisite reaches Working. The module refuses the value in code rather than working around it —
  `SkinningDescriptor::validate()` returns `NotImplemented` naming M8.b and the suite asserts it.
  **Complete moves to M8.b**, which is the tier moving rather than the requirement being scoped away.

The reasoning per row, with the evidence, is in
[the capability matrix](roadmap/capability-matrix.md#where-m6s-tiers-are-thin) and in
`tools/roadmap/milestones/m6.toml` beside `[criterion.expect_tiers]`.

*(**And M10's record audit looked at a seventh row of this milestone, in the direction no gate had
looked for — then M10's closing gate refused what it found.** The audit argued that
`developer-workflow-and-just` reached **Working** here and nobody wrote it down: the whole Content
category of the recipe surface was `_not-implemented` stubs at M5 (`git show
412c955:just/content.just`) and the stubs were gone by M6 (`ec8d087`). Its **W** cell did move from
M5's column to this one, and that move stands. **The record did not move, and holds `seed` from
M0.** The same audit argued `testing-and-quality` at M3, `build-system-and-platforms` at M4 and
`thirdparty-dependencies` at M8.b, and all four were reverted for one reason: **none of the four has
a criterion of its own**, and the only `expect_tiers` entry naming any of them expects `seed`, which
an exit-tier FLOOR can never contradict. A tier recorded on an argument that nothing re-checks is the
defect `record-matches-plan-history` exists to catch, so that criterion is a **declared gap again**,
over exactly these four cells, closing at M11. The evidence each row would need is preserved as the
brief for the criterion that would close it, in
[M10's record audit](roadmap/capability-matrix.md#m10s-record-audit-nineteen-cells-over-four-closed-milestones).)*

---

## M7 — Fidelity

*Film detail at a budget an arbiter holds.*

**Entry**: M6 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `material-compiler` | W | Graph → IR → closures → program, optimisation passes, the GPU material table, classification and binning, quality tiers, cost analysis, cooking |
| `virtual-geometry` | W | The asset, clusters, the crack-free hierarchy, geometric error, geometry pages, the always-resident root, GPU streaming feedback, GPU traversal and cluster culling, the visibility buffer and material resolve |
| `virtual-shadows` | W | Receiver-driven pages, clipmaps, the page cache, precise invalidation, update classes, the budget, derived bias, the fallback chain |
| `temporal-rendering` | W | One framework: jitter, derived motion vectors, history, invalidation, reprojection |
| `rendering-post-processing` | W | Chain order, AO, fog, exposure, DOF, bloom, tonemap, colour grading, AA, temporal upscaling |
| `rendering-global-illumination` | W | The GI scene, surface and radiance caches, distance fields, screen/software/hardware tiers, sample confidence, reflections on the same infrastructure, probes, baking |
| `denoising` | W | One accumulation and edge-aware filter for every stochastic signal |
| `ray-tracing-infrastructure` | W | Structure lifecycle, geometry adapters, ray queries, capability gating |
| `rendering-architecture` | C | **The renderer budget arbiter**, renderer profiles, pipeline configuration |
| `rendering-lighting-and-shadows` | W | Area lights, decals, light functions, channels, stochastic many-light |
| `atmosphere-sky-and-clouds` | S | An analytic sky, sufficient for GI's sky term |
| `rhi-and-render-graph` | — | A **Metal seed**, to expose Vulkan-specific assumptions while they are still cheap |

**Closing artefact**: `samples/07-fidelity` — a film-detail interior and exterior with millions of
source triangles, dynamic lighting, indirect illumination and reflections, holding a frame budget
while the arbiter reallocates under a scripted load spike.

**Exit criteria**

- The scene holds its frame budget; the arbiter's allocations converge without oscillation under a step load
- Every paged system degrades along its declared axis: a coarse root, a resident mip tail, a stale-but-valid shadow page — a frame is never missing, only coarser
- Golden images for GI, reflections and post-processing pass within tolerance
- The material compiler's IR round-trips; a graph and a hand-written material produce identical programs
- Node previews use the real compiler, proven by comparing preview and final output
- Ray tracing disabled falls back to software tracing with no visual discontinuity beyond tolerance
- The Metal seed renders the M3 golden scene

**Risk spikes**, in this order: the material IR and closure lowering; the budget arbiter's control
loop; virtual geometry's cluster hierarchy build and GPU traversal.

*Both spikes were run before the implementing work and both met their criterion; what they found is
in `implement-m7-fidelity/design.md` §1 and §2. The third was not given a spike, by decision, because
the first two settle what it may assume.*

**What M7 actually reached, which is not what it planned.** The table above is the plan M7 was
written against; the record is [status.yaml](roadmap/status.yaml), and where the two differ the
record wins. The proposal named **nine** capabilities reaching Complete here. Three did:
`rendering-materials-and-shading`, `residency` and `virtual-texturing`. Six did not, and their
**C** cell has moved to M8.b:

- `rendering-architecture` — the budget arbiter is built, and it is the best-certified thing in the
  milestone: 71 step magnitudes, none oscillating, with a negative control that oscillates 252 times
  when every controller runs its own copy of the decision. What is missing is a renderer that uses
  it. `cy::rendering-arbiter` is linked by its own two test binaries and by `samples/07-fidelity`,
  and by nothing else in the tree; the seven `SubsystemController`s exist only inside
  `samples/07-fidelity/spike.cpp`, against a hard-coded cost table, with one of the seven costs
  substituted from the device. "Subsystem controllers SHALL … report their measured cost to the
  arbiter" has no instance in `src/`.
- `rendering-culling-and-lod` — the GPU dispatch landed and agrees with M6's CPU reference, but
  `GpuCullPass::upload` refuses `kGpuCullOcclusion` with `NotImplemented`: there is no hierarchical
  depth buffer on the device, so no frame occlusion-culls, and cluster-granular occlusion for
  virtual geometry has no implementation at all.
- `shader-system` — the "Visual material editor" requirement asks for a node-graph material editor
  in the editor. The compiler exists and `cy_material` shows every lowering stage on a command line;
  the editor has no material graph panel.
- `core-assets-and-io` — `StreamingSystem` closes "Streaming". "Hot reload" names cooked outputs and
  `AssetSystem::reload` refuses a packaged asset, and the "Development file serving" scenario has no
  implementation: `RemoteFileProvider` is an interface with no transport.
- `core-memory-and-containers` — all four missing attribution axes were built, and nothing pushes
  one: `MemoryAttributionScope` appears nowhere outside `src/core/memory/`, so in a running engine
  every axis but `thread` reports `unattributed_bytes`.
- `editor-viewport-and-gizmos` — the row that moved furthest without arriving. The engine's own
  renderer fills the viewport over a dma-buf, the engine generates the gizmo geometry, a drag lands
  on a published handle and undo restores the value exactly. But the editor and the runtime are
  still two worlds — `.cyworld` is read by nothing under `src/` or `tools/` — so "what you see is
  what ships" does not hold, and engine-side picking is unexercised.

The exit criterion "the Metal seed renders the M3 golden scene" is **not evaluated**: it needs an
Apple GPU and this project has no macOS runner with one. `src/backends/rhi-metal/README.md` says of
`src/device.mm` that nothing in that file has been compiled or run.

*The closing gate's own findings — how the ledger was run, what its three failures were, and what is
thinner than a tier suggests — are in*
[Where M7's tiers are thin](roadmap/capability-matrix.md#where-m7s-tiers-are-thin). *The one a reader
of this page should carry forward: none of the thirteen renderer modules M7 added is assembled into a
frame by anything but a test or `samples/07-fidelity`, and neither is M3's `cy_rendering_forward`.
That is what M8.a will trip over.*

---

## M8.a — Authorable

*A scene a person builds by hand, and a game they can press play on.*

**Entry**: M7 green.

**Why this milestone exists at all.** M8 as planned advanced ten capabilities; M6's gate demoted five
more into it, and three pieces with no home belonged there too. That is twenty-two capability
advances against M9's ten and M10's three. And its named risk spike — one graph IR that seven
consumers must agree on — is the hardest thing on the ladder. So *the milestone that lets someone
build a scene and press play was sitting behind the riskiest design work in the project.* Nothing in
M8.a is architecturally unsettled. It was blocked only by sharing a number with work that is.

The rule that produced the split is in `delivery-roadmap` rather than applied once: a milestone whose
artefact cannot be reached without its own risk spike succeeding, when some other coherent artefact
could be reached without it, contains two. It is not a rule about size.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `editor-ui-ux` | — | **Primitive creation**: box, cylinder, sphere, plane and capsule, through the existing transaction path. There is no `scene.create` today and no shape generation anywhere in the tree |
| `asset-import-pipeline` | W | **Import from inside the editor** — there is no `asset.import` command, so content is cooked outside it — and **OBJ**, which was in no specification at all until this change |
| `physics` | W | **The ECS bridge.** `physics` requires bodies expressed as ECS components; `src/physics/`, which would register them in a world, create bodies and write `LocalTransform` back, does not exist. M4's gate flagged it and it is still absent |
| `gameplay-framework` | S | Spawning, and enough of the session model for play mode to host a world |
| `serialization-and-prefabs` | C | The requirement M6 could not close, so that what the editor authors is what the runtime loads |
| `editor-documents-and-transactions` | C | A created primitive, an imported mesh and an added body are each one transaction, and undo returns the world to empty |

**Closing artefact**: `samples/08a-authoring` — **create a sphere, drop it on a box, press play,
watch it fall, stop, and undo back to an empty world.** One run that exercises primitive creation,
the gizmo, the physics ECS bridge, play mode and the transaction system together, and that cannot be
satisfied by a fixture.

**Exit criteria**

- A primitive created in the editor is a transaction like any other, and undo removes it
- An FBX and an OBJ both import from inside the editor, through the same derivation key as every
  other format
- A body added in the inspector simulates when play is pressed, and stops when it is released
- Undo returns the world to empty, exactly — no residue in the document, the scene or the overlay
- What the editor authored is what the runtime loaded: the same world, not a fixture beside it

**Risk spike**: none, and that is the point. The one thing worth measuring first is where the
editor's document and the engine's scene meet, because M7 left them associated in first-seen order
by a file whose own header calls itself a stand-in.

**What M8.a actually reached, which is not what it planned.** The table above is the plan M8.a was
written against; the record is [status.yaml](roadmap/status.yaml), and where the two differ the
record wins. The proposal named **two** capabilities reaching Complete here. Neither did, and each
**C** cell has moved:

- `serialization-and-prefabs` — the engine now reads and writes `.cyworld`, the file's own type
  section is the schema a transaction addresses, a type this build has never heard of survives a
  round trip byte for byte, and `cy::scene::serialization::apply_transaction` decodes all twelve of
  the editor's operation variants. That is the requirement M6 could not close, and it is not the
  capability. **"Apply and extract" still has no implementation** — pushing an instance's overrides
  back onto its prefab, and lifting a subtree into a new prefab asset — and
  `src/scene/serialization/README.md` has said so since M2 and still says so. Two of the twelve
  operation variants M8.a taught the engine to decode are `InstantiatePrefab` and
  `SetPrefabOverride`, and the engine **counts them as ignored**: the reader refuses to guess their
  length, which is correct, and then does nothing with them. The **C** cell moves to M8.b, beside
  `live-editing` and `editor-viewport-and-gizmos`, which is where the remaining prefab work belongs.
- `editor-documents-and-transactions` — a created primitive, an imported mesh and an added body are
  each exactly one transaction; undo returns the world to empty in the document *and* in the
  engine's world, which the closing artefact checks from both ends; and writing around the
  transaction path is a **compile error** rather than something audited — the gate wrote a probe
  that forges a `WriteToken`, reaches for `content_mut` and reaches for `node_mut` from outside the
  crate, and all three are refused by the compiler. What is missing is not a detail of that
  machinery. **"Source control integration" has no implementation at all**: the requirement
  asks for a provider interface — status, history, diff, check out, revert, submit, lock — with Git,
  Perforce and a null provider behind it, and there is no such trait, no provider, and no null
  implementation anywhere under `editor/`. A capability cannot be Complete with one of its twelve
  requirements unstarted. The **C** cell moves to **M11**, where every other `editor-*` row
  completes.

Both demotions are the practice M5, M6 and M7's gates set: a tier is what the code supports, and the
plan is corrected rather than the record relaxed. **M8.a therefore advances no capability tier at
all.** The other three rows it names — `physics`, `asset-import-pipeline` and `gameplay-framework` —
were already recorded at the tiers it was asked to reach. What the milestone did to those three is
make their records true: `asset-import-pipeline` was Working with no way to import from inside the
editor, `gameplay-framework` was Seed with `hosting: NoRuntime` where pressing play should be, and
`physics` was Working with **no `src/physics/` in the tree at all**, so nothing turned its components
into bodies for four milestones.

Only `physics` moves its `milestone` field, from M4 to M8.a, and that is a **correction rather than
an advance**: the capability's "Physics components" requirement had no implementation when M4
recorded the row, so *"the requirements a real project depends on are satisfied"* was not true of it.
It is the same correction M5.5's gate made when it moved `editor-ui-ux`'s Working out of M5. The
other two rows keep the milestone that last advanced their tier, because that is what the field
means, and inflating it would make the record say something it cannot support.

*The closing gate's own findings — how the ledger was run, what it failed on, and what is thinner
than a tier suggests — are in*
[Where M8.a's tiers are thin](roadmap/capability-matrix.md#where-m8as-tiers-are-thin). *The one a
reader of this page should carry forward: the mesh a created primitive references reaches no
renderer. `cy::render::MeshRenderer` is a name in the scene's component catalogue with no reflected
type behind it, so the artefact's own photograph shows the authored sphere and box drawn as two
identical unit boxes through M3's fixed slots. Authoring is real; what draws it is not yet the
renderer M7 built.*

---

## M8.b — Systems

*Everything a game needs to be played.*

**Entry**: M8.a green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `gameplay-framework` | W | Rules, session fragments, participants and teams, ownership/control/authority, capabilities, tags, phases, spawning, time domains, interaction, features, indexes |
| `gameplay-abilities-and-effects` | W | Compiled ability programs, attributes and modifiers, effects and stacking, costs and cooldowns, targeting, the activation pipeline |
| `visual-scripting` | W | Shared graph infrastructure, typed pins, stable identity, the IR, execution backends, async graphs, semantic merge, debugging |
| `animation-and-skinning` | W | Skeletons and bone LOD, clips and compression, the animation graph, compiled programs, batched evaluation, layers and masks, root motion, IK, retargeting, the GPU pose world, pose sharing |
| `ai-system` | W | Agents as entities, the unified graph with tree/utility/GOAP semantics, compiled behaviour programs, batched perception, knowledge, environment queries, smart objects, AI LOD |
| `navigation` | W | Navmesh over Recast, runtime updates, streaming, A* and funnel, hierarchical paths, flow fields, off-mesh links, avoidance, crowds |
| `ui-system` | W | Dedicated element storage, the retained tree, documents, declarative authoring, layout, `.cyss`, data binding, input routing, the layer stack, the widget set, GPU-driven rendering, accessibility |
| `text-and-fonts` | W | BiDi, line breaking, Arabic joining, itemisation, the shaping cache, localisation. **Working, not Complete** — see below |
| `rendering-2d` | W | Sprites, ordering, batching, tilemaps, 2D lights and shadows, the screen-space SDF |
| `audio` | W | Acoustic geometry, importance tiers, voice virtualisation, effects, interactive audio. **Working, not Complete** — see below |
| `camera-system` | W | Rig graphs compiled to programs, blending, framing, aim, shake, volumes, cuts |
| `serialization-and-prefabs` | C | **Apply and extract** — pushing an instance's overrides back onto its prefab, and lifting a subtree into a new prefab asset — which is the one requirement between this capability and Complete. Inherited from M8.a's closing gate; it belongs here because it is the same editor-over-the-data-model work as `live-editing` and `editor-viewport-and-gizmos`, both of which complete in this milestone |

**Closing artefact**: `samples/08-vertical-slice` — a playable game: a level, characters that
animate and think, abilities with effects, a heads-up interface, sound, and a 2D menu. The cinematic
and the particles are M8.c's, layered onto this same slice rather than a second one.

**Exit criteria**

- Every gameplay-facing capability is at Working, with its own tests
- No graph is interpreted at runtime — an audit finds no per-entity virtual tick in any graph consumer
- Cost is bounded by configuration: 8,000 agents and 100 concurrent effects hold their budgets
- Ability activation, sequence playback and animation evaluation are deterministic under the
  simulation's declared profile
- Interface accessibility checks pass; the interface holds its frame budget

**The spike has run, and it refuted the plan's premise.** One IR cannot serve seven consumers: the
material IR is a hash-consed pure-expression DAG whose identity is a content hash, so it has no back
edges, cannot express a write, re-sorts commutative operands, deletes a value nothing reads, and
evaluates both arms of a select — which animation and AI each forbid by name.
`visual-scripting`'s own "No universal representation" requirement said so before the spike ran.

**What is built instead**: one authoring layer (CyberGraph — nodes, typed pins, stable identity,
semantic diff and three-way merge, migration, opaque preservation of unknown plugin nodes) that all
consumers adopt; one shared pure-expression core generalised from the material IR by four named
extensions; and a lowering per consumer, each to the form its own specification names, all compiling
and none interpreting. `src/rendering/material/` is not modified — it is M7's closed work, and the
generalised core's criterion is reproducing its reference digests instead.

*Reject on sight any later proposal to reach for one IR again because several compilers looks like
duplication. It was measured, not argued.*

**`audio` closes at Working and its Complete cell moved to M8.c, and the closing gate measured why
rather than reading it.** `audio` names Steam Audio in a requirement — "The engine SHALL support
**Steam Audio** as an `AcousticsBackend`" — and two things are true of it on the tree this milestone
closes: `SteamAudioBackend::simulate` returns `ErrorCode::NotImplemented` rather than simulating, and
`-D CY_AUDIO_STEAM_AUDIO=ON` **cannot be configured at all** —

```
CMake Error at _deps/steam_audio-src/core/CMakeLists.txt:235 (find_package):
  Could not find a package configuration file provided by "PFFFT"
```

— because upstream expects PFFFT, IPP and FFTS as pre-installed packages that `deps/manifest.toml`
does not provide. That is this milestone's own spike finding wearing a different name: an option
declared, defaulted off, and never once built. Everything else in `audio` is delivered and is what
Working records — acoustic geometry and its cache, the asynchronous simulation, importance tiers,
voice virtualisation, the effect chain and interactive music, over twenty-seven unit cases, with
`FallbackAcoustics` answering every query in every build, which is what "content SHALL NOT depend on
Steam Audio being present" asks for. The Complete cell moves to M8.c because spatial acoustics is
the same kind of work as particles and cuts: what makes a game feel finished once it already plays.

**`text-and-fonts` closes at Working too, and its Complete cell moved to M11.** M8.b built
`src/text/` — the bidirectional algorithm, line breaking with a Thai dictionary, Arabic joining,
itemisation, the shaping cache and localisation, engine-owned over `cy::core` alone, with
thirty-nine unit cases — and that is a real advance: three `TextCapabilities` flags that read
`false` at M5 are now algorithms that answer. It is not the specification's Complete.
`text-and-fonts` requires in normative text that the engine integrate **HarfBuzz**, **ICU** and
**FreeType**, that it "SHALL load: TrueType and OpenType (`.ttf`, `.otf`, `.ttc`), WOFF and WOFF2,
bitmap", and that "Text SHALL be shaped through HarfBuzz", with variable-font axes, OpenType
feature selection and colour glyphs beside it. None of those three libraries is in
`deps/manifest.toml` — which defers all three by name and states the cost, "the text server lays
out Latin and refuses Arabic" — and the only font the engine can load is `ImageGridFont`, a bitmap
grid. The Complete cell moves to M11, where three third-party integrations belong; the engine can
lay out Hebrew, Arabic and Thai and cannot yet load the font to draw them with.

**AND A LARGER RECORD FINDING THE GATE DID NOT RESOLVE, RECORDED HERE RATHER THAN LEFT.**
`docs/roadmap/capability-matrix.md` schedules **thirteen** capabilities to reach Complete at M8.b.
This milestone planned three of them — `serialization-and-prefabs`, which closes Complete, and the
`audio` and `text-and-fonts` cells that have now moved — and the other twelve —`asset-import-pipeline`, `core-assets-and-io`,
`core-memory-and-containers`, `editor-viewport-and-gizmos`, `input-and-actions`, `live-editing`,
`material-compiler`, `rendering-architecture`, `rendering-culling-and-lod`,
`rendering-geometry-and-resources`, `shader-system` and `swift-scripting` — have **no task in
`implement-m8b-systems`, no criterion in `tools/roadmap/milestones/m8b.toml`, and no entry in its
`expect_tiers`**. They are all recorded at Working or Seed and none was audited here. Most of them
arrived in this column when M6's gate moved five Complete cells "to M8" and M8 was later split, so
the column says M8.b because M8.b is where M8 ended up rather than because anyone planned it. Two
of them, `live-editing` and `editor-viewport-and-gizmos`, are also named as completing here by this
section's own prose above while appearing in no work-table row — the same drift, one document
further along. **Deciding the real destination of those twelve is a planning act with twelve
separate arguments in it, and this gate did not have the evidence to make it**; what it can say is
that `just roadmap-test` passes over the disagreement today, so the plan-consistency check compares
a milestone's work table against the matrix and does NOT compare the matrix's Complete cells against
that milestone's ledger. That missing comparison is the check this finding should become, per
`delivery-roadmap`'s "every audit finding SHALL be converted into an automated check".

---

## M8.c — Spectacle

*What makes a game look finished, once it already plays.*

**Entry**: M8.b green.

**Why this is separate.** M8.b's spike found that milestone larger than its plan assumed — six
compilers where one IR had been budgeted — so the scope was reduced before it was built rather than
after it failed to close. Nothing here is a prerequisite of anything M8.b keeps: `animation` names
VFX only as a consumer of its curves and its pose world, `camera-system` calls sequences "the
principal producer of anticipated cuts" and works without one, `gameplay-framework` requires that the
simulation cannot distinguish a sequence-issued command from any other, and `ml-inference` is
recorded as "`ai-system`, optionally".

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `vfx-system` | W | The graph compiler and IR, GPU-first simulation, derived attribute layout, the unified world and scheduler, data interfaces, GPU scene integration, GPU events, budget scalability |
| `sequencing-and-cinematics` | W | Compiled timelines, exact time, bindings, tracks and authority, batched dispatch, arbitration, capture and restore, seek and skip, preload plans |
| `ml-inference` | S | Model assets, tensors and sessions, the backend abstraction, **the determinism boundary** |
| `audio` | C | **Steam Audio, actually built.** Inherited from M8.b's closing gate, which found the backend returning `NotImplemented` and the dependency unable to configure. Spatial acoustics belongs beside particles and cuts: it is what makes a game feel finished once it already plays |

**Closing artefact**: `samples/08-vertical-slice`, extended — the same playable game with particles
and a cinematic. Extended rather than replaced, because a second slice would prove these systems work
beside a copy of the game rather than inside it.

**Exit criteria**

- Particles and a sequence run inside the existing slice, holding its frame budget
- `-D CY_AUDIO_STEAM_AUDIO=ON` configures, builds and simulates, and the slice's sound goes through
  it — the option M8.b declared and could not build
- A sequence drives cameras through the camera stack and does not write camera transforms
- **The determinism firewall holds: VFX and inference cannot write gameplay state, proven by a
  test.** Moved here with its subjects — a criterion whose subject was deferred is a criterion its
  milestone satisfies vacuously

**Risk spike**: none new. Both graph consumers lower through the layer M8.b built, and the spike that
sized that layer already accounted for them.

**`vfx-system` REACHES SEED HERE, NOT WORKING, AND THE CLOSING GATE MOVED THE CELL AFTER READING
THE MODULE'S OWN README.** What M8.c built is large and is genuinely the shape `vfx-system` asks
for: the asset model, graph compilation to VFX's own IR (an ordered write list whose values are pure
expressions — the form M8.b's spike predicts, because the shared expression core has no Store), the
compiler-derived attribute layout with its precision selection, the unified simulation world and the
global scheduler that merged 720 dispatches where 42,258 unmerged ones would have been, data
interfaces, the bounded readback with its `WriteScope`, importance classes whose reduction is
visible in a photograph rather than in a counter, and the sprite and mesh publication paths. Four
suites, five mutations each proven red, and the vertical slice plays 477 effects and 4,346 live
particles a frame inside the budget M8.b declared.

**What did not exist at M8.c was the GPU compute dispatch, and the specification's own Purpose is
what made that decisive**: *"GPU simulation is the **default**, not an advanced mode"*, over a system
*"targeting millions of particles"*. `decide_path` returned `ExecutionPath::Gpu` for every emitter
that could have it while `device_dispatch_available()` answered false, so all 36,880 emitter-steps of
the slice ran on the CPU — reported honestly, every frame, as
`FallbackReason::DeviceDispatchUnimplemented`, which was far better than a silence and still not the
requirement. **M10 built it** (task 5.1, `src/vfx/gpu/`): `device_dispatch_available()` answers true,
and `DeviceDispatchUnimplemented` no longer exists as a reason — `FallbackReason::NoDeviceInThisWorld`
replaced it, because a `SimulationWorld` holding no device is still the CPU path and must be. The
paragraphs below are M8.c's finding in M8.c's tense; **the record is
[`status.yaml`](roadmap/status.yaml)**, which carries this row at Working from M10.

The slice's own numbers put that path at 1.57 ms a tick against a peak population of
4,346 — **0.36 µs per live particle per tick measured at the peak, so a lower bound on the average**
— and a million particles is therefore at least a third of a second a frame: three orders of
magnitude from the target the capability is defined by, on the machine the capability was built on. Beside it, `Async compute` is a requirement with nothing to schedule,
six of the eight `Renderers` kinds are absent, `Collision`'s response half is absent, and there is
no GPU sort.

The tier ladder's own words decide it. **Working** is *"the requirements a real project depends on
are satisfied … optional and advanced requirements may be outstanding"* — and this specification
pre-emptively refuses that escape for this requirement, in the Purpose, by name. **Seed** is *"the
interfaces, the data model, and the invariants exist … behaviour may be minimal, single-threaded,
unoptimised, or restricted to one backend"*, which is this module exactly. **The W cell moves to
M10**, where the renderer's remaining GPU compute work already lives, and the gap this milestone
leaves is one file and one function rather than a redesign: everything above
`device_dispatch_available()` already reads it. *(That prediction held. M10 added
`src/vfx/gpu/` — particle state resident in device buffers, indirect dispatch driven by
GPU-maintained counts, async compute where the device exposes a queue, and the GPU sort behind
`BudgetLevers::sorted` — and changed one line of `src/vfx/src/runtime.cpp`. `cy::vfx` stays
device-free so `integration.vfx` stays headless. The six renderer kinds beyond `Sprite` and `Mesh`
now publish rows; what remains absent is the compositing, which `src/vfx/README.md` records.)*

**AND THE CONSEQUENCE, STATED RATHER THAN LEFT FOR SOMEBODY TO FIND.** `delivery-roadmap`'s own
milestone table gives M8's artefact as *"a playable vertical-slice game exercising every
gameplay-facing capability at Working"*, and with this cell at Seed that sentence is no longer
literally true of the M8 series: eleven gameplay-facing capabilities are at Working and the twelfth
is at Seed, in the slice, drawing particles, on the CPU. **That is an argument for correcting the
sentence, not for claiming the tier** — a milestone criterion satisfied by moving a record is the
failure mode this ladder's whole apparatus exists to catch, and it is the second time in three gates
that a planned cell had to move rather than be met. Correcting `delivery-roadmap`'s M8 row is an
OpenSpec change against that capability and is recorded here as work M9 inherits rather than
performed by a gate that did not plan it. The **M8.c work table above still says W because it
is the plan the milestone was written against**; the record is `docs/roadmap/status.yaml` and this
paragraph is the correction — the same convention M5's section uses.
*(**M10 discharged this the other way round, and the better way.** `vfx-system` is at Working from
M10, so the twelfth of the twelve counted above is no longer the exception and no sentence needed
correcting: the inherited work was to correct the claim or build the thing, and the thing was built.
The count of twelve excludes `ml-inference`, which `delivery-roadmap`'s M8 row also names and which
is **still at Seed from M8.c** — so that row is not wholly true of the M8 series even now, for a
different capability and for a reason M10 neither examined nor changed.)*

**AND ONE MORE THING THE MILESTONE HAD TO BUILD BEFORE IT COULD BUILD ANYTHING ELSE: A SHADER AND
PIPELINE LAYER.** M8.b's own closing report said a person could build all of M8.b and still not
*see* it without writing their own renderer — `FrameAssembly` hands each pass's record callback to
its caller and every caller in the tree supplied none, so the engine had a frame nothing drew into.
A particle renderer is the first consumer that cannot proceed without one, so the layer was
scheduled here rather than assumed: `src/rendering/pipeline/` is the pipeline state objects, the
per-frame bindings and the five record callbacks each of the frame's stages already declared, and
`src/rendering/particles/` is the first thing to reach the frame through its extension seam. It is
also why this milestone's closing artefact is the first one in this project's history whose picture
is **photographed rather than drawn**.

**`audio` DOES NOT REACH COMPLETE HERE, AND ITS CELL MOVES TO M11 — for the same reason
`text-and-fonts`' did at M8.b's gate, and with a great deal more evidence than the gate that moved
it in.** M8.c ran the experiment rather than repeating M8.b's sentence, and the answer is different
from the report and more specific. **Steam Audio 4.8.1 can be built on this machine** — `libphonon.so`
was produced out of tree — and what it costs is now written out in full in `deps/manifest.toml`:
upstream's own `dependencies.json` requires **PFFFT, zlib, libmysofa and flatbuffers**, of which
flatbuffers is a build-time *tool* this manifest has no field for; IPP and FFTS, which M8.b's gate
named as blockers, are optional and turn off cleanly; `core/CMakeLists.txt` runs
`-fabi-version=6` on every Linux build, which breaks libstdc++'s `<future>` under GCC 13 and is
rejected outright by clang 18, so **both pinned compilers are blocked by one line** and removing it
is a recorded patch; and Valve's Find modules look inside this repository's own `deps/` directory
under `add_subdirectory`. That is a dependency change with five arguments in it — four new manifest
entries, a patch file and cache-variable plumbing every other dependency would then share a
configure with — and it is the same shape as the three text libraries: a third-party integration
rather than a milestone's worth of engine work. `SteamAudioBackend::simulate` still returns
`ErrorCode::NotImplemented`, and `tools/roadmap/milestones/m8c.toml` declares
`steam-audio-configures` as a criterion **that fails today**, deliberately, so that the gap keeps
saying so instead of quietly disappearing from the plan. Everything else in `audio` is what Working
already records, with `FallbackAcoustics` answering every query in every build.

---

# Shipping

## M9 — Integrity

*One command log, read five ways.*

**Entry**: M8.b green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `simulation-and-determinism` | C | Determinism profiles, deterministic parallelism, stable iteration, floating-point policy, generated state codecs, hierarchical hashing, **the validator and the determinism lint** |
| `replay-and-rollback` | W | One command log, external results, snapshot kinds, checkpoints, playback and seeking, presentation tracks, rollback, **the side-effect ledger**, lockstep, resynchronisation, the crash replay buffer |
| `networking-and-replication` | W | Three network modes, the authority model, transports, replication schemas, component replication, baselines and deltas, spawning, RPCs, interest management, priority scheduling, bandwidth, prediction and reconciliation, lag compensation, the dedicated server |
| `diagnostics-profiling-and-crash` | C | Rolling capture, crash artefacts, breadcrumbs, reproduction artefacts, remote and server diagnostics, telemetry export |
| `save-and-persistence` | C | Integrity and confidentiality, storage backends, checkpoints |
| `gameplay-framework` | C | Network integration, save and replay contracts, headless operation, performance contracts |

**Closing artefact**: `samples/09-multiplayer` — a four-player session over a simulated adverse
network: prediction, reconciliation and rollback under packet loss; the session recorded and
replayed bit-exactly; a deliberately injected divergence narrowed to one field on one entity.

**Exit criteria**

- A recorded replay reproduces the final state hash exactly, including after seeking
- Rollback re-simulates without re-applying ledgered side effects, proven by a duplicate-effect test
- An injected divergence is localised to a field by the validator, with the artefact to reproduce it
- A session declaring a determinism profile a subsystem cannot meet is **rejected at configuration**, not discovered later
- Lockstep holds across two platforms for the `CrossPlatform` profile
- A crash produces an artefact that reproduces the crash from the replay buffer
- Bandwidth stays within budget as entity count scales; interest management is measured, not assumed

**Risk spike**: cross-platform floating-point determinism. It either holds under the declared
profile or the profile's definition changes — and that is cheaper to learn at the head of M9.

**What M9 closed on, and the four cells it did not reach.** The work table above is the plan; this
paragraph is the record, written the way M5's, M6's, M7's, M8.a's and M8.c's gates wrote theirs —
the plan is corrected rather than the record relaxed. **M9 completes nothing.**
`replay-and-rollback` and `networking-and-replication` reach **Working** from nothing, and
`design.md` §4's predicted demotion — networking — did not need one, though the closing gate's
adversarial pass found a defect in it that is in the engine rather than in the checking: a
reliable-ordered channel under 25 % packet loss stops delivering to the application for the rest of
the session, nothing detects it, and `m9:reliable-channel-stalls-under-loss` said so on every run
until M10 closed it. **It closed at M10, and it was two defects rather than the one M9 named.** M9
read the cause as *"`abandoned()` has no caller"*; the cause was the replay window —
`already_received()` treated anything more than 32 sequences behind the newest arrival as a replay,
which is right for an unreliable datagram and wrong for one retransmitted across a 7.5 s horizon, so
the frontier froze. The second defect was a default retransmission horizon longer than the session
itself, with no route for an application to state its own. 0 of 4 clients converged at 25 % loss
before; 4 of 4 at nine seeds in nine after, and `abandoned()` has its reader at last. `simulation-and-determinism`
advances Seed → **Working**: profiles declared and refused at *configuration*, deterministic
parallelism, stable iteration, the floating-point policy with the thirteen `<cmath>` functions the
spike measured, the generated codecs, hierarchical hashing, the validator and the lint all landed,
and the one guarantee this project cannot evaluate — cross-architecture lockstep — is *refused* by
`DeterminismConfiguration::require()` rather than claimed. Its criterion is a **declared gap that
runs and fails**, closing at M11: it carried `where = "ci"` until the closing gate read it, and that
would have reported it green the first time continuous integration ran the ledger, because `--ci`
lifts the `where` and the command underneath was a single-leg suite with no second architecture in
it.

Four rows the table marks Complete stay at Working, each because a named requirement is unmet and
each recorded as a running, failing, rung-bearing criterion in `tools/roadmap/milestones/m9.toml`
rather than as a sentence:

- **`simulation-and-determinism`** stays at Working, and **its closing gate demoted it** — the one
  demotion no implementing agent proposed, found by reading the specification requirement by
  requirement. Two of its twenty are unmet with no recorded exemption. "Floating-point policy"
  obliges the engine to provide **deterministic math types as an optional module** — fixed-point
  scalars, vectors, angles and transcendental approximations — and there is no such module;
  `profile.h` says so in its own comment, and two of the five profiles in the specification's own
  table therefore cannot be granted by this engine. "Simulation performance and testing" obliges a
  **strategy-scale determinism benchmark** — eight participants, 100 000 units, 5 000 agent groups,
  minutes of simulation under 1, 8 and 16 workers — and names it "the reference, not a small
  synthetic case"; nothing of the kind exists, which is the same evidence that demotes
  `gameplay-framework` below, and the part of it the tree does test it tests in a model:
  `integration.determinism_scale` is single-threaded and its `worker_count` seeds a permutation
  rather than starting a job system. Its Complete cell moves to M11 with the deterministic math module,
  because the guarantee that module turns on cannot be measured on one architecture.
- **`diagnostics-profiling-and-crash`** advances Seed → Working. Rolling capture, crash artefacts,
  source-location privacy and reproduction artefacts landed; **breadcrumbs are armed and have no
  callers outside their own module**, so a crash artefact reports `0 of 64`, and a *produced*
  artefact still carries the build machine's absolute paths through `backtrace_symbols_fd()`. Its
  Complete cell moves to M10.
- **`save-and-persistence`** stays at Working. Integrity, storage backends and checkpoints landed;
  **confidentiality is a vetted-AEAD dependency decision rather than a coding task**, and conflict
  resolution is unstarted. Its Complete cell moves to M10.
- **`gameplay-framework`** stays at Working. Network integration, the save and replay contracts and
  headless operation landed — `cy_require_headless()` fails the configure if a rendering, audio or
  interface module enters the closure — but the specification's **performance table is required to
  be benchmarked and is benchmarked nowhere**: `benchmarks/baseline.json` holds six entries and all
  six are `ecs/*` or `harness/*`. Its Complete cell moves to M10.

M9's own load is therefore **four cells and no Completes**.
[The capability matrix](roadmap/capability-matrix.md) carries the three moved cells in M10's column
and `simulation-and-determinism`'s in M11's — and **six more Complete cells that sat in the M9
column for rows this milestone never proposed, never touched and never audited**
(`camera-system`, `core-jobs-and-concurrency`, `ecs-core`, `gameplay-abilities-and-effects`,
`physics`, `sequencing-and-cinematics`) were moved to M11 by the closing gate rather than left
claiming a completion that did not happen. No existing check compared a milestone's column with the
status record; `m9:record-matches-plan` is that comparison, and `m9:record-matches-plan-history` was
the declared gap it opened over four earlier milestones whose columns claimed nineteen cells the
record did not support. **M10 task 6.5 audited all nineteen and closed it**: fifteen cells moved and
four rows were claimed, because in four cases the column was right and the record had simply never
been written. The per-cell evidence is in
[M10's record audit](roadmap/capability-matrix.md#m10s-record-audit-nineteen-cells-over-four-closed-milestones)
and the change is `openspec/changes/audit-closed-milestone-columns/`.

---

## M10 — Worlds

*Environment as one substrate with one producer per field.*

**Entry**: M9 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `environment-fields` | W | The shared substrate, field declaration, **one producer per field**, sparse tiled storage and streaming, CPU and GPU access, the wind field, residency levels, determinism of gameplay-visible fields |
| `terrain` | W | Tiled hierarchical storage, terrain as a geometry source, material layers and frequency separation, deformation classes, deltas through the persistence overlay, collision, navigation contribution, HLOD, the modifier stack |
| `foliage` | W | Instances that are not entities, clusters, promotion, deterministic procedural placement, GPU grass, wind response, the interaction field, the budget |
| `water` | W | Water bodies, the displacement contract, spectral ocean, rivers, shoreline, surface and underwater shading, foam, caustics, queries, buoyancy |
| `weather-and-wind` | W | Climate and weather cells, environment sampling, the wind field, precipitation, wetness and snow, storms, presets and transitions, ecosystem state, **the firewall** |
| `atmosphere-sky-and-clouds` | W | The physical atmosphere and its tables, aerial perspective, celestial bodies, volumetric clouds and their shadows, planetary scale |
| `procedural-content-generation` | W | Typed datasets, compiled graphs, execution domains, deterministic derivation, **stable generated identity**, regions and spatial invalidation, caching, output adapters, provenance, persistence of generated content |
| `vfx-system` | W | **The GPU compute dispatch M8.c did not build**: particle state resident in GPU buffers, indirect dispatch driven by GPU-maintained counts, async compute where the device exposes a queue, the GPU sort behind `BudgetLevers::sorted`, and the renderer kinds beyond `Sprite` and `Mesh`. M8.c took the capability to Seed — the asset model, the IR, the compiler, the shared simulation world, the scheduler, the budget controller and the determinism firewall's producer side — and its closing gate moved this cell here rather than claim Working over an absent default path |

**Closing artefact**: `samples/10-world` — an open world: procedurally populated terrain with rivers
and an ocean, foliage responding to a wind field driven by weather, wetness and snow accumulating,
a full day/night cycle with volumetric clouds, all streamed and all persistent.

**Exit criteria**

- Regenerating a region from the same seed produces identical content and identical generated identity
- A hand-placed override survives regeneration of its region
- Gameplay-visible fields are deterministic; presentation-only fields are firewalled, proven by tests
- Weather transitions hold their environment budgets; foliage and water hold theirs
- Terrain deformation persists through the save overlay and replays correctly
- One producer per field is enforced — a second producer registration fails
- The environment demo holds its frame budget across a full day/night cycle

**Risk spike**: region invalidation in PCG. Getting dependency-driven partial regeneration wrong
means either stale content or full-world regeneration, and both are project-defining.

**What closed here, and what did not.** The table above is the plan M10 was written against; the
record is [`status.yaml`](roadmap/status.yaml), and this paragraph is the correction — the
convention M5's, M6's, M7's, M8.a's, M8.c's and M9's sections all use. **All eight Working cells
landed**, seven of them from nothing: `src/environment/`, `src/terrain/`, `src/water/`,
`src/foliage/`, `src/weather/` and `src/pcg/` did not exist when this milestone opened, and
`atmosphere-sky-and-clouds` was extended in place at `src/rendering/sky/` rather than forked into a
second module that could disagree with M7's about what a sunset is. `vfx-system` reaches Working on
`src/vfx/gpu/`, the one file and one function M8.c's gate predicted.

**Of the three rows M9 demoted, two complete and one is demoted a second time.**
`diagnostics-profiling-and-crash` and `gameplay-framework` reach **Complete**, each because the
criterion that blocked it went green and its declaration was deleted from `m9.toml` — the ledger
fails a declared gap that starts passing, so closing one is checked rather than claimed.
**`save-and-persistence` stays at Working and its Complete cell moves to M11.** Conflict resolution
landed on a type that has no timestamp field in it; confidentiality needs a vetted AEAD, which
`thirdparty-dependencies` requires to go through the OpenSpec change flow and which M10 did not open.
A requirement-by-requirement audit then found the bill is **eleven pieces of work rather than the two
M9 named** — nine of twenty requirements satisfied, three unmet, eight partial, with the evidence in
`src/save/README.md`. `design.md` §4 wrote down in advance what a second demotion means: the row is
**mis-scoped rather than late**, and that finding is carried into M11's proposal rather than left in
a tasks file.

**`atmosphere-sky-and-clouds` is at Working with a running, failing gap against it, and the gate
wrote down why.** `m10:sky-field-round-trip`: the cloud shadow field is computed by a producer that
reports writing darkened tiles, and read back as the declared 1.0 at all twenty-five points inside
its radius; nothing outside `src/rendering/sky/` reads the field at all. That is one requirement of
thirteen and half of that one — the mechanism half is met and enforced by an absent build dependency,
and the twelve others are built and measured by five criteria over five suites. The argument for the
cell, and the statement that the row does **not** reach Complete at M11 while the gap is open, is in
[where M10's tiers are thin](roadmap/capability-matrix.md#where-m10s-tiers-are-thin).

**Two more gaps were declared here rather than fixed quietly**, each running and failing and naming
M11: `m10:fields-one-vegetation-potential` — `cy::foliage` and `cy::weather` declare
`vegetation-potential` with different encodings and different classifications, so a project
registering both producers fails at startup, which is a modelling decision across two rows and not a
gate's to take — and `m10:world-frame-budget`, which is the honest headline of the artefact: about
122 ms mean with a device drawing and about 106 ms headless, against a 16.7 ms 60 Hz frame —
**seven times over either way** — nearly flat across the cycle, with the three largest bands in the
same order in both takes: the substrate re-sampled at every terrain vertex at 63.0 ms, the cloud
march at 23.2 ms and water's foam field at 12.0 ms, all three of them work a shipping engine would do
in a shader. `m10:fields-sampled-on-a-device` is the gap behind all three: no `.slang` module
samples an environment field yet.

**The same wind defect was fixed rather than declared, because it had an owner.**
`src/foliage/` and `src/weather/` both declared the standard `wind` field — Presentation against
Authoritative — so `FieldRegistry::declare()` refused the second in either order and a project using
both failed at startup. Three requirements settle the ownership rather than declaration order, so
weather produces `wind` and foliage reads it; foliage's declaration is deleted and
`integration.standard_fields` registers every module through the entry point a project calls and
composes them in both directions.

**Three Complete cells left the M10 column for rows this milestone never proposed, touched or
audited** — `navigation`, `rendering-global-illumination` and `world-partition-and-streaming` — and
each has a first-hand refutation rather than only an absence of work: the GI–atmosphere seam that
[cycle 2](roadmap/dependencies.md#2--global-illumination--atmosphere) is entirely about was never
joined; the milestone that was to complete streaming closed on an artefact with **no streaming in
it**; and terrain's navigation contribution is one requirement of sixteen. **M11's load goes 61 →
65** on top of the 48 → 61 that M10's audit of four earlier closed columns already moved onto it.

**And two exit criteria above are not met, stated here rather than absorbed.** *"The environment demo
holds its frame budget across a full day/night cycle"* — it does not, by seven times, and the curve
across the cycle was measured rather than the claim asserted at one time of day. *"Terrain
deformation persists through the save overlay and replays correctly"* holds for terrain (289 of 289 probes bit for bit, and the program exits
non-zero if it does not) and **not** for the field half: weather's `wetness` and `snow-depth` are
declared persistent and nothing encodes them. The artefact also has **no rivers** and **no
streaming**, both on its own README's face.

---

## Reach — the 1.0 gate, and why M11 is six rungs

*The same project, everywhere, from one command.*

**M11 is not a milestone section. It is the name of six.** The six below — M11.a Foundations,
M11.b Authoring, M11.c Image, M11.d Desktop, M11.d.5 Backends, M11.e Ship — are the milestone, and
every existing
reference to "M11" in this document, in the capability matrix, in the ledgers and in the archived
changes stays valid as the name of the group, exactly as references to M8 did when it became M8.a,
M8.b and M8.c.

**Why it was split.** The matrix has said since M6 that M11 was the one milestone that could
reasonably be split, *"through a change if the work turns out to be separable along a real seam
rather than an arbitrary one"*. Two things then happened at M10's gate: an audit of four earlier
closed columns moved thirteen Complete cells here (48 → 61), and M10's own closing gate moved four
more (61 → **65**). Seventeen of the sixty-five arrived because a gate refused a claim, not because
anybody planned the work here.

**The seam is the artefact.** `split-m8-authorable-and-systems` added the rule that a milestone
whose closing artefact cannot be reached without its own risk spike succeeding contains two; that
rule is about risk, and M11 is not blocked by one spike. The rule this split adds is the sibling: **a
milestone whose scope cannot be judged by one closing artefact is several milestones sharing a
number, because a gate that cannot name what it is looking at is not a gate.** Splitting by count is
refused by the same rule — a rung has to be a claim an artefact can refute.

**And the order is what each artefact depends on.** The editor is finished before the picture is
art-directed, deliberately: a beauty shot assembled by hand in C++ proves the renderer and nothing
else, while one authored *through* the editor proves both, and is the honest demonstration of a
usable engine. Mobile is last because it is the only scope on the ladder that this project cannot
evaluate on any machine it owns.

| Rung | Rows | Reqs | Closing artefact |
|---|---:|---:|---|
| **M11.a** · Foundations | 12 | 233 | the world demo inside its frame budget, on a device, streaming |
| **M11.b** · Authoring | 24 | 420 | a real sample game, made through the editor |
| **M11.c** · Image | 15 | 232 | an art-directed beauty shot, authored through that editor |
| **M11.d** · Desktop | 9 | 117 | `samples/11-ship` on desktop, and a native platform backend |
| **M11.d.5** · Backends | 1 | 12 | one scene, three backends, the same picture |
| **M11.e** · Ship | 4 | 55 | `samples/11-ship` on every target, and the 1.0 record |

**M11.d.5 was not in the split; M11.d's spike produced it.** That rung was written carrying Metal
and D3D12, and the spike established what the plan had assumed: this project works on a Linux host
with one GPU vendor and no Apple toolchain, so **neither backend can be compiled here**. Half-building
them in a rung whose other criteria can be judged is how a project acquires a check that cannot fail.
`delivery-roadmap`'s *"A spike may resize a milestone as well as redirect it"* is the rule, and its
companion — *"A criterion follows its subject"* — is why the three-backend golden-image comparison
moved with them rather than staying behind as something M11.d could satisfy vacuously.

**Exit criteria of M11 as a whole**, unchanged by the split and owned by the rung named against each:

- Golden images match across Vulkan, Metal and D3D12 within tolerance — **M11.d.5**
- A native backend for one desktop platform passes the M0 sample and the M3 golden images, requiring no change in `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/` — **M11.d**
- The porting surface builds against a stub platform that shares no desktop assumption — **M11.d**, and against a real non-desktop one — **M11.e**
- Every capability is Complete or has a recorded deferral with its re-entry point — **M11.e**
- Every requirement maps to a test, a gate, or a recorded exemption — **M11.e**
- The documentation gate passes: every public API documented, every recipe described — **M11.d**
- The XR prerequisite checks still pass — **M11.e**
- Version, changelog and artefacts are produced by the release recipes — **M11.e**

---

## M11.a — Foundations

*The debts paid, and the frame budget made real.*

**Entry**: M10 green.

**Why this rung exists.** Seven declared gaps named M11 as the rung that closes them; **six of them
now name M11.a** and the seventh — `m9:record-matches-plan-history` — names M11.e, because it cannot
pass until the last of its four rows is evaluated *and* recorded and two of those four are M11.d's.
Every one of the seven is a criterion that runs and fails today. Nothing downstream is credible while they are
open: an editor rung authoring content into a world that costs **122 ms a frame against 16.7** is
authoring into a world nobody can ship, and an image rung tuning a picture no shader samples the
environment through is tuning the wrong thing. The three largest bands are the substrate re-sampled
at every terrain vertex (63.0 ms), the cloud march (23.2 ms) and water's foam field (12.0 ms), and
all three are work a shipping engine does in a shader.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `environment-fields` | C | `cy/field.slang` and a shader-side field sampler — unwritten today, and the one piece of work three of the seven gaps point at — bound through the GPU scene and measured on a device. Closes `m10:fields-sampled-on-a-device`, whose measurement is zero `.slang` modules |
| `terrain` / `foliage` / `water` / `weather-and-wind` | C | The three budget bands as shaders, in the order the gap names them; **one** `vegetation-potential`, which is a modelling decision across `foliage` and `weather-and-wind` before it is a code change; the persistence of `wetness` and `snow-depth` M10 declared and did not encode |
| `atmosphere-sky-and-clouds` | — | The sky's write path into `FieldStore`, the two assertions parked in `test_cloud_shadows.cpp` restored as the check, and the consumers the requirement names. The row's **C** cell is M11.c's; closing `m10:sky-field-round-trip` is this rung's, because M11.c cannot write a consumer of a field that reads back its default |
| `procedural-content-generation` | C | The cross-leg digest comparison, the standing benchmark, and the execution domains judged on more than one vendor's driver |
| `world-partition-and-streaming` | C | The streaming binder `src/terrain/`'s README records as its largest gap, so the row is judged on a world that streams rather than on one that fits |
| `save-and-persistence` | C | **Re-scoped first.** Demoted at M9 and again at M10, so by `delivery-roadmap`'s own rule it is mis-scoped rather than late: nine of twenty requirements satisfied, three unmet, eight partial, and eleven pieces of work — the inspector, the semantic diff, the ten forbidden patterns each made checkable, the missing benchmark, the engine-side consumer that lives in a sample today, and a vetted AEAD that is a dependency adoption of its own |
| `simulation-and-determinism` / `replay-and-rollback` / `networking-and-replication` | C | The 47 requirements nothing has yet read end to end at Complete grade, and the cross-platform lockstep comparison |
| `audio` | C | Steam Audio configured and simulating — a dependency adoption before it is a backend, whose cost M8.c measured in full: four upstream dependencies and an ABI flag that blocks both pinned compilers |

**And the machinery**, which is this rung's and no other's: the five ledgers, the five gates, the
five criteria floors, the matrix columns and the load table, and the seven inherited gaps re-pointed
from `m11` at the rung that actually closes each. A gap is re-pointed, never deleted, unless the
defect is fixed and its criterion is green.

**Closing artefact**: the M10 world demo **inside a 16.7 ms budget on a device**, with the substrate
sampled in a shader, the world streaming, and the ledger reporting each of the seven inherited gaps
either closed or still red with its reason.

**Exit criteria**

- A `.slang` module samples an environment field, and the count that is zero today is not zero
- The world demo holds 16.7 ms across a full day/night cycle, measured on a device and headless
- A project registering both `cy::foliage`'s and `cy::weather`'s producers starts
- The cloud shadow field reads back what the producer wrote, and something outside `src/rendering/sky/` reads it
- One CI job publishes one leg's digest and compares it with another's, answering all three of `m9:lockstep-cross-platform`, `m10:pcg-regeneration-cross-platform` and `m10:pcg-gpu-domain-agreement`
- Each of the four rows whose Working tier no criterion evaluates has a criterion that evaluates it
- Every one of the seven inherited gaps is closed, or still declared against a named rung with its reason

**Risk spike**: **port one band — the 63.0 ms substrate re-sample — to a shader and measure it
before scoping the other two.** If a GPU field sampler does not recover that band, the 122 ms figure
is not a shader problem and every estimate in this rung is wrong.

---

## M11.b — Authoring

*A real game, made in the editor.*

**Entry**: M11.a green.

**Why this rung exists.** `editor-architecture` and `live-editing` have been at **Seed since M5**
while five milestones built features on top of them, and the first thing this rung establishes is
whether that is a mis-record or a thin foundation. `samples/11-ship` is a packaging proof and each
existing demo proves one slice; twenty-four authoring and gameplay rows can only be judged by
something a person plays.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `editor-architecture` / `live-editing` | C | The three play modes — `InEditor`, `SeparateProcess`, `RemoteDevice` — which no grep finds today; the specialised editors the `CentreLower` region is reserved for; project creation and settings; the build-and-deployment client; the debugger and the frame profiler; and a per-field live edit policy with the reinitialise, recreate and restart outcomes |
| `editor-rust-application` / `editor-ui-ux` / `editor-visual-language` / `editor-viewport-and-gizmos` / `editor-documents-and-transactions` / `editor-agent-interface` | C | Source control behind a provider interface with a null provider, the eight missing view modes, and a document model in which a node has a name |
| `project-and-plugins` | C | Plugins, their lifecycle, resolution, the lockfile and trust tiers — four requirements of eleven with no implementation since M5 |
| `asset-import-pipeline` | C | Skins and animations through glTF, PNG and JPEG decoding, **BC7 and ASTC encoding**, input assets authored and cooked, and virtual-geometry cooking reachable from inside the editor |
| `visual-scripting` / `ui-system` / `text-and-fonts` / `rendering-2d` | C | The authoring surfaces a game's interface is built from, including the three text dependencies |
| `gameplay-abilities-and-effects` / `ai-system` / `animation-and-skinning` / `camera-system` / `navigation` / `physics` / `sequencing-and-cinematics` / `input-and-actions` | C | The rows a game exercises by being played, read end to end at Complete grade rather than argued from a specification |
| `ml-inference` | C | Off Seed, or an explicitly recorded deferral at M11.e with its re-entry point if the game does not want inference |
| `swift-scripting` | C | The three scripting items, and a shipping configuration with a toolchain pin verified in CI — which may prove to be M11.d's row wearing this one's name |

**Closing artefact**: **a real sample game** — a start, a loop, a way to win or lose, and content
authored **in the editor** rather than assembled in C++ — published with an honest statement of
which parts a person authored and which parts the sample's code assembles.

**Exit criteria**

- The three play modes exist and are exercised, and a live edit follows a declared per-field policy
- A texture authored in the editor is encoded to BC7 and to ASTC by the engine's own encoder
- The game is playable from its start state to an end state without a C++ fixture standing in for content
- Every editor row's remaining requirements are met, or the row is demoted to M11.e with its reason
- A node has a name, and source control has a provider interface with a null provider

**Risk spike**: **the play-mode seam.** `InEditor`, `SeparateProcess` and `RemoteDevice` are one
requirement in two specifications, and whether the hosted runtime carries all three without a second
world model is the question M5 seeded and nobody has asked since.

### M11.b editor completion evidence (2026-09-19)

The local, producer-independent editor work has advanced beyond the baseline described when this
rung was written. This evidence does **not** mark M11.b or any capability row Complete: the closing
game and the engine-owned dependencies below remain required.

| Delivered in the editor | Evidence boundary | Still open dependency |
|---|---|---|
| Restored document tabs and view state, guarded dirty close, and attributed Undo History | Document/service, view-model and shell interaction tests cover activation, Save/Discard/Cancel, save failure, restart, missing assets and undo/redo presentation | Full project graph and plugin lifecycle remain `project-and-plugins` work |
| Searchable Settings with project/platform overrides and per-user preferences; provider-neutral Source Control with Git, Perforce and null providers | Typed settings commands distinguish canonical project changes from preferences; status/history and capability-aware checkout/revert/submit/lock/unlock are command-backed | General build/cook/package/deploy and device installation remain `implement-m11d-desktop` work |
| Stable-identity Hierarchy multi-selection, rename, reparent and template creation | Each persistent action uses the registered transaction path; cycle and invalid-name cases refuse; unsupported visibility/lock controls are not drawn | Visibility/lock require authoritative document/runtime fields |
| Swift Workspace with buffered editing, fingerprint-safe save conflicts, SourceKit-LSP features, diagnostics navigation and Swift build/reload | Editing remains available without SourceKit; stale diagnostics are discarded; Save cannot silently replace an external edit | Shipping static configuration, full toolchain matrix and missing runtime ABI/lifecycle callbacks remain `swift-scripting` work |
| Identity-keyed semantic Diff and three-way Merge | Typed conflicts require local, incoming or validated replacement; a resolved merge is one attributed transaction and undo restores the pre-merge document | Domain operations without a canonical format remain explicit refusals |
| Desktop MCP alongside the window, bounded requests, confirmations/grants, Agent Sessions and privacy-labelled diagnostics | Desktop and `--mcp --headless` project the same command registry and resources; pause/revoke and disconnect clean up queued work | Remote-device session transport and its `EncodedStream` producer remain open |
| Content Browser navigation/filtering, identity-safe asset move/rename with sidecars, scene/Inspector drops and importer-declared settings | `asset.import`, `asset.move`, `asset.rename`, `asset.place`, `asset.assign` and `asset.import-setting.set` are registered commands and MCP tools; traversal, collision, external-change and unsupported cases are named | Renderer-produced thumbnails/previews, missing codecs/compressors and virtual-geometry cooking remain with their owning renderer/importer rows |

Renderer and backend presentation is deliberately not inferred from authoring state. Native engine
Metal/macOS frame transport, renderer/debugger/profiler and material-capture producers, encoded remote
streaming, and specialised editors whose domains have no canonical writer remain open. The editor
continues to show an explicit unavailable state rather than a fake frame, preview, capture or domain
editor.

---

## M11.c — Image

*What the engine actually looks like.*

**Entry**: M11.b green.

**Why this rung exists.** Every renderer row is at Working and **the demos do not look like a modern
engine**: the world is untextured procedural geometry, the virtual-geometry capture is a normals
debug view, post-processing is untuned, and nothing has been optimised so nothing has been tuned.
There are six image files in the entire tree outside `docs/`, so every material in every published
picture is a constant. The mechanisms are real and gated; the **output** is not.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `material-compiler` / `shader-system` | C | **First, and deliberately first**: image quality is expressed through them, and global illumination cannot be Complete on a material compiler that is not. Node previews through the runtime compiler, every lowering stage visible, and the interchange forms the two backends will need |
| `virtual-geometry` / `virtual-shadows` | C | The rows whose published evidence is a debug view, photographed as an image instead |
| `rendering-global-illumination` / `denoising` / `ray-tracing-infrastructure` | C | **The row the plan promised Complete at M10 and M10 did not deliver**: the GI/atmosphere seam of [cycle 2](roadmap/dependencies.md#2--global-illumination--atmosphere) — one adapter at one composition point — plus the denoiser signals with no producer |
| `rendering-post-processing` / `temporal-rendering` / `rendering-lighting-and-shadows` | C | A post chain, tone mapping and anti-aliasing that are in the frame the artefact photographs. `samples/10-world` links none of them today, so the world picture never passes through any of it |
| `rendering-culling-and-lod` | C | A hierarchical depth buffer on the device and cluster-granular occlusion, against a CPU two-pass model that exists and a device path that does not |
| `atmosphere-sky-and-clouds` | C | The sky judged as an image rather than as a table — **conditional on M11.a closing `m10:sky-field-round-trip`**, because this rung owns the illumination consumer of a field whose sampler returns its default at every point today |
| `rendering-architecture` / `rendering-geometry-and-resources` | C | Subsystem controllers reporting measured costs to the arbiter, and a skin pass with dual quaternions and blend shapes where the specification puts them |
| `vfx-system` | C | Because an art-directed shot with no particles in it does not exercise the row |

**Closing artefact**: **an art-directed beauty shot** — real materials, tone mapping,
anti-aliasing, tuned post — assembled **through the editor M11.b finished** rather than in C++, and
published beside a statement of what was authored and what the renderer produced.

**Exit criteria**

- A textured material authored in the editor reaches the renderer as textures rather than as constants
- The beauty shot passes through tone mapping and anti-aliasing, and the frame graph says so
- `gi::SkyTerm` is constructed from the atmosphere at one composition point, and cycle 2 is closed or re-argued
- The virtual-geometry and virtual-shadow evidence is an image, not a debug visualisation
- Every row's remaining requirements are met first-hand, or the row is demoted with its reason

**Risk spike**: **one authored material, end to end, before anything is scoped.** Author one
textured material in the editor's graph, compile it through the runtime compiler, encode its
textures, bind it in the assembled frame, and photograph it. Everything in this rung assumes that
path exists; nothing in the tree has ever run it.

---

## M11.d — Desktop

*One platform layer, one package, and the interface the backends will be written against.*

**Entry**: M11.c green.

**Why this rung exists.** `core-platform-abstraction` has SDL3 as its only implementation, so nothing
has yet proved the abstraction carries no SDL assumption — and proving it needs a second *native*
platform, not a second operating system, so it is the one portability claim this host can make
first-hand. Six of this rung's nine rows were last advanced at M0, M1 or M2, which makes it the rung
carrying the oldest tiers in the record.

**What this rung no longer carries, and where it went.** It was written with Metal and D3D12 in it.
Its spike measured the two questions it was scoped against, and the second answer resized it: this
host is Linux, with one GPU vendor and no Apple toolchain, so **neither backend can be compiled here,
let alone judged**. Sections 2 and 3 of its task list moved — not deleted, not descoped — into
**[M11.d.5 · Backends](#m11d5--backends)**, together with `rhi-and-render-graph`'s Complete cell and
the three-backend golden-image comparison. What stays is everything this host can actually check,
including the RHI **interface** those backends will be written against, which is settled here on
Vulkan and null *before* either backend exists — the ordering the spike argued for, now enforced by
the ladder rather than requested by a task list.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `rhi-and-render-graph` | — | **The interface, and not the backends.** The eight gaps the Metal seed recorded are interface changes before they are backends: an opaque memory-pool class, layouts derived from the access masks, a queue-ownership query, a per-format support query, a pipeline-cache token, and secondary recording. They land here, on Vulkan and the null backend, because changing `reserve_transient_memory`'s contract after two backends are written is a migration across every pass. The row's **C** cell is M11.d.5's |
| `core-platform-abstraction` | C | A **native** `Platform` and `DisplayServer` for one desktop platform, replacing SDL3 there and **requiring no change in `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`**, plus the porting surface built against a stub platform that shares no desktop assumption |
| `rendering-forward-clustered` | — | MSAA and multi-view, the desktop half. The row's **C** cell stays at M11.e with the mobile pipeline differences, because a row is not Complete on the half of its scope this rung can reach |
| `build-and-packaging` | C | Content audit, provenance and symbols |
| `testing-and-quality` | C | The full gate set and the documentation gate |
| `developer-workflow-and-just` | C | The release and second-platform targets, from the same recipes on every desktop |
| `core-assets-and-io` / `core-jobs-and-concurrency` / `core-memory-and-containers` / `ecs-core` / `engine-architecture` | C | Here rather than in M11.a for one reason: the exit criterion for the native backend is that **none of `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/` changes**, which is a first-hand audit of exactly these rows whether or not anybody calls it one |

**Closing artefact**: `samples/11-ship` built, cooked, packaged and launched on each desktop target
from one recipe, plus the M0 sample and the M3 golden images on the native platform backend with
`src/core/`, `src/ecs/`, `src/servers/` and `src/scene/` untouched.

**Exit criteria**

- The native backend passes the M0 sample and the M3 golden images, and the diff touches no engine layer
- The porting surface builds against a stub platform that shares no desktop assumption
- The documentation gate passes: every public API documented, every recipe described
- `samples/11-ship` is built, cooked, packaged and launched on each desktop target from one recipe
- The eight RHI interface gaps are closed in the interface, on Vulkan and null, with no backend written

*Golden images matching across Vulkan, Metal and D3D12 was this rung's first exit criterion and is
now M11.d.5's. It moved with its subject rather than staying behind, because a criterion whose
subject has been deferred is one the milestone can satisfy vacuously.*

**Risk spike**: **settle the eight RHI gaps as interface changes, on Vulkan and null, before a line
of either backend is written** — and, before that, find out whether a hosted macOS or Windows runner
can present a graphics device at all, because every image claim in this rung depended on the answer.
**Both were run, and the second resized the rung**: every hosted leg presents a device that draws and
presents, and not one of them is a GPU — a paravirtual Metal device reporting *no* Apple GPU family,
and `Microsoft Basic Render Driver` on every Windows image, with two adapters of which the first does
not set the software flag. The full measurement is in
[`implement-m11d-desktop`](../openspec/changes/implement-m11d-desktop/design.md)'s design §1.4.

---

## M11.d.5 — Backends

*One scene, three backends, the same picture.*

**Entry**: M11.d green.

**Why this rung exists.** It is the machine rather than the plan. M11.d was written to deliver Metal
and D3D12; **this project works on a Linux host with one GPU vendor and no Apple toolchain**, so
neither backend can be compiled where that rung is worked, let alone judged. A rung that half-builds
what it cannot run acquires a check that cannot fail, and this project has shipped nine of those. So
the two backends became a rung of their own, with a gate of its own and an artefact no single leg of
the continuous-integration matrix can produce.

**Why it is an insertion and not a renumbering.** M11.e is the terminus: renaming it would move 27
`known_gap_closes` across five ledgers, 30 criterion identifiers and 30 falsifiability entries — all
counted rather than estimated — which is churn for a label. M5.5 set the precedent — insert between the neighbours, keep every existing reference valid —
and `delivery-roadmap` requires an inserted milestone to take a **rung between its neighbours** in
every mechanism that depends on milestone order, which `roadmap-test` asserts.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `rhi-and-render-graph` | C | **Metal, native and not a translation layer**, against the interface M11.d settled; and **D3D12 from nothing** — there was not one D3D12 file in the tree when this rung opened. Both are written against capability queries rather than backend identity tests, which is the rule the eight gaps were the first real pressure on |

**Closing artefact**: the M3 golden images rendered through Vulkan, Metal and D3D12 and compared
across legs of the matrix within tolerance; one committed screenshot per backend under
`docs/design/images/`; and **each one labelled with the device that answered** — hardware,
paravirtual or software.

**Exit criteria**

- Golden images match across Vulkan, Metal and D3D12 within tolerance
- The Metal backend compiles, creates a device, draws and presents, and passes the RHI conformance suite
- The D3D12 backend does the same, from nothing
- Every device report names the device that answered, and a software device is labelled from its **identity**, never from its flag
- The engine builds and passes its suites with `CY_RENDERER_METAL` and `CY_RENDERER_D3D12` **off** as well as on
- Every requirement of `rhi-and-render-graph` maps to a test, a gate or a recorded exemption

**Hardware evidence gathered for the closing gate.** Physical runs retired three questions the hosted-runner spike
could not answer: an Apple M3 Pro exercised memoryless attachments, placement heaps and Tier 2
argument buffers; NVIDIA, Apple and AMD hardware produced matching golden images; and the Radeon
run closed the D3D12 hardware leg. D3D12 Resource Heap Tier 1 execution remains deferred because
both hosted WARP and the physical Radeon report Tier 2. Its engine-side partition policy is tested,
and [`implement-m11d5-backends`](../openspec/changes/implement-m11d5-backends/design.md) names the
Tier 1 device or validation forcing mode that re-enters the native exercise.

**Risk spike**: **already spent, by M11.d.** Two throwaway workflow runs created a device on each
hosted leg, cleared a target to a known colour, read the pixel back and presented it — a frame that
happened, not a runner manifest saying an SDK is installed. This rung starts from that answer rather
than re-deriving it, and the one question it must answer for itself is the narrow one the spike could
not: whether the *interface* M11.d settled survives contact with two backends that were not consulted
about it.

---

## M11.e — Ship

*Every target, and the 1.0 record.*

**Entry**: M11.d.5 green.

**Why this rung is last.** Mobile is the only scope on the ladder this project cannot evaluate on
any machine it owns, and the sweep — every row an earlier rung demoted — cannot be sized until the
earlier rungs have run. **Four rows is the smallest count on the ladder and not the smallest rung**:
M11.a predicts demoting `save-and-persistence` and `audio`, M11.b `ml-inference` and
`swift-scripting`, M11.c `rendering-culling-and-lod`, M11.d `build-and-packaging`, and this rung's
real load is whatever arrives.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `build-system-and-platforms` | C | Cross-compilation, mobile targets, distribution artefacts and the full continuous-integration matrix |
| `rendering-forward-clustered` | C | The mobile pipeline differences, with M11.d's desktop half already in |
| `thirdparty-dependencies` | C | The rest of the intended dependency set under the manifest's governance — about half of roughly forty-two named libraries is integrated today — and the runtime attribution API |
| `delivery-roadmap` | C | The matrix, the status record and the ledgers agreeing, and **the 1.0 record**: what 1.0 is and what it is not |
| `xr-support` | — | Prerequisites verified and held open; XR itself remains deferred, as a decision restated rather than a row skipped |
| Everything else | C | Every remaining requirement, or an explicitly recorded deferral with its re-entry point. **This is the only rung permitted to record a deferral** — a rung before it demotes with a reason instead, which is what makes the discipline enforceable |

**Closing artefact**: `samples/11-ship` on every supported target from a single recipe — the desktop
half from M11.d and at least one mobile target here — and **the 1.0 record**: 76 rows, each Complete
or deferred with a re-entry point, every deferral naming what is unmet and what would bring it back.

**Exit criteria**

- A mobile artefact is produced by the continuous-integration matrix, or mobile is a recorded deferral with a re-entry point
- The porting surface is proved against a real non-desktop platform
- Every capability is Complete or has a recorded deferral with its re-entry point, checked by a criterion that names the offending row rather than by a document asserting it
- Every requirement maps to a test, a gate, or a recorded exemption
- The XR prerequisite checks still pass
- Version, changelog and artefacts are produced by the release recipes, and no recipe refuses naming a milestone that is not on the ladder
- The 1.0 record states what 1.0 is and what it is not

**Risk spike**: **find out whether a mobile artefact can be produced in CI at all, before anything
else in this rung is scoped.** One empty project, cross-compiled, packaged and reported from a
hosted runner. If it cannot be done, mobile is a deferral with a re-entry point and this rung is the
distribution and record rung — a finding worth having on day one rather than at the gate.

**The sweep starts with `navigation` and `physics`, by name.** Both report **0 of 16 requirements**
mapped to a test, a gate or a recorded exemption, while the tests largely already exist — four
suites each. So this is connecting evidence to requirements rather than writing subsystems: the
cheapest sweep work on the ladder, and the highest-value, because both rows are on the critical path
of the first game this engine will carry. `navigation` holds A\* with a funnel, Recast/Detour navmesh
build, hierarchical queries, crowd avoidance and flow fields claiming *"20,000 agents ordered to the
same destination"* guided by one field. `physics` holds fixed-step integration — **which a lockstep
RTS desyncs without**, and which nothing currently maps. See tasks 6.1a.

---

## M12 — After 1.0

*The three capabilities 1.0 ships without.*

**Entry**: M11.e green. **1.0 ships from M11.e; this rung is after it**, and no criterion here may
be cited as a reason for M11.e to remain open.

**Why this rung exists.** `delivery-roadmap` treats a re-entry point named in the record as a
decision, and one named nowhere as an oversight wearing the same clothes. Three capabilities reach
1.0 without a Complete cell, and until this rung existed none of them had a destination that was a
rung:

| Capability | Where it stood | Why it is deferred rather than dropped |
|---|---|---|
| **Android** | iOS landed at M11.e with `platform/ios/`; Android has no platform directory, toolchain or cross-compilation leg | The engine ships on five platforms without it; adding a sixth is a rung's worth of work, not a task's |
| **`ml-inference`** | **Seed since M8.c.** `integration.ai` does not exist and the ONNX backend registers the CPU execution provider only | `m11b:ml-inference-or-a-deferral` asks for a consumer **or** a deferral with a re-entry; M11.b produced neither, and this is the re-entry |
| **`xr-support`** | The one row of seventy-six reaching 1.0 without a Complete cell; re-entry recorded as *"after 1.0"* | A date is not a rung. The entry point is already named: `cy::Runtime::tick()` takes no predicted display time, so a host cannot tell the engine when a frame will be displayed — the half of M3's seam that is not open |

**What this rung is not.** It is not a destination for work a current rung finds inconvenient. Three
capabilities enter by name and by decision; a fourth arrives only through a change that argues for
it. The ladder already carries the cost of one rung that became a collector of everything nobody
did, and it will not carry two.

**And the ladder's end is checked rather than assumed.** M11.e's note states the rule M12 had to
satisfy to exist: *either M11.e is the last rung everywhere, or whatever follows it carries a
ledger, a gate, a floor and a change directory, all four*. M12 carries
`tools/roadmap/milestones/m12.toml`, `milestone-m12` in `gates.toml`, its floor, and
`openspec/changes/implement-m12-after-one-point-oh/`. A rung present in some of the places that read
the ladder and absent from others is exactly the failure M8's split produced three times in one
change.

**Change**: [`implement-m12-after-one-point-oh`](../openspec/changes/implement-m12-after-one-point-oh/proposal.md)

---

## How this document is maintained

Re-sequencing the ladder, moving a capability between milestones, changing exit criteria, or
bringing deferred scope forward is an **OpenSpec change** against
[`delivery-roadmap`](../openspec/specs/delivery-roadmap/spec.md), stating what was learned that made
the previous ordering wrong. This document is updated in the same change.

Per-capability status lives in [the capability matrix](roadmap/capability-matrix.md) and is checked
against `openspec/specs/` by `just roadmap-status`. A change that advances a capability updates the
matrix in the same commit.

## See also

- [Implementing the roadmap](roadmap/implementing.md) — how a milestone becomes changes, and what is in flight
- [Capability matrix](roadmap/capability-matrix.md) — every capability, every milestone, every tier
- [Dependencies](roadmap/dependencies.md) — the graphs the ordering follows, and the three cycles
- [Risks and deferrals](roadmap/risks.md) — the register, the spikes, and where deferred scope re-enters
- [Specification index](../openspec/specs/README.md) — what is being built and why
