# M8.a — Authorable: a scene a person builds by hand

## Why

**Eight milestones in, nothing can be put into a world without cooking an asset outside the editor.**
The whole scene command surface is `scene.translate`, `scene.rotate`, `scene.scale`, `scene.rename`,
plus select, undo, redo and save. There is no `scene.create`. There is no shape generation anywhere
in `src/` or `editor/`. There is no `asset.import` command, so `tools/import` is reachable only from
a command line. And `src/physics/` — the module that would register physics components in a world,
create bodies from them and write `cy::scene::LocalTransform` back — **does not exist**, which M4's
gate flagged and which is still true after M7.

Each of those was verified absent rather than assumed, and each fell between capabilities the same
way: every milestone assumed content arrives from an importer somebody else wrote.

M7 gave the editor's viewport the engine's own rendered world and a gizmo that moves the engine's
object. What it did not give it is anything of the user's own to move. **The scene in the viewport is
the runtime's, not the editor's document** — entities created in the editor do not appear in it, so
*what you see is what ships* is not yet true. That is this milestone's first job and its hardest one.

**This milestone exists because M8 was split.** M8 had grown to twenty-two capability advances, and
its named risk spike — one graph IR that seven consumers must agree on — was blocking the half that
is architecturally settled. Nothing here is unsettled. It was blocked only by sharing a number with
work that is.

## What Changes

- **Primitives.** Box, cylinder, sphere, plane and capsule, created through the existing transaction
  path, so a created primitive is undoable like any other edit.
- **The editor's document is the engine's scene.** Opening a world produces the engine's own scene,
  and an entity created in the editor appears in the rendered frame. Today they are associated in
  first-seen order by a file whose own header calls itself a stand-in.
- **Import from inside the editor**, and **OBJ** alongside glTF and FBX — decided in
  `split-m8-authorable-and-systems` and specified there. One derivation key for all three.
- **The physics ECS bridge.** `src/physics/` registers the components `physics` already specifies,
  creates bodies from them, drives the stepper in the `Physics` stage, and writes transforms back.
- **Spawning and play mode against a real runtime**, enough of `gameplay-framework` that pressing
  play simulates the world the editor authored rather than reporting `hosting: NoRuntime`.

## Capabilities

### Advanced Capabilities

`physics` and `asset-import-pipeline` to **Working**; `gameplay-framework` to **Seed**;
`serialization-and-prefabs` and `editor-documents-and-transactions` to **Complete**.

### Modified Capabilities

- `editor-ui-ux` — **primitive creation** is a requirement rather than an omission. An editor that
  cannot make a box cannot be used to try anything, and the absence was never a decision.

## Impact

- **New code**: `src/physics/` (the ECS bridge), primitive mesh generation, an OBJ importer, an
  `asset.import` command, and the association between the editor's document and the engine's scene.
- **Closing artefact**: `samples/08a-authoring` — create a sphere, drop it on a box, press play,
  watch it fall, stop, and undo back to an empty world. One run exercising primitive creation, the
  gizmo, the physics bridge, play mode and the transaction system together.
- **Risk**: none architectural. The one thing worth measuring first is where the editor's document
  and the engine's scene meet, because getting that seam wrong is a migration rather than an edit.
