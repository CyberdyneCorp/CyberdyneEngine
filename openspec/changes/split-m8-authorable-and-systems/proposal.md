# Split M8 into M8.a · Authorable and M8.b · Systems

## Why

**M8 has become the milestone everything falls into.** As planned it advanced ten capabilities. M6's
gate then demoted five more into it — `asset-import-pipeline`, `serialization-and-prefabs` and
`rendering-geometry-and-resources` among them — and three further pieces have no home and belong
there: primitive creation, the physics ECS bridge, and import from inside the editor. That is
twenty-two capability advances in one milestone, against M9's ten and M10's three. Only M11, which is
explicitly "everything remaining", is larger.

**Its named risk spike is also the hardest thing on the ladder.** M8 carries the shared graph
infrastructure across *seven* consumers — abilities, AI, animation, VFX, sequences, visual scripting
and camera rigs all lowering through one IR. The roadmap already says why that is dangerous:
discovering the IR cannot express a consumer's semantics is cheap with one consumer and expensive
with six others built on it.

Putting those two facts together: **the milestone that would let someone build a scene and press play
is currently sitting behind the riskiest spike in the project.** A user asking "when can I add a box,
give it physics, place it and play the scene?" is told M8, and M8 cannot start until an IR that seven
subsystems agree on has been designed.

**These are two different milestones wearing one number.** One is content authoring: create
something, import something, give it a material, give it a body, press play. It is small, it is
demonstrable to a person in one screenshot, and nothing in it is architecturally risky. The other is
the shared graph and everything that lowers through it: animation, sequences, AI, abilities, visual
scripting, VFX. It is the most coupled work remaining and it deserves its own spike, its own gate and
its own failure budget.

**The precedent is M5.5, and the lesson is the same one.** M5 claimed `editor-ui-ux` at Working while
closing on a scripted artefact with no window; the repair was to insert a milestone rather than
pretend the row was right. Here the defect is not a false claim but a false *unit*: two milestones
recorded as one, where the cheap and visible half is blocked by the expensive and risky half.

## What Changes

- **M8.a · Authorable** — *a scene a person builds by hand, and a game they can press play on.*
  Primitives, import from inside the editor, materials assigned, a body on an object, spawning, and
  play mode against a real runtime. Closing artefact: **create a sphere, drop it on a box, press
  play, watch it fall, stop, and undo back to an empty world** — one run that exercises primitive
  creation, the gizmo, the physics ECS bridge, play mode and the transaction system together.
- **M8.b · Systems** — *everything that lowers through one graph.* The shared IR spike first, then
  visual scripting, animation and skinning, AI, abilities and effects, sequences and cinematics, VFX,
  camera rigs, navigation, the interface, 2D and audio. Closing artefact: `samples/08-vertical-slice`,
  unchanged — a playable game with characters that animate and think.
- **Three pieces that today have no owner are named in M8.a**, each verified absent rather than
  assumed: primitive creation (no `scene.create` in the registry, no shape generation anywhere in
  `src/` or `editor/`), the physics ECS bridge (`src/physics/` does not exist; M4's gate flagged it
  and it is still absent after M6), and import from inside the editor (`tools/import` handles
  `.fbx`, `.gltf`, `.glb` and `.tga`, but there is no `asset.import` command).
- **The insertion is recorded rather than renumbered**, as M5.5's was, so every existing reference to
  M9 through M11 stays valid.

## Capabilities

### Modified Capabilities

- `asset-import-pipeline` — **OBJ is supported**, decided rather than left as an omission. FBX
  landed at M6 via ufbx and glTF has been the primary format since M5; OBJ was in no specification
  at all, which is how it came to be missing rather than declined. It is additive and not a
  substitute for either: a text format with no rig, no animation and no scene graph, exercising only
  steps 1–6 and 9 of the import sequence. The requirement also gains the rule that a step a format
  cannot express is *reported* rather than *warned about* — an absent capability is not a defect in
  the file.
- `delivery-roadmap` — the ladder gains one entry. M8 becomes M8.a and M8.b, and the rule that
  produced this split is written down: a milestone whose closing artefact cannot be reached without
  its own risk spike succeeding is two milestones.

## Impact

- **Roadmap**: M9 through M11 keep their numbers and their content.
- **The ledger**: `record.MILESTONES` gains two entries in order. M6's gate already made that an
  explicit ordered tuple after M5.5's insertion sorted `m5b` to the end of the ladder and would have
  broken M6's inheritance — the same mistake is available here and this change must not repeat it.
- **Risk**: the split does not reduce the shared-IR risk, it isolates it. M8.b still carries the
  hardest spike on the ladder; what changes is that a person can build and play a scene without
  waiting for it.
