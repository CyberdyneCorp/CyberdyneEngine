# Proposal: Audio tools in the editor

## Why

Issue #29 lists the audio tools as High for the M12 RTS: a bus and mixer editor
(`Domain::AudioBusesAndMixing`), cue assets with preview, audio during editor Play, and spatial
audio preview in the viewport. The engine side is built: `cy::audio::AudioServer` has the bus graph,
cycle rejection, sends, a four-kind effect chain, voices and spatialisation (`audio`), and ABI 1.3
gives Swift `Audio.cue`, `Audio.bus` and `Audio.play`. The editor has no panel for any of it, and
#14 left audio during Play open: the hosted runtime answers every Play with "audio unavailable in
this host".

## What Changes

- **A mixer asset and its editor.** `audio/mixer.cymixer` (`cymixer 1`) holds the bus graph: Master
  first, each bus's output, gain, mute, solo, bypass, up to four sends and up to eight effects.
  The Audio Mixer panel is a `SpecialisedTool` on the scaffold from #59 and shows the graph as a
  table, with each bus's level as the engine measured it. Every edit (`audio.bus.add`, `.remove`,
  `.volume`, `.flag`, `.route`, `.send`, `.effect.add`, `.effect.set`, `.effect.remove`,
  `audio.mixer.create`) is one undoable project transaction and an MCP tool of the same name. A
  route or send that would close a cycle is refused before it is saved, and the engine refuses it
  again.
- **The engine applies it.** `cy::editor::AudioAuthoring` (`src/editor_backend/`) reconciles an
  `AudioServer`'s bus graph with a mixer: buses that keep their name keep their handle, so a gain
  change does not interrupt a voice, and a removed bus stops what played on it. The backend service
  gains `audio.capabilities.get`, `audio.mixer.apply`, `audio.cue.preview`, `audio.preview.stop`
  and `audio.state.get`. Each answers with the server's state after the request: every bus's gain,
  routing, effect chain and last-block peak and RMS, the voices, and what the last preview started.
- **Cues with preview.** A `.cycue` (`cycue 1`) names a clip (`tone:<hertz>:<seconds>`, or a
  project-relative 16-bit PCM or 32-bit float 48 kHz `.wav`), its bus, gain, pitch, variation and
  looping. `audio.cue.save` saves one undoably; `audio.cue.preview` plays it through the engine's
  mixer; the panel lists the project's cues with Preview and Stop.
- **Audio during editor Play (#14's open task).** The hosted runtime owns one `AudioAuthoring`,
  opened on the miniaudio device under `CY_AUDIO` and on the null backend otherwise, which it
  names. On Play it applies the project's mixer, starts every enabled, autoplaying
  `cy::audio::AudioSource` at its transform, names every project cue for Swift, and binds the
  server's `AudioAdapter` as ABI 1.3's audio backend. Pause holds those voices and Stop ends them.
  The listener follows the camera the frame was rendered from.
- **Spatial audio preview.** `audio.source.create` places an entity with a
  `cy::audio::AudioSource` (cue, `min_distance`, `max_distance`, attenuation model, autoplay).
  The engine draws each source in the Editor view as a speaker, a solid ring at `min_distance` and
  a dashed ring at `max_distance`, projected with the frame's own view, and a click on it selects
  it. `audio.source.range` edits the radii undoably, and `audio.source.preview` plays the source's
  cue at its position, heard from the viewport camera. The engine reports the gain and pan its own
  curves gave.

## Capabilities

### Modified Capabilities

- `editor-architecture`: the audio buses and mixing editor, cue preview, and audio during Play.
- `editor-viewport-and-gizmos`: the spatial audio preview.

## Impact

- Engine: `src/editor_backend/` (new `audio_authoring`, `audio_service`; `MaterialService` gains
  `set_audio`), `samples/05b-editor-window/runtime/` (`scene_audio`, Play, the emitter overlay and
  picking, the Swift audio binding). `cy::editor-backend` now links `cy::servers-audio`,
  `cy::game-backend` and `cy::scene-serialization`; the runtime links
  `cy::backends-audio-miniaudio` under `CY_AUDIO`. No ABI or bridge message change: the new
  operations use the existing service request and event messages.
- Editor: `cy-editor-services` (`audio`, `audio_commands`, `audio_requests`, `audio_status`;
  `ProjectHost` gains four defaulted audio methods), `cy-editor-interface` (the audio domain opens
  on the engine's vocabulary), `cy-editor-shell` (`panels/audio_mixer.rs`).
- No existing behaviour changes except the Play detail, which now names the audio sources and the
  backend instead of "audio unavailable in this host".
