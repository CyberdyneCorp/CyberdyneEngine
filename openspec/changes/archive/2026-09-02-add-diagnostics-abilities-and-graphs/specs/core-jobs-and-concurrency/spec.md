## MODIFIED Requirements

### Requirement: Concurrency diagnostics
Development builds SHALL provide: thread-role assertions, a data-race detector integration
(TSan), job system statistics (queue depths, steal counts, per-job durations), a visual frame
timeline of jobs and stages, and detection of systems that exceed a configurable time budget.

Job and task events SHALL be emitted into the **shared trace** defined in
`diagnostics-profiling-and-crash` — task creation, enqueue, start, end, worker, priority, parent, and
suspension — so that task behaviour correlates with memory, streaming, GPU, and simulation events on
one timeline rather than in a separate tool.

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

#### Scenario: Task and streaming stalls correlate
- **WHEN** a task stalls waiting on content
- **THEN** the task and streaming events SHALL appear on one timeline, because both went through the
  shared trace
