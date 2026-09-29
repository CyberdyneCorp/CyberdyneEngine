# Tasks

## 1. Node-graph canvas

- [x] 1.1 Move the canvas, pin gestures, diagnostics strip and property controls from `material_graph.rs` to `panels/graph_canvas.rs`; port the material and VFX panels with their existing tests unchanged.
- [x] 1.2 Add `node_palette`, `catalogue_palette` and `palette_slot`; test that every openable graph domain hosts its `Domain::node_types` palette, and drive drags and pin connections through egui frames.

## 2. Specialised-editor scaffold

- [x] 2.1 Add `SpecialisedTool` and `specialised::show` (header with Undo/Redo, target, diagnostics area, domain session, body).
- [x] 2.2 Add `register_tool`/`register_specialised_tools`, refusing unregistered, agent-excluded and non-undoable panel commands; call it from `Application::new`.
- [x] 2.3 Port Terrain onto the scaffold; test the empty state, the header's Undo, the diagnostics area, and terrain authoring with undo/redo over MCP.

## 3. Timeline widget

- [x] 3.1 Add `move_key`, `remove_key`, `restore_key`, `duration` and `frame_rate` to `TimelineSurface`, with identity tests.
- [x] 3.2 Add `panels/timeline.rs`: ruler scrub, tracks, keys, clips, zoom, selection, Escape cancel, and invertible `TimelineEdit`s; drive each gesture through egui frames.

## 4. Records

- [x] 4.1 Document "Adding a specialised editor" in `editor/README.md`.
- [x] 4.2 Record the mutation proofs in `evidence/falsification.txt` (`evidence/mutate.py`).
- [x] 4.3 Record in `tools/roadmap/requirements-coverage.toml` which tests answer the two added requirements. Every existing editor requirement is already answered, and the new ones can be mapped only once this change is archived into `openspec/specs/`.
- [ ] 4.4 On archive, add those two `rust:` entries and rerun `just quality-requirements editor-architecture`.
