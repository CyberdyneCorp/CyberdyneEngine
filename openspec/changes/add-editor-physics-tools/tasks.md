# Tasks

## 1. Engine

- [x] 1.1 Write entity references in `.cyworld` as node positions and resolve them to identities on read, in `worldfile.cpp` and `worldfile.rs`; regression test for the unreadable identity word; tests for renumbering and a deleted target.
- [x] 1.2 Move constraint drawing out of the Jolt backend into `cy::physics::debug_draw_constraint`; unit tests for anchors, limits and a free hinge.
- [x] 1.3 Append `physics_overlays` to `GizmoIntent`, optional on decode; test both an older and a newer message.
- [x] 1.4 Read authored joints (`cy/gameplay/play/joints.h`) and hand them to the bridge at play, rejoining after a body is rebuilt; `integration.gameplay_joints`, including a Jolt pendulum against a no-joint control.
- [x] 1.5 Draw the requested physics layers and the selected authored joint in `cy_editor_window_runtime`; `unit.editor_window_physics_overlay`.

## 2. Editor

- [x] 2.1 `cy_editor_viewport::physics_view` mirroring `DebugDrawFlags`; remove physics colliders from `PLANNED_VIEWS`.
- [x] 2.2 `viewport.physics.<layer>` and `viewport.physics.hide-all` read commands; the layers in the gizmo request.
- [x] 2.3 `cy_editor_services::joints`: `physics.joint.add`, `physics.joint.set`, `physics.joint.remove`, each one transaction; `tests/a_joint_is_a_transaction.rs` reads the engine's names from `joints.h`.
- [x] 2.4 The `physics` panel on the scaffold's header, diagnostics area and parity check; frames in `new_panels_are_accessible.rs`.
- [x] 2.5 MCP parity: `physics_authoring_is_an_undoable_mcp_peer_of_the_physics_panel`.
- [ ] 2.6 Ragdoll profile setup: blocked on skeleton import (model import step 7). Stated in the panel and exempted in the coverage map.

## 3. Records

- [x] 3.1 Mutation proofs in `evidence/falsification.txt` (`evidence/mutate.py`).
- [x] 3.2 Panel snapshot under `docs/design/images/editor-physics-*.png`.
- [x] 3.3 `editor/README.md`, `docs/guides/physics.md`, `src/physics/README.md` and `src/gameplay/play/README.md` updated.
- [x] 3.4 Coverage: the view-mode exemption note updated; the requirements this change adds are listed for mapping on archive.
- [ ] 3.5 On archive, map the added requirements and rerun `just quality-requirements`.
