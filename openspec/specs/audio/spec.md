# audio Specification

## Purpose

Defines audio: the mixing graph, the driver layer, spatialisation, effects, streaming, and the
thread-safety rules that keep a realtime mixer glitch-free while gameplay mutates state.

## Requirements

### Requirement: Audio driver layer
`AudioDriver` SHALL abstract the platform output device, with implementations for WASAPI
(Windows), CoreAudio (macOS/iOS), ALSA and PulseAudio/PipeWire (Linux), AAudio/OpenSL (Android),
Web Audio (Web), and a **null** driver.

The driver SHALL call back from its own realtime thread requesting a buffer of frames, and SHALL
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
music, plus a lightweight ADPCM or similar for short effects.

Assets SHALL declare a load mode: **fully decoded in memory** (short sounds), **compressed in
memory, decoded on demand** (medium), or **streamed from disk** (music and ambience).

Streaming SHALL decode ahead into a ring buffer on the asset thread, with underrun producing
silence and a diagnostic rather than a stall.

#### Scenario: Music streams
- **WHEN** a long music track plays
- **THEN** it SHALL be streamed and decoded ahead, with only the ring buffer resident

#### Scenario: Seamless loop
- **WHEN** a track declares loop points
- **THEN** looping SHALL be sample-accurate with no gap or click

### Requirement: Spatial audio
The engine SHALL provide 2D and 3D spatialisation with:

- distance attenuation models: inverse, inverse-squared, linear, logarithmic, and a custom curve,
  with a reference distance and a maximum distance
- directionality: a cone with inner and outer angles and outer gain
- panning appropriate to the output channel layout, and **HRTF** binaural rendering for headphones
- **Doppler** shift computed from relative velocity, with a configurable scale
- **occlusion and obstruction**: low-pass filtering and attenuation driven by physics queries or
  by explicit gameplay input
- per-listener rendering, with support for more than one listener (split screen)

#### Scenario: Sound behind the listener
- **WHEN** HRTF is enabled and a source is behind the listener
- **THEN** binaural filtering SHALL produce a perceptible front-back distinction

#### Scenario: Occlusion
- **WHEN** a wall lies between a source and the listener
- **THEN** the source SHALL be low-pass filtered and attenuated by the occlusion parameters

#### Scenario: Doppler
- **WHEN** a source moves toward the listener at speed
- **THEN** its pitch SHALL rise proportionally, scaled by the Doppler factor

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
Audio SHALL be exposed as components: `AudioListener`, `AudioSource` (2D or 3D), and
`AudioBusEffectVolume` (a spatial region that modifies bus routing or effect parameters, for
reverb zones).

`AudioSource` SHALL support: play, stop, pause, seek, loop, volume, pitch, bus assignment,
priority, a randomised pitch and volume range, and one-shot fire-and-forget playback that does
not require keeping a handle.

#### Scenario: Reverb zone
- **WHEN** a listener enters an `AudioBusEffectVolume`
- **THEN** the configured reverb send SHALL blend in over the volume's transition distance

#### Scenario: One-shot
- **WHEN** a one-shot is fired
- **THEN** it SHALL play to completion and release itself, with no gameplay handle required

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

### Requirement: Audio diagnostics
The engine SHALL report: active and virtual voice counts, per-bus peak and RMS levels, CPU time
per bus and per effect, underrun counts, and the voices culled by the voice limit.

A debug view SHALL visualise 3D source positions, attenuation ranges, listener orientation, and
occlusion rays.

#### Scenario: Diagnosing dropouts
- **WHEN** audio glitches
- **THEN** the underrun counter and per-effect CPU breakdown SHALL identify whether the mixer is
  exceeding its budget and where
