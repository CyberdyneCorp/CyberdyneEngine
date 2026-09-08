# Tasks: M8.a — Authorable

Ordered. Section 1 first and alone: everything else is downstream of the editor's document and the
engine's scene being one world.

## 1. One world — the seam M7 left open

- [ ] 1.1 **The editor's document IS the engine's scene.** Opening a world produces the engine's own
      scene and an entity created in the editor appears in the rendered frame. Today the two are
      associated in first-seen order by a file whose own header calls itself a stand-in
- [ ] 1.2 `.cyworld` is read and written by the engine, or it stops existing. M7's gate: it is a
      third authoring format that nothing under `src/` or `tools/` reads, beside `cydoc` and
      `CookedCell`
- [ ] 1.3 A change made in the editor is visible in the viewport in the same frame the transaction
      commits — measured, not asserted
- [ ] 1.4 Engine-side picking answers a `PickRequest`: M7 left `Message::Pick` decoded and refused by
      name, because the runtime renders through M3's sample renderer and publishes no `GpuInstance`
      records

## 2. Primitives — `editor-ui-ux`

- [ ] 2.1 Box, sphere, cylinder, plane and capsule, generated beside the importers rather than in
      the editor, with editable parameters
- [ ] 2.2 One transaction per primitive; undo removes it and redo restores the same identity
- [ ] 2.3 **A generated primitive is an ordinary mesh instance.** No `PrimitiveNode`, no shape enum
      the rest of the editor branches on. Assert that selection, the inspector, the gizmo, saving,
      cooking and physics cannot tell it from an import
- [ ] 2.4 Generation goes through the single derivation key M7 unified, so a generated mesh is
      cacheable and deterministic like any other derived artefact

## 3. Import from inside the editor — `asset-import-pipeline` → W

- [ ] 3.1 An `asset.import` command in the registry, so import is an editor action and therefore an
      agent tool, not only a command line
- [ ] 3.2 **OBJ**, with its `.mtl`, per the requirement `split-m8-authorable-and-systems` added
- [ ] 3.3 An OBJ reaches only steps 1–6 and 9 of the import sequence and the report NAMES the steps
      it did not reach rather than warning about them
- [ ] 3.4 All three formats — glTF, FBX, OBJ — share one derivation key and one cache
- [ ] 3.5 An imported mesh lands in the open world as an entity, through a transaction

## 4. The physics ECS bridge — `physics` → W

- [ ] 4.1 `src/physics/` at layer 4: register the components `physics` specifies, create bodies from
      them, drive `PhysicsStepper` in the `Physics` stage, write `cy::scene::LocalTransform` back
- [ ] 4.2 Read `samples/04-character`'s host first — it is the specification of this module written
      out longhand in C++, and the behaviour is settled even though its home is not
- [ ] 4.3 A body added in the inspector is a transaction, and undo removes it
- [ ] 4.4 **Teardown under load, in a test.** M5.5's gate found Jolt destroying its job free list
      underneath a worker still releasing a job, one run in forty, as a fault with no physics call
      on the stack. Play mode creates and destroys worlds constantly

## 5. Play mode — `gameplay-framework` → S

- [ ] 5.1 Spawning, and enough of the session model that pressing play hosts the authored world
      rather than reporting `hosting: NoRuntime`
- [ ] 5.2 **Stop restores exactly**: no residue in the document, the scene or the persistence
      overlay. A play session that leaves any makes undo a lie
- [ ] 5.3 `serialization-and-prefabs` and `editor-documents-and-transactions` to Complete, which is
      what one world makes possible

## 6. The artefact — `samples/08a-authoring`

- [ ] 6.1 Create a sphere and a box in an empty world, from the editor
- [ ] 6.2 Place the sphere above the box with the gizmo
- [ ] 6.3 Add a rigid body to each, in the inspector
- [ ] 6.4 Press play; the sphere falls and lands on the box
- [ ] 6.5 Stop; the world is exactly as it was authored
- [ ] 6.6 Undo back to an empty world, exactly
- [ ] 6.7 Runs from a single recipe; a recorded gap exits non-zero; the headline figure reproduces
- [ ] 6.8 Commit a screenshot

## 7. Records and gates

- [ ] 7.1 Write `tools/roadmap/milestones/m8a.toml` — M8.a's own criteria only; the ledger is flat
- [ ] 7.2 Declare `milestone-m8a` in `gates.toml` and raise `selftest.MINIMUM_CRITERIA`
- [ ] 7.3 An `m8b-open` criterion using the double-star glob form
- [ ] 7.4 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them — they caught three parsing failures in the M8 split
- [ ] 7.5 Move `ci.yml`'s milestone job to `m8a` in the same commit that flips the gate green
- [ ] 7.6 Open the M8.b change

## 8. The gate

- [ ] 8.1 Full audit, per `delivery-roadmap`: the audit runs in full through M8
- [ ] 8.2 Clean build of every profile from empty; `test-all` in each
- [ ] 8.3 Every gate by hand; the M8.a ledger run once
- [ ] 8.4 **Every criterion actually executes something** — M6 shipped four that did not
- [ ] 8.5 Adversarial pass on M8.a's own invariants: create a primitive and prove nothing downstream
      knows it was generated; write around the transaction path; tear down a physics world under
      load; run a play session and diff the document byte for byte against its pre-play state; undo
      to empty and confirm no residue anywhere
- [ ] 8.6 Records verified against what the code supports, not what the plan claimed
