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
| [core-type-system](core-type-system/spec.md) | Reflection, `Var`, generational handles, events and signals, `Callable`, diagnostics |
| [core-memory-and-containers](core-memory-and-containers/spec.md) | Allocators, containers, handle pools, chunk storage, frame arenas |
| [core-math](core-math/spec.md) | Types, coordinate and depth conventions, SIMD, BVH, geometry, curves, RNG |
| [core-jobs-and-concurrency](core-jobs-and-concurrency/spec.md) | Job system, thread roles, safe-by-construction parallel systems |
| [core-assets-and-io](core-assets-and-io/spec.md) | Asset identity, cooking, virtual filesystem, packages, streaming, hot reload |
| [core-platform-abstraction](core-platform-abstraction/spec.md) | Platform services, display, input, the porting surface |

**2 — World model**
| Capability | Covers |
|---|---|
| [ecs-core](ecs-core/spec.md) | Entities, components, archetypes, queries, systems, change detection, snapshots |
| [scene-graph-and-nodes](scene-graph-and-nodes/spec.md) | Node façade, transforms, lifecycle, behaviours, coherence invariants |
| [serialization-and-prefabs](serialization-and-prefabs/spec.md) | Text and binary forms, prefabs, overrides, variants, schema migration |

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
| [rendering-forward-clustered](rendering-forward-clustered/spec.md) | Cluster assignment, sort keys, prepass modes, pass order |
| [shader-system](shader-system/spec.md) | Slang, permutations, reflection-driven binding, caching, hot reload |
| [rendering-materials-and-shading](rendering-materials-and-shading/spec.md) | The BRDF in concrete terms, shading models, IBL, material model |
| [rendering-lighting-and-shadows](rendering-lighting-and-shadows/spec.md) | Light types, physical units, LTC area lights, atlas, cascades, filtering |
| [rendering-global-illumination](rendering-global-illumination/spec.md) | Lightmaps, probes, dynamic diffuse GI, reflections, sky |
| [rendering-post-processing](rendering-post-processing/spec.md) | Chain order, AO, fog, exposure, DOF, bloom, tonemap, AA, upscaling |
| [rendering-geometry-and-resources](rendering-geometry-and-resources/spec.md) | Vertex formats, compression, LOD, instancing, skinning, textures, particles |
| [rendering-2d](rendering-2d/spec.md) | Sprites, batching, tilemaps, 2D lights and shadows, screen-space SDF |

**5 — Simulation**
| Capability | Covers |
|---|---|
| [physics](physics/spec.md) | `PhysicsServer`, Jolt backend, components, queries, character controller, determinism |
| [animation-and-skinning](animation-and-skinning/spec.md) | Clips, skeletons, animation graph, IK, retargeting, root motion, tweens |
| [navigation](navigation/spec.md) | Navmesh generation, A* + funnel, off-mesh links, local avoidance |
| [audio](audio/spec.md) | Bus graph, lock-free mixing, spatialisation, effects, scheduling |

**6 — Content and tooling**
| Capability | Covers |
|---|---|
| [text-and-fonts](text-and-fonts/spec.md) | `TextServer`, fonts, shaping, BiDi, line breaking, glyph atlases |
| [ui-system](ui-system/spec.md) | Layout, input and focus, theming, widgets, data binding, accessibility |
| [asset-import-pipeline](asset-import-pipeline/spec.md) | Importers, cook cache, texture and model import, packaging |
| [editor-architecture](editor-architecture/spec.md) | Editor as an engine app, play mode, inspector, plugins, build pipeline |

**7 — Systems and process**
| Capability | Covers |
|---|---|
| [networking-and-replication](networking-and-replication/spec.md) | Transports, authority, replication, prediction, interest management, security |
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
- **Integrate where it is not differentiating.** Jolt, HarfBuzz, ICU, FreeType, Slang, Recast,
  meshoptimizer. Build the ECS, renderer, scene model, asset pipeline, UI, and editor.
- **Reversed-Z, Y-up, right-handed, −Z forward, metres and radians.** Stated once, normatively,
  because a silent mismatch here corrupts everything downstream.
- **Deferred decisions are written down**, not assumed — see the non-goals in
  `engine-architecture` and the whole of `xr-support`.
