## ADDED Requirements

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

## MODIFIED Requirements

### Requirement: Deterministic scheduling mode
The scheduler SHALL support a **deterministic mode** in which system execution order is a fixed
topological order of the dependency graph, independent of worker timing, and parallel loops use a
fixed partitioning.

In deterministic mode: deadline hints SHALL be ignored, parallel reductions SHALL use their fixed
combination order, and command and event commit SHALL follow their declared deterministic order.

Determinism SHALL NOT require single-threaded execution. Parallel execution SHALL remain
deterministic through fixed partitioning and ordered commit rather than through serialisation.

The engine SHALL also provide a **single-threaded deterministic** mode for debugging, in which the
same results are produced without parallelism, so a discrepancy between the two modes localises a
scheduling-dependent defect.

#### Scenario: Reproducible run for testing
- **WHEN** deterministic mode and a fixed simulation step are enabled
- **THEN** two runs with identical inputs SHALL produce identical world state

#### Scenario: Parallel and single-threaded agree
- **WHEN** the same inputs are run in deterministic parallel mode and single-threaded deterministic
  mode
- **THEN** the results SHALL be identical, and any divergence SHALL indicate a defect in a
  system's ordering assumptions

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
