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

Twelve milestones, four eras. Each milestone ends in something that runs, committed to the
repository and exercised by continuous integration — so it becomes a regression gate for everything
after it.

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
        M3 --> M4 --> M5
    end
    subgraph S["Production scale"]
        direction TB
        M6["M6 · Scale<br/><i>build graph, streaming</i>"]
        M7["M7 · Fidelity<br/><i>the modern renderer</i>"]
        M8["M8 · Game systems<br/><i>a vertical slice</i>"]
        M6 --> M7 --> M8
    end
    subgraph SH["Shipping"]
        direction TB
        M9["M9 · Integrity<br/><i>determinism, network</i>"]
        M10["M10 · Worlds<br/><i>environment, PCG</i>"]
        M11["M11 · Reach<br/><i>platforms · 1.0</i>"]
        M9 --> M10 --> M11
    end
    M2 --> M3
    M5 --> M6
    M8 --> M9

    classDef gate fill:#3b1f1f,stroke:#f87171,stroke-width:2px,color:#fee2e2
    class M11 gate
```

| Era | Milestones | What exists at the end |
|---|---|---|
| **Foundation** | M0 – M2 | A headless simulation that is deterministic and serializable. Nothing user-visible. |
| **First playable** | M3 – M5 | A game you can write in Swift and edit in the editor. |
| **Production scale** | M6 – M8 | A world larger than memory, rendered at film detail, playable as a real game. |
| **Shipping** | M9 – M11 | Multiplayer, open worlds, every platform, 1.0. |

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

---

# Production scale

## M6 — Scale

*A world larger than memory, and a build that is a graph rather than a script.*

**Entry**: M5 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `build-and-packaging` | W | The derivation graph, derivation keys, explicit inputs, immutable artefacts, the derived data cache, the build service, precise invalidation, packages, chunk-level patching |
| `world-partition-and-streaming` | W | Partitioning, stable cell identity, spatial binding, **cells cooked in ECS-native form**, streaming sources, shapes and prediction, channels, priorities and deadlines, staged atomic activation, layers, HLOD, the persistence overlay |
| `residency` | W | **Shared policy with separate storage: importance, priority, deadlines, budgets, pressure, eviction, churn control** |
| `virtual-texturing` | W | Virtual address spaces, page tables, tiles, the physical cache, the resident mip tail, GPU feedback, prefetch, runtime producers |
| `save-and-persistence` | W | The overlay as the save, scopes and traits, persistent identity, dirty tracking, the journal, atomic generations, migration |
| `rendering-culling-and-lod` | W | GPU-driven culling, visibility ranges, HLOD, shadow caster culling |
| `asset-import-pipeline` | C | ufbx, xatlas, mesh processing, cook profiles, packaging |
| `core-assets-and-io` | C | Streaming under the residency policy |

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

---

## M8 — Game systems

*Everything a game touches, at Working.*

**Entry**: M7 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `gameplay-framework` | W | Rules, session fragments, participants and teams, ownership/control/authority, capabilities, tags, phases, spawning, time domains, interaction, features, indexes |
| `gameplay-abilities-and-effects` | W | Compiled ability programs, attributes and modifiers, effects and stacking, costs and cooldowns, targeting, the activation pipeline |
| `visual-scripting` | W | Shared graph infrastructure, typed pins, stable identity, the IR, execution backends, async graphs, semantic merge, debugging |
| `sequencing-and-cinematics` | W | Compiled timelines, exact time, bindings, tracks and authority, batched dispatch, arbitration, capture and restore, seek and skip, preload plans |
| `animation-and-skinning` | W | Skeletons and bone LOD, clips and compression, the animation graph, compiled programs, batched evaluation, layers and masks, root motion, IK, retargeting, the GPU pose world, pose sharing |
| `ai-system` | W | Agents as entities, the unified graph with tree/utility/GOAP semantics, compiled behaviour programs, batched perception, knowledge, environment queries, smart objects, AI LOD |
| `navigation` | W | Navmesh over Recast, runtime updates, streaming, A* and funnel, hierarchical paths, flow fields, off-mesh links, avoidance, crowds |
| `vfx-system` | W | The graph compiler and IR, GPU-first simulation, derived attribute layout, the unified world and scheduler, data interfaces, GPU scene integration, GPU events, budget scalability |
| `ui-system` | W | Dedicated element storage, the retained tree, documents, declarative authoring, layout, `.cyss`, data binding, input routing, the layer stack, the widget set, GPU-driven rendering, accessibility |
| `text-and-fonts` | C | Shaping, BiDi, line breaking, layout objects, localisation |
| `rendering-2d` | W | Sprites, ordering, batching, tilemaps, 2D lights and shadows, the screen-space SDF |
| `audio` | C | Steam Audio, acoustic geometry, importance tiers, voice virtualisation, effects, interactive audio |
| `ml-inference` | S | Model assets, tensors and sessions, the backend abstraction, **the determinism boundary** |
| `camera-system` | W | Rig graphs compiled to programs, blending, framing, aim, shake, volumes, cuts |

**Closing artefact**: `samples/08-vertical-slice` — a playable game: a level, characters that
animate and think, abilities with effects, a cinematic, a heads-up interface, effects, sound, and a
2D menu.

**Exit criteria**

- Every gameplay-facing capability is at Working, with its own tests
- No graph is interpreted at runtime — an audit finds no per-entity virtual tick in any graph consumer
- Cost is bounded by configuration: 8,000 agents and 100 concurrent effects hold their budgets
- Ability activation, sequence playback and animation evaluation are deterministic under the
  simulation's declared profile
- The determinism firewall holds: VFX and inference cannot write gameplay state, proven by a test
- Interface accessibility checks pass; the interface holds its frame budget

**Risk spike**: the shared graph infrastructure across seven consumers. If one consumer needs a
semantic the shared IR cannot express, that is better known before six others are built on it.

---

# Shipping

## M9 — Integrity

*One command log, read five ways.*

**Entry**: M8 green.

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

---

## M11 — Reach — the 1.0 gate

*The same project, everywhere, from one command.*

**Entry**: M10 green.

**Work**

| Capability | → | Scope |
|---|:---:|---|
| `rhi-and-render-graph` | C | **Metal** (native, not a translation layer) and **D3D12** to parity with Vulkan |
| `build-system-and-platforms` | C | Cross-compilation, the **porting surface**, mobile targets, distribution artefacts, full continuous integration matrix |
| `core-platform-abstraction` | C | A **native** `Platform` and `DisplayServer` backend for one desktop platform, replacing SDL3 there and proving the abstraction carries no SDL assumption; the porting surface proven to carry no desktop assumption |
| `build-and-packaging` | C | Content audit, provenance and symbols, downloadable content, distributed execution |
| `rendering-forward-clustered` | C | Mobile pipeline differences, MSAA, multi-view |
| `xr-support` | — | Prerequisites verified and held open; XR itself remains deferred |
| Everything else | C | Every remaining requirement, or an explicitly recorded deferral |
| `testing-and-quality` | C | The full gate set, the documentation gate |

**Closing artefact**: `samples/11-ship` — one project built, cooked, packaged and launched on every
supported target from a single recipe.

**Exit criteria**

- Golden images match across Vulkan, Metal and D3D12 within tolerance
- A native backend for one desktop platform passes the M0 sample and the M3 golden images, requiring no change in `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`
- The porting surface builds against a stub platform that shares no desktop assumption
- Every capability is Complete or has a recorded deferral with its re-entry point
- Every requirement maps to a test, a gate, or a recorded exemption
- The documentation gate passes: every public API documented, every recipe described
- The XR prerequisite checks still pass
- Version, changelog and artefacts are produced by the release recipes

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
