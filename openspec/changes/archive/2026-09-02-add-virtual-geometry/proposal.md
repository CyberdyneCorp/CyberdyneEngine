# Add CyberGeometry: virtualised geometry

## Why

The engine currently renders geometry the conventional way: meshes with authored or generated LOD
chains, selected per instance on the CPU by projected coverage. That is a solved, well-understood
design, and it caps what the engine can do with detail.

Two things push past it. **Authoring cost**: manual LOD chains are a persistent tax on every asset,
and generated chains still require per-asset tuning and still pop. **Scale**: a
30-million-triangle scan and a 5,000-triangle prop are the same problem to a system that selects
detail by *projected geometric error* rather than by picking one of four meshes.

The engine is unusually well positioned for this. The GPU scene, the render graph, GPU-driven
culling, the HZB, the asset cooker with content-addressed pages, and the budget-controller pattern
all already exist in the specification. Virtualised geometry is the thing they were leading toward,
and specifying it now — while the renderer is still on paper — is far cheaper than retrofitting it.

The goal is not to reimplement Nanite. It is to build a virtualised geometry system whose
architecture is native to this engine's GPU scene, streaming, budgeting, and animation systems
rather than adjacent to them.

## What Changes

- **New `virtual-geometry` capability.** Meshes cook into a hierarchy of triangle **clusters**
  with per-node geometric error, packed into streamable **pages**, with a small always-resident
  root so an asset can always render.
- **Detail is selected on the GPU by screen-space error**, not by CPU LOD choice. Quality is
  expressed in **pixels of acceptable geometric error**, which is a meaningful unit in a way that
  "LOD distance = 37 m" is not.
- **Cluster-granular culling**: instance culling narrows to objects, then hierarchy traversal and
  cluster culling (frustum, normal cone, HZB occlusion, screen size) narrow to visible clusters —
  all GPU-driven, with no CPU draw loop.
- **Geometry page streaming** driven by **GPU feedback**: the GPU reports pages it needed and did
  not have, requests are deduplicated and serviced asynchronously, and rendering meanwhile falls
  back to the nearest resident ancestor.
- **A visibility-buffer pipeline** as a third shipped pipeline, separating "what triangle is here"
  from "what does it look like", with material resolve binned by material. This is where micro-
  triangle geometry pays off.
- **A budget controller** adjusting the screen-error threshold to hold a GPU time target, with
  per-object **importance** so gameplay-critical geometry keeps detail under pressure.
- **A fallback mesh** on every asset, for unsupported hardware, ray tracing, physics proxy
  generation, and editor tooling — virtual geometry is an acceleration path, not the only
  representation.
- **Deformation classification**, so the system can grow from static and rigid-instanced geometry
  to terrain, destruction, and skinned clusters without redesign.
- **Extensive visualisation and profiling**, because these systems are otherwise close to
  undebuggable.

Non-goals for this change: a compute micro-triangle rasteriser, skinned virtual geometry, terrain,
foliage-specific representations, and native ray tracing against virtual geometry. Each is recorded
as a phase with its seam reserved.

## Capabilities

### New Capabilities

- `virtual-geometry` — asset representation, cluster hierarchy and error metric, page format and
  streaming, GPU traversal and culling, rasterisation paths, visibility buffer and material
  resolve, residency and budgeting, fallback and platform paths, authoring and diagnostics.

### Modified Capabilities

- `rendering-architecture` — add the **visibility buffer** pipeline to the shipped set.
- `rendering-culling-and-lod` — LOD becomes one of two detail strategies; cluster-granular culling
  and occlusion are specified as extensions of the existing passes.
- `rendering-geometry-and-resources` — a mesh asset may carry a traditional representation, a
  virtual representation, or both; mesh LOD is scoped to the traditional path.
- `asset-import-pipeline` — virtual geometry cooking, with collision and navigation explicitly
  derived from separate representations.
- `thirdparty-dependencies` — virtual geometry recorded as engine-built; meshoptimizer's role
  extended to cluster building and simplification.
- `build-system-and-platforms` — add `CY_VIRTUAL_GEOMETRY`.

## Impact

- **Renderer**: introduces a second detail strategy and a third pipeline. The Forward+ pipeline
  continues to work with virtual geometry, with documented limitations; the visibility-buffer
  pipeline is where the full benefit lands.
- **Streaming**: geometry pages become a major consumer of the asset system and a new residency
  budget.
- **Physics and navigation**: unchanged, and explicitly so — render geometry SHALL NOT become
  collision geometry.
- **Known gaps this depends on**: **world partition** (streaming coordination, referenced again
  here after `networking-and-replication`) and **virtual textures** (the natural pair for
  virtualised geometry). Both are specified as seams, not invented here.
- **Risk**: this is the largest single capability in the specification set, and the phasing matters
  more than the feature list.
