# core-jobs-and-concurrency Specification

## Purpose

Defines the concurrency model: the job system that owns every worker thread, the thread roles and
what each may touch, the asynchronous model, synchronisation primitives, and the rules that make
parallel ECS system execution safe by construction rather than by convention.

Systems declare their data access and the scheduler derives the dependency graph, so parallelism is
a property of the declarations rather than of hand-placed locks. Asynchronous work — file reads,
decompression, GPU results — is expressed with **C++20 coroutines** whose continuations resume as
ordinary tasks, and the rule that follows is absolute: **a worker thread never blocks on I/O or on
the GPU**. A fiber runtime is a non-goal.

Every task carries a **context** with its worker index, a scratch allocator, and a cancellation
token, which is where this system meets the memory model: temporary allocation lands in per-worker
scratch by default, task records come from per-worker slabs, and cancellation is available where
the work is. Cancellation is cooperative; nothing is forcibly terminated.

Determinism is a mode, not an accident: fixed topological ordering, fixed partitioning, fixed
reduction order, and ordered commit of commands and events — deterministic without being
sequential. Deadlines and priorities may influence *when* work runs, never *what* the simulation
computes.

The diagnostic that matters most is the **critical path**: a frame's duration is its longest
dependency chain, not the sum of its jobs, and the specification requires that reporting to exist
before work-stealing behaviour is tuned.

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

### Requirement: Task context
Every task SHALL receive a **task context** providing at minimum: its worker index, a **scratch
allocator** for the task's lifetime, and its **cancellation token**.

Temporary allocation within a task SHALL default to that scratch allocator, so the common case is
contention-free and reclaimed in bulk without the author choosing an allocator.

Task records and coroutine frames SHALL be allocated from **per-worker slabs**, so scheduling a
task does not call the general allocator.

Scratch memory SHALL NOT outlive the task, and development builds SHALL poison it on release.

#### Scenario: Temporary buffers are free
- **WHEN** a navigation task needs a temporary node array
- **THEN** it SHALL allocate from its context's scratch allocator, and the memory SHALL be reclaimed
  in bulk when the task ends

#### Scenario: Scheduling does not allocate
- **WHEN** a system schedules ten thousand tasks in a frame
- **THEN** their records SHALL come from per-worker slabs with no general heap allocation

### Requirement: Coroutines are the asynchronous model
Asynchronous engine work SHALL be expressed with **C++20 coroutines** whose continuations the job
system resumes as ordinary tasks.

A coroutine SHALL be able to await: another job or coroutine, an asynchronous I/O completion, a GPU
fence, a task group, a timer, and a cancellation.

A **fiber runtime** is a non-goal. Stackful suspension SHALL NOT be introduced without its own
proposal.

Awaiting SHALL never allocate from the general heap on the common path; coroutine frames SHALL come
from per-worker slabs where their size is known.

#### Scenario: A load reads as a sequence
- **WHEN** a cell load reads a file, decompresses it, and activates the result
- **THEN** it SHALL be written as a coroutine awaiting each step, not as a chain of callbacks

#### Scenario: Continuations are tasks
- **WHEN** an awaited operation completes
- **THEN** the continuation SHALL be scheduled as a task with the awaiting coroutine's priority,
  and MAY run on a different worker

### Requirement: Workers never block on I/O or the GPU
A worker thread SHALL NOT block waiting for file I/O, decompression performed elsewhere, network
completion, or a GPU fence.

Asynchronous I/O SHALL be submitted to the platform's asynchronous mechanism and its completion
SHALL schedule a continuation. GPU fences SHALL be awaited through backend completion notification,
never by busy-waiting or by a blocking wait on a worker.

A development build SHALL detect and report a worker thread blocked beyond a configurable threshold,
naming the task, since a blocked worker is a defect rather than a slow operation.

#### Scenario: File read does not consume a worker
- **WHEN** a task reads a file
- **THEN** the read SHALL be submitted asynchronously, the worker SHALL run other ready work, and
  the continuation SHALL be scheduled on completion

#### Scenario: GPU readback
- **WHEN** a system needs the result of GPU work
- **THEN** it SHALL await the fence, and no worker SHALL spin or block on it

#### Scenario: Blocking is detected
- **WHEN** a worker blocks longer than the configured threshold in a development build
- **THEN** it SHALL be reported with the task's name

### Requirement: Cancellation
Tasks and coroutines SHALL support **cooperative cancellation** through a cancellation token
carried in the task context.

Cancellation SHALL NOT forcibly terminate running code. A cancelled task observes its token at a
natural boundary, releases what it holds, and returns; the framework SHALL define what a cancelled
result means for its awaiters.

Cancellation SHALL propagate to child tasks and to awaited operations that support it, so cancelling
a composite operation cancels its parts.

Long-running work SHALL check its token at bounded intervals, and a task that does not observe
cancellation within a configurable time SHALL be reported.

#### Scenario: Streaming is cancelled
- **WHEN** the camera moves away and a cell is no longer needed
- **THEN** its pending preparation SHALL be cancelled, its partial work released, and its resources
  returned

#### Scenario: Cancellation propagates
- **WHEN** a parent load is cancelled
- **THEN** its child decompression and activation tasks SHALL observe cancellation too

#### Scenario: Unresponsive task is reported
- **WHEN** a cancelled task continues past the configured interval without observing its token
- **THEN** it SHALL be reported with its name

### Requirement: Priority classes, fairness, and deadlines
Tasks SHALL declare a **priority class**: `Critical`, `High`, `Normal`, `Background`, or `Idle`.

The scheduler SHALL guarantee **fairness**: lower classes SHALL NOT be starved indefinitely by
higher ones. Background and idle work SHALL receive bounded minimum progress, so that decompression
and streaming continue under sustained high-priority load.

Tasks MAY carry an optional **deadline hint** — a frame index or time by which the result is
wanted — which the scheduler MAY use to order work.

Deadlines and priorities SHALL influence **when work runs**, never **what the simulation computes**.
In deterministic mode (see below) deadline hints SHALL be ignored and scheduling SHALL follow the
fixed order. Work whose result a simulation stage consumes SHALL complete before that stage
regardless of hints; a deadline is a scheduling preference, not a correctness contract.

#### Scenario: Background work still progresses
- **WHEN** high-priority work saturates the workers for several seconds
- **THEN** background decompression SHALL still receive bounded progress rather than stalling
  indefinitely

#### Scenario: Teleport beats prefetch
- **WHEN** an urgent cell preparation and a speculative prefetch are both pending
- **THEN** the urgent one SHALL be preferred

#### Scenario: Determinism ignores deadlines
- **WHEN** deterministic mode is enabled
- **THEN** deadline hints SHALL not affect execution order and results SHALL be identical to a run
  without them

### Requirement: Long-running work is chunked
Work that may run longer than a frame — asset cooking, navigation generation, light baking, import
— SHALL be partitioned into bounded units or SHALL yield at intervals, and SHALL NOT occupy a worker
indefinitely.

Such work SHALL be schedulable at `Background` or `Idle` priority, SHALL be cancellable, and SHALL
report progress.

A development build SHALL report a task exceeding a configurable duration, naming it.

#### Scenario: Editor import does not stall the editor
- **WHEN** a large asset is imported
- **THEN** the work SHALL be chunked or yielding, and the editor SHALL remain responsive

#### Scenario: Overlong task is reported
- **WHEN** a single task runs longer than the configured limit
- **THEN** it SHALL be reported with its name and duration

### Requirement: Deterministic scheduling mode
The scheduler SHALL support a **deterministic mode** in which system execution order is a fixed
topological order of the dependency graph, independent of worker timing, and parallel loops use a
fixed partitioning.

In deterministic mode: deadline hints SHALL be ignored, parallel reductions SHALL use their fixed
combination order, and command and event commit SHALL follow their declared deterministic order.

Ordering keys used for commit SHALL be built from stable logical identity — system, partition, local
sequence — and **SHALL NOT include worker or thread identity**, since work stealing makes a worker's
identity a function of timing.

Determinism SHALL NOT require single-threaded execution. Parallel execution SHALL remain
deterministic through fixed partitioning and ordered commit rather than through serialisation.

The engine SHALL also provide a **single-threaded deterministic** mode for debugging, in which the
same results are produced without parallelism, so a discrepancy between the two modes localises a
scheduling-dependent defect.

The scheduler SHALL additionally provide a **chaos mode** that deliberately randomises permitted
execution order, worker counts, and chunk assignment, so that undeclared ordering dependencies
surface in testing rather than in production (see `simulation-and-determinism`).

#### Scenario: Reproducible run for testing
- **WHEN** deterministic mode and a fixed simulation step are enabled
- **THEN** two runs with identical inputs SHALL produce identical world state

#### Scenario: Parallel and single-threaded agree
- **WHEN** the same inputs are run in deterministic parallel mode and single-threaded deterministic
  mode
- **THEN** the results SHALL be identical, and any divergence SHALL indicate a defect in a
  system's ordering assumptions

#### Scenario: Chaos exposes a hidden dependency
- **WHEN** chaos mode randomises permitted ordering
- **THEN** a system whose result depends on execution order SHALL produce differing results and be
  identified

### Requirement: Deterministic parallel primitives
The engine SHALL provide parallel primitives whose results do not depend on execution order:
`parallel_for`, `parallel_reduce`, `parallel_scan`, and `parallel_sort`.

Reductions SHALL use a **fixed combination order** derived from the partitioning, not from
completion order, so floating-point results are reproducible.

Work that produces externally visible effects — events, commands, spawns — SHALL accumulate into
per-worker buffers and be **committed in a deterministic order** derived from a declared key or from
worker and submission index, never from the order tasks happened to finish.

Systems SHALL NOT rely on scheduler execution order for correctness, and development builds SHALL
be able to randomise permitted ordering to expose such reliance.

#### Scenario: Reduction is reproducible
- **WHEN** a parallel floating-point reduction runs twice with different worker timing
- **THEN** it SHALL produce bit-identical results

#### Scenario: Events commit deterministically
- **WHEN** several workers produce damage events in one stage
- **THEN** they SHALL be committed in a deterministic order independent of which worker finished
  first

#### Scenario: Order reliance is caught
- **WHEN** development builds randomise permitted ordering
- **THEN** a system depending on completion order SHALL produce varying results and be caught

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

The profiler SHALL report the frame's **critical path**: the longest dependency chain through the
task graph, with its constituent tasks. Frame duration is determined by that chain rather than by
the sum of task durations, and optimising work that is not on it does not shorten the frame.

The profiler SHALL additionally report: worker utilisation and idle time, queue latency between
scheduling and execution, blocked-worker detections, cancelled task counts, and per-priority-class
throughput.

Task profiling and critical-path reporting SHALL exist **before** work-stealing behaviour is tuned,
since throughput improvements off the critical path do not shorten frames.

#### Scenario: Job timeline
- **WHEN** profiling is enabled
- **THEN** each named job SHALL emit begin and end markers so the frame timeline shows
  per-worker occupancy and stalls

#### Scenario: The critical path is visible
- **WHEN** a frame exceeds its budget
- **THEN** the profiler SHALL show the longest dependency chain, so effort is directed at work that
  determines frame duration

#### Scenario: Idle workers are explained
- **WHEN** workers are idle while the frame is long
- **THEN** the report SHALL show whether the cause is dependency serialisation, insufficient
  partitioning, or blocked workers
