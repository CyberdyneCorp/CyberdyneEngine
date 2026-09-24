# Tasks

## 1. Metal frame visibility

- [x] 1.1 Align the frame's MSL bindings with Metal descriptor sets and verify a captured frame has shaded pixels.
- [x] 1.2 Restore Metal pixel assertions, add a regression for identical-image comparison, and verify the Metal suite is registered in mixed backend builds.
- [x] 1.3 Commit a Metal frame capture beside the Vulkan reference and update shader generation notes; verify the image shows the lit geometry.

## 2. Authored mesh frame

- [x] 2.1 Resolve `.cyprim` and imported cooked mesh references into cached mesh data, and verify a world with each kind resolves to actual geometry.
- [x] 2.2 Pack cached mesh streams and material slots for `FrameRecorder`, and verify a non-box mesh is captured on Metal.
- [x] 2.3 Convert visible world nodes to spatial entries and full instance transforms, including parent transforms, and verify nonuniform scale and rotation in the capture.
- [x] 2.4 Replace the host's first-light frame with `FrameAssembly` and `FrameRecorder` output while retaining viewport publication; verify the editor opens an empty world and displays an imported mesh.
- [x] 2.5 Use transformed mesh bounds for picking and framing, remove the fixed node cap, and verify gizmo and pick identities on a non-box mesh.
- [x] 2.6 Cook embedded and external FBX base-colour images as texture sub-assets, link their stable identities from cooked materials, and cover both sources with import regressions.
- [x] 2.7 Upload referenced cooked textures in the authored frame, bind material texture slots, and verify texture detail on imported geometry.
- [x] 2.8 Enable bounded material texture sampling on Metal, regenerate shader artefacts, and verify a textured frame differs from its constant-colour control.
- [x] 2.9 Synchronize newly declared component fields to the live runtime without saving, and cover the empty-world snapshot and bridge wire with regressions.

## 3. Workflow and documentation

- [x] 3.1 Extend the editor smoke flow to import an external FBX into an empty scene, save it, and perform Move, Rotate, and Scale; verify the saved world and viewport captures.
- [x] 3.2 Document the import and placement workflow and validate this OpenSpec change.
