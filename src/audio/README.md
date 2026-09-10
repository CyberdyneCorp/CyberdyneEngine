# `src/audio/` — layer 4

**CyberAudio's acoustics half**: the acoustics backend Steam Audio sits behind, its geometry
extraction, the asynchronous simulation, importance tiers and voice virtualisation, the effect chain,
and interactive music.

**Governed by**: `audio`. M8.b tasks 10.1 and 10.2.

## Where the rest of the audio system is

`src/servers/audio/` (layer 2, M4) is the `AudioServer`: the bus graph, voices, mixing, the command
queue, spatialisation — panning, attenuation, cones, Doppler, filter-based occlusion — and the null
backend, with miniaudio behind `CY_AUDIO` in `src/backends/audio-miniaudio/`. **Read that first.**
This module is what layer 2 cannot reach:

| here | because |
|---|---|
| acoustic geometry and its cache | it comes from the world's collision geometry, through an extraction interface the host implements |
| importance and tiers | scoring reads a listener and a world position per source per frame |
| the acoustics backend | Steam Audio is a fetched dependency behind an engine-owned interface |
| the effect chain | a bus's effects are engine code above the mixer's own gain and pan |
| interactive music | playlists, layers and transitions are gameplay-facing, not mixer-facing |

## Steam Audio: what is declared, what is verified, and what is not

`deps/manifest.toml` now carries **steam_audio 4.8.1** (Apache-2.0), `optional = true`,
`feature = "CY_AUDIO_STEAM_AUDIO"`, `source_subdir = "core"`, behind the interface
`cy::audio::AcousticsBackend`. `THIRD_PARTY.md` is regenerated from it and
`python3 tools/deps/attribution.py --check` passes.

**Verified on this machine**: the manifest entry parses; `cmake` reports
`dependency steam_audio: excluded — CY_AUDIO_STEAM_AUDIO is off, so it is neither fetched, built nor
linked`; the default build fetches nothing and contains none of its code; `src/acoustics.cpp`
compiles with the option off, with every Steam Audio reference inside
`#if defined(CY_AUDIO_STEAM_AUDIO)`.

**NOT verified, and this is the gap**: the option has not been turned ON here, so upstream has never
been fetched or built on this machine, and the backend behind the option **returns
`ErrorCode::NotImplemented`** rather than pretending to simulate. Turning it on will fetch Steam
Audio and its own third-party tree; whether that configures and builds is unknown, and the honest
place to find out is the milestone's gate rather than a claim in this file.

**What that costs a game: nothing.** `audio` requires exactly that — "Content SHALL NOT depend on
Steam Audio being present: it SHALL improve audio quality, never enable or gate gameplay" —
`FallbackAcoustics` answers every query in every build, and `steam_audio_compiled_in()` and
`AcousticsBackend::capabilities()` answer the build and runtime questions separately.

## What is here, and the requirement each answers

| file | requirement |
|---|---|
| `acoustics.h` | "Steam Audio as the spatial acoustics backend", "Acoustic geometry and materials", "Asynchronous acoustic simulation" |
| `tiers.h` | "Audio importance and simulation tiers", "Voice virtualisation" |
| `effects.h` | "Effects" |
| `interactive.h` | "Interactive and adaptive audio", and the scheduling half of "Timing and synchronisation" |

## Three decisions worth knowing

**The read path takes one atomic load.** `ResultStore` is a double buffer with an index published by
one release store; the audio callback reads whichever buffer was published last. There is no lock on
the read path, because a lock on the read path is a lock the callback can block on.

**Hysteresis may not break a budget.** A source hovering at a tier boundary is held in place to stop
it flapping — but only while its tier still has room. "The cost of audio is bounded by configuration
rather than by content" would otherwise be false by a source or two per boundary, which is exactly
the kind of quiet overrun a budget is meant to prevent.

**An effect that is not implemented says so.** `effect_is_implemented()` reports which of the sixteen
kinds process samples in this build. Gain, the biquad family, delay, the dynamics processors,
distortion, panner, widener, the analysis tap and custom effects do; the reverbs, chorus, flanger,
phaser and pitch shift are declared with their latency and pass audio through. A chorus that silently
did nothing would be a bug report from a sound designer; a chorus that reports `implemented == false`
is a greyed-out control in a mixer window.

## What `audio` asks for that this module does not yet have

* The five effects above, and convolution's partitioned processing.
* The ECS components — `AudioSource`, `AudioListener`, `AcousticMaterialRef`, `AudioEffectVolume` —
  and their Swift overlay. This module has no ECS dependency and the components belong beside the
  other gameplay components.
* Hardware-accelerated acoustic simulation, which is Steam Audio's to expose.
* The audio clock's own query surface: the scheduling arithmetic is here (`next_transition_point`),
  and samples-played and output-latency belong to the device backend.

## Testing

`unit.audio_acoustics` — 27 cases: the geometry cache's static caching and dynamic updates, the
fallback's completeness and its refusal to claim capabilities, the build/runtime Steam Audio
questions, the double buffer, the importance budget and its deferral, interpolation of a sharp
occlusion change, scoring and tier budgets at 1,200 sources, hysteresis, pinning, virtualisation and
resumption, the filters and dynamics, parameter interpolation, latency, the unimplemented-effect
passthrough, the delay line, a custom effect, the meter, the panner and widener, musical time,
transitions with and without a segment, playlists (including a deterministic shuffle) and layers.
