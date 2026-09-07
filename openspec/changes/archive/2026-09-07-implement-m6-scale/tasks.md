# Tasks: M6 — Scale

Ordered. The ladder fix first because every later ledger depends on it, then the spike, because the
derivation key model is the decision that is expensive to reverse.

## 0. The ladder, before anything writes a ledger

- [x] 0.1 Give `m5b` a real rung between M5 and M6 in `tools/roadmap/record.py` — `record.MILESTONES`
      does not contain it, so `criteria.rung("m5b")` answers `len(MILESTONES)` and M5.5's ledger
      sorts to the END of the ladder
- [x] 0.2 Regression test: M6's ledger inherits M5.5's criteria, and M5.5's does not inherit M6's.
      This is the property the flattened evaluator depends on and nothing currently asserts it
- [x] 0.3 Confirm `roadmap-status` and `roadmap-test` still pass, and that `record._entry` accepts
      `m5b` so `status.yaml` can record M5.5 at all

## 0b. The attribution gap M5.5's gate found, before more dependencies arrive

- [x] 0b.1 **386 third-party Rust crates are in the tree and no attribution gate can see them.**
      `deps/manifest.toml`, `deps/host-tools.toml` and `THIRD_PARTY.md` contain none of them, and
      `just maintenance-deps-check` compares `THIRD_PARTY.md` against the two C/C++ manifests only —
      so it passed. `thirdparty-dependencies` requires every dependency in one machine-readable
      manifest with a licence identifier and a justification
- [x] 0b.2 Make `maintenance-deps-check` read the editor's Cargo lockfile, so the gate that reported
      green is the gate that would have caught this
- [x] 0b.3 Record Rust 1.95.0 in `deps/host-tools.toml`: it is a hard MSRV and absent, which is the
      same omission that file's own docstring says "stayed undeclared through a whole milestone"
- [x] 0b.4 Do this BEFORE M6 adds ufbx and xatlas, so the new entries land under a working gate

## 1. Spike — the derivation key model, on one criterion

- [x] 1.1 Prototype outside the repository (`~/cyberdyne-spikes/m6-keys-spike/`), as M3's and M5.5's
- [x] 1.2 Settle what belongs in a key: tool versions, cook profile, options, input content hashes,
      and the transitive closure — and what deliberately does not
- [x] 1.3 Prove a one-asset change invalidates exactly its dependents, and no more
- [x] 1.4 Prove a cold build and a cache-warm build produce **byte-identical** artefacts
- [x] 1.5 Find any existing cook step whose output is not deterministic, before the graph is built on
      top of it
- [x] 1.6 Record the findings in `design.md` so the implementing agents do not re-derive them

## 2. Worlds load — `serialization-and-prefabs` → C, `core-assets-and-io` → C

- [ ] 2.1 Complete `serialization-and-prefabs`: every requirement mapped to a test, a gate or a
      recorded exemption
- [ ] 2.2 Complete `core-assets-and-io`, streaming under the residency policy
- [ ] 2.3 Complete `core-memory-and-containers`
- [x] 2.4 **A world loads with content**: `DocumentService::open` produces a schema declaring the
      engine's registered component types, so `TransformBinding::of_schema` finds a `Transform`
- [x] 2.5 Call `viewport.pump(&mut session, now)` once per frame in `ViewportLink::begin_frame` —
      M5.5's recorded one-line defect: the viewport model never learns a frame arrived, so `pick`
      answers `None` beside an overlay reading "announced 1016"
- [x] 2.6 Carry a pick request to the engine: `cy-editor-protocol` has no message and the SDK has no
      call, so engine-side picking is unreachable from the editor
- [ ] 2.7 Publish gizmo geometry from the engine, per `editor-viewport-and-gizmos`
- [ ] 2.8 **The artefact M5.5 could not author**: a person opens a world, selects an object, drags a
      gizmo, undoes, and saves — end to end, in the window

## 3. The world is larger than memory — `world-partition-and-streaming` → W

- [x] 3.1 Partitioning and stable cell identity
- [x] 3.2 Spatial binding, and **cells cooked in ECS-native form**
- [x] 3.3 Streaming sources, shapes and prediction
- [x] 3.4 Channels, priorities and deadlines
- [x] 3.5 Staged atomic activation
- [x] 3.6 Layers and HLOD
- [x] 3.7 The persistence overlay
- [x] 3.8 Continuous traversal holds the frame budget with no hitch above threshold, over a fixed route

## 4. Residency — `residency` → W

- [x] 4.1 Shared policy with separate storage: importance, priority, deadlines, budgets
- [x] 4.2 Pressure, eviction and churn control
- [x] 4.3 **Prove the separation**: a test holds bytes resident with simulation off

## 5. Textures page — `virtual-texturing` → W

- [x] 5.1 Virtual address spaces, page tables and tiles
- [x] 5.2 The physical cache and the resident mip tail
- [x] 5.3 GPU feedback and prefetch, and runtime producers
- [x] 5.4 Feedback never blocks a frame; the mip tail guarantees a frame is never missing

## 6. The save is the overlay — `save-and-persistence` → W

- [x] 6.1 Scopes and traits, persistent identity, dirty tracking
- [x] 6.2 The journal and atomic generations
- [x] 6.3 Migration
- [x] 6.4 Round-trip an **unloaded** region's state; generations survive `kill -9`

## 7. The build is a graph — `build-and-packaging` → W

- [x] 7.1 The derivation graph and derivation keys, per the spike
- [x] 7.2 Explicit inputs and immutable artefacts
- [x] 7.3 The derived data cache and the build service
- [x] 7.4 Precise invalidation
- [x] 7.5 Packages and chunk-level patching
- [x] 7.6 A patch applies atomically and rolls back cleanly when interrupted

## 8. Import and culling — `asset-import-pipeline` → C, `rendering-culling-and-lod` → W

- [x] 8.1 **ufbx, so FBX imports** — today `tools/import` handles glTF alone
- [x] 8.2 xatlas and mesh processing
- [x] 8.3 Cook profiles and packaging
- [ ] 8.4 Complete `rendering-geometry-and-resources`
- [ ] 8.5 GPU-driven culling, visibility ranges, HLOD, shadow caster culling

## 9. The artefact — `samples/06-open-world`

- [x] 9.1 A multi-kilometre world traversed continuously at speed: cells stream in and out
- [x] 9.2 Textures page under traversal
- [x] 9.3 Saved, quit, reloaded, resumes in the same state
- [x] 9.4 A content change is cooked, packaged and shipped as a patch
- [x] 9.5 Runs from a single recipe, and the run is the evidence

## 10. Records and gates

- [x] 10.1 Write `tools/roadmap/milestones/m6.toml` — M6's own criteria only, no `m5b-green`; the
      ledger has been flat since M5
- [x] 10.2 Declare `milestone-m6` in `gates.toml` and raise `selftest.MINIMUM_CRITERIA`
- [x] 10.3 Include an `m7-open` criterion using the double-star glob form
- [x] 10.4 **Add `milestone-m5b`'s CI jobs**, which M5.5 could not: an `editor-window` gate for a
      self-hosted runner with a display, and an `agent` gate beside it
- [x] 10.5 **Replace the three wrong reference images** and fold M5.5's verdicts into
      `docs/design/editor-visual-language.md`: they brand the publisher as the product, the console
      carries another engine's vocabulary, and the header says nothing about the runtime
- [x] 10.6 Update `docs/roadmap/status.yaml`, `docs/roadmap/capability-matrix.md` and `docs/ROADMAP.md`
- [x] 10.7 Fix `capability-matrix.md`'s stale note — it says `physics` scopes ragdolls to
      `animation-and-skinning`, "which is M6". It is M8
- [x] 10.8 Resolve the matrix's own rule violation: `rendering-geometry-and-resources` completes at M6
      while its Skinning requirement reads bone matrices from the GPU pose world, which is
      `animation-and-skinning` at M8. A capability may not reach Complete before its prerequisites
      reach Working — either scope the requirement or move the tier
- [x] 10.9 **Six milestone gates are green and no CI job runs any of them.** `check_workflows.py`
      skips any gate whose class is not `permanent`, and `ci.yml` names `roadmap-milestone` only in a
      comment. This is the unmet precondition `delivery-roadmap` sets for reducing the audit at M9,
      so it is worth closing here rather than discovering at M9
- [x] 10.10 Open the M7 change

## 11. The gate

- [x] 11.1 Full audit, per `delivery-roadmap`: the audit runs **in full through M8**
- [x] 11.2 Clean build of every profile from empty; `test-all` in each
- [x] 11.3 Every gate by hand; the M6 ledger run **once** — the ledger is flat
- [x] 11.4 Adversarial pass on M6's own invariants: collide two derivation keys; mutate an
      "immutable" artefact; interrupt a patch; `kill -9` mid-save; hold bytes resident with
      simulation off; make feedback block a frame
- [x] 11.5 Records verified against what the code supports, not what the plan claimed

---

## What this milestone did NOT close, and why

Seven tasks above are deliberately unchecked. Each was reported by the agent that owned it or by the
gate, with a reason, rather than being quietly ticked:

- **2.1 `serialization-and-prefabs` → Complete** — 23 requirements, no requirement-to-test mapping
  closed for the capability as a whole. Its *Apply and extract* requirement has no implementation
  anywhere and no exemption. Demoted to M8.
- **2.2 `core-assets-and-io` → Complete** — not independently doable. Two of ten requirements have no
  implementation, and the one this task names (Streaming, driven by a residency budget and renderer
  feedback) is exactly the one M6 was supposed to supply. `git diff --stat -- src/core/` is EMPTY for
  this milestone, and `asset_system.h` still reads "STREAMING IS M6. There is no residency budget".
  Demoted to M7.
- **2.3 `core-memory-and-containers` → Complete** — 16 requirements, unaudited, and nothing was added
  under `src/core/memory/`. Nothing built here needed a new container, so there was no honest way to
  close it as a side effect. Demoted to M7.
- **2.7 engine-side gizmo geometry** — half done. The protocol carries a `GizmoLayout`, the editor
  asks for it and hit-tests it, and a runtime *double* answers over a real socket. Nothing in
  `src/servers/render/` generates one. Moves to M7 with the viewport pixels, which is the same seam.
- **2.8 the window artefact's gizmo drag** — and this one is two failures, not one. The drag aims at a
  hard-coded (47 %, 38 %) hoping a handle is under it, and the artefact reports `GAP` and **returns
  0**, so `smoke.editor_window` is green while the milestone's headline interaction is unproven. That
  is M5.5's failure shape recurring: an artefact narrowing to what happens to work. Separately, XTEST
  does not deliver synthesised key events in this X session, reproduced with a 30-line python-xlib
  program unrelated to this repository — so it could not be driven to completion here either.
- **8.4 skinning reads the GPU pose world** — deliberately unmet and reported rather than faked.
  The pose world is `animation-and-skinning` at M8; inventing a second pose upload is precisely what
  that requirement exists to prevent. This is why `rendering-geometry-and-resources` was demoted
  rather than the requirement being scoped away.
- **8.5 the GPU culling compute pass** — the data model, the CPU reference, the HZB and the two-pass
  state are complete and tested; the shader and its render-graph pass live in `src/rendering/`, which
  no agent this milestone owned.

**M6 therefore completes nothing and advances six capabilities to Working.** Five rows the plan had
completing here were demoted, each naming an unmet requirement. That is the honest picture, and
`docs/roadmap/status.yaml` says it.
