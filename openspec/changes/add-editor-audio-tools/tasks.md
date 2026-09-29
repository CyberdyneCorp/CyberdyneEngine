# Tasks

## 1. Engine

- [x] 1.1 `cy::editor::AudioAuthoring`: parse and validate `cymixer 1` and `cycue 1`, reconcile the bus graph by name, load tone and 48 kHz WAV clips, preview flat and spatial, and encode the state and the vocabulary.
- [x] 1.2 `audio.capabilities.get`, `audio.mixer.apply`, `audio.cue.preview`, `audio.preview.stop` and `audio.state.get` on the backend service (`MaterialService::set_audio`), refused by name without a server.
- [x] 1.3 Play: apply the project mixer, name every project cue for ABI 1.3, start autoplay sources at their transforms; pause and stop them; bind the adapter into the hosted Play host (#14's task 3.3b).
- [x] 1.4 The hosted runtime's `SceneAudio`: the miniaudio device under `CY_AUDIO`, or the null backend named on stderr; the listener follows the frame's camera.
- [x] 1.5 Source gizmos: speaker, solid `min_distance` ring and dashed `max_distance` ring, `projected_radius`, and click-to-select.
- [x] 1.6 `integration.editor_backend_audio`, `integration.editor_window_overlay` and `unit.editor_window_runtime` cases, and the committed wire fixtures.

## 2. Editor

- [x] 2.1 `cy_editor_services::audio`: the mixer, cue, preview payload, state and vocabulary models, validated as the engine validates them.
- [x] 2.2 Undoable commands with MCP parity: `audio.mixer.create`, `audio.bus.{add,remove,volume,flag,route,send}`, `audio.bus.effect.{add,set,remove}`, `audio.cue.save`, `audio.source.{create,range}`; and the reads `audio.mixer.read`, `audio.mixer.apply`, `audio.cue.{read,preview}`, `audio.preview.stop`, `audio.refresh`, `audio.status`, `audio.source.preview`.
- [x] 2.3 `AudioRequests`: one request in flight and a bounded queue, vocabulary on demand, 4 Hz state polling while the panel is drawn, and the project's mixer sent again on reconnect; undo sends a restored mixer.
- [x] 2.4 `Domain::AudioBusesAndMixing` opens only on the engine's vocabulary.
- [x] 2.5 The Audio Mixer panel on the scaffold: the bus table with engine meters, sends, effects, cues with preview, and the selected source's range and preview.

## 3. Records

- [x] 3.1 Rust cases in services, interface, shell and MCP; panel snapshots `docs/design/images/editor-audio-{mixer,source-range}.png`.
- [x] 3.2 `samples/05b-editor-window/audio_window.py` against the real runtime; captures `docs/design/images/editor-audio-source-{viewport,window}.png`.
- [x] 3.3 Mutation proofs in `evidence/falsification.txt` (`evidence/mutate.py`), and for every MCP
  mixer and source tool's edit reaching the saved mixer or world (r14 to r22) in
  `evidence/falsification-r14_route_tool_is_a_no_op.txt`.
- [x] 3.4 `tools/roadmap/requirements-coverage.toml`: two `audio` requirements mapped; the four added requirements recorded for mapping on archive.
- [x] 3.5 `editor/README.md`, `src/editor_backend/README.md`, `samples/05b-editor-window/README.md`.
- [ ] 3.6 On archive, add the four `test:`/`rust:` entries recorded in the coverage file.
