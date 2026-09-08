# Tasks: M7 — Fidelity

Ordered. The two spikes first, because both settle decisions that every later section builds on.
Sections 2 to 5 are the debts M6 recorded, placed before the rendering work rather than after it —
each is an edit today and a migration once this milestone's cooks and systems depend on it.

## 0. The spikes

- [x] 0.1 Spike the **material IR and closure lowering** outside the repository, as M3's, M5.5's and
      M6's were. The criterion is M7's own: the IR round-trips, and a graph and a hand-written
      material produce identical programs
- [x] 0.2 Spike the **budget arbiter's control loop**. The criterion is convergence without
      oscillation under a step load, over the lever shape `residency` already declares
- [x] 0.3 Record both findings in `design.md` so the implementing agents do not re-derive them

## 1. M6's debts, before anything new depends on them

- [x] 1.1 **One derivation key and one derived-data cache.** `cy::import::import_derivation_key`
      contributes no compiler, no flags and no library versions and it is the function `cy_import_cli`
      uses; `tools/build/`'s refuses a key without a toolchain fingerprint. `asset-import-pipeline`
      requires one cache covering all derived data
- [x] 1.2 Fold `cy::shader::CacheKeyInputs` into the same key, per M6 `design.md` §1.8: the merged key
      is the union of what each got right
- [ ] 1.3 **One persistent identity and one overlay.** `cy::world::PersistenceOverlay` and
      `cy::save::Overlay` are two structures for one requirement; `src/save/README.md` carries the
      resolution
- [ ] 1.4 **`cy_cook` and `cy_import_cli` become nodes in the build graph**, with the two-run
      determinism gate `just content-validate` already provides
- [x] 1.5 The eight clang diagnostics M6's spike recorded — `-Wunused-private-field` in
      `src/core/assets/include/cy/core/assets/watch.h` and seven `-Wdouble-promotion` in
      `tools/import/src/mesh.cpp` — so the tree builds under both compilers and "which compiler
      produced this artefact" can have two answers

## 2. `core-assets-and-io` → Complete

- [x] 2.1 Partial residency for textures (mips), meshes (LODs) and audio, driven by the `residency`
      budget and by renderer feedback — the requirement M6 planned to complete and did not touch
- [x] 2.2 A not-yet-resident level falls back to the highest resident level, and never blocks a frame

## 3. `core-memory-and-containers` → Complete

- [x] 3.1 Attribution by type, by thread, by world cell and by asset — the four axes of the five that
      `cy/core/memory/diagnostics.h` has no field for. The world cell exists from M6

## 4. `virtual-texturing` → Complete, and `residency` → Complete

- [x] 4.1 A feedback buffer written by a shader and resolved without a per-pixel stream reaching the
      CPU
- [x] 4.2 A page table sampled by a shader, and the mip tail's guarantee held on the device
- [ ] 4.3 More than one subsystem registered against the residency policy in a shipped path, so the
      arbitration the capability exists for is exercised by a frame rather than by a test

## 5. `rendering-culling-and-lod` → Complete

- [x] 5.1 The compute dispatch, checked against `cpu_reference_cull` by comparing buffers
- [x] 5.2 The renderer consumes it: `cy::servers-render-culling` is linked by something other than its
      own tests

## 5b. `editor-viewport-and-gizmos` → Complete: the engine's own image, and a drag that lands

**THIS TIER IS NAMED IN THE PROPOSAL AND HAD NO WORK BEHIND IT.** That is the exact shape of the M5
slip — `editor-ui-ux` was claimed at Working in a table while the editor had no window — and
repairing that one cost the whole M5.5 insertion. A capability cannot reach Complete while the only
thing on the far end of its transport is a test fixture.

- [x] 5b.1 **The engine's renderer publishes its rendered frames over the viewport transport.**
      M6 delivered the CONTROL half only: `src/servers/render/viewport_transport.{h,cpp}` carries
      frame identity, view state and cost, and its own header says there is no device and no
      swapchain here, and that the bytes "are the business of the module that owns a device". Nothing
      under `src/` or `samples/` hosts the pixel half, so the editor's viewport shows
      `cy-viewport-publisher` — a Vulkan fixture in the editor's Cargo workspace that clears an image
      to a colour and moves a white bar
- [ ] 5b.2 The editor opens **the engine's** world. It currently opens `.cyworld`, a THIRD authoring
      format that nothing in `src/` or `tools/` reads or writes, beside `cydoc` and `CookedCell`
- [x] 5b.3 **Engine-side gizmo geometry**, finishing M6's task 2.7. The protocol carries a
      `GizmoLayout`, the editor asks for it and hit-tests it, and a runtime *double* answers over a
      real socket — but nothing in `src/servers/render/` generates one. `transform-gizmo.png` has
      been normative since M3 and is realised in no pixel
- [x] 5b.4 **The gizmo drag lands on a handle rather than a coordinate.** M6's artefact drags from a
      hard-coded (47 %, 38 %) hoping a handle is there, reports `GAP`, and **returns 0** — so
      `smoke.editor_window` is green while the milestone's headline interaction is unproven. Aim from
      the published layout
- [x] 5b.5 **An artefact that reports a GAP SHALL NOT exit zero.** The defect above is not the drag;
      it is that a narrowed artefact passed. Make this structural, in the artefact harness, so no
      later sample can do it again
- [ ] 5b.5b **An artefact SHALL headline a stable statistic, never an extreme.** The same harness
      rule, in the other direction. M6's open-world sample printed `worst tick 358 us` and that is
      what reached the screenshot, the report and the roadmap page — while four re-runs on a verified
      empty machine gave 365.9 to 392.2, every one of them worse. Its median over the same four runs
      moved 0.6 us. A single sample of an extreme-value statistic is a draw, not a measurement, and
      the counters beside it did not move at all. Print the max if a hitch has to show up somewhere,
      but the figure an artefact leads with must be one that reproduces
- [x] 5b.6 `smoke.editor_window` derives the viewport socket from the build tree, and a Unix socket
      path is capped at 108 bytes — it breaks on a long build directory. Found by M6's release run
- [x] 5b.7 **The exit criterion**: a screenshot in which the editor's viewport shows the engine's
      rendered world with a transform gizmo on a selected object, and a drag that moves it, undone
      exactly. Not a fixture, not an approximation

## 6. The material compiler → Working

- [x] 6.1 Graph → IR → closures → program, per the spike
- [ ] 6.2 Optimisation passes, the GPU material table, classification and binning
- [ ] 6.3 Quality tiers, cost analysis, cooking — as a graph node
- [x] 6.4 Node previews use the real compiler, proven by comparing preview and final output

## 7. Virtual geometry → Working

- [x] 7.1 The asset, clusters, the crack-free hierarchy, geometric error
- [x] 7.2 Geometry pages, the always-resident root, GPU streaming feedback
- [ ] 7.3 GPU traversal and cluster culling
- [ ] 7.4 The visibility buffer and material resolve
- [x] 7.5 The cook is deterministic and cache-friendly at cluster granularity, which is what takes
      `asset-import-pipeline`'s "Virtual geometry cooking" requirement off the blocking list

## 8. Virtual shadows, temporal rendering, post-processing

- [ ] 8.1 Receiver-driven pages, clipmaps, the page cache, precise invalidation, update classes
- [x] 8.2 The shadow budget, derived bias, the fallback chain
- [x] 8.3 **One** temporal framework: jitter, derived motion vectors, history, invalidation,
      reprojection
- [ ] 8.4 The post chain in its defined order: AO, fog, exposure, DOF, bloom, tonemap, grading, AA,
      temporal upscaling

## 9. Global illumination, denoising, ray tracing

- [x] 9.1 The GI scene, surface and radiance caches, distance fields
- [ ] 9.2 Screen, software and hardware tiers with sample confidence; reflections on the same
      infrastructure; probes and baking
- [x] 9.3 One accumulation and edge-aware filter for every stochastic signal
- [ ] 9.4 Structure lifecycle, geometry adapters, ray queries, capability gating
- [x] 9.5 Ray tracing disabled falls back to software tracing with no visual discontinuity beyond
      tolerance

## 10. `rendering-architecture` → Complete: the arbiter

- [ ] 10.1 The renderer budget arbiter, per the spike
- [x] 10.2 Renderer profiles and pipeline configuration
- [ ] 10.3 Area lights, decals, light functions, channels, stochastic many-light
- [ ] 10.4 An analytic sky sufficient for GI's sky term
- [ ] 10.5 The Metal seed, which renders the M3 golden scene

## 11. The artefact — `samples/07-fidelity`

- [x] 11.1 A film-detail interior and exterior with millions of source triangles
- [x] 11.2 Dynamic lighting, indirect illumination and reflections
- [x] 11.3 The frame budget held while the arbiter reallocates under a scripted load spike
- [x] 11.4 Runs from a single recipe, and the run is the evidence

## 12. Records and gates

- [x] 12.1 Write `tools/roadmap/milestones/m7.toml` — M7's own criteria only; the ledger is flat
- [x] 12.2 Declare `milestone-m7` in `gates.toml` and raise `selftest.MINIMUM_CRITERIA`
- [x] 12.3 Include an `m8-open` criterion using the double-star glob form
- [x] 12.4 **Move `.github/workflows/ci.yml`'s `milestone` job from `m5b` to the newest closed
      milestone in the same commit that flips a milestone gate to green.** `just ci-check` fails
      otherwise, which is M6 task 10.9's forcing function working
- [ ] 12.5 Update `docs/roadmap/status.yaml`, `docs/roadmap/capability-matrix.md` and `docs/ROADMAP.md`
- [x] 12.6 **The tier plan obeys the dependency rules, checked by a tool.** `delivery-roadmap` says
      each forbidden roadmap pattern SHALL be checkable and this one is not: M6's plan had two rows
      reaching Complete before a prerequisite reached Working and both were found by hand at its
      closing gate
- [x] 12.7 **The four plan documents agree, checked by a tool** — the roadmap's work table, the
      matrix column, the matrix's load summary and the ledger's expected tiers. Three of the four
      disagreed about M5's scope and two disagreed about M6's
- [x] 12.8 Open the M8 change

## 13. The gate

- [x] 13.1 Full audit, per `delivery-roadmap`: the audit runs **in full through M8**
- [ ] 13.2 Clean build of every profile from empty; `test-all` in each
- [x] 13.3 Every gate by hand; the M7 ledger run **once** — the ledger is flat
- [x] 13.4 Adversarial pass on M7's own invariants: step-load the arbiter and look for oscillation;
      starve every paged system at once and confirm each degrades rather than disappears; compare a
      node preview against a final frame; run with ray tracing disabled
- [x] 13.5 Records verified against what the code supports, not what the plan claimed

---

## What this milestone did NOT close

Nineteen tasks above are unchecked. Each was reported by the agent that owned it or by the closing
gate, with a reason, and the ledger's own notes carry the same list so a reader who never opens this
file still finds them.

**The convergence claim was narrowed rather than met, and that was a decision.** The arbiter settles
without oscillation given at least four deadbands of nominal headroom; below that it still
reallocates, still respects every reserved minimum and still restores authored quality, but it may
keep moving levers. `rendering-architecture` now says so, `integration.render_arbiter_sweep` sweeps
the operating point and asserts it, and `samples/07-fidelity` reports the headroom that put a run
inside the envelope instead of failing there. The alternative — fixing the law to hold budget at one
deadband — is real work and is not done. What made the narrowing honest rather than a dodge is that
the criterion now tests the axis it never tested: nothing swept the nominal cost before, so the
guarantee had only ever been checked where it was comfortable.

**Rendering, where the module exists and the frame does not:**

- **7.3 occlusion culling** — the two-pass hierarchical-depth scheme has no HZB, so
  `nodes_pruned_by_occlusion` exists and is always zero.
- **9.4 the hardware ray-tracing tier never reaches a device**, and the reason is the engine rather
  than the machine: this RTX 5060 has `VK_KHR_ray_query` and the Vulkan backend never asks for it.
  `cy::rhi::Capability::RayTracing` is an enumerator nothing sets. So every GI and reflection figure
  in the artefact is the software tier against a CPU reference.
- **6.2 / 6.3** — the material compiler emits Slang that nothing compiles, and issues no GPU
  classification dispatch. The generated source imports a shader standard library that does not
  exist yet.
- **10.5 the Metal seed has never been compiled**, here or anywhere: there is no Apple toolchain on
  this machine. `metal_seed_status().renders` states what it can and cannot draw rather than leaving
  a reader to infer it from what happens to build.
- **7.4 / 8.1 / 8.4 / 9.2 / 10.1 / 10.3 / 10.4** — each is a part delivered against a reference
  rather than assembled into a frame. The gate's summary of the shape: *thirteen modules and no
  renderer.*

**The editor, and the seam that is still open:**

- **5b.2 the editor opens `.cyworld`**, a third authoring format that nothing under `src/` or
  `tools/` reads or writes. The viewport shows the engine's world and a gizmo drag moves the
  engine's object — but the scene is the runtime's, not the editor's document, so an entity created
  in the editor does not appear in it. *What you see is what ships* is not yet true.
- **4.3** — virtual texturing's residency registration is in shipped engine code and is exercised by
  a real GPU frame; what is not shown is more than one subsystem arbitrating against the policy in a
  shipped path.

**Debts that moved rather than closed:**

- **1.3 one persistent identity and one overlay** — `cy::world::PersistentId` is a `u64` and
  `cy::save::PersistentId` is 128-bit. Merging them crosses three modules at once and no agent owned
  all three. A save format has now shipped, so this is a migration from here.
- **1.4** — `content-validate` does not yet cook through the graph as well as through the CLI.
- **5b.5b** — `samples/06-open-world` still leads with `worst tick`, the exact figure the rule was
  written about. The harness enforces it, `window.py` adopts it, and this one artefact predates both.
- **12.5 / 13.2** — records and the clean four-profile build are the closing change's, not the
  implementing agents'.

**M7 therefore advances the renderer to Working across nine capabilities and completes
`rendering-architecture` at Working rather than Complete.** The tier the plan wanted is not the tier
the code supports, and the record says the second one.
