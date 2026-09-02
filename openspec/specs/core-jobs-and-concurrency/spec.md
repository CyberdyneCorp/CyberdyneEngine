# core-jobs-and-concurrency Specification

## Purpose

Defines the concurrency model: the job system that owns every worker thread, the thread roles and
what each may touch, synchronisation primitives, the command queue used to marshal work to
single-threaded servers, and the rules that make parallel ECS system execution safe by
construction rather than by convention.

## Requirements

### Requirement: Single job system owns all worker threads
The engine SHALL create exactly one `JobSystem` at startup which owns all general-purpose worker
threads. Subsystems SHALL NOT spawn their own threads except for the documented dedicated
threads listed below.

Worker count SHALL default to `hardware_concurrency() - 1` (reserving the main thread) and be
overridable by configuration.

The job system SHALL support:
- `submit(fn)` returning a `JobHandle`
- `submit_parallel_for(count, grain, fn)` for indexed parallel loops
- explicit dependencies: `submit(fn, deps)` where `deps` is a span of `JobHandle`
- `wait(handle)` and `wait_all(handles)`
- job priorities (`High`, `Normal`, `Low`) and named jobs for profiling

Scheduling SHALL use per-worker deques with work stealing.

#### Scenario: Waiting worker does useful work
- **WHEN** a job waits on another job while running on a worker thread
- **THEN** the worker SHALL execute other ready jobs instead of blocking, so recursive job
  submission cannot deadlock the pool

#### Scenario: Parallel loop grain size
- **WHEN** `submit_parallel_for` is called with a grain size
- **THEN** work SHALL be partitioned into ranges of at least that size, so small loops do not pay
  more in scheduling than they save

#### Scenario: Dependencies are honoured
- **WHEN** job B declares job A as a dependency
- **THEN** B SHALL NOT begin until A has completed

### Requirement: Thread roles
The engine SHALL define these thread roles and their ownership:

| Thread | Owns / may touch |
|---|---|
| Main | Platform event pump, window and input, the frame schedule, editor UI |
| Simulation | ECS world mutation outside parallel system execution, deferred command flush |
| Job workers | Parallel system execution, culling, animation sampling, asset decode, physics jobs, acoustic simulation |
| Render | Render graph recording and GPU submission; owns all RHI objects |
| Audio (realtime) | Audio mixing and effect processing; owns playback state; hard-realtime, never blocks |
| Asset I/O | File reads, decompression, streaming; never touches ECS or GPU objects |

Where the platform requires it (macOS, Windows message pumps), the main and simulation roles MAY
share one OS thread; the ownership rules still apply.

Work whose results the realtime audio thread consumes — acoustic simulation in particular — SHALL
run on job workers and publish through a double-buffered store. The realtime audio thread SHALL
NOT wait on job workers, and job workers SHALL NOT block on the audio thread.

#### Scenario: RHI object touched off the render thread
- **WHEN** code on a worker thread attempts to record RHI commands
- **THEN** a development-build assertion SHALL fire naming the violated thread role

#### Scenario: Asset thread hands off safely
- **WHEN** the asset I/O thread finishes loading
- **THEN** it SHALL publish the result through a queue consumed on the simulation thread; it
  SHALL NOT insert into the ECS world directly

#### Scenario: Realtime audio never waits
- **WHEN** acoustic simulation on job workers has not completed a new result
- **THEN** the audio callback SHALL use the previously published result and meet its deadline

### Requirement: Parallel system execution is safe by construction
Systems SHALL declare their data access as part of their signature: for each component type,
`Read`, `Write`, or `Exclude`; plus declared access to resources (singleton state) and event
channels.

The scheduler SHALL build a dependency graph per stage from these declarations and run systems
in parallel when their access sets do not conflict: two `Read`s never conflict; a `Write`
conflicts with any other access to the same component type.

A system SHALL NOT perform structural changes (create or destroy entities, add or remove
components) during parallel execution; it SHALL record them into a per-thread command buffer
applied at the stage's flush point.

#### Scenario: Independent systems run concurrently
- **WHEN** one system writes `Velocity` while reading `Input`, and another writes `Health` while
  reading `Damage`
- **THEN** the scheduler SHALL run them on different workers in the same stage

#### Scenario: Conflicting systems are serialised
- **WHEN** two systems both write `Transform`
- **THEN** the scheduler SHALL order them, using the declared explicit ordering constraint if
  one exists and a stable deterministic order otherwise

#### Scenario: Undeclared access is caught
- **WHEN** a development build detects a system touching a component it did not declare
- **THEN** an assertion SHALL fire identifying the system and the component type

#### Scenario: Structural change is deferred
- **WHEN** a system running in parallel spawns an entity
- **THEN** the spawn SHALL be recorded in the thread's command buffer and applied at the flush
  point in submission order

### Requirement: Deterministic scheduling mode
The scheduler SHALL support a deterministic mode in which system execution order is a fixed
topological order of the dependency graph, independent of worker timing, and parallel loops use a
fixed partitioning.

#### Scenario: Reproducible run for testing
- **WHEN** deterministic mode and a fixed simulation step are enabled
- **THEN** two runs with identical inputs SHALL produce identical world state

### Requirement: Synchronisation primitives
`core/threading` SHALL provide `Thread`, `Mutex`, `RecursiveMutex`, `RwLock`, `Semaphore`,
`ConditionVariable`, `SpinLock`, `Event`, and the atomic wrappers `Atomic<T>`, `AtomicFlag`,
and `AtomicRefCount`.

Locks SHALL be avoided in per-entity hot paths; where cross-thread data must be shared, the
preferred mechanisms are the job system's dependency graph, double buffering, and lock-free
single-producer/single-consumer queues.

#### Scenario: Lock in a hot loop is a design error
- **WHEN** a system needs shared mutable state per entity
- **THEN** it SHALL restructure the data (per-thread accumulation merged at the flush point)
  rather than lock inside the loop

### Requirement: Command queue for single-threaded servers
`CommandQueue` SHALL serialise calls into a byte buffer for replay on an owning thread,
supporting fire-and-forget submission and synchronous submission that blocks until the command
has executed and a result is written back.

Commands SHALL store their arguments by value and be trivially relocatable; a command exceeding
the inline size limit SHALL allocate from the queue's arena rather than the heap.

#### Scenario: Fire-and-forget render call
- **WHEN** the simulation thread updates a render instance transform
- **THEN** the call SHALL be enqueued and return immediately, applied on the render thread at the
  next drain

#### Scenario: Synchronous call is flagged
- **WHEN** a synchronous cross-thread call is made every frame in a development build
- **THEN** a warning SHALL identify it, because it serialises the frame

### Requirement: Double buffering across thread boundaries
State read by one thread and written by another SHALL be double buffered where possible, with an
explicit swap at a defined synchronisation point, rather than shared under a lock.

This SHALL apply to: transforms consumed by rendering, event channels, input state, and
debug-draw command lists.

#### Scenario: Render reads a stable snapshot
- **WHEN** the render thread builds a frame
- **THEN** it SHALL read the previous frame's published snapshot, and simulation SHALL write the
  next one concurrently without synchronisation

### Requirement: Frame pacing and synchronisation points
The engine SHALL define explicit synchronisation points per frame: after each simulation stage
flush, after the frame stage, at render snapshot publication, and at GPU frame submission.

The renderer SHALL support up to `frames_in_flight` (default 2) concurrent GPU frames, with
per-frame resources ring-buffered accordingly.

#### Scenario: CPU runs ahead of GPU
- **WHEN** the CPU completes a frame while `frames_in_flight` frames are already queued
- **THEN** it SHALL wait on the oldest frame's fence before reusing that frame's resources

### Requirement: Concurrency diagnostics
Development builds SHALL provide: thread-role assertions, a data-race detector integration
(TSan), job system statistics (queue depths, steal counts, per-job durations), a visual frame
timeline of jobs and stages, and detection of systems that exceed a configurable time budget.

#### Scenario: Job timeline
- **WHEN** profiling is enabled
- **THEN** each named job SHALL emit begin and end markers so the frame timeline shows
  per-worker occupancy and stalls
