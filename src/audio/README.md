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

## M8.c, task 4b: what the gate found, what was measured, and what is still open

M8.b's closing gate demoted `audio` from Complete because `-D CY_AUDIO_STEAM_AUDIO=ON` could not be
configured. M8.c ran the experiment. Three things came out of it, and only one of them is what the
gate was looking for.

### 1. The option gated a fetch and nothing else, and that is now fixed

`src/audio/src/acoustics.cpp` did **not** include `<cy_features.h>`, and `cmake/features.cmake` does
not turn a `CY_*` option into a compile definition — it writes `#define CY_AUDIO_STEAM_AUDIO 1` into
that generated header. So on the tree M8.b closed on, a build with the option ON would have linked
`cy::dep::steam_audio` into `cy_audio` while **every `#if defined(CY_AUDIO_STEAM_AUDIO)` in that
file evaluated false**: `SteamAudioBackend` was not compiled, `steam_audio_compiled_in()` answered
`false` in a build that had just built Steam Audio, and `create_steam_audio` returned `Unavailable`
naming the flag the caller had already passed.

It was invisible because the option had never configured successfully, so the two directions were
never compared. The include is now there, and `unit.audio_acoustics`'s parity case asserts
`steam_audio_compiled_in()` against this build's own `CY_AUDIO_STEAM_AUDIO` rather than trusting
either — the regression test for a defect that has no symptom other than silence.

### 2. Steam Audio 4.8.1 **can** be built here, and the recipe is written down

`libphonon.so` was produced out of tree during M8.c. It needs four upstream-required dependencies
(pffft, zlib, libmysofa, flatbuffers-with-`flatc`), one patch (`-fabi-version=6`, which breaks GCC
13's `<future>` and is rejected outright by clang 18), and two build-flag workarounds. **The full
measured recipe, with the exact commits, the reduced two-line repro for the compiler flag, and what
integrating it would cost this manifest, is in `deps/manifest.toml` beside the entry.** M8.b's
report named PFFFT, IPP and FFTS; IPP and FFTS are optional and turn off cleanly, and PFFFT is one
of four.

### 3. What is still open, stated as a gap rather than a plan

`SteamAudioBackend::simulate` **still returns `NotImplemented`**, and integrating the four
dependencies into `deps/manifest.toml` is **not done**.

**And the gap has a red gate attached to it, which nobody has been reading.**
`tools/deps/test_gating.py` derives its "everything on" configuration from every optional entry's
gating feature, so `CY_AUDIO_STEAM_AUDIO=ON` has been in that set since M8.b declared Steam Audio —
and the ON half of that test has been failing since, at
`steam_audio-src/core/CMakeLists.txt:235 (find_package)`, which is `find_package(PFFFT REQUIRED)`.
The OFF half passes. Measured at M8.c: 53 checks pass, then the ON configure fails. Writing the simulation against headers this
build cannot compile would be exactly the "fake" this milestone's rules forbid; the honest order is
the dependency change first, then the implementation, then a parity case that runs both backends.
The first of those three is a reviewed dependency decision with five arguments in it and it did not
belong in a session that had one shared CMake file to touch safely.

**What that costs a game today: still nothing**, and that is the requirement rather than a
consolation. `unit.audio_acoustics`'s parity case now writes down the contract content may depend
on — every query answered in order, a unit arrival direction, every coefficient in [0, 1], a
positive reverb time, an undersized result span refused rather than half-filled — and asserts it
over whichever backends the build has. A caller written against that contract behaves identically in
both builds, which is what "Content SHALL NOT depend on Steam Audio being present" means when it is
checked instead of stated.

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
