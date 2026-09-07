# Tasks: M7 — Fidelity

Ordered. The two spikes first, because both settle decisions that every later section builds on.
Sections 2 to 5 are the debts M6 recorded, placed before the rendering work rather than after it —
each is an edit today and a migration once this milestone's cooks and systems depend on it.

## 0. The spikes

- [ ] 0.1 Spike the **material IR and closure lowering** outside the repository, as M3's, M5.5's and
      M6's were. The criterion is M7's own: the IR round-trips, and a graph and a hand-written
      material produce identical programs
- [ ] 0.2 Spike the **budget arbiter's control loop**. The criterion is convergence without
      oscillation under a step load, over the lever shape `residency` already declares
- [ ] 0.3 Record both findings in `design.md` so the implementing agents do not re-derive them

## 1. M6's debts, before anything new depends on them

- [ ] 1.1 **One derivation key and one derived-data cache.** `cy::import::import_derivation_key`
      contributes no compiler, no flags and no library versions and it is the function `cy_import_cli`
      uses; `tools/build/`'s refuses a key without a toolchain fingerprint. `asset-import-pipeline`
      requires one cache covering all derived data
- [ ] 1.2 Fold `cy::shader::CacheKeyInputs` into the same key, per M6 `design.md` §1.8: the merged key
      is the union of what each got right
- [ ] 1.3 **One persistent identity and one overlay.** `cy::world::PersistenceOverlay` and
      `cy::save::Overlay` are two structures for one requirement; `src/save/README.md` carries the
      resolution
- [ ] 1.4 **`cy_cook` and `cy_import_cli` become nodes in the build graph**, with the two-run
      determinism gate `just content-validate` already provides
- [ ] 1.5 The eight clang diagnostics M6's spike recorded — `-Wunused-private-field` in
      `src/core/assets/include/cy/core/assets/watch.h` and seven `-Wdouble-promotion` in
      `tools/import/src/mesh.cpp` — so the tree builds under both compilers and "which compiler
      produced this artefact" can have two answers

## 2. `core-assets-and-io` → Complete

- [ ] 2.1 Partial residency for textures (mips), meshes (LODs) and audio, driven by the `residency`
      budget and by renderer feedback — the requirement M6 planned to complete and did not touch
- [ ] 2.2 A not-yet-resident level falls back to the highest resident level, and never blocks a frame

## 3. `core-memory-and-containers` → Complete

- [ ] 3.1 Attribution by type, by thread, by world cell and by asset — the four axes of the five that
      `cy/core/memory/diagnostics.h` has no field for. The world cell exists from M6

## 4. `virtual-texturing` → Complete, and `residency` → Complete

- [ ] 4.1 A feedback buffer written by a shader and resolved without a per-pixel stream reaching the
      CPU
- [ ] 4.2 A page table sampled by a shader, and the mip tail's guarantee held on the device
- [ ] 4.3 More than one subsystem registered against the residency policy in a shipped path, so the
      arbitration the capability exists for is exercised by a frame rather than by a test

## 5. `rendering-culling-and-lod` → Complete

- [ ] 5.1 The compute dispatch, checked against `cpu_reference_cull` by comparing buffers
- [ ] 5.2 The renderer consumes it: `cy::servers-render-culling` is linked by something other than its
      own tests

## 5b. `editor-viewport-and-gizmos` → Complete: the engine's own image, and a drag that lands

**THIS TIER IS NAMED IN THE PROPOSAL AND HAD NO WORK BEHIND IT.** That is the exact shape of the M5
slip — `editor-ui-ux` was claimed at Working in a table while the editor had no window — and
repairing that one cost the whole M5.5 insertion. A capability cannot reach Complete while the only
thing on the far end of its transport is a test fixture.

- [ ] 5b.1 **The engine's renderer publishes its rendered frames over the viewport transport.**
      M6 delivered the CONTROL half only: `src/servers/render/viewport_transport.{h,cpp}` carries
      frame identity, view state and cost, and its own header says there is no device and no
      swapchain here, and that the bytes "are the business of the module that owns a device". Nothing
      under `src/` or `samples/` hosts the pixel half, so the editor's viewport shows
      `cy-viewport-publisher` — a Vulkan fixture in the editor's Cargo workspace that clears an image
      to a colour and moves a white bar
- [ ] 5b.2 The editor opens **the engine's** world. It currently opens `.cyworld`, a THIRD authoring
      format that nothing in `src/` or `tools/` reads or writes, beside `cydoc` and `CookedCell`
- [ ] 5b.3 **Engine-side gizmo geometry**, finishing M6's task 2.7. The protocol carries a
      `GizmoLayout`, the editor asks for it and hit-tests it, and a runtime *double* answers over a
      real socket — but nothing in `src/servers/render/` generates one. `transform-gizmo.png` has
      been normative since M3 and is realised in no pixel
- [ ] 5b.4 **The gizmo drag lands on a handle rather than a coordinate.** M6's artefact drags from a
      hard-coded (47 %, 38 %) hoping a handle is there, reports `GAP`, and **returns 0** — so
      `smoke.editor_window` is green while the milestone's headline interaction is unproven. Aim from
      the published layout
- [ ] 5b.5 **An artefact that reports a GAP SHALL NOT exit zero.** The defect above is not the drag;
      it is that a narrowed artefact passed. Make this structural, in the artefact harness, so no
      later sample can do it again
- [ ] 5b.5b **An artefact SHALL headline a stable statistic, never an extreme.** The same harness
      rule, in the other direction. M6's open-world sample printed `worst tick 358 us` and that is
      what reached the screenshot, the report and the roadmap page — while four re-runs on a verified
      empty machine gave 365.9 to 392.2, every one of them worse. Its median over the same four runs
      moved 0.6 us. A single sample of an extreme-value statistic is a draw, not a measurement, and
      the counters beside it did not move at all. Print the max if a hitch has to show up somewhere,
      but the figure an artefact leads with must be one that reproduces
- [ ] 5b.6 `smoke.editor_window` derives the viewport socket from the build tree, and a Unix socket
      path is capped at 108 bytes — it breaks on a long build directory. Found by M6's release run
- [ ] 5b.7 **The exit criterion**: a screenshot in which the editor's viewport shows the engine's
      rendered world with a transform gizmo on a selected object, and a drag that moves it, undone
      exactly. Not a fixture, not an approximation

## 6. The material compiler → Working

- [ ] 6.1 Graph → IR → closures → program, per the spike
- [ ] 6.2 Optimisation passes, the GPU material table, classification and binning
- [ ] 6.3 Quality tiers, cost analysis, cooking — as a graph node
- [ ] 6.4 Node previews use the real compiler, proven by comparing preview and final output

## 7. Virtual geometry → Working

- [ ] 7.1 The asset, clusters, the crack-free hierarchy, geometric error
- [ ] 7.2 Geometry pages, the always-resident root, GPU streaming feedback
- [ ] 7.3 GPU traversal and cluster culling
- [ ] 7.4 The visibility buffer and material resolve
- [ ] 7.5 The cook is deterministic and cache-friendly at cluster granularity, which is what takes
      `asset-import-pipeline`'s "Virtual geometry cooking" requirement off the blocking list

## 8. Virtual shadows, temporal rendering, post-processing

- [ ] 8.1 Receiver-driven pages, clipmaps, the page cache, precise invalidation, update classes
- [ ] 8.2 The shadow budget, derived bias, the fallback chain
- [ ] 8.3 **One** temporal framework: jitter, derived motion vectors, history, invalidation,
      reprojection
- [ ] 8.4 The post chain in its defined order: AO, fog, exposure, DOF, bloom, tonemap, grading, AA,
      temporal upscaling

## 9. Global illumination, denoising, ray tracing

- [ ] 9.1 The GI scene, surface and radiance caches, distance fields
- [ ] 9.2 Screen, software and hardware tiers with sample confidence; reflections on the same
      infrastructure; probes and baking
- [ ] 9.3 One accumulation and edge-aware filter for every stochastic signal
- [ ] 9.4 Structure lifecycle, geometry adapters, ray queries, capability gating
- [ ] 9.5 Ray tracing disabled falls back to software tracing with no visual discontinuity beyond
      tolerance

## 10. `rendering-architecture` → Complete: the arbiter

- [ ] 10.1 The renderer budget arbiter, per the spike
- [ ] 10.2 Renderer profiles and pipeline configuration
- [ ] 10.3 Area lights, decals, light functions, channels, stochastic many-light
- [ ] 10.4 An analytic sky sufficient for GI's sky term
- [ ] 10.5 The Metal seed, which renders the M3 golden scene

## 11. The artefact — `samples/07-fidelity`

- [ ] 11.1 A film-detail interior and exterior with millions of source triangles
- [ ] 11.2 Dynamic lighting, indirect illumination and reflections
- [ ] 11.3 The frame budget held while the arbiter reallocates under a scripted load spike
- [ ] 11.4 Runs from a single recipe, and the run is the evidence

## 12. Records and gates

- [ ] 12.1 Write `tools/roadmap/milestones/m7.toml` — M7's own criteria only; the ledger is flat
- [ ] 12.2 Declare `milestone-m7` in `gates.toml` and raise `selftest.MINIMUM_CRITERIA`
- [ ] 12.3 Include an `m8-open` criterion using the double-star glob form
- [ ] 12.4 **Move `.github/workflows/ci.yml`'s `milestone` job from `m5b` to the newest closed
      milestone in the same commit that flips a milestone gate to green.** `just ci-check` fails
      otherwise, which is M6 task 10.9's forcing function working
- [ ] 12.5 Update `docs/roadmap/status.yaml`, `docs/roadmap/capability-matrix.md` and `docs/ROADMAP.md`
- [ ] 12.6 **The tier plan obeys the dependency rules, checked by a tool.** `delivery-roadmap` says
      each forbidden roadmap pattern SHALL be checkable and this one is not: M6's plan had two rows
      reaching Complete before a prerequisite reached Working and both were found by hand at its
      closing gate
- [ ] 12.7 **The four plan documents agree, checked by a tool** — the roadmap's work table, the
      matrix column, the matrix's load summary and the ledger's expected tiers. Three of the four
      disagreed about M5's scope and two disagreed about M6's
- [ ] 12.8 Open the M8 change

## 13. The gate

- [ ] 13.1 Full audit, per `delivery-roadmap`: the audit runs **in full through M8**
- [ ] 13.2 Clean build of every profile from empty; `test-all` in each
- [ ] 13.3 Every gate by hand; the M7 ledger run **once** — the ledger is flat
- [ ] 13.4 Adversarial pass on M7's own invariants: step-load the arbiter and look for oscillation;
      starve every paged system at once and confirm each degrades rather than disappears; compare a
      node preview against a final frame; run with ray tracing disabled
- [ ] 13.5 Records verified against what the code supports, not what the plan claimed
