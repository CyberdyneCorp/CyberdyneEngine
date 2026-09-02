# Tasks: CyberGI

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog and belong to
future changes, sequenced by the phase table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: the GI scene as a named representation with its own error target,
      the surface cache as what makes hybrid tracing affordable, secondary material programs,
      confidence as a computed number, the shared diffuse/specular infrastructure, the emissive
      firefly classification, the convergence-versus-reproducibility resolution, why direct
      lighting stays out of the GI solver, baked lighting as a seed rather than a legacy path, and
      the phase table
- [x] 1.2 `rendering-global-illumination` reworked into CyberGI: engine-owned architecture and
      subsystem decomposition, GI scene, surface cache, distance field, radiance cache, probe
      update scheduling, incremental invalidation, screen-space tracing, tracing tiers and
      selection, sample confidence, shared reflections, emissive classification, dynamic object
      classification, far field, budget and importance, GI volumes, transparency and media,
      convergence and capture, path tracer and ground truth, diagnostics, and debug visualisation
- [x] 1.3 New `denoising` capability: one framework, the pipeline, material and geometry
      awareness, signal-specific configuration, budget levers, and diagnostics
- [x] 1.4 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `rendering-global-illumination` — "Screen-space reflections" removed with a supersession
      note and replaced by "Screen-space tracing"; keeping it named as a reflection technique was
      what allowed diffuse GI and reflections to be built as unrelated systems. Both original
      scenarios preserved.
- [x] 2.2 `material-compiler` — secondary and far-field programs added, with automatic derivation,
      author override, and the reported albedo difference that makes a wrong derivation visible
- [x] 2.3 `rendering-lighting-and-shadows` — light channels and the stochastic many-light path
      added; "Light culling and limits" modified so the per-cluster bound is stated as applying to
      the clustered path only, and the active path is reported
- [x] 2.4 `rendering-post-processing` — volumetric fog takes indirect from the radiance cache; AO
      gains a ray-traced option and is denoised as a visibility term
- [x] 2.5 `virtual-geometry` — illumination representations come from the existing cluster
      hierarchy at a world-space error target, not from a separately cooked simplification
- [x] 2.6 `testing-and-quality` — golden images of GI scenes use converged mode and fail rather
      than flake; path-traced reference comparison added as a tracked error metric
- [x] 2.7 `thirdparty-dependencies` — illumination architecture and denoising framework recorded as
      engine-built, with a drop-in GI library evaluated against the coupling it cannot provide
- [x] 2.8 `rendering-architecture`, `temporal-rendering`, `ray-tracing-infrastructure` — reviewed;
      no change needed. The budget arbiter already allocates to GI; the temporal framework already
      exposes what denoising and GI accumulation need; the ray tracing service already exposes
      tiers, adapters, and budgets.
- [x] 2.9 **Gaps recorded, not closed**: virtual shadow maps (deliberately excluded — a shadow
      system, not an illumination one), **world partition** (now the fourth subsystem to specify a
      seam into it), virtual textures, volumetric clouds, refraction and caustics

## 3. Phase 1 — physical direct lighting (deferred)

- [ ] 3.1 Verify the direct lighting path, physical units, and exposure are correct before any
      indirect work, since indirect error is unfalsifiable on top of incorrect direct light
- [ ] 3.2 Light channels and channel-aware cluster assignment

## 4. Phase 2 — screen-space GI (deferred)

- [ ] 4.1 Screen-space tracing for diffuse and specular with per-sample confidence
- [ ] 4.2 Denoising framework: accumulation, variance, spatial filter, history validation
- [ ] 4.3 Material- and instance-aware edge stopping from visibility buffer identifiers
- [ ] 4.4 Confidence-weighted resolve

## 5. Phase 3 — radiance cache (deferred, the milestone that matters)

- [ ] 5.1 Adaptive geometry-aware probe placement
- [ ] 5.2 Probe encoding with visibility, and clipmap organisation
- [ ] 5.3 Priority-scheduled probe updates with progress guarantees
- [ ] 5.4 Incremental invalidation from GPU scene change detection
- [ ] 5.5 Indirect diffuse resolved from the cache; multi-bounce feedback

## 6. Phase 4 — GI scene and software tracing (deferred)

- [ ] 6.1 GI scene with illumination error targets, sourced from the geometry hierarchy
- [ ] 6.2 Surface cards and the surface cache
- [ ] 6.3 Secondary material programs in the material compiler
- [ ] 6.4 Sparse distance field clipmaps with per-asset fields composited by transform
- [ ] 6.5 Software sphere tracing and sky visibility
- [ ] 6.6 Cell-scoped GI scene residency (blocked on world partition)

## 7. Phase 5 onward (deferred)

- [ ] 7.1 Phase 5: hardware tracing as a tier, resolving hits through the surface cache
- [ ] 7.2 Phase 6: reflections unified into the tracer with the roughness strategy
- [ ] 7.3 Phase 7: stochastic many-light direct lighting with reservoir reuse
- [ ] 7.4 Phase 8: offline path tracer, baking, cache seeding, ground-truth comparison

## 8. Cross-cutting (deferred)

- [ ] 8.1 GI budget distribution with importance and foveation
- [ ] 8.2 GI volumes
- [ ] 8.3 Emissive classification and promotion to lights
- [ ] 8.4 Illumination classification of dynamic objects; skinned proxies from the GPU pose world
- [ ] 8.5 Far-field representation and blending
- [ ] 8.6 Convergence metric and converged mode
- [ ] 8.7 Diagnostics able to answer causal questions, and the GI profiler

## 9. Validation (deferred)

- [ ] 9.1 Light leak tests across walls, floors, and doorways
- [ ] 9.2 Convergence tests: bounded frames to converge after light, geometry, and material changes
- [ ] 9.3 Golden images in converged mode across backends and profiles
- [ ] 9.4 Path-traced reference comparison with tracked error per scene
- [ ] 9.5 Energy conservation tests: multi-bounce does not gain energy over frames
- [ ] 9.6 Firefly tests: small bright emitters produce no sparse bright samples
- [ ] 9.7 Budget tests: GI holds its allocation without visible pumping, and reports when at its
      minimum
- [ ] 9.8 Software-tier parity: the same scene lit with and without hardware tracing, with the
      difference measured rather than assumed

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `rendering-global-illumination` is reworked
into CyberGI (29 requirements), `denoising` is a new capability, and seven other capabilities were
updated in the same change. The unchecked items from section 3 onward are the implementation
backlog, sequenced by the phase table in `design.md`; **phase 3, the scheduled world-space radiance
cache, is the milestone that matters** — screen-space GI before it is a demo.
