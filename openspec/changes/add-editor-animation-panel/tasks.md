# Tasks

## 1. Engine

- [x] 1.1 `pose.state`'s `state` output; the locomotion builder wires transitions from it; an unnamed clip clock is the node's own. Regression cases in `integration.graph_compiler`.
- [x] 1.2 `animation.catalogue.get`, `animation.compile` (with the authoring checks) and `animation.preview.{set,get,stop}` on `MaterialService` through `AnimationPreviewRuntime`.
- [x] 1.3 `AnimationPreview`: the mannequin, its clips and mesh; clip and state machine previews; authored events; play.
- [x] 1.4 `AuthoredFrame::set_skinned_preview`; the hosted runtime ticks and draws the preview.
- [x] 1.5 `integration.editor_backend_animation` (previews against direct evaluation, an edited transition, events, the committed wire) and `smoke.editor_authored_frame_vulkan` (the preview drawn and scrubbed).
- [x] 1.6 The hosted runtime queues a service request the service is too busy for instead of dropping it (regression case in `unit.editor_window_runtime`).

## 2. Editor

- [x] 2.1 `cy_editor_services::animation_graph`: references, events, the wire.
- [x] 2.2 `AnimationRequests`: the catalogue on demand, compiles per graph, the newest preview, the playing preview followed.
- [x] 2.3 Commands with MCP parity: `animation.graph.create`, `animation.node.*`, `animation.event.*` (undoable); `animation.graph.{read,compile}`, `animation.preview.*`, `animation.status` (reads). The preview follows an edit, an undo and a redo.
- [x] 2.4 The Animation panel on the scaffold, the canvas and the timeline; the default workspace's Animation tab opens it.

## 3. Records

- [x] 3.1 Rust cases in services, interface, shell and MCP; panel snapshots and viewport photographs in `docs/design/images/editor-animation-*.png`; `samples/05b-editor-window/animation_window.py` against the real runtime.
- [x] 3.2 Mutation proofs in `evidence/falsification.txt`.
- [x] 3.3 `tools/roadmap/requirements-coverage.toml`: the added requirements and their tests recorded for mapping on archive.
- [x] 3.4 `docs/guides/animation.md`, `editor/README.md`, `src/editor_backend/README.md`, `src/graph/README.md`, `samples/05b-editor-window/README.md`.
- [ ] 3.5 On archive, add the `test:`/`rust:` entries recorded in the coverage file.

## 4. Later slices (not in this change)

- [ ] 4.1 Preview a project's own cooked character once the editor imports skeletons and skins.
- [ ] 4.2 Cook a graph's authored events into its clips.
- [ ] 4.3 Curve editing on the animation timeline; blend spaces once #92 adds them.
- [ ] 4.4 The skinned preview on Metal and D3D12 once their skinned pipelines have run on a device.
