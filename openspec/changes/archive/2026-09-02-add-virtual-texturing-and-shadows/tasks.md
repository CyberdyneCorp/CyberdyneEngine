# Tasks: CyberTexture and CyberShadow

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by the
phase table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: shared policy with separate storage and why unifying storage is the
      trap, one render importance instead of five, residency models as a property rather than a
      type, the resident-tail pattern appearing a third time, prediction covering latency while
      feedback establishes accuracy, derivative reconstruction under the visibility buffer,
      receiver-driven shadow pages, clipmap snapping as the precondition for caching, shadow
      geometry error decoupled from primary error, the shadow–texture cycle broken in three places,
      staleness as a budget lever, static/dynamic page separation as a benchmark rather than a
      mandate, conventional shadows retained, deadline propagation, and the phase table
- [x] 1.2 New `residency`: shared policy and separate storage, unified render importance, request
      priority, deadline propagation, budgets and pressure response, eviction and churn control, the
      no-blocking rule, and diagnostics
- [x] 1.3 New `virtual-texturing`: residency models, address spaces, page tables, tiles and borders,
      the shared physical cache, the resident mip tail, GPU feedback, predictive prefetch, runtime
      producers, runtime invalidation and persistence, semantics and mip generation, compression and
      page storage, UDIM and multi-layer assets, virtual lightmaps, sampling and derivatives, and
      diagnostics
- [x] 1.4 New `virtual-shadows`: shadow modes, virtual address spaces, receiver-driven page marking,
      clipmaps and snapping, the page cache, precise invalidation, deformation and cache validity,
      shadow geometry detail, GPU-driven caster selection, shadow material programs, the
      never-wait-on-textures rule, update classes and staleness, budget, filtering and softness,
      derived bias, contact and traced refinement, the fallback chain, content-specific caster
      policy, multi-view page sharing, and diagnostics
- [x] 1.5 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 **Both remaining named gaps are closed.** `virtual-geometry` recorded virtual textures as a
      seam and `terrain` specified kilometre-scale materials against a capability that did not
      exist; virtual shadow maps were the announced companion to the illumination change. Neither is
      a placeholder any more.
- [x] 2.2 `virtual-geometry` — the streaming seam is closed: three levels of streaming are named,
      geometry and texture residency are jointly budgeted through the shared policy, and the policy
      layer explicitly does not take ownership of page storage
- [x] 2.3 `terrain` — terrain materials are produced through a runtime virtual texture producer, so
      an expensive layer graph is evaluated once per page rather than per pixel per frame, with
      local invalidation on deformation
- [x] 2.4 `rendering-lighting-and-shadows` — the conventional atlas is reframed as the shipping path
      for constrained profiles and the capability fallback, explicitly not deprecated, with the two
      paths coexisting per light
- [x] 2.5 `rendering-geometry-and-resources` — texture streaming gains residency models, and mip
      streaming is retained as a first-class model rather than a legacy path
- [x] 2.6 `material-compiler` — the shadow program joins the program family, with distance tiers,
      shadow-critical texture marking, and opacity coverage difference reported so a wrong
      derivation is visible
- [x] 2.7 `rendering-architecture` — render importance is published once in the GPU scene and
      consumed by every quality decision; foliage, terrain, and water are named as producers; and
      previous and current bounds are stated as shared by shadow invalidation and motion vectors
- [x] 2.8 `core-memory-and-containers` — paged subsystems reduce together through the residency
      layer rather than as independent evictions competing for the same memory
- [x] 2.9 `thirdparty-dependencies` — virtualisation and residency policy recorded as engine-built,
      with codecs and encoders integrated beneath them
- [x] 2.10 `rendering-global-illumination`, `foliage`, `world-partition-and-streaming` — reviewed;
      no change needed. GI already consumes shadowed direct lighting through the surface cache and
      keeps its representations independent; foliage already declares surface classes the shadow
      caster policy consumes; the world already propagates prediction, which the residency layer now
      turns into deadlines.
- [x] 2.11 **Remaining gaps**: weather, hydrology and erosion, and procedural generation beyond
      foliage rules. No capability is currently working around any of them.

## 3. Phase 1 — conventional foundations (deferred)

- [ ] 3.1 Texture cook, semantic-aware mip generation, block compression, bindless access
- [ ] 3.2 Mip streaming with feedback and budgets
- [ ] 3.3 Conventional cascades, spot and point shadow maps, filtering, derived bias
- [ ] 3.4 Establish the correctness reference both virtual systems will be compared against

## 4. Phase 2 — virtualisation (deferred)

- [ ] 4.1 Virtual texture address spaces, page tables, physical caches, borders, mip tail
- [ ] 4.2 Offline tile cooking, content-addressed page storage
- [ ] 4.3 Virtual shadow address spaces for spot lights; receiver-driven marking; dirty-page render
- [ ] 4.4 Shadow page cache with basic invalidation

## 5. Phase 3 — feedback and clipmaps (deferred)

- [ ] 5.1 GPU feedback generation, compaction, deduplication, priority
- [ ] 5.2 Adaptive feedback density
- [ ] 5.3 Directional clipmaps with world-space page snapping
- [ ] 5.4 Analytic derivative reconstruction for the visibility buffer path, with the mip
      visualisation

## 6. Phase 4 — GPU-driven and integrated (deferred)

- [ ] 6.1 Asset and I/O integration, streaming budgets, async page loads
- [ ] 6.2 GPU-driven caster culling through the geometry hierarchy at the shadow error target
- [ ] 6.3 Page-batched indirect shadow rasterisation
- [ ] 6.4 Shadow material programs with distance tiers; shadow-critical texture pinning

## 7. Phase 5 onward (deferred)

- [ ] 7.1 Runtime texture producers: terrain composition, decals, world state, procedural
- [ ] 7.2 Runtime page persistence as a delta in the world overlay
- [ ] 7.3 Precise shadow invalidation from GPU scene bounds; deformation modes
- [ ] 7.4 Shared residency layer: scoring, budgets, deadline propagation, coordinated pressure
- [ ] 7.5 Update classes, staleness, shadow budget controller
- [ ] 7.6 Content-specific caster policy for foliage, ground cover, terrain, water
- [ ] 7.7 Stochastic shadow filtering through the shared denoiser
- [ ] 7.8 Contact and traced refinement
- [ ] 7.9 Evaluate static/dynamic shadow page separation against measurement before adopting it
- [ ] 7.10 Evaluate GPU decompression and direct storage-to-GPU page transfer

## 8. Validation (deferred)

- [ ] 8.1 Reference comparison: virtual texturing and virtual shadows against the conventional paths
      from phase 1, as correctness and quality baselines
- [ ] 8.2 Mip selection parity between the forward path and the visibility buffer path within the
      declared tolerance — the derivative reconstruction regression guard
- [ ] 8.3 Filtering across tile borders produces no seams under anisotropic sampling
- [ ] 8.4 Fallback tests: forced starvation shows coarse content, never missing content, and never a
      stall
- [ ] 8.5 **Cycle test**: shadow rasterisation of masked geometry with non-resident opacity pages
      completes without stalling and without a wrong silhouette
- [ ] 8.6 Clipmap snapping test: sub-page camera movement invalidates no cached page
- [ ] 8.7 Invalidation precision test: moving one object dirties only the pages its bounds project
      into
- [ ] 8.8 Camera cut test: a cut does not produce a frame spike; pages refresh by priority over
      several frames
- [ ] 8.9 Churn tests: sustained demand slightly above budget does not oscillate, and churn is
      reported
- [ ] 8.10 Coordinated pressure test: geometry, texture, and shadow reduce by importance rather than
      competing
- [ ] 8.11 Determinism: page request sets are reproducible for a fixed camera path in deterministic
      mode
- [ ] 8.12 Benchmarks: shadow cost against the conventional path at equal quality, and texture memory
      against fully resident content, as regression guards

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `residency`, `virtual-texturing` and
`virtual-shadows` are in `openspec/specs/`, and eight existing capabilities were updated. **Both
remaining named gaps are closed** — terrain had been specified against virtual textures for three
changes, and virtual shadow maps were the announced companion to the illumination work. The
unchecked items from section 3 onward are the implementation backlog; **phase 1 is deliberately
conventional in both columns**, because virtualising before ordinary textures and shadows work
leaves no correct reference to compare against.
