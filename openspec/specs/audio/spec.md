# audio Specification

## Purpose

Defines audio: the engine-owned `AudioServer` and mixing graph, the backends beneath it, spatial
acoustics, the importance system that bounds cost at scale, and the thread-safety rules that keep
a realtime mixer glitch-free while gameplay mutates state.

The layering is deliberate. The engine owns `AudioServer`, the bus graph, voice management,
streaming policy, and the importance and tiering policy, because those interact directly with the
job system, the ECS, and the frame budget. Beneath that, **miniaudio** provides device I/O,
conversion, decoding, and streaming, and **Steam Audio** provides high-end spatial acoustics —
each behind an engine-owned interface, each replaceable. Commercial middleware (FMOD, Wwise) is
supported as an optional plugin backend but is never a dependency.

## Requirements

### Requirement: Engine-owned audio architecture
`AudioServer`, the bus graph, voice management, streaming policy, spatialisation policy, and the
importance system SHALL be engine code. Third-party audio libraries SHALL sit behind two
engine-owned interfaces:

- **`AudioBackend`** — device lifecycle and the realtime callback, sample-rate and channel
  conversion, decoding, and streaming primitives
- **`AcousticsBackend`** — spatial acoustics: HRTF rendering, occlusion, transmission,
  reflections, reverb, and propagation

No backend type SHALL appear in any engine or game-facing header outside its own backend module,
mirroring the treatment of the physics backend.

#### Scenario: Backend types do not leak
- **WHEN** engine or game code is compiled
- **THEN** no backend library type SHALL appear in any header outside `backends/audio/`

#### Scenario: Backend is replaceable
- **WHEN** a different audio backend is selected
- **THEN** no gameplay code, component definition, or audio asset SHALL require changes

#### Scenario: Engine owns the policy
- **WHEN** voice limits, tier budgets, streaming residency, or job scheduling are decided
- **THEN** they SHALL be enforced by engine code, not delegated to a backend's own policy

### Requirement: miniaudio as the default low-level backend
The default `AudioBackend` SHALL be implemented over **miniaudio**, used for: device enumeration
and lifecycle, the realtime callback, sample-rate and channel conversion, decoding of the formats
it supports natively, and streaming ring buffers.

The engine SHALL NOT use miniaudio's node graph, its high-level engine API, or its built-in 3D
spatialisation; the engine's own bus graph and spatialisation policy sit above it. This keeps the
depended-upon surface small enough that the backend stays genuinely replaceable.

miniaudio SHALL be a required dependency whenever `CY_AUDIO` is enabled, and SHALL be excluded
entirely when it is not.

#### Scenario: Platform coverage
- **WHEN** the engine runs on a supported or planned platform
- **THEN** the backend SHALL use that platform's native audio API through miniaudio, with no
  engine-side platform branching

#### Scenario: Small dependency surface
- **WHEN** the miniaudio backend is reviewed
- **THEN** the set of miniaudio APIs it calls SHALL be documented and limited to device I/O,
  conversion, decoding, and streaming

### Requirement: Audio driver layer
`AudioBackend` SHALL abstract the platform output device. The default implementation SHALL be
built on **miniaudio**, covering WASAPI (Windows), CoreAudio (macOS/iOS), ALSA and
PulseAudio/PipeWire (Linux), AAudio/OpenSL (Android), and Web Audio (Web). A **null** backend
SHALL also be provided.

The backend SHALL call back from its own realtime thread requesting a buffer of frames, and SHALL
expose: mix rate, channel layout, buffer size, device enumeration and selection, and input
capture.

The engine SHALL support device change, reinitialising the mixer without dropping playback state.

#### Scenario: Device changes mid-session
- **WHEN** the default output device changes
- **THEN** the driver SHALL reinitialise at the new device's mix rate and channel layout, and
  playback SHALL continue

#### Scenario: Realtime constraints
- **WHEN** the driver callback runs
- **THEN** the mixer SHALL NOT allocate, take a blocking lock, perform file I/O, or call into
  script

#### Scenario: Null backend for tests
- **WHEN** the engine runs headless or in tests
- **THEN** the null backend SHALL satisfy the interface, advancing playback positions
  deterministically without a device

### Requirement: Bus graph
Audio SHALL be routed through a graph of **buses**, each with a volume, mute, solo, and bypass
flag, an ordered effect chain, and a send to another bus.

The graph SHALL be a directed acyclic graph, with a `Master` bus as the root. Cycles SHALL be
rejected when configured.

Buses SHALL support multiple sends with independent levels, enabling reverb and submix routing.

#### Scenario: Reverb send
- **WHEN** a sound sends 30 % to a reverb bus and 100 % to its main bus
- **THEN** both paths SHALL be mixed, with the reverb bus processing its send before summing into
  Master

#### Scenario: Cycle rejected
- **WHEN** a send would create a cycle
- **THEN** the configuration SHALL be rejected with a diagnostic

#### Scenario: Solo
- **WHEN** a bus is soloed
- **THEN** buses that do not feed it SHALL be silenced for the mix

### Requirement: Mixing
Mixing SHALL proceed per driver callback: clear bus buffers, mix each active voice into its
target buses with per-channel gains, process each bus's effects in dependency order, apply bus
gain, and sum into sends and finally Master.

Voice gains SHALL be **interpolated** across the buffer from their previous to current values, so
parameter changes do not click.

Mixing SHALL be block-based with a configurable block size, and MAY be parallelised across
independent bus subtrees when the graph permits.

#### Scenario: No clicks on volume change
- **WHEN** a voice's volume changes between callbacks
- **THEN** the gain SHALL ramp across the buffer rather than stepping

#### Scenario: Voice limit
- **WHEN** more voices are requested than the configured limit
- **THEN** voices SHALL be prioritised by importance and audibility, and the least important
  SHALL be virtualised (tracked but not mixed) rather than dropped abruptly

### Requirement: Thread-safe state handoff
Gameplay and audio threads SHALL exchange state without locks in the mixer: commands SHALL be
enqueued to a lock-free queue consumed at the start of each callback, and voice state SHALL be
double buffered.

Stopping or destroying a voice SHALL apply a short **fade-out** rather than cutting, and memory
SHALL be reclaimed on the main thread after the audio thread has released it.

#### Scenario: Stop during a callback
- **WHEN** gameplay stops a sound while it is being mixed
- **THEN** the mixer SHALL fade it out over the remainder of the block and mark it for
  reclamation, avoiding a click

#### Scenario: No allocation in the callback
- **WHEN** a new voice starts
- **THEN** its resources SHALL have been prepared on the game thread, with the callback only
  activating a pre-allocated slot

### Requirement: Audio assets and streaming
The engine SHALL support: uncompressed PCM, and compressed **Vorbis** and **Opus** for streamed
music, plus a lightweight ADPCM or similar for short effects. Cooked tracks SHALL also carry FLAC and
MP3 bitstreams kept from their sources, so a streamed track is never re-encoded at cook time.

Assets SHALL declare a load mode: **fully decoded in memory** (short sounds), **compressed in
memory, decoded on demand** (medium), or **streamed from disk** (music and ambience).

Streaming SHALL decode ahead into a ring buffer on the asset thread, with underrun producing
silence and a diagnostic rather than a stall. A stream's decoding, its rate and channel conversion,
and every read of its source SHALL run as jobs — reads that block on storage on the asynchronous
service — and never on the audio thread; the audio thread SHALL only drain the ring. A refill SHALL
fill the ring to capacity, so a stream SHALL NOT underrun while no refill is delayed by more than the
ring's length less one mixer block.

A stream's gain SHALL be an envelope the audio thread evaluates per frame, so a crossfade between two
tracks is equal-power and both of its ramps start on the same sample. A seek SHALL fade the old
position out over one mixer block, refill the ring from the new position, and ramp the new position
in once the ring is primed.

#### Scenario: Music streams
- **WHEN** a long music track plays
- **THEN** it SHALL be streamed and decoded ahead, with only the ring buffer resident

#### Scenario: Seamless loop
- **WHEN** a track declares loop points
- **THEN** looping SHALL be sample-accurate with no gap or click

#### Scenario: A starved job system within the buffer
- **WHEN** every refill of a stream is delayed by up to the ring's length less one mixer block
- **THEN** the stream SHALL play every frame in order with no underrun

#### Scenario: Underrun is silence and a diagnostic
- **WHEN** a stream's refills are delayed past the ring's length
- **THEN** the missing frames SHALL play as silence, the underrun SHALL be counted and reported as a
  diagnostic naming the track, and the stream SHALL continue once refills arrive

#### Scenario: Seek is sample-accurate
- **WHEN** a stream seeks to a position in its track
- **THEN** the frames it plays after the seek's ramp SHALL be the track's frames from that position,
  and a decoder's seek SHALL deliver the same samples a decode from the start delivers there

#### Scenario: Crossfade is equal-power
- **WHEN** one track crossfades into another
- **THEN** the outgoing gain SHALL fall as cos(t·π/2) and the incoming rise as sin(t·π/2), starting on
  the same sample, so the sum of their squares stays one, and the outgoing stream SHALL stop itself
  at the end of its ramp

### Requirement: Spatial audio
The engine SHALL provide 2D and 3D spatialisation with:

- distance attenuation models: inverse, inverse-squared, linear, logarithmic, and a custom curve,
  with a reference distance and a maximum distance
- directionality: a cone with inner and outer angles and outer gain
- panning appropriate to the output channel layout, and **HRTF** binaural rendering for headphones
- **Doppler** shift computed from relative velocity, with a configurable scale
- **occlusion and obstruction**: low-pass filtering and attenuation driven by acoustic geometry,
  by physics queries, or by explicit gameplay input
- per-listener rendering, with support for more than one listener (split screen)

The fidelity applied to a given source SHALL be determined by its simulation tier: sources at
`FullAcoustic` SHALL additionally receive propagation, reflections, and geometry-based occlusion
from the acoustics backend, while lower tiers SHALL use the panning, attenuation, and
filter-based occlusion described above.

#### Scenario: Sound behind the listener
- **WHEN** HRTF is enabled and a source is behind the listener
- **THEN** binaural filtering SHALL produce a perceptible front-back distinction

#### Scenario: Occlusion
- **WHEN** a wall lies between a source and the listener
- **THEN** the source SHALL be low-pass filtered and attenuated by the occlusion parameters

#### Scenario: Doppler
- **WHEN** a source moves toward the listener at speed
- **THEN** its pitch SHALL rise proportionally, scaled by the Doppler factor

#### Scenario: Fidelity follows the tier
- **WHEN** a source is demoted from `FullAcoustic` to `Spatialised`
- **THEN** propagation and reflections SHALL be replaced by the fallback path, cross-faded so the
  transition is not audible as a jump

### Requirement: Steam Audio as the spatial acoustics backend
The engine SHALL support **Steam Audio** as an `AcousticsBackend`, providing HRTF binaural
rendering, ambisonic encoding and decoding, geometry-aware occlusion, material-dependent
transmission, real-time reflections, reverb, and sound propagation.

Steam Audio SHALL be **optional and capability-gated** behind the `CY_AUDIO_STEAM_AUDIO` build
option and a runtime capability query. When absent or disabled, the engine SHALL fall back to its
own panning, distance attenuation, filter-based occlusion, and reverb sends.

Content SHALL NOT depend on Steam Audio being present: it SHALL improve audio quality, never
enable or gate gameplay.

Hardware acceleration for acoustic simulation SHALL be used where the backend and device provide
it, and SHALL be capability-gated with a CPU path always available.

#### Scenario: Acoustics backend absent
- **WHEN** the engine is built without Steam Audio
- **THEN** all audio SHALL still play, spatialised by the fallback path, with no missing sounds
  and no gameplay difference

#### Scenario: Occlusion through geometry
- **WHEN** a wall stands between an emitter and the listener with acoustics enabled
- **THEN** the direct path SHALL be attenuated and filtered per the wall's transmission
  properties, and audible energy MAY still arrive via reflections

#### Scenario: Acceleration is optional
- **WHEN** hardware acceleration is unavailable
- **THEN** acoustic simulation SHALL run on the CPU within its configured budget

### Requirement: Asynchronous acoustic simulation
Acoustic simulation (propagation, reflections, and geometry-based occlusion) SHALL run
asynchronously on the job system at its own configurable rate, decoupled from both the simulation
tick and the audio callback.

Simulation results — direction, occlusion and transmission coefficients, reflection and reverb
parameters — SHALL be published into a double-buffered store. The realtime callback SHALL read the
most recently completed result and SHALL NOT wait on simulation.

Applied parameters SHALL be interpolated toward newly published results over a configurable time,
so a change in simulated acoustics is not audible as a step.

Acoustic simulation SHALL have a per-frame time budget; work exceeding it SHALL be deferred to
subsequent frames, prioritised by source importance.

#### Scenario: Callback never blocks
- **WHEN** acoustic simulation is mid-update as the audio callback runs
- **THEN** the callback SHALL use the previous published result and complete within its deadline

#### Scenario: Parameters change smoothly
- **WHEN** a listener moves and occlusion changes sharply between simulation updates
- **THEN** the applied filtering SHALL interpolate rather than switching abruptly

#### Scenario: Budget exceeded
- **WHEN** more sources request simulation than the budget allows
- **THEN** the highest-importance sources SHALL be simulated and the remainder deferred, with the
  deferral reported

### Requirement: Acoustic geometry and materials
The engine SHALL derive **acoustic geometry** from the same ECS world and the same collision
geometry used by physics, so the environment has one semantic description rather than separately
authored visual, physical, and acoustic worlds.

An `AcousticMaterial` SHALL define at least: absorption, transmission, and scattering
coefficients, authored per material and referenced by surfaces.

Surfaces SHALL be able to override the material derived from their physics material, and geometry
SHALL be able to opt out of contributing to acoustics entirely.

Extraction SHALL be incremental and bounded: only geometry within a configurable radius of active
listeners SHALL be extracted, static geometry SHALL be cached, and dynamic geometry SHALL be
updated only when it moves.

Audio SHALL consume geometry through an extraction interface and SHALL NOT call into the physics
server directly, so the two subsystems remain decoupled.

#### Scenario: One description of a wall
- **WHEN** a wall has collision geometry and a physics material
- **THEN** it SHALL contribute acoustic geometry with an acoustic material derived from that
  physics material, without separate acoustic authoring

#### Scenario: Override for a mismatched proxy
- **WHEN** a collision box stands in for an open grille that should not block sound
- **THEN** that geometry SHALL be able to opt out of acoustics or override its transmission

#### Scenario: Extraction is bounded
- **WHEN** a large world is loaded
- **THEN** only geometry near active listeners SHALL be extracted, with static results cached
  across frames

### Requirement: Audio importance and simulation tiers
Every playing source SHALL be scored each frame and assigned a **simulation tier**, so that the
cost of audio is bounded by configuration rather than by content.

Tiers SHALL be, from most to least expensive:

| Tier | Treatment |
|---|---|
| `FullAcoustic` | Full acoustic simulation: propagation, reflections, geometry occlusion, HRTF |
| `Spatialised` | Panning, distance attenuation, filter-based occlusion, reverb send |
| `Simple` | Stereo or mono mixing with distance attenuation only |
| `Virtual` | Position advanced, not mixed (see voice virtualisation) |

Importance scoring SHALL consider at least: distance to the listener, emitter volume and priority,
listener orientation, sound category, gameplay importance flags, current occlusion estimate, and
the source's tier in the previous frame.

Each tier SHALL have a configurable **budget** as a maximum source count; sources beyond a tier's
budget SHALL be demoted to the next tier by importance rank.

Tier transitions SHALL be hysteretic and cross-faded, so a source oscillating near a threshold
does not produce audible artefacts.

Gameplay SHALL be able to pin a source to a minimum tier, so narratively critical audio is not
demoted.

#### Scenario: Thousands of sources stay bounded
- **WHEN** 8 000 sources are playing and 1 100 are audible
- **THEN** tier budgets SHALL limit full acoustic simulation to the configured count, with the
  remainder spatialised, simply mixed, or virtualised

#### Scenario: Hysteresis prevents oscillation
- **WHEN** a source hovers at a tier boundary
- **THEN** hysteresis SHALL prevent per-frame tier flapping, and any change SHALL be cross-faded

#### Scenario: Pinned source
- **WHEN** a dialogue line is pinned to at least `Spatialised`
- **THEN** it SHALL never be demoted below that tier regardless of distance or budget pressure

#### Scenario: Budgets are configurable
- **WHEN** a project targets lower-end hardware
- **THEN** reducing tier budgets SHALL reduce audio cost predictably without content changes

### Requirement: Voice virtualisation
A source that is inaudible or beyond budget SHALL be **virtualised** rather than stopped: its
playback position SHALL continue to advance, but no mixing or DSP SHALL be performed for it.

When a virtualised source becomes audible again it SHALL resume at the correct playback position,
fading in over a short configurable time.

Sources explicitly stopped by gameplay SHALL be stopped, not virtualised.

#### Scenario: Looping ambience returns correctly
- **WHEN** the listener leaves and later re-enters a looping ambience's range
- **THEN** it SHALL resume at the position it would have reached, not restart

#### Scenario: Virtual sources are cheap
- **WHEN** thousands of sources are virtualised
- **THEN** their per-frame cost SHALL be limited to advancing a position and scoring importance

### Requirement: Effects
The engine SHALL provide bus effects: gain, parametric EQ, low/high/band-pass and shelf filters,
compressor, limiter, gate, reverb (algorithmic and convolution), delay, chorus, flanger,
phaser, distortion, pitch shift, stereo widener, panner, and an analysis tap (spectrum and level
metering).

Effects SHALL be parameterisable at runtime with interpolated parameter changes, and SHALL report
their latency so the engine can compensate.

Custom effects SHALL be implementable in native code and registered like built-ins.

#### Scenario: Convolution reverb
- **WHEN** a convolution reverb is configured with an impulse response
- **THEN** it SHALL process using partitioned convolution to bound per-callback cost

#### Scenario: Latency compensation
- **WHEN** an effect reports processing latency
- **THEN** the engine SHALL account for it in timing queries used for synchronisation

### Requirement: Audio components and playback
Audio SHALL be ECS-native. The engine SHALL provide the components:

| Component | Contents |
|---|---|
| `AudioSource` | Clip, volume, pitch, looping, spatialised flag, bus, category, priority, minimum tier |
| `AudioListener` | Priority, used to select or weight active listeners |
| `AcousticMaterialRef` | Overrides the acoustic material derived from physics |
| `AudioEffectVolume` | A spatial region modifying bus routing or effect parameters, for reverb zones |

An audio system SHALL feed each source's world transform and velocity from its entity's transform
automatically, so a moving entity's audio is spatialised without gameplay code.

`AudioSource` SHALL support: play, stop, pause, seek, loop, volume, pitch, bus assignment,
priority, a randomised pitch and volume range, and one-shot fire-and-forget playback that does not
require keeping a handle.

Components SHALL be exposed through the Swift overlay so gameplay code reads naturally while the
mixing and simulation remain native.

#### Scenario: Reverb zone
- **WHEN** a listener enters an `AudioEffectVolume`
- **THEN** the configured reverb send SHALL blend in over the volume's transition distance

#### Scenario: One-shot
- **WHEN** a one-shot is fired
- **THEN** it SHALL play to completion and release itself, with no gameplay handle required

#### Scenario: Transform drives spatialisation
- **WHEN** an entity with an `AudioSource` moves
- **THEN** its audio position and velocity SHALL follow automatically, with velocity used for
  Doppler

#### Scenario: Swift ergonomics
- **WHEN** Swift gameplay code plays a sound on an entity
- **THEN** it SHALL do so through the component API without managing voices, buses, or handles

### Requirement: Timing and synchronisation
The engine SHALL expose the audio clock: samples played, the current output time, time to the
next callback, and output latency — so gameplay can schedule events precisely against audio.

Playback SHALL be schedulable at a **future audio time**, so sounds can be started exactly on a
beat regardless of frame timing.

#### Scenario: Rhythm game
- **WHEN** a game needs the exact playback position
- **THEN** it SHALL combine the audio clock with output latency to obtain the position the
  listener is actually hearing

#### Scenario: Scheduled start
- **WHEN** a sound is scheduled for a specific audio time
- **THEN** it SHALL begin at that sample, not at the start of the next frame

### Requirement: Interactive and adaptive audio
The engine SHALL support: **playlists** with ordering and transition rules, **layered** music
where stems fade with gameplay parameters, and **transitions** that occur at musically meaningful
points (immediate, next beat, next bar, next marker, end of clip) with optional transition
segments.

#### Scenario: Combat transition
- **WHEN** combat begins and the transition is set to "next bar"
- **THEN** the music SHALL switch at the next bar boundary, optionally through a transition stem

### Requirement: Optional middleware backends
The engine SHALL permit **FMOD** and **Wwise** to be used as alternative `AudioBackend`
implementations supplied as optional plugins, for studios with existing sound-design pipelines.

Neither SHALL be a dependency of the engine, appear in the dependency manifest as required, or be
referenced by engine code. Middleware plugins SHALL live outside the engine repository or in an
explicitly optional module excluded from default builds.

When a middleware backend is active, engine-owned policy that the middleware duplicates (its own
bus graph, its own streaming) MAY be delegated to it, and the specification SHALL document which
engine features are unavailable in that configuration.

#### Scenario: Engine has no middleware dependency
- **WHEN** the engine is built with default options
- **THEN** no proprietary audio middleware SHALL be fetched, built, linked, or required

#### Scenario: Studio adopts existing pipeline
- **WHEN** a project supplies a Wwise backend plugin
- **THEN** gameplay code using `AudioServer` SHALL work unchanged, with documented feature
  differences

### Requirement: Audio diagnostics
The engine SHALL report: active, virtual, and per-tier voice counts; per-bus peak and RMS levels;
CPU time per bus and per effect; acoustic simulation time and budget utilisation; underrun counts;
and the voices demoted or virtualised by budget pressure.

A debug view SHALL visualise 3D source positions and their tiers, attenuation ranges, listener
orientation, extracted acoustic geometry with its materials, occlusion and propagation paths, and
reflection probes.

#### Scenario: Diagnosing dropouts
- **WHEN** audio glitches
- **THEN** the underrun counter and per-effect CPU breakdown SHALL identify whether the mixer is
  exceeding its budget and where

#### Scenario: Diagnosing tier pressure
- **WHEN** important sounds are being demoted
- **THEN** per-tier counts and demotion statistics SHALL show which budget is saturated

#### Scenario: Acoustic geometry mismatch
- **WHEN** sound passes through a wall unexpectedly
- **THEN** the acoustic geometry debug view SHALL show whether that wall contributed geometry and
  with what material
