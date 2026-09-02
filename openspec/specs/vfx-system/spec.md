# vfx-system Specification

## Purpose

Defines the visual effects framework: a GPU-first, compiler-driven system for particles and
effects, targeting millions of particles and RTS-scale scenes.

Four choices are foundational and are the reason this is a capability rather than a renderer
feature. GPU simulation is the **default**, not an advanced mode. Effect graphs are **compiled**
through a typed IR into engine shader source, never interpreted. All effects share **one
simulation world** with a global scheduler that merges compatible work into few dispatches rather
than one per effect. And cost is bounded by a **frame-time budget** with importance classes,
rather than by a quality preset.

The system is engine-owned. Unlike physics, text, and audio — where mature libraries make building
your own hard to justify — VFX has no permissive equivalent, and the leading implementations are
engine-internal because the system must be co-designed with the renderer's GPU scene, the shader
pipeline, and the frame budget.

One boundary is stated as a hard requirement: VFX is non-deterministic and **must not write
gameplay state**, so it can never break physics determinism, network reconciliation, or replay.

## Requirements

### Requirement: Engine-owned VFX runtime
The VFX system SHALL be engine code: the asset model, the graph compiler, particle storage,
simulation scheduling, event routing, scalability policy, and the renderers.

No third-party VFX runtime SHALL be integrated. Algorithms with published references (noise
functions, sorting networks, curl fields) MAY be implemented from those references in engine
shader code; that is not a dependency.

The system SHALL be removable at build time via `CY_VFX` without affecting the rest of the
renderer.

#### Scenario: No VFX dependency
- **WHEN** the dependency manifest is audited
- **THEN** it SHALL contain no VFX runtime library, and disabling `CY_VFX` SHALL remove the
  subsystem without disturbing other rendering features

#### Scenario: Co-design is possible
- **WHEN** the renderer changes its instance representation or frame schedule
- **THEN** VFX SHALL be adaptable in the same change, because both are engine-owned

### Requirement: VFX asset model
A **VFX system** asset SHALL consist of: one or more **emitters**, a set of typed **parameters**
exposed to gameplay and to the editor, declared **event channels**, an **importance class**, and a
**scalability policy**.

An emitter SHALL declare stages, each a graph:

| Stage | Runs | Purpose |
|---|---|---|
| `Spawn` | Per spawn request | Determine how many particles to create |
| `Initialise` | Per newly created particle | Set initial attribute values |
| `Update` | Per live particle per simulation step | Advance the simulation |
| `Event` | Per received event | React to events from this or another emitter |
| `Render` | Per particle at draw time | Produce renderer inputs |
| `Compute` | Per emitter per step | Custom whole-emitter work (reductions, grids) |

Emitters within a system SHALL be able to reference each other's attributes and events, so a
system is a coherent effect rather than a bag of independent emitters.

#### Scenario: Multi-emitter effect
- **WHEN** an explosion system contains fireball, debris, smoke, and spark emitters
- **THEN** they SHALL share the system's parameters and lifetime, and be able to spawn from each
  other's events

#### Scenario: Parameter exposed to gameplay
- **WHEN** a system declares an `Intensity` float parameter
- **THEN** it SHALL be settable per effect instance from gameplay and from the editor, and
  readable by every stage graph in the system

### Requirement: GPU-first simulation
GPU compute simulation SHALL be the **default** execution path for all emitters, designed to
support particle counts in the millions across the simulation world.

Particle state SHALL live in GPU buffers and remain there: the CPU SHALL NOT be required to read
or write per-particle data during normal operation.

Simulation SHALL use indirect dispatch driven by live particle counts maintained on the GPU, so
dispatch size tracks actual population without CPU knowledge of it.

#### Scenario: Millions of particles
- **WHEN** the simulation world contains one million live particles across many effects
- **THEN** simulation SHALL proceed without per-particle CPU work and without CPU-side
  per-particle allocation

#### Scenario: Population changes without CPU involvement
- **WHEN** particles are spawned by GPU events and killed by lifetime expiry within one frame
- **THEN** dispatch sizes SHALL follow via indirect arguments, with no CPU readback of counts

### Requirement: CPU simulation path
A CPU simulation path SHALL exist for two explicitly distinct purposes:

1. **Capability fallback** — devices lacking adequate compute support, with documented lower
   particle budgets and a documented subset of supported features
2. **CPU-visible effects** — effects whose per-particle results gameplay must read synchronously

An effect SHALL declare which path it requires. The CPU path SHALL NOT be presented as merely a
degraded GPU path, because its use cases differ.

#### Scenario: Device lacks compute
- **WHEN** the device cannot run the GPU path
- **THEN** effects SHALL fall back to CPU simulation with reduced budgets, and features the CPU
  path does not support SHALL be reported at cook time rather than failing at runtime

#### Scenario: Gameplay needs per-particle results
- **WHEN** an effect's particle positions drive gameplay logic synchronously
- **THEN** it SHALL declare the CPU path, and the documentation SHALL state that this bounds its
  particle count

### Requirement: Graph compilation to an intermediate representation
Effect graphs SHALL be **compiled**, never interpreted at runtime.

Compilation SHALL proceed: graph → typed **VFX IR** → optimisation → Slang source → the engine's
existing shader compilation pipeline (see `shader-system`).

The IR SHALL be typed and SSA-formed, and the optimiser SHALL perform at minimum: **attribute
liveness analysis**, **dead-code elimination**, **constant folding** of parameters known at cook
time, and **kernel fusion** of stages that can share a dispatch.

The compiler SHALL emit **Slang**, not backend-specific source, so VFX kernels reuse the engine's
existing reflection, caching, hot-reload, and cross-backend translation rather than introducing a
second shader toolchain.

#### Scenario: Graph is compiled at cook time
- **WHEN** a VFX asset is cooked
- **THEN** its kernels SHALL be compiled to shader artefacts, and the runtime SHALL contain no
  graph interpreter and no graph-to-source compiler

#### Scenario: Constant folding
- **WHEN** an artist sets a module's parameter to a constant and does not expose it
- **THEN** the value SHALL be folded into the generated code rather than read from a buffer

#### Scenario: Kernel fusion
- **WHEN** an emitter's initialise and update stages can share a dispatch
- **THEN** the compiler SHALL fuse them, reducing dispatch count and intermediate traffic

#### Scenario: Compile error is actionable
- **WHEN** a graph fails to compile
- **THEN** the error SHALL identify the offending node and pin, not only the generated source line

### Requirement: Compiler-derived attribute layout
Particle storage SHALL be **structure-of-arrays**, and the set of attribute arrays SHALL be
**derived by the compiler** from the attributes the graphs actually read or write.

Attributes never referenced by any stage of an emitter SHALL NOT be allocated.

The compiler SHALL additionally select each attribute's storage precision from declared ranges and
usage where a reduced precision is provably sufficient, with an explicit authoring override.

#### Scenario: Unused attributes cost nothing
- **WHEN** a graph uses position, velocity, age, lifetime, and colour but never rotation or mass
- **THEN** only the used attributes SHALL be allocated, and simulation SHALL touch no other
  per-particle memory

#### Scenario: Layout is reported
- **WHEN** an effect is cooked
- **THEN** its derived attribute layout, per-particle byte size, and resulting maximum population
  for a given memory budget SHALL be reported to the author

#### Scenario: Layout change invalidates state
- **WHEN** a graph edit changes the derived layout during hot reload
- **THEN** affected effect instances SHALL be restarted rather than having their particle state
  migrated, and this SHALL be stated in the authoring documentation

### Requirement: Unified simulation world and global scheduler
All active emitters SHALL be simulated within a single **VFX simulation world** backed by shared
particle memory, rather than each effect instance owning isolated buffers and dispatches.

A **global scheduler** SHALL, each simulation step: collect active emitters, group those sharing a
compiled kernel and compatible bindings, and issue **merged indirect dispatches** over the grouped
population.

Allocation from the shared pool SHALL be subject to per-importance-class reservations, so a
high-volume decorative effect cannot starve a critical one.

#### Scenario: Many instances, few dispatches
- **WHEN** 400 instances of the same explosion effect are active
- **THEN** they SHALL be simulated by a small number of merged dispatches, not 400 separate ones

#### Scenario: Reservation protects critical effects
- **WHEN** decorative effects request more particles than remain
- **THEN** the reservation for `Critical` effects SHALL remain available, and the decorative
  request SHALL be reduced

#### Scenario: Pool exhaustion is graceful
- **WHEN** the shared pool is exhausted
- **THEN** spawn requests SHALL be reduced by importance rank and the shortfall reported, rather
  than overwriting live particles or failing the frame

### Requirement: Data interfaces
Graphs SHALL access engine data through typed **data interfaces**: declared, versioned sources
exposing readable fields and the GPU resources they bind.

The engine SHALL provide at least: scene depth, scene normals, the scene signed distance field,
the GPU scene, physics collision queries, terrain height and material, static and skeletal mesh
sampling, texture and curve sampling, camera state, wind and force fields, audio spectrum and
level, ECS query results, and a generic structured buffer.

Data interfaces SHALL be **extensible**: modules and projects SHALL be able to register their own
without modifying the compiler.

Each data interface SHALL declare its cost class and whether it is available on the CPU path, so
the compiler can reject or warn about use in effects that require CPU simulation.

#### Scenario: Sampling the scene SDF
- **WHEN** a graph samples the scene signed distance field to detect collision
- **THEN** the compiler SHALL bind the SDF resources and generate the sampling code, with no
  bespoke compiler support for that specific case

#### Scenario: Custom data interface
- **WHEN** a project registers a data interface exposing its own simulation grid
- **THEN** graphs SHALL be able to read it as a first-class typed source with no engine change

#### Scenario: Unavailable on the CPU path
- **WHEN** an effect declares CPU simulation and uses a GPU-only data interface
- **THEN** cooking SHALL fail with a diagnostic naming the interface

### Requirement: GPU scene integration for mesh particles
Mesh particles SHALL publish instances directly into the renderer's **GPU scene** — the shared
GPU-side instance representation — producing per particle at minimum: a transform, a mesh
reference, a material reference, and instance flags.

Published instances SHALL participate in GPU-driven culling, LOD selection, and indirect drawing
identically to instances from any other source.

Mesh particles SHALL NOT require ECS entities, per-particle CPU submission, or CPU readback.

#### Scenario: A million mesh particles
- **WHEN** an effect produces a million mesh particles
- **THEN** they SHALL be rendered through the GPU scene with no corresponding ECS entities and a
  small, bounded number of CPU draw submissions

#### Scenario: Culling applies uniformly
- **WHEN** mesh particles leave the view frustum
- **THEN** they SHALL be culled by the same GPU-driven culling pass as other instances

#### Scenario: Particle LOD
- **WHEN** a mesh particle's projected size falls below a LOD threshold
- **THEN** the GPU scene's LOD selection SHALL apply, without VFX-specific logic

### Requirement: GPU-to-GPU events
Emitters SHALL be able to raise **events** on the GPU — from collisions, lifetime expiry, spawn,
or explicit graph logic — that other emitters consume in their spawn or event stages, without any
CPU round trip.

Events SHALL be written into typed GPU event buffers carrying payload attributes declared by the
channel, and SHALL be consumable in the same frame where the dependency order permits, or the next
frame otherwise.

Every event channel SHALL declare a **maximum events per frame** and every chain a **maximum
depth**. Exceeding either SHALL drop events by a deterministic rank and report the overflow,
rather than compounding.

#### Scenario: Chained effects stay on the GPU
- **WHEN** a bullet impact spawns sparks, and a spark collision spawns dust
- **THEN** both spawns SHALL occur through GPU event buffers with no CPU involvement

#### Scenario: Feedback loop is bounded
- **WHEN** an event chain would spawn faster than its budget allows
- **THEN** events SHALL be dropped by rank at the channel's limit and the overflow reported, so
  the loop cannot diverge

#### Scenario: Chain depth limit
- **WHEN** an effect graph creates events whose chain exceeds the configured maximum depth
- **THEN** the chain SHALL terminate at that depth and the truncation SHALL be reported

### Requirement: Bounded CPU event and readback path
Where gameplay genuinely requires knowledge of simulation results, the system SHALL provide a
**bounded readback path**: declared event channels or attribute reductions copied back to the CPU.

Readback SHALL have a documented latency of at least one frame, SHALL be budgeted as a maximum
bytes-per-frame, and SHALL never stall the frame waiting on the GPU.

Readback SHALL be opt-in per channel. The documentation SHALL state that it is not the default
mechanism and that GPU-to-GPU events should be preferred.

#### Scenario: Gameplay reacts to an effect
- **WHEN** an effect declares a readback channel for collision events
- **THEN** those events SHALL be delivered to CPU systems with at least one frame of latency,
  clearly documented

#### Scenario: Readback never stalls
- **WHEN** readback data for the current frame is not yet available
- **THEN** the system SHALL proceed with the most recent available data rather than blocking

#### Scenario: Readback budget exceeded
- **WHEN** declared readback exceeds the per-frame budget
- **THEN** the excess SHALL be deferred and reported, not silently truncated

### Requirement: VFX does not influence gameplay state
The VFX system SHALL be **non-deterministic by design** and SHALL NOT write gameplay state.

VFX MAY read gameplay and world state. It SHALL NOT be a source of truth for: damage, hit
detection, entity creation or destruction, physics state, network-replicated values, or any value
consumed by deterministic simulation.

The readback path SHALL be documented as a mechanism for *presentation* reactions (audio cues,
non-authoritative decals) and SHALL NOT be used to drive authoritative gameplay.

Development builds SHALL detect and report attempts to write replicated or physics-owned
components from VFX-driven code paths.

#### Scenario: Determinism is preserved
- **WHEN** a simulation is re-simulated during network reconciliation or replayed from inputs
- **THEN** the result SHALL be identical regardless of what VFX did, because VFX contributed no
  gameplay state

#### Scenario: Attempted gameplay write is caught
- **WHEN** code driven by a VFX readback attempts to write a replicated component
- **THEN** a development-build diagnostic SHALL report it

#### Scenario: Presentation reaction is fine
- **WHEN** a VFX collision readback triggers an audio one-shot
- **THEN** this SHALL be permitted, because audio presentation is not gameplay state

### Requirement: Importance classes and frame-budget scalability
Every effect SHALL declare an **importance class**: `Critical`, `Important`, `Ambient`, or
`Decorative`.

A **budget controller** SHALL measure GPU time spent on VFX and adjust, to hold a configured
target: spawn rates, simulation frequency, particle count caps, collision quality, renderer
feature level (lighting, shadows, sorting), and effect LOD.

Adjustment SHALL proceed from least to most important: `Decorative` first, `Critical` last.
`Critical` effects SHALL have reserved capacity and SHALL be degraded only when no other headroom
remains.

Adjustments SHALL be applied smoothly and hysteretically so quality does not visibly oscillate.

A **pinned mode** SHALL disable adaptation for cinematics, capture, and deterministic testing.

#### Scenario: Battle exceeds the budget
- **WHEN** measured VFX GPU time is 3.4 ms against a 2.0 ms target
- **THEN** the controller SHALL reduce decorative and ambient cost first, and SHALL report which
  levers it applied and the resulting time

#### Scenario: Gameplay-legible effects survive
- **WHEN** the scene is heavily overloaded
- **THEN** `Critical` effects SHALL still render at their configured minimum quality

#### Scenario: Pinned mode for capture
- **WHEN** pinned mode is enabled
- **THEN** no adaptive adjustment SHALL occur, and exceeding the budget SHALL be reported rather
  than corrected

#### Scenario: No visible oscillation
- **WHEN** load hovers around the budget
- **THEN** hysteresis SHALL prevent quality from flickering between levels

### Requirement: Decoupled simulation frequency
Effects SHALL simulate at a frequency selected independently of the render frame rate, chosen from
importance, distance, and screen coverage.

Rendering SHALL interpolate particle attributes between simulation steps so a reduced rate is not
visible as stepping.

Frequency SHALL be adjustable by the budget controller and overridable per effect.

#### Scenario: Distant smoke is cheap
- **WHEN** a distant smoke effect is assigned an 8 Hz simulation rate while rendering runs at
  120 FPS
- **THEN** it SHALL be simulated at that rate and interpolated for display, with no visible
  stepping

#### Scenario: Rate change is smooth
- **WHEN** an effect's simulation rate changes as it approaches the camera
- **THEN** the transition SHALL not produce a visible discontinuity in motion

#### Scenario: Interpolation is bounded
- **WHEN** an effect's particles change discontinuously (spawn, kill, teleport)
- **THEN** interpolation SHALL be suppressed for those particles rather than smearing them

### Requirement: Async compute
Where the device exposes an asynchronous compute queue, VFX simulation SHALL be schedulable on it,
overlapping graphics work, with the render graph inserting the required synchronisation.

Async execution SHALL be capability-gated and SHALL be disableable, since overlap benefit is
device-dependent and it complicates profiling.

#### Scenario: Simulation overlaps rendering
- **WHEN** async compute is available and VFX simulation has no dependency on the current frame's
  graphics work
- **THEN** it SHALL be scheduled on the async queue, overlapping shadow or G-buffer passes

#### Scenario: Dependency forces ordering
- **WHEN** simulation reads the current frame's depth buffer
- **THEN** the graph SHALL order it after depth production, on whichever queue is correct

### Requirement: Renderers
The system SHALL provide these particle renderers, each consuming attributes produced by the
render stage:

| Renderer | Produces |
|---|---|
| `Sprite` | Camera-facing or axis-aligned quads, with flipbook and sub-UV animation |
| `Mesh` | Mesh instances published into the GPU scene |
| `Ribbon` | Connected strips through particle chains, with width and twist over length |
| `Beam` | Point-to-point strips with sag and noise |
| `Trail` | Per-particle history ribbons |
| `Decal` | Projected decals |
| `Light` | Light instances contributing to scene lighting |
| `Volume` | Volumetric primitives compositing into the volumetric pipeline |

Renderers SHALL support: material assignment, sorting mode, lighting participation (unlit, vertex,
per-pixel), shadow casting and receiving, motion vector output, and per-renderer LOD.

Deferred renderers whose seams SHALL be reserved but not implemented: hair and fibre, voxel, and
fluid surface.

#### Scenario: Particle lights
- **WHEN** a `Light` renderer emits lights from particles
- **THEN** they SHALL participate in clustered light assignment subject to a hard per-frame count
  budget, degraded by the budget controller like any other VFX cost

#### Scenario: Motion vectors
- **WHEN** temporal anti-aliasing or motion blur is enabled
- **THEN** particle renderers SHALL output motion vectors derived from the previous simulation
  step, so particles do not smear or ghost

#### Scenario: Ribbon continuity
- **WHEN** particles in a ribbon chain are killed mid-chain
- **THEN** the ribbon SHALL terminate cleanly rather than connecting across the gap

### Requirement: Transparency and sorting
Transparent particles SHALL support sorting modes: none, by distance to camera, by age, by a
custom key, and per-emitter draw order.

Distance sorting SHALL be performed on the GPU over the particle population, with the sort cost
budgeted and reducible by the budget controller.

The system SHALL support order-independent approximations where exact sorting is too expensive,
selectable per effect.

#### Scenario: Sorted additive particles
- **WHEN** an effect requires back-to-front ordering
- **THEN** a GPU sort SHALL order it before drawing, and the sort SHALL be a reportable cost

#### Scenario: Sorting is degraded under load
- **WHEN** the budget controller reduces quality
- **THEN** distant effects MAY drop to unsorted or approximate ordering, prioritised by importance

### Requirement: Collision
Particles SHALL support collision against: analytic primitives (plane, sphere, box, capsule), the
**scene signed distance field**, the **depth buffer** in screen space, and **physics queries** for
the CPU path.

Each mode SHALL declare its accuracy and cost, and its limitations SHALL be documented — notably
that depth-buffer collision cannot see off-screen or occluded geometry.

Collision response SHALL support: bounce with restitution, friction, sliding, kill, and raising a
GPU event.

Collision quality SHALL be adjustable by the budget controller.

#### Scenario: SDF collision against complex geometry
- **WHEN** particles collide using the scene SDF
- **THEN** they SHALL respond to arbitrary static geometry including geometry outside the view

#### Scenario: Depth-buffer limitation
- **WHEN** depth-buffer collision is used
- **THEN** particles SHALL pass through off-screen and occluded geometry, and this SHALL be
  documented rather than treated as a defect

#### Scenario: Collision raises an event
- **WHEN** a particle collides and its response is configured to raise an event
- **THEN** the event SHALL be written to the GPU event buffer for consumption by another emitter

### Requirement: Authoring
The editor SHALL provide a VFX graph editor supporting: node-graph authoring per stage, a library
of built-in modules, user-defined reusable modules, typed parameters with ranges and inspector
metadata, curve and gradient editors, live preview with playback controls (play, pause, step,
scrub, loop, restart), and per-node inspection of values on a sampled particle.

Editing a graph SHALL trigger recompilation and live update of running instances, subject to the
layout-change restart rule.

The editor SHALL surface compiler output: the derived attribute layout, per-particle size,
estimated cost, generated IR, and generated Slang source.

#### Scenario: Live iteration
- **WHEN** an artist edits a module parameter
- **THEN** the preview SHALL update within a frame or two without restarting the editor

#### Scenario: Cost is visible while authoring
- **WHEN** an artist adds a module that forces an additional attribute into the layout
- **THEN** the editor SHALL show the increased per-particle size and its effect on the maximum
  population

#### Scenario: Debugging a graph
- **WHEN** an effect misbehaves
- **THEN** the author SHALL be able to inspect a sampled particle's attribute values at each node

### Requirement: Cooking and asset pipeline
VFX assets SHALL be cooked: graphs compiled to shader artefacts, attribute layouts resolved,
parameter defaults folded, permutations enumerated, and the result content-addressed like any
other cooked asset.

Cooking SHALL report per effect: kernel count, per-particle size, estimated GPU cost at a
reference population, and any features unsupported on target platforms.

The runtime SHALL contain no graph compiler.

#### Scenario: Platform-unsupported feature
- **WHEN** an effect uses a data interface unavailable on a target platform
- **THEN** cooking for that platform SHALL fail or produce a documented degraded variant, with the
  decision explicit rather than discovered at runtime

#### Scenario: Cook cache hit
- **WHEN** an unchanged effect is cooked again
- **THEN** the content-addressed cache SHALL be reused, as for other assets

### Requirement: Gameplay API
VFX SHALL be exposed to gameplay through components and a server interface, with the scripting
surface deliberately thin: gameplay describes *what* should happen, and never simulates particles.

An effect instance SHALL support: play, stop (immediately or by allowing existing particles to
finish), pause, parameter set and get, transform binding to an entity, and completion
notification.

A fire-and-forget spawn SHALL be available that requires no handle management.

#### Scenario: Effect follows an entity
- **WHEN** an effect is attached to an entity
- **THEN** its transform SHALL follow automatically, without gameplay code updating it per frame

#### Scenario: Fire-and-forget impact
- **WHEN** a system spawns an impact effect at a position and normal
- **THEN** it SHALL play to completion and release itself with no handle retained

#### Scenario: Graceful stop
- **WHEN** an effect is stopped by allowing completion
- **THEN** spawning SHALL cease and existing particles SHALL live out their lifetimes

### Requirement: Fluids are deferred with reserved seams
Fluid simulation (2D grid and 3D sparse volumes for fire, smoke, and gas) SHALL NOT be implemented
in the initial system.

The architecture SHALL reserve the seams that would allow it later without restructuring:

- a **grid data interface** contract for graphs to read and write volumetric data
- the **`Volume` renderer** integrating with the volumetric rendering pipeline
- the scheduler's ability to host **non-particle simulation stages** within an effect

A change adding fluids SHALL go through the OpenSpec flow; whether the solver is written or
integrated is explicitly not decided here.

#### Scenario: A change would close the seam
- **WHEN** a proposal would make the scheduler particle-only, or remove the grid data interface
  contract
- **THEN** it SHALL be flagged against this requirement and either revised or accepted as an
  explicit decision to drop fluid support

### Requirement: VFX diagnostics
The system SHALL report per frame: live particle count by effect and by importance class, spawn
and kill rates, dispatch count before and after merging, GPU time by stage (spawn, update, event,
sort, render), event counts and overflows, budget controller state and applied adjustments, shared
pool occupancy, and readback bytes.

Debug visualisation SHALL include: per-effect bounds and importance class, particle attribute
visualisation (velocity, age, size, collision normals), overdraw for particle rendering,
simulation-rate heat map, and sorted-versus-unsorted indication.

The system SHALL support inspecting a single effect instance in isolation: its resolved kernels,
attribute layout, current population, and per-stage timing.

#### Scenario: Diagnosing a frame spike
- **WHEN** VFX exceeds its budget
- **THEN** per-stage timing and per-effect particle counts SHALL identify which effects and which
  stages are responsible

#### Scenario: Dispatch merging is measurable
- **WHEN** the scheduler merges emitters
- **THEN** the before-and-after dispatch counts SHALL be reported, so the merging's benefit is
  visible rather than assumed

#### Scenario: Overdraw investigation
- **WHEN** particle rendering is fill-bound
- **THEN** the overdraw visualisation SHALL show where, so effects can be authored with smaller
  or fewer sprites

### Requirement: Validation
Because GPU VFX is non-deterministic and visual, it SHALL be validated by:

- **Compiler tests** — graph to IR to Slang, asserting attribute liveness, dead-code elimination,
  constant folding, and fusion produce expected IR for known inputs
- **Statistical simulation tests** — with a fixed seed and fixed simulation rate, asserting
  aggregate properties (population over time, bounds, mean velocity, collision counts) within
  tolerance rather than exact per-particle values
- **Golden-image tests** — fixed effects, fixed camera, fixed seed and simulation rate, pinned
  quality, compared perceptually across every backend
- **Budget tests** — asserting the controller holds a target under synthetic overload and that
  `Critical` effects survive
- **Event bound tests** — asserting feedback chains terminate at their configured limits

Determinism SHALL be achievable only in the constrained test configuration (fixed seed, fixed
rate, pinned quality, single backend) and this SHALL be documented; it is not a runtime guarantee.

#### Scenario: Cross-backend divergence
- **WHEN** Vulkan and Metal produce visibly different results for the same effect beyond tolerance
- **THEN** the golden-image test SHALL fail identifying both backends

#### Scenario: Statistical rather than exact
- **WHEN** a simulation test runs
- **THEN** it SHALL assert aggregate properties within tolerance, because exact GPU reproduction
  across devices is not achievable

#### Scenario: Budget controller under overload
- **WHEN** a synthetic scene demands far more VFX than the budget allows
- **THEN** the controller SHALL converge to the target and `Critical` effects SHALL remain at
  their minimum quality
