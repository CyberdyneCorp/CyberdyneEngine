# sequencing-and-cinematics Specification

## Purpose

Defines **CyberSequence**: compiled timelines that orchestrate camera, animation, audio, effects,
materials, lighting, environment, interface and gameplay — and own none of them.

Six capabilities left hooks for this before it existed: residency propagates deadlines for "a
cinematic declaring a camera cut", the camera stack has a cinematic override priority, temporal
rendering invalidates history on cuts, weather requires that sequencing drive its state rather than a
parallel path, animation provides markers, and audio provides timing. Those hooks exist because the
alternative — a cinematic system with its own camera, its own weather and its own playback — is the
failure this specification is written to prevent.

Timelines **compile**, for the seventh time in this engine, and here the payoff is twofold: runtime
cost scales with active sections rather than authored keys, and the compiler derives a **preload
plan**. That second point is the differentiator — every other streaming predictor in the engine is
reactive or extrapolates a few hundred milliseconds, while a sequence knows exactly where its camera
will be in six seconds and what each shot needs.

Two boundaries keep it safe. Time is **exact and not the simulation tick** except in the simulation
domain, so a twenty-four-frame cinematic, a sixty-hertz simulation and a high-refresh display coexist
without corrupting one another. And authoritative change crosses the **gameplay command stream** like
every other producer — a sequence is the sixth, alongside input, agents, network peers, replay and
tests.

Two requirements exist because they are where these systems usually fail: seeking must not replay
from zero, and **skipping must apply what it skips** — a skipped cutscene that leaves its door locked
is a defect that ships regularly.

## Requirements

### Requirement: Sequences orchestrate, they do not own
A sequence SHALL coordinate subsystems by producing **commands for them**, and SHALL NOT implement or
own camera evaluation, animation playback, audio playback, effect simulation, environment state, or
gameplay state.

Camera rigs SHALL be evaluated by `camera-system`, poses by `animation-and-skinning`, playback by
`audio`, effects by `vfx-system`, environmental state by `weather-and-wind`, and gameplay by
`gameplay-framework`. A sequence selects, parameterises, and schedules; it does not replace.

There SHALL NOT be a cinematic-only implementation of any subsystem the engine already provides.

#### Scenario: A cinematic camera is a camera
- **WHEN** a sequence controls the camera
- **THEN** it SHALL select rigs, lenses, and blends through the camera stack, and the camera system
  SHALL evaluate them

#### Scenario: No parallel weather
- **WHEN** a sequence brings in a storm
- **THEN** it SHALL drive the weather system's state, and no cinematic-only weather path SHALL exist

### Requirement: Compiled programs
A sequence SHALL be authored as tracks, sections, and channels, and **compiled** into a program with:
sorted evaluation segments, binding tables, resolved property accessors, compact channel data, event
tables, a preload plan, dependency metadata, and a debug map back to authored identities.

Runtime SHALL evaluate the program. Editor timeline object graphs SHALL NOT be traversed at runtime.

Property access SHALL use **resolved bindings**; string paths SHALL NOT be looked up during
evaluation.

Many playing instances SHALL share one immutable program, with per-instance state limited to time,
playback state, and active section bookkeeping.

#### Scenario: A hundred instances, one program
- **WHEN** the same sequence plays a hundred times concurrently
- **THEN** one program SHALL exist and each instance SHALL hold only small mutable state

#### Scenario: No string lookup per frame
- **WHEN** a track animates a property
- **THEN** its target SHALL have been resolved at compile time

### Requirement: Cost scales with what is active
The compiler SHALL build **interval and event indexes**, so that evaluation and seeking locate active
sections and crossed events without scanning tracks.

Evaluation cost SHALL scale with **active sections, active channels, and events crossed in the
current interval**, not with the number of authored keys.

Cooking SHALL be able to compress channel data — redundant key removal, quantisation within a
declared tolerance, constant-section folding, and rotation-specific encoding — with the achieved
error reported.

#### Scenario: A million keys, a few evaluations
- **WHEN** a long cinematic with a million authored keys plays
- **THEN** per-frame work SHALL be proportional to currently active channels

#### Scenario: Seeking does not scan
- **WHEN** a seek lands at an arbitrary time
- **THEN** active sections and crossed events SHALL be found through the index

### Requirement: Exact time
Sequence time SHALL be represented **exactly** — a frame and subframe at a declared rational rate —
and SHALL NOT be an accumulated floating-point value.

The same time reached by playing, seeking, stepping, or replaying SHALL be the same instant, and
evaluation at that instant SHALL produce the same result.

Frame stepping and range looping SHALL be exact, and repeated loops SHALL NOT drift.

#### Scenario: No drift over a long sequence
- **WHEN** a sequence loops for hours
- **THEN** its time SHALL remain exact rather than accumulating error

#### Scenario: Arriving by any route
- **WHEN** a time is reached by playing and by seeking
- **THEN** the evaluated result SHALL be identical

### Requirement: Clock domains
A sequence SHALL declare a **clock domain**: presentation, simulation, cinematic, real time, or
external.

| Domain | Use | Authoritative gameplay |
|---|---|---|
| `Presentation` | Most cutscenes and cosmetic sequences | Not permitted |
| `Simulation` | Timelines whose events affect authoritative state | Permitted |
| `Cinematic` | Controlled time for capture and offline rendering | Not permitted |
| `RealTime` | Interface and non-gameplay flows | Not permitted |
| `External` | Editor scrubber, network time source, host application | Not permitted |

**Sequence time is not the simulation tick** except in the simulation domain, where the two are
related by a declared mapping. A twenty-four frame-per-second cinematic over a sixty-hertz simulation
SHALL be correct in both.

Only the simulation domain SHALL be eligible to carry authoritative gameplay tracks, and this SHALL
be validated at compile time.

#### Scenario: Three rates coexist
- **WHEN** a cinematic plays over a running simulation at a high display rate
- **THEN** each SHALL advance on its own clock without corrupting the others

#### Scenario: An impossible combination is rejected
- **WHEN** a presentation-domain sequence contains an authoritative gameplay track
- **THEN** compilation SHALL fail naming the track

### Requirement: Bindings
A sequence SHALL address its targets through **bindings** identified by stable identifiers, resolved
at play time to entities, participants, cameras, interface elements, services, world layers, audio
buses, or project-defined targets.

Bindings SHALL NEVER be raw pointers or transient runtime indices, and SHALL use the persistent
identity mechanisms already defined so that a binding survives streaming and reload.

Bindings SHALL declare whether they are **required, optional, or have a fallback**, and MAY declare
**constraints** — that a target must have particular components or capabilities — validated at
author time where possible and at play time otherwise.

A sequence with unresolved required bindings SHALL fail to start with a structured error naming
them, rather than playing partially.

#### Scenario: One sequence, many subjects
- **WHEN** a reveal sequence is authored against a subject and a camera binding
- **THEN** it SHALL be playable for any valid subject without duplication

#### Scenario: A missing requirement is explicit
- **WHEN** a required binding cannot be resolved
- **THEN** the sequence SHALL fail to start and name the binding

### Requirement: Tracks, sections, and channels
A sequence SHALL be organised as **tracks** containing **sections** over time ranges, containing
**channels** of typed keyed data.

Channels SHALL be **typed** — scalar, vector, rotation, colour, boolean, enumeration, transform —
with interpolation appropriate to the type, and SHALL NOT be a generic dynamic value curve.

Sections SHALL declare: their range, priority, weight, blending, pre-roll and post-roll, loop
behaviour, and completion policy.

Track kinds SHALL include at minimum: property, transform, animation, camera and camera cut, audio,
effects, material, light, environment, gameplay event, gameplay command, interface, world layer, time
scale, nested sequence, and marker.

#### Scenario: Rotation interpolates as rotation
- **WHEN** a rotation channel is evaluated between keys
- **THEN** it SHALL interpolate as an orientation rather than component-wise

#### Scenario: Pre-roll prepares
- **WHEN** a section declares pre-roll
- **THEN** its subsystem SHALL be prepared before the section becomes visible

### Requirement: Track authority classification
Every track SHALL declare an **authority class**: presentation only, local gameplay, authoritative
gameplay, or deterministic simulation.

The compiler SHALL validate that a track's class is permitted by the sequence's clock domain,
determinism profile, and network policy, and SHALL fail with a diagnostic naming the track when it is
not.

**Authoritative gameplay changes SHALL be expressed as commands and events**, not as property tracks
writing authoritative component data.

#### Scenario: A cinematic cannot quietly change the world
- **WHEN** a presentation-only sequence attempts to modify authoritative state
- **THEN** compilation SHALL fail

#### Scenario: Gameplay goes through gameplay
- **WHEN** a sequence enables a spawner
- **THEN** it SHALL emit a gameplay command, validated like any other

### Requirement: Sequences are a command producer
A sequence SHALL be a producer of the **gameplay command stream** defined in `gameplay-framework`,
alongside human input, artificial intelligence, network peers, replay, and automation.

Commands emitted by a sequence SHALL be indistinguishable to the simulation from commands emitted by
any other producer, and SHALL be validated identically.

Sequence-emitted commands SHALL carry the sequence instance and track identity for diagnostics, and
that provenance SHALL NOT affect validation or execution.

#### Scenario: The simulation cannot tell
- **WHEN** a sequence issues a command
- **THEN** it SHALL be validated and executed exactly as a player's or an agent's would be

#### Scenario: Provenance is for diagnosis only
- **WHEN** a sequence-emitted command is rejected
- **THEN** the rejection SHALL name the sequence and track, and the rejection rules SHALL be the
  ordinary ones

### Requirement: Batched subsystem dispatch
Evaluation SHALL fill **per-subsystem command buffers** — camera, animation, audio, effects,
material, light, environment, interface, gameplay — which subsystems consume at their defined points.

Tracks SHALL NOT invoke subsystems directly during traversal, and SHALL NOT mutate arbitrary engine
state mid-evaluation.

Evaluation SHALL proceed in defined phases — prepare, evaluate, resolve, dispatch — so that
parallelism and ordering are well defined.

Independent tracks and sections SHALL be evaluable in parallel through the task system, with results
merged in a **stable order** derived from sequence instance, track, section, and event index — never
from worker identity.

#### Scenario: Subsystems receive batches
- **WHEN** a sequence with many tracks is evaluated
- **THEN** each subsystem SHALL receive one batch rather than many individual calls

#### Scenario: Parallel evaluation is deterministic
- **WHEN** tracks are evaluated across workers
- **THEN** the resulting commands SHALL be ordered by stable identity

### Requirement: Arbitration between sequences
Where more than one sequence, or a sequence and another system, drives the same target, resolution
SHALL be by **declared arbitration** — priority, blend group, exclusive group, and weight — not by
last writer.

Subsystems SHALL receive a **resolved result**, and the contributions that produced it SHALL be
inspectable.

Exclusive groups SHALL be supported, so that starting one sequence in a group predictably stops or
suspends another.

#### Scenario: A boss cinematic outranks the gameplay camera
- **WHEN** a high-priority sequence takes the camera
- **THEN** the camera system SHALL receive one resolved result, and the gameplay camera's
  contribution SHALL be blended or suspended as declared

#### Scenario: Contributions are attributable
- **WHEN** a value is driven by two sequences
- **THEN** the debugger SHALL show each contribution and its weight

### Requirement: State capture and restoration
Sections SHALL declare a **completion policy**: restore the prior value, hold the final value, keep
the change permanently, or a declared custom behaviour.

Where restoration is required, the adapter for that property SHALL provide **capture and restore**,
and only the properties the sequence touches SHALL be captured. An arbitrary object snapshot SHALL
NOT be taken.

A sequence stopped early, cancelled, or interrupted SHALL restore according to the same policies, so
that an abnormal ending does not leave the world altered in ways nobody declared.

#### Scenario: A light returns to its value
- **WHEN** a sequence raises a light's intensity for a shot and ends
- **THEN** the previous value SHALL be restored if the section declares restoration

#### Scenario: Interruption is not a special case
- **WHEN** a sequence is stopped mid-playback
- **THEN** completion policies SHALL apply as they would at natural completion

### Requirement: Nested sequences and parameters
A sequence SHALL be able to contain **nested sequences**, with declared time transforms — offset,
scale, trim, and loop — and the compiler MAY flatten them while preserving debug provenance.

Sequences SHALL declare **typed parameters** with defaults, overridable at play time, and a parent
SHALL be able to map its parameters onto a child's.

Parameters SHALL be resolved to identifiers at compile time; runtime parameter application SHALL NOT
require string lookup.

Nesting SHALL be acyclic, and a cycle SHALL be rejected at author time.

#### Scenario: A reusable shot
- **WHEN** an explosion sequence takes a target and an intensity
- **THEN** it SHALL be reusable across contexts by parameterisation rather than duplication

#### Scenario: Provenance survives flattening
- **WHEN** nested sequences are flattened at compile time
- **THEN** the debugger SHALL still attribute values to the nested sequence and track that authored
  them

### Requirement: Events and markers
Sequence events SHALL be **typed records** with a time and a typed payload. Serialised function
pointers or arbitrary callbacks SHALL NOT be the mechanism.

**Markers** SHALL be editorial and synchronisation references — named points usable for jumping,
synchronisation, debugging, and waiting — and SHALL be distinct from gameplay events and from
animation markers, which are owned by their own systems.

Waiting on a marker SHALL be available to gameplay graphs and gameplay code.

Events SHALL declare a **side-effect policy** — reversible, idempotent, speculative, or confirmed
only — so that seeking, reversing, and rollback can treat them correctly.

#### Scenario: Three kinds of marker are distinct
- **WHEN** an animation marker, a sequence marker, and a gameplay event coincide
- **THEN** each SHALL remain its own concept with its own owner

#### Scenario: An irreversible event is declared
- **WHEN** an event grants a permanent unlock
- **THEN** it SHALL be declared confirmed-only and SHALL NOT fire during preview scrubbing

### Requirement: Seeking and scrubbing
Seeking SHALL locate the target time through the index, restore or reconstruct subsystem state, and
resume — **without replaying the sequence from its start**.

Seeking SHALL declare a **mode**: preview, runtime, replay, or reconstruct, determining whether
crossed events fire, are suppressed, or are applied as state.

Each track adapter SHALL declare its **seek capability**: evaluate directly at a time, reconstruct
state, simulate forward with a pre-roll, or restart. Capabilities differ legitimately — a pose can be
evaluated at a time, a particle system may need to simulate, an audio stream may need to seek or
restart.

**Where an adapter claims direct seekability, playing to a time and seeking to that time SHALL
produce equivalent state**, and this SHALL be tested.

#### Scenario: Scrubbing does not fire mission events
- **WHEN** an author scrubs a timeline in preview mode
- **THEN** irreversible gameplay events SHALL be suppressed

#### Scenario: Play and seek agree
- **WHEN** a seekable track is played to a time and separately seeked to it
- **THEN** the resulting state SHALL be equivalent

### Requirement: Skipping applies what it skips
A sequence SHALL declare a **skip policy**: not skippable, presentation only, apply required
outcomes, or a declared custom behaviour.

**Skipping a sequence that carries gameplay consequence SHALL apply its required authoritative
outcomes** — its declared events, commands, and final state — before advancing presentation to the
end. Stopping playback SHALL NOT be the implementation of skipping.

A sequence carrying authoritative gameplay tracks that does not declare its required outcomes SHALL
NOT be marked skippable, and this SHALL be validated at compile time.

Fast-forwarding SHALL be distinct from skipping, and adapters MAY use simplified evaluation while it
is active.

#### Scenario: The door still opens
- **WHEN** a player skips a cutscene that would have unlocked a door
- **THEN** the required outcome SHALL be applied and the door SHALL be unlocked

#### Scenario: Undeclared consequence is caught
- **WHEN** a sequence with authoritative tracks is marked skippable without declaring outcomes
- **THEN** compilation SHALL fail

### Requirement: Preload plans
The compiler SHALL derive a **preload plan** from the sequence: for each asset, the time it is
required, the time it may be released, and its priority.

At play time the plan SHALL be published so that assets are requested **ahead of need**, with
deadlines propagated through the residency layer.

A sequence SHALL support being **prepared** before it is played, so that a critical cinematic can be
guaranteed to start without a hitch, and a project SHALL be able to choose between failing if not
ready, starting with fallbacks, or waiting for required content.

Preload misses SHALL be reported, naming the asset, the deadline, and what was substituted.

#### Scenario: A shot is warm before it starts
- **WHEN** a cinematic is played
- **THEN** its early shots' assets SHALL have been requested in advance from the plan

#### Scenario: A guarantee is available
- **WHEN** a sequence must not hitch
- **THEN** it SHALL be preparable and playable only once ready

### Requirement: Streaming source
A playing sequence SHALL publish a **streaming source** carrying the world regions its upcoming shots
will require, with the times at which they are needed and their importance.

This SHALL use the same streaming source mechanism as cameras, so that a sequence's future is fed
into world, geometry, texture, shadow, and illumination prefetching without a separate path.

A sequence SHALL be able to publish **future camera bounds** derived from its camera tracks, which is
information no reactive or extrapolative predictor can obtain.

Anticipated **camera cuts** SHALL be announced in advance so that temporal history, illumination,
shadow caches, and residency prepare rather than react.

#### Scenario: The world loads before the camera arrives
- **WHEN** a shot will cut to a distant region in six seconds
- **THEN** that region SHALL be requested against that deadline

#### Scenario: A cut is not a surprise
- **WHEN** a cut is approaching
- **THEN** it SHALL be announced so dependent systems prepare

### Requirement: Playback control
A sequence SHALL be playable through a **service scoped to a world, session, local player, or editor
preview** — not a global singleton — supporting: play, pause, resume, stop, seek, set play rate, set
parameters, and jump to a marker.

Playback modes SHALL include once, loop, ping-pong, hold, and manual.

Instance lifecycle SHALL be explicit: created, preparing, ready, playing, paused, seeking,
completing, completed, stopped, and failed — with failure carrying a structured reason.

Reverse playback SHALL be supported where semantics permit: channels evaluate in reverse, and events
follow their declared reverse policy. Subsystems SHALL NOT be run backwards.

#### Scenario: Local-player scoping
- **WHEN** a camera flourish plays for one local player in split screen
- **THEN** it SHALL affect that player only

#### Scenario: Failure is diagnosable
- **WHEN** a sequence fails to start
- **THEN** the reason SHALL be structured — unresolved binding, unready asset, incompatible clock, or
  unsupported track

### Requirement: Time scaling and pausing
A time scale track SHALL declare **which domain it scales** — presentation, simulation, animation,
audio, or a declared target — and SHALL NOT implicitly scale every clock.

Pausing SHALL be defined per concern: pausing a sequence, pausing simulation, and pausing
presentation are distinct, and an interface sequence SHALL be able to continue while gameplay is
paused.

Scaling the simulation clock from a sequence SHALL be permitted only in the simulation domain and
SHALL be subject to the determinism profile.

#### Scenario: Slow motion is scoped
- **WHEN** a sequence slows the action
- **THEN** it SHALL scale the declared domain, and the interface SHALL remain at full speed unless
  also declared

#### Scenario: Menus over a paused game
- **WHEN** gameplay is paused
- **THEN** an interface sequence SHALL be able to continue

### Requirement: Spawned content
A sequence MAY create temporary content — effects, lights, audio emitters, or entities from templates
— and SHALL declare its **lifetime**: for the section, for the sequence, persistent, or manually
managed.

Spawned content SHALL use the engine's existing mechanisms — entity templates, effect instances,
audio voices — and SHALL NOT be a sequence-private object hierarchy.

**Persistent lifetime SHALL transfer ownership explicitly** to a system that will manage it.
Ownership SHALL never be left ambiguous when a sequence ends.

Content spawned by a sequence that is stopped, skipped, or fails SHALL be cleaned up according to its
declared lifetime.

#### Scenario: Nothing is orphaned
- **WHEN** a sequence is stopped abruptly
- **THEN** content it spawned SHALL be released according to its declared lifetime

#### Scenario: A handover is explicit
- **WHEN** a sequence spawns something intended to outlive it
- **THEN** ownership SHALL be transferred explicitly to an owning system

### Requirement: Network policies
A sequence SHALL declare a **network policy**: local only, server triggered, synchronised, or
deterministic.

Under **server triggered**, the authority SHALL replicate the sequence identity, start time,
bindings, and parameters, and clients SHALL evaluate the program locally. Interpolated track values
SHALL NOT be replicated.

Under **synchronised**, clients SHALL additionally receive periodic clock corrections, applied
smoothly rather than as jumps.

Under **deterministic**, the sequence SHALL use the simulation clock and its authoritative events
SHALL be reproducible.

**Late join SHALL seek**: a peer joining mid-sequence receives identity, current time, bindings,
parameters, and persistent section state, and seeks — it SHALL NOT replay the timeline in real time.

#### Scenario: A cinematic costs little bandwidth
- **WHEN** a cutscene plays for eight players
- **THEN** its identity, start time, bindings, and parameters SHALL be replicated, and each client
  SHALL evaluate locally

#### Scenario: A late player catches up instantly
- **WHEN** a player joins four minutes into a sequence
- **THEN** they SHALL seek to the current time rather than replaying from the start

### Requirement: Replay and rollback
Replays SHALL reconstruct sequences from recorded **semantic operations** — started, stopped,
parameter changed, branch selected — together with gameplay events, rather than from recorded track
values.

A sequence started speculatively on predicted state and invalidated by a rollback SHALL reconcile
through the **side-effect ledger** defined in `replay-and-rollback`, so that its audio, effects,
camera shakes, and interface notifications are not realised twice.

Sequence-emitted events SHALL carry their simulation point so the ledger can identify them.

Presentation-only sequences MAY be reconstructed from the gameplay events that triggered them rather
than being recorded separately.

#### Scenario: A rolled-back cinematic does not double
- **WHEN** a predicted activation started a sequence and is rolled back
- **THEN** its realised effects SHALL be suppressed on re-simulation rather than repeated

#### Scenario: Replays record intent
- **WHEN** a session is replayed
- **THEN** sequences SHALL be reconstructed from recorded operations, not from recorded animated
  values

### Requirement: Persistence
A sequence SHALL declare its **persistence class**: none, session, or save game.

Where persisted, a save SHALL record the sequence identity, its program version, current time,
bindings by stable identity, parameters, and the section and event state required to resume — never
runtime pointers or transient indices.

A save or replay referring to a changed sequence SHALL follow a declared compatibility policy —
exact, migratable, best effort, or restart — and an incompatible reference SHALL be reported rather
than misinterpreted.

#### Scenario: A long mission sequence survives a save
- **WHEN** a save occurs during a persisted sequence
- **THEN** it SHALL resume at the same time with the same bindings

#### Scenario: A changed sequence is handled by policy
- **WHEN** a save refers to a sequence whose structure changed
- **THEN** the declared compatibility policy SHALL apply and the outcome SHALL be reported

### Requirement: Stable identity, source form, and merging
Sequences, tracks, sections, channels, keys, markers, bindings, and parameters SHALL carry **stable
identifiers** preserved across benign edits, using the engine's identity mechanism.

Source form SHALL be **deterministic text** suitable for version control, with visual and editorial
state — track ordering for display, colours, folding, zoom — stored separately from semantics.

The engine SHALL provide **semantic diff**: tracks and sections added or removed, sections moved, key
values changed, bindings changed — and **three-way merge** over those identities, with conflicts
reported rather than resolved arbitrarily.

Opaque binary source SHALL NOT be the only representation.

#### Scenario: A meaningful diff
- **WHEN** a section is moved and a key value changed
- **THEN** the diff SHALL say so rather than showing a binary difference

#### Scenario: Reordering tracks is not a conflict
- **WHEN** two authors reorder tracks differently for display
- **THEN** it SHALL not be a semantic conflict

### Requirement: Hot reload
Editing a sequence during play SHALL recompile it and publish a new program generation, with running
instances handled by a declared policy: continue at the equivalent time, restart, keep the previous
program, or stop.

Where a structural change makes continuation impossible, the editor SHALL explain why rather than
silently restarting.

Hot reload SHALL preserve binding and parameter state where the change permits.

#### Scenario: Iterating on a cinematic
- **WHEN** a sequence is edited while playing
- **THEN** it SHALL recompile and continue at the equivalent time where the change permits

### Requirement: Track extension
Track types SHALL be extensible by modules and plugins through the extension points in
`project-and-plugins`, registering a source schema, a compiler contribution, a runtime adapter, an
editor factory, and migrations.

A custom track SHALL **declare its properties**: thread safety, determinism, seekability,
reversibility, network safety, and whether it is editor-only. The compiler SHALL validate its use
against the sequence's domain, determinism profile, and network policy.

Adapter interfaces SHALL be **coarse-grained and batched**; there SHALL NOT be a virtual call per key
or per channel sample.

#### Scenario: A plugin adds a track
- **WHEN** a plugin registers a track type
- **THEN** it SHALL compile, evaluate, seek, and debug through the same infrastructure

#### Scenario: An unsuitable track is rejected
- **WHEN** a non-deterministic track is used in a deterministic sequence
- **THEN** compilation SHALL fail naming the track and the property that disqualified it

### Requirement: Accessibility metadata
A sequence SHALL be able to declare **accessibility metadata**: camera motion intensity, rapid
luminance changes, shake magnitude, whether subtitles are required, and whether it is skippable.

Accessibility settings SHALL attenuate **presentation** — reducing camera motion, shake, and flashing
— without altering the sequence's authoritative outcomes or its timing.

Content declaring rapid luminance changes SHALL be attenuable by a player setting, and the
attenuation SHALL be reportable.

#### Scenario: Reduced motion still tells the story
- **WHEN** a player enables reduced motion
- **THEN** camera motion and shake SHALL be attenuated while events, timing, and outcomes are
  unchanged

### Requirement: Sequence diagnostics
The engine SHALL provide inspection showing: active sequences with their clock domain, current time,
playback state, active sections, resolved bindings, parameters, pending preloads, arbitration state,
and network synchronisation state.

For any value a sequence drives, development builds SHALL answer **why it has that value** — the
sequence instance, track, section, channel, and the blend contributions that produced it.

Sequences SHALL emit into the shared trace: start, stop, seek, section activation, event emission,
preload miss, binding failure, and clock correction — so that timeline cost and stalls appear on the
same timeline as everything else.

#### Scenario: Why is this value what it is
- **WHEN** a light is unexpectedly bright
- **THEN** the tooling SHALL name the sequence, track, and section driving it, and the blend weights

#### Scenario: A stall is attributable
- **WHEN** a cinematic hitches
- **THEN** the trace SHALL show whether the cause was a preload miss, a binding failure, or
  evaluation cost

### Requirement: Sequence performance and testing
Sequence evaluation SHALL allocate nothing per frame on the normal path, and its cost SHALL be
reported per sequence and per track.

The engine SHALL maintain a **sequencing benchmark**: many concurrent instances across thousands of
active tracks and tens of thousands of channels, drawn from assets containing on the order of a
million authored keys — where per-frame cost reflects active ranges only and remains a small fraction
of frame time excluding the subsystem work it commands.

Testing SHALL include: **evaluation tests** asserting values at given times headlessly with no
renderer; **seek equivalence tests** for every adapter claiming direct seekability; **determinism
tests** across worker counts and chaos scheduling for authoritative output; **network tests** covering
late join and clock correction; and **preload tests** under artificially slow input and output that
verify misses are reported rather than hidden.

#### Scenario: Authored size does not set runtime cost
- **WHEN** the benchmark runs
- **THEN** per-frame cost SHALL reflect active sections and channels rather than authored key count

#### Scenario: Sequences are testable headlessly
- **WHEN** a sequence's values at given times are asserted
- **THEN** the test SHALL run with no renderer, audio, or interface

### Requirement: Forbidden sequencing patterns
The following SHALL NOT appear, and each SHALL be checkable:

- A sequence owning camera, animation, audio, effect, environment, or gameplay implementation
- Runtime traversal of editor timeline object graphs
- String-based property resolution in an evaluation path
- Persistent bindings held as raw pointers or transient indices
- Authoritative gameplay changes written by property tracks rather than through commands
- Timing derived from accumulated render-frame deltas where determinism or networking is required
- Replicating interpolated track values that clients could evaluate locally
- Seeking implemented by replaying from time zero
- Skipping a gameplay-relevant sequence without applying its required outcomes
- Sequence logic expanding into a general replacement for gameplay graphs

#### Scenario: A proposal is checked
- **WHEN** a change would add general branching and looping to the timeline language
- **THEN** it SHALL be flagged against this requirement
