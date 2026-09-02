# Design: audio backend architecture

## Context

Audio sits between two constraints. The mixing callback is a hard-realtime context: it cannot
allocate, lock, or block. Meanwhile gameplay mutates audio state constantly from the simulation
thread, and high-quality spatial acoustics involve ray tracing against world geometry that is far
too expensive to run inside that callback.

The engine also targets scenes with tens of thousands of entities. Audio cost at that scale is
dominated by *how many sources receive expensive treatment*, not by mixer throughput.

## Decisions

### 1. Own the AudioServer, integrate the backends

`AudioServer`, the bus graph, voice management, streaming policy, and the importance system are
engine code. miniaudio and Steam Audio sit behind `AudioBackend` and `AcousticsBackend`.

**Rationale.** These are the layers where engine-level policy lives: budget enforcement, job
system integration, ECS coupling, and determinism. A middleware engine imposes its own threading
and asset model, which would compete with the job system and the asset pipeline. Conversely,
device enumeration and HRTF convolution are not places where owning the code makes the engine
better.

**Alternative rejected — FMOD or Wwise as the fundamental engine.** Both are excellent and have
better authoring tools than this engine will have for years. They were rejected as the *foundation*
because: they are proprietary with per-title commercial terms, which conflicts with the project's
open-source positioning; they own the mixing thread and asset pipeline, duplicating systems the
engine already has; and depending on them makes audio the one subsystem the project cannot fix.
They remain fully supported as optional plugin backends, which is the arrangement that serves
studios with existing pipelines without imposing on everyone else.

### 2. miniaudio as the default low-level backend

**Rationale.** Single-file C, no dependencies beyond the standard library, permissive licensing
(public domain / MIT-0), and native backends for WASAPI, CoreAudio, ALSA, PulseAudio, JACK,
AAudio, OpenSL ES, and Web Audio — the entire target and planned-target platform set. It provides
device I/O, resampling, decoding, and streaming primitives without dictating architecture above
them.

**What we use it for.** Device lifecycle and the realtime callback, sample rate conversion,
channel conversion, decoding for the formats it supports, and streaming ring buffers.

**What we do not use it for.** Its node graph, its 3D spatialisation, and its high-level engine
API. The engine's own bus graph and spatialisation policy sit above it. This keeps the surface we
depend on small, which is what makes the backend genuinely replaceable.

**Alternative considered — writing the platform layer ourselves.** Roughly a month per platform to
reach parity, with ongoing maintenance as OS audio APIs change, in exchange for no differentiating
benefit.

### 3. Steam Audio as the spatial acoustics backend

**Rationale.** Open sourced under Apache 2.0, with a C API explicitly intended for integration
into custom engines. It provides HRTF binaural rendering, ambisonics, material-dependent
transmission, geometry-aware occlusion, real-time reflections, and sound propagation — a body of
work that would take years to reach independently.

**Capability-gated and optional.** Steam Audio is not required. With it absent or disabled, the
engine falls back to its own panning, distance attenuation, filter-based occlusion, and reverb
sends. Content SHALL NOT depend on Steam Audio being present; it improves quality rather than
enabling gameplay.

### 4. Acoustic simulation is asynchronous and rate-decoupled

Three rates, deliberately different:

| Stage | Rate | Thread |
|---|---|---|
| Gameplay audio state | Simulation tick (60 Hz) | Simulation |
| Acoustic simulation (propagation, reflections) | Configurable, typically 10–20 Hz, amortised | Job workers |
| Mixing and DSP | Device callback (~100–350 Hz) | Realtime audio |

Acoustic simulation publishes results — direction, occlusion and transmission coefficients,
reflection and reverb parameters — into a double-buffered store. The callback reads the most
recent complete result and interpolates toward it. It never waits on simulation.

**Rationale.** Propagation ray tracing cannot meet callback deadlines, and its results change
slowly relative to the callback rate. Decoupling makes the expensive part budgetable and the
realtime part bounded. Interpolating toward published results prevents audible stepping.

### 5. Acoustic geometry shares the world's semantic description

Acoustic geometry is derived from the same ECS world and the same collision geometry `physics`
uses, tagged with an `AcousticMaterial` (absorption, transmission, scattering). Surfaces may
override it, and a mesh may opt out of contributing.

**Rationale.** Three separately authored descriptions of the same walls — visual, physical,
acoustic — drift apart and produce bugs where a sound passes through a wall the player can see and
collide with. Deriving one from the other means the environment has a single semantic truth. The
subsystems remain decoupled: audio consumes geometry through an extraction interface, not by
calling into physics.

**Trade-off accepted.** Collision geometry is often simplified relative to visual geometry, which
is usually right for acoustics too, but occasionally wrong — a collision box standing in for an
open grille. The override and opt-out mechanisms exist for exactly that case.

### 6. Importance tiers are the audio performance policy

Every audible source is scored and assigned a tier each frame:

```
        8,000 playing AudioSources
                    │
             audibility cull            (distance, volume, budget)
                    │
        ┌───────────┴───────────┐
   not audible               audible
      6,900                   1,100
                                │
                       importance scoring
                                │
        ┌───────────────┬───────┴───────┬───────────────┐
        ▼               ▼               ▼               ▼
     Tier 0          Tier 1          Tier 2         virtualised
   full acoustic   basic 3D        stereo/mono       tracked,
    simulation    spatialisation     mixing          not mixed
       ~50            ~200             ~850
```

Scoring inputs: distance, emitter volume and priority, listener orientation, sound category,
gameplay importance flags, current occlusion estimate, and previous-frame tier (hysteresis).

Each tier has a **budget** — a maximum source count — enforced by ranking, so cost is bounded by
configuration rather than by content. Tier changes are hysteretic and cross-faded so a source
moving between tiers does not click or jump in apparent position.

**Rationale.** This is the difference between audio scaling to RTS workloads and not. It is
specified as engine policy rather than left to games because getting it wrong is the difference
between 5 % and 40 % of frame time.

### 7. Virtualisation rather than stopping

A source below the audibility threshold or over budget is *virtualised*: its playback position
continues to advance, but no mixing occurs. When it becomes audible again it resumes at the
correct position.

**Rationale.** Stopping and restarting produces audible restarts of looping sounds and loses sync
for music and long ambiences. Virtualisation costs a position counter.

## Risks

- **Steam Audio's simulation cost is workload-dependent.** Mitigated by the tier budget: only
  Tier 0 sources receive propagation, and Tier 0 has a hard count limit.
- **miniaudio's abstraction may not expose a platform capability we later need.** Mitigated by the
  small surface we depend on; a platform-specific backend can be added behind `AudioBackend`
  without disturbing anything above it.
- **Acoustic geometry extraction cost in large worlds.** Mitigated by extracting only within a
  radius of active listeners, incrementally, and caching static geometry.
- **Two backends means two failure modes.** Mitigated by requiring the engine to be fully
  functional with Steam Audio absent.

## Open questions

- Whether baked acoustics (offline-computed propagation probes) are worth adding later for static
  environments, trading bake time for runtime cost. Deferred; the tier system's interface would
  accommodate a baked source of acoustic parameters without restructuring.
- Whether ambisonic bus rendering should be the internal representation for Tier 0 sources rather
  than direct binaural, which would make listener rotation cheaper at the cost of memory. Deferred
  pending measurement.
