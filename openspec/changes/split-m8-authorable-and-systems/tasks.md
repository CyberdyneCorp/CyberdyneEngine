# Tasks: split M8 into M8.a · Authorable and M8.b · Systems

**APPLY THIS AFTER M7's GATE FINISHES, NOT BEFORE.** The gate owns `docs/ROADMAP.md`,
`docs/roadmap/capability-matrix.md` and `docs/roadmap/status.yaml` while it runs, and it also opens
the M8 change as its task 12.8 — which this change supersedes. Editing those files underneath a
running gate is how two of this project's false failures happened.

The proposal and the `delivery-roadmap` delta are already written and committed. What remains is
applying the split to the plan documents and the ladder tooling.

## 1. The ladder tooling, before the documents

- [ ] 1.1 Add `m8a` and `m8b` to `record.MILESTONES` **in position**, replacing `m8`. M6's gate made
      this an explicit ordered tuple precisely so an insertion could be ordered rather than appended
- [ ] 1.2 A test asserting the rung order: M8.a inherits M7's criteria, M8.b inherits M8.a's, and
      neither inherits from above. **This is the `delivery-roadmap` requirement this change adds**,
      and it exists because M5.5's insertion sorted `m5b` to the end of the ladder — harmless only
      because nothing sat above it
- [ ] 1.3 `just roadmap-test` and `just ci-check` green

## 2. Reconcile with whatever M7's gate opened

- [ ] 2.1 M7's task 12.8 says "open the M8 change". If the gate created `implement-m8-*`, fold its
      content into two changes and delete the single one — do not leave both
- [ ] 2.2 If the gate instead recorded that M8 is being split, nothing to reconcile

## 3. The plan documents — all four must agree

- [ ] 3.1 `docs/ROADMAP.md`: M8 becomes two sections. **M8.a · Authorable** entry "M7 green", closing
      artefact *create a sphere, drop it on a box, press play, watch it fall, stop, and undo back to
      an empty world*. **M8.b · Systems** entry "M8.a green", closing artefact
      `samples/08-vertical-slice`, unchanged, and the shared graph IR spike stays with it
- [ ] 3.2 `docs/roadmap/capability-matrix.md`: split the M8 column into M8.a and M8.b, and move each
      row's tier to whichever half actually advances it
- [ ] 3.3 `docs/roadmap/status.yaml`: no tier changes — this is a plan change, not an implementation
      one. Only the `milestone` fields that name `M8` need to say which half
- [ ] 3.4 `docs/roadmap/dependencies.md`: the M8 subgraph splits with it
- [ ] 3.5 **M7's task 12.7 built a tool that checks the four plan documents agree.** Run it. This
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
- [ ] 4.4 **OBJ** — decide it explicitly: add it to `asset-import-pipeline` or record why not. It is
      not in any specification today. Trivial to implement, but a spec change rather than a bug fix,
      so it needs the user's decision
