# M6 — Scale: a world larger than memory, and a build that is a graph

## Why

M5.5 gave the editor a window, a cross-process viewport and an agent interface. It also produced the
clearest statement of what the engine is still missing, and its own gate wrote it down rather than
softening it: **`DocumentService::open` calls `Document::new` — a name and an empty schema — because
there is no world loader.** Nothing is selectable, no `Transform` binds, the inspector correctly
reports that nothing is described, and a gizmo drag commits nothing. The editor is real and it has
nothing to edit.

That is this milestone's first job, and it is not editor work. A world that loads is
`serialization-and-prefabs` at Complete and `world-partition-and-streaming` at Working — cells with
stable identity, cooked in ECS-native form, activated atomically. The editor becomes useful as a
side effect of the engine being able to name, cook and stream content.

The second job is the one the roadmap has always placed here: **the build becomes a graph.** Five
milestones have been built by a script that is correct because it is re-run from scratch. That does
not survive a multi-kilometre world, a cook profile per platform, or a patch. `build-and-packaging`
brings derivation keys, explicit inputs, immutable artefacts and precise invalidation — and the
derivation key model is the named risk spike, because keys that are not precise make the cache either
wrong or useless, and every later milestone builds on top of it.

The third job is the one a person can see. M6 completes `asset-import-pipeline` — **ufbx, so FBX
imports**, xatlas, mesh processing, cook profiles and packaging. Combined with a world that loads,
this is the first milestone at which someone can bring a model into the engine, place it, and look
at it through the editor's own viewport.

## What Changes

- **Worlds load.** `serialization-and-prefabs` and `core-assets-and-io` reach Complete;
  `world-partition-and-streaming` reaches Working. Opening a world in the editor produces a schema
  and nodes, which is what makes M5.5's gizmos, inspector and picking reachable outside a test.
- **The world is larger than memory.** Partitioning, stable cell identity, spatial binding, streaming
  sources and prediction, channels, priorities and deadlines, staged atomic activation, layers, HLOD,
  and the persistence overlay.
- **Residency is a shared policy with separate storage.** Importance, priority, deadlines, budgets,
  pressure, eviction and churn control — proven separate by a test that holds bytes resident with
  simulation off.
- **Textures page.** `virtual-texturing` to Working: virtual address spaces, page tables, tiles, the
  physical cache, the resident mip tail, GPU feedback, prefetch and runtime producers.
- **The save is the overlay.** `save-and-persistence` to Working: scopes and traits, persistent
  identity, dirty tracking, the journal, atomic generations under `kill -9`, and migration.
- **The build is a derivation graph.** Derivation keys, explicit inputs, immutable artefacts, the
  derived data cache, the build service, precise invalidation, packages and chunk-level patching.
- **FBX imports.** `asset-import-pipeline` to Complete via ufbx, with xatlas, mesh processing and
  cook profiles. Today the importer handles glTF alone.
- **Culling moves to the GPU.** `rendering-culling-and-lod` to Working, with visibility ranges, HLOD
  and shadow caster culling.

## What M5.5 handed forward, and this change owns

M5.5's ledger recorded five items it could not close in a crate it owned. They are listed here so
they are carried deliberately rather than rediscovered:

- **`Viewport::pump` is never called.** `cy-editor-shell::viewport_link` imports the texture and
  calls `set_view_state`, and nothing calls `pump`, so the viewport model never learns a frame
  arrived: `pick` answers `None` and a click reports "No frame has arrived yet" beside an overlay
  reading "announced 1016". `ViewportSession` already implements `Transport`; the fix is one call per
  frame in `ViewportLink::begin_frame`.
- **Picking has no wire.** `cy-editor-protocol` carries no pick message and the SDK has no call, so
  engine-side picking cannot be reached from the editor.
- **Gizmo geometry is the engine's and nothing publishes it**, per `editor-viewport-and-gizmos`.
- **The reference images are wrong in three places** — they brand the publisher as the product, the
  console carries another engine's vocabulary, and the header says nothing about the runtime. M5.5
  argued each difference and this change replaces the files and folds the verdicts into
  `docs/design/editor-visual-language.md`.
- **`milestone-m5b` needs its CI jobs**: an `editor-window` gate on a self-hosted runner with a
  display, and an `agent` gate beside it. A gate and its job land in the same change or neither does.

## The ladder defect this change must fix first

`record.MILESTONES` is `m0`…`m11` and contains no `m5b`, so `criteria.rung("m5b")` answers
`len(MILESTONES)` and M5.5's ledger sorts to the **end** of the ladder. That was harmless while
nothing sat above it. **It stops being harmless the day M6 exists**: M6 would sort below M5.5, so
M6's ledger would not inherit M5.5's criteria and M5.5 would try to inherit M6's. The flattened
evaluator depends on the ordering being true. This is the first task, before any M6 criterion is
written.

## Capabilities

### Advanced Capabilities

`build-and-packaging`, `world-partition-and-streaming`, `residency`, `virtual-texturing`,
`save-and-persistence` and `rendering-culling-and-lod` to **Working**; `asset-import-pipeline`,
`core-assets-and-io`, `core-memory-and-containers`, `serialization-and-prefabs` and
`rendering-geometry-and-resources` to **Complete**.

### Modified Capabilities

- `delivery-roadmap` — record that an inserted milestone takes a rung between its neighbours rather
  than at the end of the ladder, and that the flattened evaluator's inheritance is defined by that
  rung. The insertion of M5.5 exposed the assumption; M6 is the milestone at which it becomes wrong.

## Impact

- **New code**: the derivation graph and build service, the partition and streaming subsystem, the
  residency policy, the virtual texturing pipeline, the persistence overlay, and ufbx behind the
  importer's existing interface.
- **New dependencies**: ufbx and xatlas, both named by `asset-import-pipeline` and both behind
  engine-owned interfaces per `thirdparty-dependencies`.
- **Closing artefact**: `samples/06-open-world` — a multi-kilometre world traversed continuously at
  speed: cells stream in and out, textures page, the game is saved, quit, reloaded, and resumes in
  the same state. Then a content change is cooked, packaged and shipped as a patch.
- **Risk**: the derivation key model. If keys are not precise the cache is either wrong or useless,
  and it is the foundation every later milestone's build sits on. It gets the spike.
