# M7 — Fidelity: film detail at a budget an arbiter holds

## Why

M6 made the world larger than memory and the build a graph. It did not put either on a GPU. That is
the sentence this milestone exists to make false, and it is not a criticism of M6 — the CPU side of
`virtual-texturing` is complete and measured, `rendering-culling-and-lod`'s reference computes
exactly what a dispatch must compute, and both were built that way deliberately so that the shader
which lands here is checked against a reference rather than against a screenshot. But the tier
records say what a real project can depend on, and today a project cannot get a virtual texture onto
the screen or issue an indirect draw the engine culled.

M7 is also the milestone at which the renderer stops being a pipeline and becomes a set of systems
that negotiate. Six paged or stochastic systems arrive here — virtual geometry, virtual shadows,
global illumination, reflections, denoising and temporal reconstruction — and each of them can spend
an unbounded amount of a frame. `rendering-architecture` reaches Complete because the **budget
arbiter** is what makes six unbounded systems into one frame, and a milestone that added them without
it would be a milestone whose artefact is a slideshow.

## What Changes

- **The material compiler.** Graph → IR → closures → program, optimisation passes, the GPU material
  table, classification and binning, quality tiers, cost analysis and cooking.
- **Virtual geometry.** The asset, clusters, the crack-free hierarchy, geometric error, geometry
  pages, the always-resident root, GPU streaming feedback, GPU traversal and cluster culling, the
  visibility buffer and material resolve.
- **Virtual shadows.** Receiver-driven pages, clipmaps, the page cache, precise invalidation, update
  classes, the budget, derived bias and the fallback chain.
- **Temporal rendering** as one framework: jitter, derived motion vectors, history, invalidation and
  reprojection — written once and used by everything stochastic rather than five times.
- **Post-processing**, **global illumination**, **denoising** and **ray-tracing infrastructure**.
- **The renderer budget arbiter**, renderer profiles and pipeline configuration; `rendering-architecture`
  to Complete.
- **Area lights, decals, light functions, channels and stochastic many-light**; an analytic sky
  sufficient for GI's sky term.
- **A Metal seed**, to expose Vulkan-specific assumptions while they are still cheap.

## What M6 handed forward, and this change owns

M6's closing gate recorded these. They are listed here so that they are carried deliberately rather
than rediscovered, in the order of what each costs if it is ignored.

- **There are still two derivation-key functions and two derived-data caches, and the one that cooks
  real content is the blind one.** `tools/build/`'s `derivation_key` refuses a key without a
  toolchain fingerprint, which is the correctness bug M6's spike was commissioned to find. But
  `cy::import::import_derivation_key` (`tools/import/src/importer.cpp`) is unchanged and contributes
  no compiler, no flags and no library versions — and it is the function `cy_import_cli` actually
  uses. `asset-import-pipeline` requires "one cache covering all derived data" and
  `build-and-packaging` says the same; `design.md` §1.8 and §1.9 item 1 of the M6 change say what
  merging them costs. **Until they are one, a shared import cache can serve an artefact built by a
  different compiler and report a hit.**
- **The real cooks are not nodes in the build graph.** `cy_cook` and `cy_import_cli` run outside it;
  the graph's producers are its own four builtins. M7 cooks materials, virtual geometry and shadow
  data, all of them expensive and all of them exactly what a derivation graph is for.
- **Nothing samples a virtual texture and nothing dispatches a cull.** `cy::servers-render-culling`
  and `cy::servers-render-geometry` are linked by nothing but their own test binaries — not by the
  renderer, and not by M6's own artefact. Their tiers are recorded accordingly and M7 is where they
  are earned.
- **`rendering-geometry-and-resources` cannot complete until the GPU pose world exists.**
  `SkinningDescriptor::validate()` refuses `PoseSource::GpuPoseWorld` with `NotImplemented` naming
  M8. That row's Complete moved to M8 at M6's gate; M7 must not quietly invent a second pose upload
  to get around it, which is the failure that requirement exists to prevent.
- **Two overlay models.** `cy::world::PersistenceOverlay` and `cy::save::Overlay` are two structures
  with two identity types for one requirement, and M6's artefact converts between them. `src/save/`'s
  README carries the resolution — one `PersistentId` at layer 0, and the world holding a save overlay
  — and doing it after a save format has shipped is what migration is for. **It is cheaper here than
  at M8.**
- **`core-assets-and-io` still has no streaming.** Its "Streaming" requirement — partial residency
  for textures, meshes and audio driven by a residency budget and renderer feedback — is what M6
  planned to complete and did not touch. M7 has the feedback and the budget; the row's Complete
  moved to M7 for that reason.
- **`core-memory-and-containers` reports on three of five attribution axes.** By type, by thread, by
  world cell and by asset are what "why is this region consuming this much" needs, and the world cell
  now exists. Its Complete moved to M7.

## Capabilities

### Advanced Capabilities

`material-compiler`, `virtual-geometry`, `virtual-shadows`, `temporal-rendering`,
`rendering-post-processing`, `rendering-global-illumination`, `denoising`,
`ray-tracing-infrastructure` and `rendering-lighting-and-shadows` to **Working**;
`atmosphere-sky-and-clouds` to **Seed**; `rendering-architecture`, `rendering-culling-and-lod`,
`residency`, `virtual-texturing`, `shader-system`, `rendering-materials-and-shading`,
`editor-viewport-and-gizmos`, `core-assets-and-io` and `core-memory-and-containers` to **Complete**;
a **Metal seed** in `rhi-and-render-graph`.

## Impact

- **New code**: the material compiler and its IR, virtual geometry's cook and GPU traversal, virtual
  shadows, the temporal framework, the post chain, the GI and reflection tiers, the denoiser, the
  ray-tracing layer, and the budget arbiter.
- **New dependencies**: to be settled by the spikes; none is assumed by this proposal.
- **Closing artefact**: `samples/07-fidelity` — a film-detail interior and exterior with millions of
  source triangles, dynamic lighting, indirect illumination and reflections, holding a frame budget
  while the arbiter reallocates under a scripted load spike.
- **Risks**, in the order the roadmap gives them: the material IR and closure lowering; the budget
  arbiter's control loop; virtual geometry's cluster hierarchy build and GPU traversal. The first two
  get spikes, because both are decisions that are expensive to reverse once six systems depend on
  them.
