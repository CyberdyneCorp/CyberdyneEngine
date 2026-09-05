# `src/servers/audio/` — layer 2

The engine-owned `AudioServer`: the driver interface, the bus graph, voice management, playback and
spatialisation policy.

**Governed by**: `audio`, which reaches **Seed** at M4 — tasks 4.3.4 and 4.3.5.

## The layering, which is the whole architecture

`audio` puts the policy in the engine and the library behind an interface:

> `AudioServer`, the bus graph, voice management, streaming policy, spatialisation policy, and the
> importance system SHALL be engine code. […] **WHEN** voice limits, tier budgets, streaming
> residency, or job scheduling are decided **THEN** they SHALL be enforced by engine code, not
> delegated to a backend's own policy.

That is why this module mixes rather than asking miniaudio to. miniaudio has a node graph, a
high-level engine API and its own 3D spatialisation, and the specification says in as many words that
the engine "SHALL NOT use" any of the three. What it is asked for is four things — device I/O,
conversion, decoding, streaming — and keeping the surface that small is what makes the backend
genuinely replaceable rather than nominally so.

**No miniaudio type appears here or above here.** The one directory that names an `ma_` symbol is
`src/backends/audio-miniaudio/`, at layer 3.

## Interface first, and the null backend is permanent

design.md §4: "the audio driver layer [is] defined and exercised by a trivial implementation
**before** miniaudio [is] linked. […] The retained trivial implementation is not ceremony: it is what
proves at every build that the interface does not leak the library."

`NullAudioBackend` is therefore kept forever, not until the real one lands. Three things depend on it:

* every audio suite in continuous integration, on machines with no sound card;
* `audio`'s own requirement that a headless build advance playback positions deterministically;
* the proof that the interface is an interface — a method miniaudio needed that the null backend
  could not implement would be a method that had leaked the library.

It is **driven**, not free-running: `advance(output, frames)` pulls exactly the frames it is asked
for and keeps the mix, so a test says "mix twenty milliseconds" and reads the samples.

## Two threads, and which one owns what

| | owns | may not |
|---|---|---|
| **Game thread** | every `VoiceControl`, the importance scores, the tier assignment, the target gains | — |
| **Audio thread** | each voice's playback position, filter state and fade | allocate, take a lock, touch a file, call into script |

They meet in exactly two places and nowhere else: the single-producer/single-consumer
`CommandQueue`, drained at the top of every `render()`; and the **double-buffered** mix state,
published by `update()` with one release store and read by `render()` with one acquire load. A
voice's immutable half — its clip, its bus, whether it loops — is written before the `Play` command is
pushed and read only after it is popped, which is what the queue's release/acquire pair makes safe.

The split is structural rather than documented: a field lives in exactly one of `VoiceControl`,
`VoicePlayback` and `VoiceMixState`, so "which thread may write this" is answered by where it is
declared.

## What is here

| file | what it holds |
|---|---|
| `handles.h` | buses, clips, voices and listeners, as generational handles |
| `format.h` | channel layouts, device formats, device info, and the audio clock |
| `backend.h` | `AudioBackend`, and the retained `NullAudioBackend` |
| `bus.h` | the mixing graph: `Master`, routing, sends, effects, solo, cycle rejection, the compiled order |
| `voice.h` | clips, the three halves of a voice, the tiers, and the importance score |
| `spatial.h` | attenuation models, cones, constant-power panning, Doppler, filter-based occlusion |
| `commands.h` | the lock-free command ring |
| `server.h` | the server: lifecycle, playback, `update()` on the game thread, `render()` on the audio thread |

## Three decisions worth knowing before changing anything

**A cycle in the graph is refused at configuration time, because there is nowhere else to catch it.**
A loop in a mixing graph is not a slow mix, it is an infinite one, and the thread it hangs is the
realtime thread — the symptom is a locked-up device rather than a stack trace. `set_output()` and
`add_send()` each ask whether the edge would close a loop and refuse it.

**Virtualising is not stopping, and the difference is a requirement.** A source past its budget keeps
advancing its playback position and mixes nothing, so a looping ambience the listener walked away
from resumes where it would have been rather than restarting. A voice *leaving* the mix is mixed once
more with a ramp to silence, because dropping it in one block is a step of its whole amplitude.

**A clip's samples are borrowed, so `destroy_clip()` stops before it releases.** The audio thread may
be part way through a block that reads them and there is no lock to wait on it with, so the slot is
reused only once `update()` has seen every voice on it finish. The stop is a hard one rather than a
fade: a click is preferable to reading memory the caller is about to free.

## What Seed means here

**Real and exercised**: the backend interface and its null implementation; the bus graph with cycle
rejection, solo, sends and a four-effect chain; play, stop, pause, seek, loop, volume, pitch, bus
assignment, randomised variation and one-shots; sample-accurate looping and scheduled starts; block
mixing with per-block gain ramps; five attenuation models, cones, panning, Doppler and filter-based
occlusion; the importance score, the four tiers, their budgets, hysteresis and virtualisation; the
audio clock; the lock-free queue and the double-buffered mix state; the diagnostics.

**Absent, deliberately, and named so nobody mistakes it for more**: decoding and streaming (the
engine plays float PCM the asset system owns); the sixteen effects beyond the four in `bus.h`;
`AcousticsBackend`, HRTF, propagation, reflections and acoustic geometry (M8 — and the fallback path
here is what `audio` requires to exist without them); interactive music and playlists; the middleware
plugin backends. None is stubbed: a stub that returns a plausible value is worse than an absent
function, because the first produces a mix nobody can tell is wrong.
