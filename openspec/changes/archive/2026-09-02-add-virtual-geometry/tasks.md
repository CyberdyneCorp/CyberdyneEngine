# Tasks: CyberGeometry

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis.

Sections 3 onward record the implementation and are **deliberately deferred to implementation
changes**. Unusually for this project, the **order matters more than the list** — see the phase
table in `design.md`. Phases 1 to 3 are the milestone that constitutes virtualised geometry;
everything after is reach.

## 1. Specification

- [x] 1.1 Record rationale, the visibility-buffer-versus-Forward+ resolution, the phase table, and
      the world partition and virtual texture gaps in `design.md`
- [x] 1.2 New `virtual-geometry` capability: asset representation, clusters, grouped simplification
      and crack-free hierarchy, geometric error and detail selection, pages, resident root, GPU page
      table and residency, GPU-driven streaming feedback, instance and cluster culling, occlusion,
      rasterisation paths, visibility buffer and material resolve, tangent policy, compression,
      budget and importance, instancing and assemblies, fallback and platform paths, collision
      separation, deformation classes, surface classification, authoring, diagnostics, streaming
      seams, and the gameplay API
- [x] 1.3 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `rendering-architecture` — visibility buffer added as a shipped pipeline alongside
      Forward+, with their differing strengths stated and content portability required
- [x] 2.2 `rendering-culling-and-lod` — LOD becomes one of two detail strategies; GPU-driven
      culling and occlusion extended to cluster granularity and two-pass visibility
- [x] 2.3 `rendering-geometry-and-resources` — a mesh may carry traditional and virtual
      representations; the traditional one serves as fallback and is still produced
- [x] 2.4 `asset-import-pipeline` — virtual geometry cooking, deterministic output for content
      addressing, fine-grained caching, and collision derived independently
- [x] 2.5 `thirdparty-dependencies` — virtual geometry recorded as engine-built; meshoptimizer's
      role extended to cluster generation, with the algorithm-integrated/architecture-owned
      distinction made explicit
- [x] 2.6 `build-system-and-platforms` — `CY_VIRTUAL_GEOMETRY` added, with the rule that disabling
      an acceleration path leaves the subsystem functional
- [x] 2.7 `animation-and-skinning` — reviewed; no change needed. The GPU pose world is already the
      contract skinned virtual geometry would consume, and that phase is deferred.
- [x] 2.8 `physics` — reviewed; no change needed. The collision-separation requirement lives in
      `virtual-geometry`, and physics receives proxies as it already does.
- [x] 2.9 **Gaps recorded, not closed**: world partition (now referenced by this capability and by
      `networking-and-replication`) and virtual textures. Seams are specified; both deserve their
      own changes.

## 3. Phase 0 — prerequisites (deferred; largely covered by other capabilities)

- [ ] 3.1 Verify GPU scene, render graph, HZB, indirect execution, async I/O, and GPU profiling are
      implemented and adequate before starting phase 1

## 4. Phase 1 — clusters and GPU culling (deferred)

- [ ] 4.1 Cluster builder with configurable size policy
- [ ] 4.2 Cluster metadata format and compact encoding
- [ ] 4.3 GPU instance culling from the GPU scene
- [ ] 4.4 GPU cluster culling: frustum, normal cone, screen size
- [ ] 4.5 Indirect draw of selected clusters
- [ ] 4.6 Benchmark against the traditional path to establish the baseline

## 5. Phase 2 — hierarchy and automatic detail (deferred)

- [ ] 5.1 Cluster grouping and group-constrained simplification
- [ ] 5.2 Iterative hierarchy build with level re-partitioning
- [ ] 5.3 Geometric error computation and validation
- [ ] 5.4 GPU hierarchy traversal with screen-error selection and subtree pruning
- [ ] 5.5 Watertightness test across levels and transitions
- [ ] 5.6 Quality presets in pixels of error

## 6. Phase 3 — virtualisation (deferred)

- [ ] 6.1 Page format, packing, and content addressing
- [ ] 6.2 Always-resident root region
- [ ] 6.3 GPU page table with generations and state flags
- [ ] 6.4 Shared GPU geometry cache and scored residency manager
- [ ] 6.5 GPU streaming feedback: request generation, compaction, async servicing
- [ ] 6.6 Predictive prefetching from camera motion and importance
- [ ] 6.7 Fallback to nearest resident ancestor, verified under forced streaming starvation

## 7. Phase 4 onward (deferred)

- [ ] 7.1 Phase 4: HZB cluster occlusion, two-pass visibility
- [ ] 7.2 Phase 5: visibility buffer, material classification and binning, compute rasteriser
- [ ] 7.3 Phase 6: virtual texture integration and joint residency budgeting
- [ ] 7.4 Phase 7: terrain, foliage and aggregate policies, destruction fragments
- [ ] 7.5 Phase 8: skinned virtual geometry over the GPU pose world, integrated with animation LOD

## 8. Cross-cutting (deferred)

- [ ] 8.1 Geometry budget controller with hysteresis and importance scaling
- [ ] 8.2 Assemblies: referenced sub-geometry with transforms
- [ ] 8.3 Tangent policy and attribute compression with reported error
- [ ] 8.4 Fallback path selection by capability query
- [ ] 8.5 Editor visualisation modes and the per-object inspector
- [ ] 8.6 Profiler with causal queries

## 9. Validation (deferred)

- [ ] 9.1 Watertightness tests across levels, transitions, and asset classes
- [ ] 9.2 Determinism tests for cooking, so content addressing holds
- [ ] 9.3 Streaming starvation tests: forced eviction, verify nothing disappears
- [ ] 9.4 Golden-image tests across pipelines and backends
- [ ] 9.5 Budget controller convergence and absence of visible pumping
- [ ] 9.6 Benchmarks: instance counts, cluster counts, and cache pressure, as regression guards
- [ ] 9.7 Comparison against the traditional path to quantify the benefit at each phase

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: the `virtual-geometry` capability is in
`openspec/specs/`, and the six affected capabilities were updated in the same change. The
unchecked items from section 3 onward are the implementation backlog and belong to future changes,
sequenced by the phase table in `design.md`.
