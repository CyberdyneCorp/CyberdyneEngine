# Tasks: CyberRenderer and CyberMaterial

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog and belong to
future changes, sequenced by the milestone table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: the competing-controllers problem and its resolution, bindless as
      architecture rather than optimisation, closures lowering to shading models, derived static
      and runtime parameter classification, the semantic attribute interface and its honest cost
      under Forward+, quality tiers as a bounded permutation axis, the temporal framework, the PSO
      relocation, and the milestone table
- [x] 1.2 New `material-compiler` capability: authoring forms, material IR, optimisation passes,
      closures, lowering, parameter classification, attribute interface, programs and instances,
      GPU material table, classification and binning, quality tiers, escape hatches, cost
      analysis, node previews, the API, cooking and versioning, validation, and portability
- [x] 1.3 New `temporal-rendering` capability: framework ownership, jitter, derived motion
      vectors, history resources, invalidation events, reprojection and disocclusion,
      diagnostics, and pinned determinism
- [x] 1.4 New `ray-tracing-infrastructure` capability: the service boundary, geometry adapters per
      source, structure lifecycle and budget, the ray query interface, capability gating, and
      diagnostics
- [x] 1.5 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `rendering-architecture` — renderer profiles, the **budget arbiter**, render features,
      and the render pipeline configuration asset added; the view model extended with view
      families and per-view allocations
- [x] 2.2 `vfx-system` and `virtual-geometry` — their budget controllers now hold an allocation
      and report cost, rather than measuring frame time. This is the change that removes three
      competing control loops, and it required editing requirements written in earlier changes;
      every original scenario is preserved
- [x] 2.3 `rendering-post-processing` — dynamic resolution becomes an arbiter allocation;
      anti-aliasing and upscaling consume the temporal framework
- [x] 2.4 `rhi-and-render-graph` — bindless becomes the default architecture, with the
      compatibility path's real limitation on GPU-generated workloads written down
- [x] 2.5 `shader-system` — PSO management and a distributed compilation service with a tiered
      cache added; material shader generation and the visual editor delegate authoring to
      `material-compiler` and retain compilation, caching, and hot reload
- [x] 2.6 `rendering-forward-clustered` — "Pipeline compilation strategy" removed with a
      supersession note; both scenarios preserved in the new home
- [x] 2.7 `rendering-materials-and-shading` — shading models become the lowered form of closures;
      the material model gains the static/runtime split; parameter storage moves to the GPU
      material table
- [x] 2.8 `rendering-lighting-and-shadows` — decals become GPU scene residents with a budget
- [x] 2.9 `rendering-global-illumination` — ray-traced effects consume the new infrastructure
- [x] 2.10 `editor-architecture` — render graph debugger, budget arbiter view, and the shader and
      material inspector, specified as built alongside the renderer rather than after it
- [x] 2.11 `thirdparty-dependencies` — material compiler, budget arbiter, and temporal framework
      recorded as engine-built, with the owned-IR/integrated-backend line drawn explicitly
- [x] 2.12 `rendering-culling-and-lod`, `animation-and-skinning`, `ui-system` — reviewed; no
      change needed. Culling and LOD already defer detail policy to the consuming systems;
      animation LOD is a CPU concern and does not compete for the GPU allocation; UI is composited
      at native resolution and is already excluded from the scaled path.
- [x] 2.13 **Gaps recorded, not closed**: virtual shadow maps and the dynamic GI stack (the
      announced next change), virtual textures, and world partition

## 3. V1 — the foundation (deferred)

- [ ] 3.1 RHI with the bindless resource model as the default path
- [ ] 3.2 Render graph: culling, scheduling, aliasing, barriers, async compute, parallel recording
- [ ] 3.3 GPU scene with incremental extraction
- [ ] 3.4 GPU-driven culling and indirect draw
- [ ] 3.5 Material compiler: graph and text front-ends, IR, optimisation passes, standard closures
- [ ] 3.6 Lowering to the `Lit` model; GPU material table; parameter classification
- [ ] 3.7 One pipeline end to end: depth, clustered lights, basic shadows
- [ ] 3.8 Temporal framework: jitter, motion vectors, history, invalidation; TAA on top of it
- [ ] 3.9 Tonemapping and the post chain
- [ ] 3.10 Budget arbiter with allocations, floors, and reporting
- [ ] 3.11 PSO manifest, cache warming, and the fallback pipeline
- [ ] 3.12 Render graph debugger and material inspector — built now, not later

## 4. V2 — density (deferred)

- [ ] 4.1 Visibility buffer pipeline with material resolve
- [ ] 4.2 Material classification and binning
- [ ] 4.3 Virtual geometry integration (see `virtual-geometry`)
- [ ] 4.4 Virtual textures (unspecified; needs its own change)
- [ ] 4.5 Async compute scheduling across the frame
- [ ] 4.6 Quality tier generation and per-instance tier selection

## 5. V3 — fidelity (deferred)

- [ ] 5.1 Ray tracing infrastructure: adapters, lifecycle, budget, queries
- [ ] 5.2 Dynamic GI and reflections (next change)
- [ ] 5.3 Virtual shadow maps (next change)
- [ ] 5.4 Advanced transparency, volumetrics, atmosphere
- [ ] 5.5 Terrain and water as geometry sources with their own adapters

## 6. V4 — reach (deferred)

- [ ] 6.1 GPU-driven skinned virtual geometry
- [ ] 6.2 Hair, path tracing, cinematic rendering
- [ ] 6.3 Foveated XR rendering
- [ ] 6.4 ML-based upscaling as an upscaler backend

## 7. Validation (deferred)

- [ ] 7.1 Golden-image tests per pipeline, per profile, per backend
- [ ] 7.2 Content portability tests: the same scene under every shipped profile and pipeline
- [ ] 7.3 Budget arbiter stability tests: verify no oscillation when load hovers at the budget,
      and that no subsystem adjusts for a cost it did not incur
- [ ] 7.4 Temporal correctness tests: camera cuts, resolution changes, teleports, disocclusion
- [ ] 7.5 Material compiler tests: closure lowering equivalence, optimisation pass correctness
      under bisection, permutation counts against budget
- [ ] 7.6 Determinism tests: identical command streams and identical pinned-mode temporal state
- [ ] 7.7 PSO coverage tests: verify a shipped manifest covers a full playthrough with no
      fallback-pipeline draws

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `material-compiler`, `temporal-rendering`,
and `ray-tracing-infrastructure` are in `openspec/specs/`, and twelve existing capabilities were
updated in the same change — including the three budget controllers that previously measured frame
time independently. The unchecked items from section 3 onward are the implementation backlog,
sequenced by the milestone table in `design.md`; **V1 is the milestone that matters**.
