# Tasks: split M8 into M8.a · Authorable and M8.b · Systems

**APPLY THIS AFTER M7's GATE FINISHES, NOT BEFORE.** The gate owns `docs/ROADMAP.md`,
`docs/roadmap/capability-matrix.md` and `docs/roadmap/status.yaml` while it runs, and it also opens
the M8 change as its task 12.8 — which this change supersedes. Editing those files underneath a
running gate is how two of this project's false failures happened.

The proposal and the `delivery-roadmap` delta are already written and committed. What remains is
applying the split to the plan documents and the ladder tooling.

## 1. The ladder tooling, before the documents

- [x] 1.1 Add `m8a` and `m8b` to `record.MILESTONES` **in position**, replacing `m8`. M6's gate made
      this an explicit ordered tuple precisely so an insertion could be ordered rather than appended
- [x] 1.2 A test asserting the rung order: M8.a inherits M7's criteria, M8.b inherits M8.a's, and
      neither inherits from above. **This is the `delivery-roadmap` requirement this change adds**,
      and it exists because M5.5's insertion sorted `m5b` to the end of the ladder — harmless only
      because nothing sat above it
- [x] 1.3 `just roadmap-test` and `just ci-check` green

## 2. Reconcile with whatever M7's gate opened

- [ ] 2.1 M7's task 12.8 says "open the M8 change". If the gate created `implement-m8-*`, fold its
      content into two changes and delete the single one — do not leave both
- [ ] 2.2 If the gate instead recorded that M8 is being split, nothing to reconcile

## 3. The plan documents — all four must agree

- [x] 3.1 `docs/ROADMAP.md`: M8 becomes two sections. **M8.a · Authorable** entry "M7 green", closing
      artefact *create a sphere, drop it on a box, press play, watch it fall, stop, and undo back to
      an empty world*. **M8.b · Systems** entry "M8.a green", closing artefact
      `samples/08-vertical-slice`, unchanged, and the shared graph IR spike stays with it
- [x] 3.2 `docs/roadmap/capability-matrix.md`: split the M8 column into M8.a and M8.b, and move each
      row's tier to whichever half actually advances it
- [x] 3.3 `docs/roadmap/status.yaml`: no tier changes — this is a plan change, not an implementation
      one. Only the `milestone` fields that name `M8` need to say which half
- [x] 3.4 `docs/roadmap/dependencies.md`: the M8 subgraph splits with it
- [x] 3.5 **M7's task 12.7 built a tool that checks the four plan documents agree.** Run it. This
      change is the first real exercise of it, and if it does not catch a mistake here it is not
      doing its job

## 4. The three pieces that had no owner, named in M8.a

- [ ] 4.1 **Primitive creation** — box, cylinder, sphere, plane, capsule, through the existing
      transaction path. Verified absent 2026-09-07: no `scene.create` in the registry, no shape
      generation anywhere in `src/` or `editor/`. Belongs to `editor-ui-ux` and `scene-graph-and-nodes`
- [ ] 4.2 **The physics ECS bridge** — `src/physics/` does not exist. `physics` requires "Physics
      SHALL be expressed as ECS components"; the module that registers them in a world, creates
      bodies and writes `cy::scene::LocalTransform` back was flagged missing by M4's gate and is
      still missing after M6
- [ ] 4.3 **Import from inside the editor** — `tools/import` handles `.fbx`, `.gltf`, `.glb` and
      `.tga`, but there is no `asset.import` command, so content is cooked outside the editor
- [ ] 4.4 **OBJ** — DECIDED: supported. The user confirmed on 2026-09-07 that OBJ must work
      alongside FBX, not instead of it, and the spec delta in this change adds it to
      `asset-import-pipeline`'s Model import requirement. There was never a trade to make: FBX
      landed at M6 and OBJ is additive — a text format with no rig, no animation and no scene graph,
      slotting into the importer interface that already has two implementations to copy
- [ ] 4.5 An OBJ carries no rig, so it reaches only steps 1–6 and 9. **The report names the steps it
      did not reach, and does not warn about them** — a format's absent capability is not a defect
      in the file, and the FBX importer already sets the precedent by naming what it skipped
- [ ] 4.6 It goes through the SAME derivation key as every other format. M7 unified those; a second
      key added for a third importer is exactly the correctness bug M7 spent its first section
      repairing

---

## What applying this split actually found

**Task 3.5 asked whether M7's plan-consistency tool would catch a mistake in this change. It caught
three, and none of them was visible in the documents.**

A milestone heading with an insertion suffix is read in *three* places, each with a regular
expression of its own, and all three admitted only `.5`:

| Reader | What it did with `M8.a` / `M8.b` |
|---|---|
| the matrix column header | dropped both columns — **every M8 cell vanished**, and nine capabilities' Complete column pointed at a milestone the matrix no longer contained |
| the ROADMAP section heading | read `## M8.b — Systems` as a continuation of M7, so **36 of M8.b's tier claims became M7's** |
| the load-table row label | found no row for either half |

The documents looked correct throughout. `check_documents_agree` reported 34, then 36, then 23, then
2 problems as each reader was fixed, and the count only reached zero when all three could see the
headings. Without it this change would have gone in with a matrix that silently lost a column.

`_check_every_reader_admits_an_insertion` in `tools/roadmap/selftest.py` now asserts all three, so
the next insertion fails loudly in one place instead of quietly in three.

**Also corrected while here:** four dependency-rule violations that were reported and then
disappeared. They were an artefact of the missing columns rather than real — the checker was reading
a matrix with a hole in it. Worth recording because a checker's findings are only as good as its
parse, and a confident wrong answer is the failure mode this whole apparatus exists to prevent.

## What is deliberately left unchecked

Tasks 2.1, 2.2 and section 4 belong to M8.a's own implementation rather than to this split:
primitive creation, the physics ECS bridge, import from inside the editor, and OBJ. They are
recorded in `docs/ROADMAP.md`'s M8.a section as that milestone's work, which is where an implementing
agent will look for them.
