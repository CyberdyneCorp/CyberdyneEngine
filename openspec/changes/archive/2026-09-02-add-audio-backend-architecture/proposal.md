# Add audio backend architecture: miniaudio + Steam Audio behind an engine-owned AudioServer

## Why

The `audio` specification describes the audio architecture the engine needs but does not name the
backends that implement it, leaving the most consequential decision open: whether audio is
engine-owned or delegated to commercial middleware. Audio is a runtime system whose scheduling,
threading, and budget policy interact directly with the job system and ECS, so the engine must own
that layer. At the same time, device I/O and high-end spatial acoustics are large, well-solved
problems worth integrating rather than rebuilding.

A second gap is scale. The current spec bounds concurrent voices but has no notion of *how much
simulation each audible sound deserves*. For the RTS-scale workloads this engine targets —
tens of thousands of entities, thousands simultaneously making noise — uniform treatment of every
audible source is the dominant audio cost, far more than mixer efficiency.

## What Changes

- **Establish the backend boundary.** `AudioServer` and the audio graph become explicitly
  engine-owned; backends implement an `AudioBackend` interface. No backend type appears in any
  engine header outside its module, matching the treatment of Jolt in `physics`.
- **Adopt miniaudio** as the default low-level backend: device I/O, resampling, decoding,
  streaming primitives, and the low-level mixing substrate. Permissive (public domain / MIT-0),
  dependency-free, and covers every target platform including Web.
- **Adopt Steam Audio** (Apache 2.0) as the spatial acoustics backend for HRTF, occlusion,
  transmission, reflections, reverb, and sound propagation — capability-gated and optional.
- **Separate acoustic simulation from the audio callback.** Propagation and reflection simulation
  run asynchronously on the job system at their own rate; the realtime callback consumes their
  most recent published results and never waits on them.
- **Add acoustic geometry and materials**, derived from the same ECS world and collision geometry
  physics uses, so the environment has one semantic description rather than three.
- **Add an audio importance system** assigning each audible source a simulation tier
  (full propagation, basic spatialisation, or plain mixing), with per-tier budgets. This is the
  central performance policy for audio at scale.
- **Define optional middleware backends.** FMOD and Wwise are explicitly supported as *plugins*
  implementing `AudioBackend`, never as engine dependencies — studios with existing sound-design
  pipelines can adopt them without the engine depending on either.
- Update the build-vs-buy record in `thirdparty-dependencies` to reflect these choices.

Non-goals for this change: authoring tools for sound design, a DAW-like editor, procedural audio
synthesis, and baked acoustics precomputation workflows. These remain deferred.

## Capabilities

### New Capabilities

None. This change refines existing capabilities rather than introducing new ones.

### Modified Capabilities

- `audio` — backend abstraction, miniaudio and Steam Audio selection, asynchronous acoustic
  simulation, acoustic geometry and materials, the importance and tiering system, ECS-native
  components, optional middleware backends, and the corresponding diagnostics.
- `thirdparty-dependencies` — record miniaudio and Steam Audio in the intended dependency set,
  record the AudioServer and audio graph as engine-built, and record the optional-middleware
  policy.

## Impact

- **Dependencies**: adds miniaudio (runtime, required when audio is enabled) and Steam Audio
  (runtime, optional, capability-gated). Both permissive. Existing audio codec dependencies remain
  for formats miniaudio does not decode natively.
- **Interfaces**: introduces `AudioBackend` and `AcousticsBackend` as engine-owned interfaces;
  `AudioServer`'s public surface is unchanged in shape.
- **Systems**: adds an audio importance system running in the frame stage, and an acoustic
  geometry extraction path that consumes the same collision geometry as `physics`.
- **Build**: adds `CY_AUDIO_STEAM_AUDIO` as a feature option; audio remains removable entirely via
  `CY_AUDIO`.
- **Cross-references**: touches `physics` (shared collision geometry), `ecs-core` (importance
  system scheduling), and `build-system-and-platforms` (feature options) without changing their
  requirements.
