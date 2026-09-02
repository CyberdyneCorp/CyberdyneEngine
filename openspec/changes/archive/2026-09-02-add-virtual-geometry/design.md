# Design: CyberGeometry

## Context

Virtualised geometry is the most infrastructure-dependent subsystem in the engine. It needs a GPU
scene, indirect execution, an HZB, asynchronous streaming, a content-addressed asset store, and GPU
profiling — all of which the specification already has. It is also the subsystem where attempting
everything at once most reliably produces nothing.

The design therefore states the architecture in full and the **build order** explicitly, because
the order is the part that determines whether this ships.

## Decisions

### 1. Detail is a continuous function of screen error, not a choice among meshes

A mesh cooks into a hierarchy of clusters, each carrying a **geometric error** — how far this
simplified representation deviates from its source. At runtime the GPU converts that to a
screen-space error and descends the hierarchy until the error is below threshold.

```
   screen_error ≈ geometric_error × projection_scale / distance

   screen_error < threshold  →  render this cluster
   otherwise                 →  descend to children
```

Quality is expressed in **pixels of acceptable error**. This is the substantive improvement over
LOD chains: it is a unit that means something across every asset in the project, whereas a
per-asset LOD distance is a number someone guessed.

**Consequence.** Manual LOD authoring largely disappears for virtual-geometry assets. Traditional
LOD chains remain for the fallback path and for platforms without the virtual path.

### 2. Clusters are grouped, and groups are simplified together

Simplifying clusters independently produces cracks at their shared boundaries. The builder
therefore simplifies **groups** of neighbouring clusters together, with shared boundary vertices
constrained, and emits a smaller set of parent clusters for the group.

```
        Group (level 2)
        /            \
   Group (L1)     Group (L1)
    /    \          /    \
   C      C        C      C
```

**Rationale.** This is the crux of crack-free virtual geometry, and getting it wrong is visible as
holes in every asset. Grouping also gives better topological coherence than simplifying a whole
mesh and re-clustering.

**Trade-off accepted.** Group boundaries still constrain simplification, so a cluster's error is
bounded by its group's boundary. This is why groups are re-partitioned between levels rather than
nested rigidly.

### 3. Pages, not clusters, are the streaming unit

Clusters are small — a couple of kilobytes. Streaming them individually would be dominated by
per-request overhead. Clusters are packed into **pages** of a configurable size, and pages are the
unit of I/O, caching, compression, content addressing, and eviction.

**Page size is a benchmark, not a constant.** The specification requires it to be configurable and
reported; 64 KB, 128 KB, and 256 KB are the range to measure, because the answer depends on storage
characteristics and decompression throughput.

### 4. A resident root guarantees an asset can always render

Every asset keeps a small always-resident representation: the hierarchy root and the coarsest
clusters. When a required page is not resident, rendering uses the nearest resident ancestor.

**Rationale.** The alternative — an object that does not render until streaming completes — is
unacceptable, and every virtualised system that lacks this guarantee produces visible pop-in or
missing geometry under load. Paying a small fixed residency cost per asset buys the guarantee that
nothing is ever missing.

### 5. Streaming is driven by GPU feedback, asynchronously

The GPU knows exactly which pages it needed and did not have. It appends requests to a buffer;
those are deduplicated and serviced without a synchronous readback.

```
   frame N     GPU records page requests
   frame N+1   CPU reads the request buffer, deduplicates, issues I/O
   frame N+2+  pages become resident; coarse ancestors render meanwhile
```

**Predictive streaming supplements it.** Camera position and velocity, world streaming, and
importance let pages be prefetched rather than only requested reactively — which matters most for
an RTS, where the camera moves fast and reactive-only streaming is always a frame or two behind.

### 6. A visibility buffer pipeline, alongside Forward+ rather than replacing it

Micro-triangle geometry interacts badly with shading during the geometry pass: quad overdraw
dominates, and material complexity multiplies it. The classic answer is to separate visibility from
shading.

```
   geometry pass  →  visibility buffer { instance id, primitive id }
                          │
                  material classification → bins
                          │
                  material resolve (compute, per bin)
```

**Decision: add it as a third pipeline, do not replace Forward+.**

`rendering-architecture` already specifies pipelines as pluggable, with Forward+ and Mobile
shipping. Virtual geometry works with Forward+ — clusters rasterise into the depth prepass and the
forward pass like any other geometry — but the micro-triangle efficiency and material binning
benefits require the visibility buffer.

**Rationale for not replacing Forward+.** Forward+ handles transparency, MSAA, and varied shading
models in ways a visibility buffer does not, and those are the reasons it was chosen. Replacing it
would trade one set of strengths for another rather than adding. Two pipelines is honest about the
fact that they suit different content.

**Trade-off accepted.** Two pipelines is real maintenance cost, and content must work in both. The
specification requires virtual geometry to function in Forward+ with documented limitations, so a
project is never forced into the visibility-buffer pipeline.

### 7. Render geometry is not collision geometry

A 50-million-triangle statue does not give the physics engine 50 million triangles. Collision and
navigation representations are derived separately at cook time — proxy meshes, convex
decomposition, or SDFs.

**Rationale.** This is stated as a requirement because it is the single most tempting shortcut in a
virtualised geometry system, and taking it destroys physics performance in a way that is hard to
walk back once content depends on it.

### 8. Budget-driven quality, with gameplay-aware importance

The screen-error threshold is adjusted by a controller holding a GPU time target, and each object
carries an **importance** that scales its effective threshold.

```
   effective_error = global_threshold × importance_modifier

   hero robot    → 0.25 px
   far mountain  → 2.0 px
```

**Rationale.** This is the fifth appearance of the budget-controller pattern (VFX, audio, AI,
animation, networking). Consistency is itself valuable: a developer who understands one understands
all of them. Gameplay-aware importance is the part that matters for an RTS, where the units the
player is looking at should keep detail while the terrain behind them degrades.

**Adjustment must be smooth.** A threshold that oscillates produces visible detail pumping, which
is worse than a consistently lower quality level. Hysteresis and rate limiting are requirements.

### 9. Deformation is classified, and the system grows through the classes

| Class | Phase |
|---|---|
| Static | First |
| Rigid instanced | First |
| Terrain | Later |
| Destructible | Later |
| Skinned | Last |

Skinned virtual geometry is the hardest and the most differentiating: only visible clusters need
deformation, and combining that with animation LOD and the GPU pose world is where an RTS with
100,000 animated units becomes plausible. It is deliberately last, and the architecture reserves
the seam — clusters carry bone influence sets, and the GPU pose world already exists.

### 10. Build order is part of the design

| Phase | Delivers | Depends on |
|---|---|---|
| 0 | GPU scene, async I/O, render graph, HZB, indirect execution, GPU profiling | — |
| 1 | Clusters, GPU frustum and normal-cone culling, indirect draw | Phase 0 |
| 2 | Cluster hierarchy, offline simplification, screen-error traversal, automatic LOD | Phase 1 |
| 3 | Pages, GPU page table, streaming feedback, resident roots, geometry cache | Phase 2 |
| 4 | HZB cluster occlusion, two-pass visibility | Phase 3 |
| 5 | Visibility buffer, material binning, compute rasteriser | Phase 4 |
| 6 | Virtual texture integration | Phase 5, virtual textures |
| 7 | Terrain, foliage, destruction | Phase 5 |
| 8 | Skinned virtual geometry | Phase 7, animation |

**Phases 1 to 3 are the milestone that matters.** At the end of phase 3 the engine has genuine
virtualised geometry — automatic detail selection and on-demand streaming. Everything after is
optimisation and reach.

## Risks

- **Crack-free simplification** is the correctness risk. Mitigation: group-constrained boundaries
  are a requirement, and a dedicated test asserts watertightness across level transitions.
- **Streaming latency under fast camera motion**, the RTS case. Mitigation: resident roots
  guarantee something renders; predictive streaming reduces how often the fallback is visible.
- **Two pipelines** is maintenance cost. Mitigation: virtual geometry must work in both, so the
  visibility-buffer pipeline is an optimisation rather than a requirement.
- **Debuggability.** GPU-driven systems are hard to inspect. Mitigation: the visualisation and
  profiler requirements are as detailed as the runtime ones, and are not optional.
- **Scope.** This is the largest capability specified. Mitigation: the phase table, with phases 1–3
  as the milestone.

## Open questions

- **World partition.** Referenced by this capability and by `networking-and-replication`, and still
  absent. Geometry page prefetching wants cell membership; networking wants it for interest. Two
  subsystems now improvise around the same missing thing.
- **Virtual textures.** Virtualised geometry without virtualised textures leaves the pairing
  incomplete: a photogrammetry asset streams its geometry on demand and its textures wholesale.
  A `virtual-texture` capability is the natural companion change.
- Whether the visibility-buffer pipeline should eventually become the default for desktop, with
  Forward+ retained for transparency-heavy content. Deferred until both exist and can be measured.
- Whether cluster group re-partitioning between hierarchy levels should be topology-driven or
  spatial. Affects simplification quality and build time; needs measurement.
