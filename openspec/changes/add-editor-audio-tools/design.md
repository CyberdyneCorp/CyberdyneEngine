# Design: the editor's audio tools (#29)

## Context

The editor is a client. The engine's `cy::audio::AudioServer` owns the bus graph, the voices and
the mix; ABI 1.3's `audio_*` entries reach it through `cy::game_backend::AudioAdapter`. The
hosted runtime (`samples/05b-editor-window/runtime`) answers the editor's backend-service requests
through `cy::editor::MaterialService`, and until now it held no audio server at all. Wave 0 (#59)
gave specialised tools one scaffold, with undo in the header and a parity check that refuses a
panel command an agent cannot reach or undo cannot reverse.

## Decisions

### The mixer and cues are project assets; a source is a scene component

A bus graph belongs to the project, not to one world, so it is a file:
`game/audio/mixer.cymixer`. It lives under `game/` because the `author` agent scope may write only
there, and a mixer an agent could not edit would break MCP parity for real agents, not only in
tests. A cue is a file for the same reason. A source is placed in a world, so it is a component on
an entity, `cy::audio::AudioSource`, read by name the way `cy::vfx::Effect` is.

Asset saves follow #17's VFX pattern. The editor writes the file and records one `Domain`
operation (`audio_asset:<path>`, before and after text) in the active world's history, so
`edit.undo` puts the file back. The undo path also sends a restored mixer to the engine.

### Line-oriented text, validated identically on both sides

`cymixer 1` is one line per bus, then per send, then per effect. `cycue 1` is one key and value
per line. The Rust model (`cy_editor_services::audio::Mixer`) validates exactly what
`cy::editor::parse_mixer` validates: Master first with no output or sends, unique names, known
routes and kinds, gain 0 to 4, send level 0 to 1, at most four sends and eight effects, at most 32
buses, and no cycle through outputs and sends together. An edit is therefore refused before it is
saved, and the engine refuses it again if something else wrote the file.

### Reconcile by name, never rebuild

`AudioAuthoring::reconcile` first routes every existing bus straight to Master with no sends, which
is acyclic. It then destroys the buses the new mixer no longer names, creates the new ones, and
sets every route, send and effect. An intermediate cycle is impossible: any cycle among a subset of
the final edges would be a cycle in the final graph, which the parser already refused. A bus that
keeps its name keeps its handle, so a gain edit does not interrupt a voice. A voice on a removed bus
is stopped rather than rerouted, because which bus it belongs on is the author's decision.

### Every reply is the server's state after the request

Every `audio.*` operation except the vocabulary answers with the same encoding: each bus as
`BusGraph` holds it, with its last-block peak and RMS from `AudioServer::bus_levels`; the voices;
Play; the loaded cues; and the last preview's distance, gain and pan. The preview's gain and pan
are computed with the server's own `attenuation_gain` and `pan_stereo`. The panel and
`audio.status` show only decoded replies, so a refused edit cannot look applied.

`audio.state.get` may advance the mix first, and only on the null backend, where nothing else
pulls samples. That is how the suites measure a level without a device. With a device, the device
thread mixes, and an advance is refused rather than double-pumping the game-thread half.

### One queue, polled only when someone is looking

`AudioRequests` keeps one request in flight and queues the rest (up to 32; a queued state read is
dropped before an edit is). Nothing audio is sent until a panel, a command or an agent asks. The
vocabulary is requested first. While the mixer panel is drawn, the state is read at 4 Hz. On
reconnect the project's mixer is sent again.

### The engine owns Play's audio

At Play, `AudioAuthoring::start_play` applies the project's mixer from disk, whether or not an
editor sent it, so a runtime started with a project plays through its buses. It then names every
project cue for `audio_find_cue` (by file stem, the same walk the panel lists, skipping hidden
directories and `build/`) and starts each enabled, autoplaying source, spatialised at its
transform. The runtime binds the same server's adapter into the Play host, so Swift reaches the
same cues and buses. Pause pauses those voices and Stop stops them. Each frame the listener follows
the camera the frame rendered from, and the mix advances by the time that frame covered.

### Gizmos are the engine's

`editor-viewport-and-gizmos` gives handle geometry to the engine, as light and camera markers
already are. The runtime reads the sources from the world it holds and draws each one in the Editor
view. `projected_radius` measures each radius along the camera's right through `project_to_pixel`,
the same projection the frame used. The rings are walked along their perimeter rather than filled
over their bounding box, so a large silence radius costs its circumference. They are solid for
`min_distance` and dashed for `max_distance`, so line style carries the distinction as well as
colour. A click within 13 pixels of a speaker selects its entity, through the same path that
selects lights.

### A cross-language contract with one artefact per direction

The fixtures live in `src/editor_backend/tests/data/audio_*`:

- The mixer, the cue and the preview request are compared byte for byte with the Rust encoders.
  The C++ suite submits those same bytes and reads the result out of `BusGraph` and `AudioServer`.
- The C++ suite writes the state, Play and vocabulary replies (`CY_UPDATE_AUDIO_WIRE=1`) and
  compares later runs against them within a tolerance, since mixed levels are floating point.
- The Rust suites decode those replies, and the MCP tests replay them as the runtime's answers.

`samples/05b-editor-window/audio_window.py` removes both stand-ins: the real runtime and the real
editor over MCP.

## Found, not fixed here

- The status glyph problem from Wave 0 applies here too, so the send list says "Send to" rather
  than drawing an arrow.
- `SceneAudio` re-reads the world's sources each frame for the gizmos and for picking. That is a
  walk over the nodes, which is cheap at editor scale, but it belongs on the world's change
  notification once the runtime has one.

## Not in this change

Decoding formats other than 48 kHz WAV (the asset system's job, per `src/servers/audio/README.md`),
effect volumes and reverb zones, Doppler preview, the sequencer's audio tracks, and Steam Audio.
The mixer has no automation or snapshots.
