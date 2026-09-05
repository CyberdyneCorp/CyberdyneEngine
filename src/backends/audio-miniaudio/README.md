# `src/backends/audio-miniaudio/` — layer 3

The default `AudioBackend`, over **miniaudio**. Task 4.3.4.

**Governed by**: `audio` ("miniaudio as the default low-level backend", "Audio driver layer") and
`thirdparty-dependencies`.

## The only directory that may name an `ma_` symbol

`audio`: "No backend type SHALL appear in any engine or game-facing header outside its own backend
module", with the scenario "**WHEN** engine or game code is compiled **THEN** no backend library type
SHALL appear in any header outside `backends/audio/`."

`include/cy/backends/audio/miniaudio_backend.h` declares an opaque `MiniaudioState*` and includes
nothing from miniaudio; `src/miniaudio_backend.cpp` is the one translation unit in the engine that
includes `miniaudio.h`. That costs one indirection on a path entered once per callback rather than
once per sample, and it buys the property the requirement is after: replacing miniaudio is a change
to this directory rather than to every translation unit that ever included an audio header.

## What miniaudio is asked for, in full

Because `audio` requires the depended-upon surface to be "documented and limited to device I/O,
conversion, decoding, and streaming":

| used | for |
|---|---|
| `ma_context` | device enumeration |
| `ma_device` | the output stream and its realtime callback |
| `ma_format_f32` at the engine's rate | the format, rate and channel conversion the specification allows a backend to do |

**Not used, and named because the specification names them**: `ma_node_graph` (the engine has a bus
graph), `ma_engine` (the engine has an `AudioServer`), and `ma_spatializer` / `ma_sound`'s 3D model
(the engine owns the spatialisation policy). The check is one command:

```sh
grep -o 'ma_[a-z_]*' src/backends/audio-miniaudio/src/*.cpp | sort -u
```

## Two things worth knowing

**A device is identified by its reported name, not by `ma_device_id`.** That type is a union of
platform-specific handles — a GUID on Windows, a string on ALSA, an integer on CoreAudio — so it is
neither printable nor portable, and a settings file holding one would not survive a move between
machines. The name is what a settings menu shows anyway, and `select_output()` resolves it against
the backend's own copy of the last enumeration.

**`format()` reports what the device negotiated, not what was asked for.** A device running at
44.1 kHz says so, and the mixer resamples its clips against it. Reporting the request would make every
clip play at the wrong speed with nothing to say why.

## The suite is not device-gated

`tests/test_miniaudio.cpp` runs every case on every machine, and asserts **both** outcomes: a host
with an audio system opens a device, reports a self-consistent format, starts, stops and mixes; a
host without one fails `initialize()` with `Unavailable` — and nothing else — and shuts down cleanly.
A suite that skipped would be green on precisely the machines where this backend is least exercised.
