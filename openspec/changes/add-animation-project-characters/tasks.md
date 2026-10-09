# Tasks

## 1. Engine

- [x] 1.1 `Clip::clear_events`, with a case in `unit.animation`.
- [x] 1.2 `AnimationCharacter`: the mannequin, and a project character from cooked records through
  `AnimationAssetSource`. A foreign clip is refused by name; a mesh-less model is drawn as one box per
  bone.
- [x] 1.3 `AnimationPreview` on a character; `animation.character.set`; the catalogue and the compile
  against the character's clips (`AnimationClipCatalogue`).
- [x] 1.4 `AnimationRigBaker`, `cyrig 1` and `animation.bake`; `MaterialService::set_animation_baker`.
- [x] 1.5 The hosted runtime:
  - `ProjectAnimationAssets` for the preview and the bake;
  - `PlayAnimation` at Play;
  - `ScriptRuntime::bind_animation` and `frame`;
  - the mesh re-uploaded when the character changes.
- [x] 1.6 Tests:
  - `integration.editor_backend_animation`: an FBX-imported character previews bit for bit against
    its clip sampled directly and its program evaluated directly; a foreign clip is refused; the bake
    cooks the events in.
  - `integration.editor_window_play_animation`: a baked rig plays in Play, and its events arrive on
    their ticks.
  - `smoke.editor_animation_events`: the same in Swift's `onUpdate`.
  - `smoke.editor_authored_frame_vulkan`: the imported character is drawn.

## 2. Editor

- [x] 2.1 The character and bake wire, `project_characters` and `project_clips`, and the character
  file.
- [x] 2.2 `AnimationRequests`: the character sent once and before its preview; the vocabulary asked
  for again; bakes handed over once.
- [x] 2.3 `animation.character.list`, `animation.character.set` (undoable) and `animation.bake`, with
  MCP parity. The bake's files are written into the rig's directory. Undo and redo of a choice show
  the preview on the restored character.
- [x] 2.4 The panel's Character row, the engine's account of the character, and Bake.

## 3. Records

- [x] 3.1 The Rust cases in services and MCP. The panel snapshot `editor-animation-character.png` and
  the viewport photographs `editor-animation-viewport-imported-*.png`.
- [x] 3.2 Mutation proofs in `evidence/falsification.txt`.
- [x] 3.3 `tools/roadmap/requirements-coverage.toml`: the added requirements recorded for mapping on
  archive.
- [x] 3.4 `docs/guides/animation.md`, `editor/README.md`, `src/editor_backend/README.md` and
  `samples/05b-editor-window/README.md`.
- [ ] 3.5 On archive, add the coverage file's recorded entries.

## 4. Later slices (not in this change)

- [ ] 4.1 Curve editing on the timeline, once something a game runs reads a clip's curve tracks.
- [ ] 4.2 Retarget a project clip cooked for another skeleton instead of refusing it.
- [ ] 4.3 Cook and package baked rigs with the content pipeline.
- [ ] 4.4 Draw Play's animated characters in its viewport.
