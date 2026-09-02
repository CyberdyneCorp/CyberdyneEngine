# CyberdyneEngine — Specifications

Target specifications for CyberdyneEngine, written **before** the code. Each
`<capability>/spec.md` states design commitments as `SHALL` requirements with concrete scenarios.

These are not descriptions of existing code — the engine is greenfield. They are the contract an
implementation must satisfy, and the place where architectural decisions are recorded and
justified. Changes go through the OpenSpec flow (`/opsx:propose` → `/opsx:apply` →
`openspec validate` → `/opsx:archive`), so the reasoning behind a decision stays with it.

## Reading order

**1 — Foundations**
| Capability | Covers |
|---|---|
| [engine-architecture](engine-architecture/spec.md) | Layers, servers, ECS + scene duality, modules, startup, main loop, non-goals |
| [core-type-system](core-type-system/spec.md) | Reflection, persistent type and field identity, `Var`, handles, events, `Callable` |
| [core-memory-and-containers](core-memory-and-containers/spec.md) | Allocators, memory domains and budgets, pressure, epochs, containers, chunk storage |
| [core-math](core-math/spec.md) | Types, coordinate and depth conventions, SIMD, BVH, geometry, curves, RNG |
| [core-jobs-and-concurrency](core-jobs-and-concurrency/spec.md) | Job system, coroutines, cancellation, deterministic parallelism, critical path |
| [core-assets-and-io](core-assets-and-io/spec.md) | Asset identity, cooking, virtual filesystem, packages, streaming, hot reload |
| [core-platform-abstraction](core-platform-abstraction/spec.md) | Platform services, display, input, the porting surface |

**2 — World model**
| Capability | Covers |
|---|---|
| [ecs-core](ecs-core/spec.md) | Entities, components, archetypes, queries, systems, change detection, snapshots |
| [scene-graph-and-nodes](scene-graph-and-nodes/spec.md) | Node façade, transforms, lifecycle, behaviours, coherence invariants |
| [serialization-and-prefabs](serialization-and-prefabs/spec.md) | Prefab/scene/world assets, exposed parameters, overrides, entity templates, migration |
| [world-partition-and-streaming](world-partition-and-streaming/spec.md) | CyberWorld: partitioning, cell streaming and activation, layers, persistence overlay, HLOD |
| [environment-fields](environment-fields/spec.md) | CyberField: sparse world-scale data — biome, moisture, wind, flow, wetness — with one producer each |
| [terrain](terrain/spec.md) | CyberTerrain: tiled hierarchical surface, geometry source, deformation classes, deltas |
| [foliage](foliage/spec.md) | CyberFoliage: GPU instances, ECS promotion, deterministic placement, GPU grass |
| [water](water/spec.md) | CyberWater: water bodies, spectral ocean, rivers, shoreline, the displacement contract |

**3 — Scripting**
| Capability | Covers |
|---|---|
| [native-abi](native-abi/spec.md) | The versioned flat C ABI, module entry points, hot reload, ABI CI gates |
| [swift-scripting](swift-scripting/spec.md) | `CyberdyneKit`, behaviours and systems, macros, ARC and concurrency rules |

**4 — Rendering**
| Capability | Covers |
|---|---|
| [rhi-and-render-graph](rhi-and-render-graph/spec.md) | Explicit RHI, automatic barriers and aliasing, backend roadmap |
| [rendering-architecture](rendering-architecture/spec.md) | Render server, snapshot boundary, frame structure, extension points |
| [rendering-culling-and-lod](rendering-culling-and-lod/spec.md) | Spatial indexing, frustum and occlusion culling, LOD, HLOD |
| [virtual-geometry](virtual-geometry/spec.md) | CyberGeometry: clusters, crack-free hierarchy, geometry pages, GPU streaming, visibility buffer |
| [rendering-forward-clustered](rendering-forward-clustered/spec.md) | Cluster assignment, sort keys, prepass modes, pass order |
| [shader-system](shader-system/spec.md) | Slang, permutations, reflection-driven binding, caching, hot reload |
| [rendering-materials-and-shading](rendering-materials-and-shading/spec.md) | The BRDF in concrete terms, shading models, IBL, material model |
| [material-compiler](material-compiler/spec.md) | CyberMaterial: graph → IR → closures → program, quality tiers, GPU material table, cost attribution |
| [rendering-lighting-and-shadows](rendering-lighting-and-shadows/spec.md) | Light types, physical units, LTC area lights, atlas, cascades, filtering |
| [rendering-global-illumination](rendering-global-illumination/spec.md) | CyberGI: GI scene, surface and radiance caches, screen/software/hardware tracing, reflections, baking, sky |
| [denoising](denoising/spec.md) | CyberDenoiser: one accumulation and edge-aware filter for every stochastic signal |
| [ray-tracing-infrastructure](ray-tracing-infrastructure/spec.md) | Acceleration structure lifecycle, geometry adapters, ray queries, capability gating |
| [rendering-post-processing](rendering-post-processing/spec.md) | Chain order, AO, fog, exposure, DOF, bloom, tonemap, AA, upscaling |
| [temporal-rendering](temporal-rendering/spec.md) | CyberTemporal: jitter, motion vectors, history, reprojection, invalidation |
| [rendering-geometry-and-resources](rendering-geometry-and-resources/spec.md) | Vertex formats, compression, LOD, instancing, skinning, textures, particles |
| [rendering-2d](rendering-2d/spec.md) | Sprites, batching, tilemaps, 2D lights and shadows, screen-space SDF |
| [vfx-system](vfx-system/spec.md) | GPU-first VFX: graph compiler, unified simulation world, data interfaces, GPU events, budget scalability |

**5 — Simulation**
| Capability | Covers |
|---|---|
| [physics](physics/spec.md) | `PhysicsServer`, Jolt backend, components, queries, character controller, determinism |
| [animation-and-skinning](animation-and-skinning/spec.md) | CyberAnimation: skeleton/rig split, compiled programs, GPU pose world, LOD and pose sharing, motion matching |
| [ai-system](ai-system/spec.md) | CyberAI: ECS agents, unified compiled behaviour graph, batched perception, knowledge, smart objects, AI LOD |
| [navigation](navigation/spec.md) | Navmesh and volumes, A* + funnel, flow fields, hierarchical paths, avoidance, crowds |
| [ml-inference](ml-inference/spec.md) | CyberML: model assets, tensors, backend abstraction, determinism boundary |
| [audio](audio/spec.md) | Engine-owned AudioServer over miniaudio + Steam Audio, acoustic geometry, importance tiers |

**6 — Content and tooling**
| Capability | Covers |
|---|---|
| [text-and-fonts](text-and-fonts/spec.md) | `TextServer`, fonts, shaping, BiDi, line breaking, glyph atlases |
| [ui-system](ui-system/spec.md) | CyberUI: dedicated element storage, retained tree, declarative authoring, `.cyss`, GPU-driven, layer navigation |
| [asset-import-pipeline](asset-import-pipeline/spec.md) | Importers, cook cache, texture and model import, packaging |
| [editor-architecture](editor-architecture/spec.md) | Editor as an engine app, play mode, inspector, plugins, build pipeline |

**7 — Systems and process**
| Capability | Covers |
|---|---|
| [networking-and-replication](networking-and-replication/spec.md) | CyberNet: three network modes, replication schemas, priority-scheduled interest, rollback, dedicated server |
| [xr-support](xr-support/spec.md) | Deferred; the prerequisites the engine must not preclude |
| [build-system-and-platforms](build-system-and-platforms/spec.md) | CMake, configurations, Swift toolchain, codegen, porting surface, CI |
| [testing-and-quality](testing-and-quality/spec.md) | Test taxonomy, golden images, determinism, benchmarks, merge gates |
| [thirdparty-dependencies](thirdparty-dependencies/spec.md) | Dependency policy, the intended set, what we build ourselves |

## The decisions that shape everything else

- **ECS is the storage; the node tree is the interface.** Two views, one truth. The coherence
  invariants in `scene-graph-and-nodes` are what keep that honest.
- **The scripting boundary is a flat C ABI**, append-only and version-gated in CI. Swift is a
  generated overlay on top. This is what makes hot reload, stable modules, and future language
  bindings tractable.
- **Barriers are computed, not written.** The render graph owns synchronisation and transient
  memory; no renderer code writes a barrier.
- **Parallelism is safe by construction.** Systems declare their access; the scheduler derives the
  dependency graph. Undeclared access is an assertion, not a race.
- **Integrate where it is not differentiating.** Jolt, miniaudio, Steam Audio, HarfBuzz, ICU,
  FreeType, Slang, Recast, meshoptimizer. Build the ECS, renderer, scene model, asset pipeline,
  audio graph and policy, UI, and editor.
- **Cost is bounded by configuration, not content.** Rendering has LOD and culling budgets; audio
  has importance tiers with per-tier source budgets; VFX has importance classes driven by a
  frame-time budget controller. At RTS scale, deciding *how much simulation each thing deserves*
  matters far more than micro-optimising the work itself.
- **Graphs are compiled, not interpreted.** Material graphs and VFX graphs both lower to engine
  shader source through the same Slang pipeline, so authoring convenience costs nothing at
  runtime — and VFX attribute layouts are *derived* from what a graph actually uses.
- **Determinism is a per-subsystem contract, decided deliberately.** AI is deterministic, because
  it drives gameplay that must survive reconciliation and replay — which is why its schedule
  derives from simulation state, never from measured frame time. Animation is partly so: root
  motion is gameplay, so it is computed on a deterministic CPU path even when the pose is evaluated
  on the GPU. VFX and ML inference are not deterministic and are firewalled from authoritative
  state. Each boundary is a requirement, not an assumption.
- **Identity is assigned and recorded, never derived from a name.** A type's and a field's
  identifiers live in a committed manifest with tombstones and a CI gate, so renaming a field or
  moving a type into a namespace leaves every scene, override, save, animation binding and
  replication schema resolving. Identifiers are never recycled: a recycled one produces data that
  loads successfully and is wrong.
- **A worker thread never blocks.** Asynchronous work is coroutines whose continuations resume as
  tasks; file reads and GPU fences suspend rather than occupy a core. Every task carries its
  worker's scratch allocator and its cancellation token, which is where the scheduler and the
  memory model meet.
- **Memory has budgets, not just tags.** Domains are apportioned by a budget tree and a pressure
  level tells every cache to trim at once — the memory counterpart of the renderer's GPU-time
  arbiter. An over-budget frame is a stutter; an over-budget heap is a crash.
- **Environmental data is a substrate, not a subsystem's property.** Moisture, biome, wind, flow
  and wetness live in sparse world-scale fields with one producer each, sampled by terrain,
  foliage, water, VFX, audio and AI alike. Put moisture inside terrain and foliage must ask terrain
  whether the ground is wet; put wind inside foliage and water must ask foliage which way it blows.
- **Physics and rendering must agree about where the water is.** A water body declares which
  displacement bands are authoritative — evaluated identically for rendering, physics and gameplay
  — and which are visual only. A boat floating on a flat plane beneath visible swell is a
  specification violation, not a tuning problem.
- **Residency, activation, asset detail, and simulation detail are four different things.**
  Collapsing them is what makes crossing a world boundary mean *load everything now*. Kept apart,
  approach is a gradient: metadata far out, resources resident, entities instantiated later, full
  detail last — each step cheap because the expensive one already happened.
- **Author hierarchically; ship flat.** Prefabs, scenes, and worlds are authoring assets with
  nesting, variants, and overrides, all of which are resolved at cook time into archetype blocks.
  Activation is a bulk copy into ECS chunks, and shipping builds carry no prefab link at all.
- **Use the cheapest source that can give a trustworthy answer.** Illumination is a hybrid of
  screen tracing, world-space caches, software tracing and hardware rays, selected per sample by a
  computed confidence rather than a fixed fallback order. A traced hit is a *cache lookup*, not a
  material evaluation — which is what makes it affordable, and which gives multi-bounce for free.
- **Illumination sees a coarser world than the camera does.** The GI scene targets centimetres of
  world-space error where primary visibility targets sub-pixels, and takes that from the geometry
  hierarchy that already exists. One hierarchy, two error targets.
- **One component measures the frame; everything else receives an allocation.** VFX, virtual
  geometry, and dynamic resolution each had their own controller reading total GPU time. Three
  feedback loops on one shared signal is not three budgets, it is one unstable control system. The
  renderer budget arbiter now owns measurement and allocation; subsystem controllers hold their
  own allocation with their own levers, on a faster time constant, and report cost upward. A slow
  outer loop over fast inner loops is stable.
- **Closures are how a material is authored; shading models are what it compiles to.** A closure
  set matching a known model costs exactly what that model costs, so layering is available without
  taxing the materials that never layer. Generality that is always paid for is not generality, it
  is overhead.
- **Detail is a continuous function, not a list of levels.** Virtual geometry selects triangle
  clusters per frame against a screen-space error target, so rendering cost tracks pixels rather
  than the triangle count of the asset. Clusters are simplified in groups so boundaries stay
  watertight; pages, not clusters, are the streaming unit; and a root region is always resident so
  an object is never missing, only coarse.
- **Render geometry is not collision geometry.** Physics, navigation and ray tracing consume
  separate representations derived from the same source asset. Conflating them is the mistake that
  makes virtualised geometry unusable for gameplay.
- **Known gaps are written down, not implied.** Cross-platform lockstep is unsupported because
  physics guarantees determinism only within a platform, and that is stated rather than discovered.
  Virtual textures and virtual shadow maps are currently specified as seams rather than
  capabilities — and terrain materials have made virtual textures the most load-bearing of those.
  Weather and hydrology are specified as field *producers* against systems that do not exist, which
  costs nothing today and no rework later. World partition was named as a gap by four subsystems
  before it was specified, and specifying it removed four workarounds rather than adding one
  system — which is the argument for writing gaps down in the first place.
- **Graphs compile; shared programs, per-instance state.** Materials, VFX, AI behaviour, control
  rigs and animation graphs all lower through a typed IR to a program shared by every instance
  using it. It is the same answer to the same problem each time, and the reason instance counts can
  be large.
- **VFX cannot touch gameplay state.** GPU simulation is non-deterministic; the firewall is a
  requirement, so effects can never break physics determinism or network reconciliation.
- **ECS is not the answer to everything.** UI elements deliberately live outside it: their counts
  are an order of magnitude higher, their workload is a tree walk rather than an archetype scan,
  and they need element-granular invalidation that chunk-granular change detection cannot give.
  Where a subsystem gains nothing from ECS, it does not go in ECS.
- **Reversed-Z, Y-up, right-handed, −Z forward, metres and radians.** Stated once, normatively,
  because a silent mismatch here corrupts everything downstream.
- **Deferred decisions are written down**, not assumed — see the non-goals in
  `engine-architecture` and the whole of `xr-support`.
