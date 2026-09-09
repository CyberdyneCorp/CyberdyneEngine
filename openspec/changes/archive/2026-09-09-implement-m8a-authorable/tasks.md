# Tasks: M8.a — Authorable

Ordered. Section 1 first and alone: everything else is downstream of the editor's document and the
engine's scene being one world.

## 1. One world — the seam M7 left open

- [x] 1.1 **The editor's document IS the engine's scene.** Opening a world produces the engine's own
      scene and an entity created in the editor appears in the rendered frame. Today the two are
      associated in first-seen order by a file whose own header calls itself a stand-in
- [x] 1.2 `.cyworld` is read and written by the engine, or it stops existing. M7's gate: it is a
      third authoring format that nothing under `src/` or `tools/` reads, beside `cydoc` and
      `CookedCell`
- [x] 1.3 A change made in the editor is visible in the viewport in the same frame the transaction
      commits — measured, not asserted
- [ ] 1.4 Engine-side picking answers a `PickRequest`: M7 left `Message::Pick` decoded and refused by
      name, because the runtime renders through M3's sample renderer and publishes no `GpuInstance`
      records

## 2. Primitives — `editor-ui-ux`

- [x] 2.1 Box, sphere, cylinder, plane and capsule, generated beside the importers rather than in
      the editor, with editable parameters
- [x] 2.2 One transaction per primitive; undo removes it and redo restores the same identity
- [ ] 2.3 **A generated primitive is an ordinary mesh instance.** No `PrimitiveNode`, no shape enum
      the rest of the editor branches on. Assert that selection, the inspector, the gizmo, saving,
      cooking and physics cannot tell it from an import
- [x] 2.4 Generation goes through the single derivation key M7 unified, so a generated mesh is
      cacheable and deterministic like any other derived artefact

## 3. Import from inside the editor — `asset-import-pipeline` → W

- [x] 3.1 An `asset.import` command in the registry, so import is an editor action and therefore an
      agent tool, not only a command line
- [x] 3.2 **OBJ**, with its `.mtl`, per the requirement `split-m8-authorable-and-systems` added
- [x] 3.3 An OBJ reaches only steps 1–6 and 9 of the import sequence and the report NAMES the steps
      it did not reach rather than warning about them
- [x] 3.4 All three formats — glTF, FBX, OBJ — share one derivation key and one cache
- [ ] 3.5 An imported mesh lands in the open world as an entity, through a transaction

## 4. The physics ECS bridge — `physics` → W

- [x] 4.1 `src/physics/` at layer 4: register the components `physics` specifies, create bodies from
      them, drive `PhysicsStepper` in the `Physics` stage, write `cy::scene::LocalTransform` back
- [x] 4.2 Read `samples/04-character`'s host first — it is the specification of this module written
      out longhand in C++, and the behaviour is settled even though its home is not
- [x] 4.3 A body added in the inspector is a transaction, and undo removes it
- [x] 4.4 **Teardown under load, in a test.** M5.5's gate found Jolt destroying its job free list
      underneath a worker still releasing a job, one run in forty, as a fault with no physics call
      on the stack. Play mode creates and destroys worlds constantly

## 5. Play mode — `gameplay-framework` → S

- [x] 5.1 Spawning, and enough of the session model that pressing play hosts the authored world
      rather than reporting `hosting: NoRuntime`
- [x] 5.2 **Stop restores exactly**: no residue in the document, the scene or the persistence
      overlay. A play session that leaves any makes undo a lie
- [ ] 5.3 `serialization-and-prefabs` and `editor-documents-and-transactions` to Complete, which is
      what one world makes possible

## 6. The artefact — `samples/08a-authoring`

- [x] 6.1 Create a sphere and a box in an empty world, from the editor
- [x] 6.2 Place the sphere above the box with the gizmo
- [x] 6.3 Add a rigid body to each, in the inspector
- [x] 6.4 Press play; the sphere falls and lands on the box
- [x] 6.5 Stop; the world is exactly as it was authored
- [x] 6.6 Undo back to an empty world, exactly
- [x] 6.7 Runs from a single recipe; a recorded gap exits non-zero; the headline figure reproduces
- [x] 6.8 Commit a screenshot

## 7. Records and gates

- [x] 7.1 Write `tools/roadmap/milestones/m8a.toml` — M8.a's own criteria only; the ledger is flat
- [x] 7.2 Declare `milestone-m8a` in `gates.toml` and raise `selftest.MINIMUM_CRITERIA`
- [x] 7.3 An `m8b-open` criterion using the double-star glob form
- [x] 7.4 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them — they caught three parsing failures in the M8 split
- [ ] 7.5 Move `ci.yml`'s milestone job to `m8a` in the same commit that flips the gate green
- [x] 7.6 Open the M8.b change

## 8. The gate

- [x] 8.1 Full audit, per `delivery-roadmap`: the audit runs in full through M8
- [x] 8.2 Clean build of every profile from empty; `test-all` in each
- [x] 8.3 Every gate by hand; the M8.a ledger run once
- [x] 8.4 **Every criterion actually executes something** — M6 shipped four that did not
- [x] 8.5 Adversarial pass on M8.a's own invariants: create a primitive and prove nothing downstream
      knows it was generated; write around the transaction path; tear down a physics world under
      load; run a play session and diff the document byte for byte against its pre-play state; undo
      to empty and confirm no residue anywhere
- [x] 8.6 Records verified against what the code supports, not what the plan claimed

---

## What this milestone did NOT close

Five tasks are unchecked. The milestone's own nineteen criteria are green and its artefact runs end
to end; these are the edges the gate would not let pass as done.

**2.3 — a mesh instance has no component to hold its mesh, and this is the one that shows.**
`cy::render::MeshRenderer` is a declared name in `src/scene/src/node_template.cpp` with **no reflected
type behind it**, so the mesh a primitive references reaches no renderer and the runtime still draws
every node through M3's fixed slots. The artefact's picture is a box above a box. Placement,
hierarchy, transport and simulation are real; the shape is not, and the artefact says so on every run
rather than letting the screenshot imply otherwise. This is the largest single thing M8.b inherits.

**1.4 — engine-side picking is answered and nothing calls it.** The engine resolves a `PickRequest`
over the wire; no production caller exists in `cy-editor-app` or `cy-editor-services`. Selection in
the artefact is by outliner row.

**3.5 — `.cyworld` and `cydoc` are still two grammars.** `cydoc` carries prefab instances with
overrides, variants, motion classification and flatten policy; `.cyworld` carries none of it. One
world was achieved at the level of *state*, not yet at the level of *format*.

**5.3 — two tiers declined rather than claimed.** `serialization-and-prefabs` stays short of Complete
because *Apply and extract* has no implementation anywhere — its README has said so since M2, and
M8.a taught the engine to *decode* `InstantiatePrefab` and `SetPrefabOverride` and count them as
IGNORED. `editor-documents-and-transactions` stays short because *Source control integration* is
unstarted: no provider trait, no Git, no Perforce, not even a null provider — seven grep hits, all
comments.

**7.5 — the gate flip is deliberately unfinished, and the reason is a finding.** `milestone-m7` was
recorded `state = "green"` while `just roadmap-milestone m7` could not pass, because its ledger
declares the `m1:four-profiles` that was failing. A permanent merge gate was green in the record and
red when run, and nothing detected it: the only job that runs a ledger runs the newest one nightly,
and the workflow check asks whether a job *exists*, not whether it passes. The gate declined to add a
second instance of M2's and M4's finding. **The budget calibration in `4845e82` is what makes both
records true again**, and the flip belongs to the closing change rather than to the audit that found
the problem.

## One correction to the record

`physics`'s `milestone` field moves M4 → M8.a. M4 recorded it at Working with `src/physics/` absent
from the tree, and it stayed absent for four milestones. The tier was right; the milestone that
earned it was not.

**M8.a advances no capability tier**, and `m8a.toml` says so in as many words: its `roadmap-tiers`
criterion passes over an unchanged tier column because all six floors were already met before the
milestone began. It is a floor, not evidence of an advance.
