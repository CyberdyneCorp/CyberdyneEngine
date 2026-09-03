# diagnostics-profiling-and-crash Specification

## Purpose

Defines the engine's shared diagnostics infrastructure: one trace, one profiler, one capture
artefact, one crash artefact, and the link that turns a bug report into a reproduction.

It exists because nearly every capability already ends with a diagnostics requirement — the job
system reports critical paths, residency answers *why is this not resident*, illumination answers
*why is this area dark*, generation answers *why is this tree here*. Each is well chosen and none
share a transport. Thirty-nine subsystems about to invent thirty-nine tracing formats is the
definition of a missing foundation, and correlating a streaming stall with a task stall with a memory
spike would be impossible if each were recorded its own way.

Two properties are non-negotiable. Telemetry must never be the reason a frame is slow: compiled
identifiers, per-thread buffers, no allocation, no global lock, and a declared loss policy that drops
verbose channels before breadcrumbs. And **the profiler cannot be started after the problem** — a
rolling buffer is always on, and a hitch, an overrun or a fault freezes the window around it.

Crash artefacts are built for the worst moment: no editor, no debugger, no symbols on the player's
machine, and possibly no GPU. They are self-contained and symbolicate later. Device loss carries the
render graph and the last executing pass, because a driver message is not a diagnosis.

## Requirements

### Requirement: One trace, many producers
The engine SHALL provide a single **trace** transport and schema into which every subsystem emits.
Subsystems SHALL NOT define their own event formats, buffers, or capture files.

The trace SHALL carry at minimum: scope begin and end, counters, instants, flow begin and end,
allocations and frees, task lifecycle, GPU events, input and output requests, network events,
simulation ticks, state hashes, and crash breadcrumbs.

Each capability's diagnostics requirement defines **what it emits and what question that answers**;
this capability defines **how it is transported, buffered, captured, and viewed**.

Diagnostics SHALL be available in editor, standalone, dedicated-server, and remote-device builds.

#### Scenario: Correlating across subsystems
- **WHEN** a streaming stall, a task stall, and a memory spike occur together
- **THEN** they SHALL appear on one timeline with one clock, because they were recorded through one
  transport

#### Scenario: A new subsystem needs no new format
- **WHEN** a capability adds diagnostics
- **THEN** it SHALL emit into the shared trace rather than defining its own events and viewer

### Requirement: Trace identity and formatting
Trace event names, categories, and structured field names SHALL be **compiled to stable
identifiers**, registered once, with debug names held in a metadata table.

Strings SHALL NOT be formatted, copied, or hashed in the emission path. Emission SHALL NOT allocate.

Identifiers SHALL be resolvable by a viewer that does not have the running process, so a capture is
readable offline.

#### Scenario: Emission is cheap
- **WHEN** a scope is entered in a hot loop
- **THEN** the recorded event SHALL contain identifiers and a timestamp, with no string work and no
  allocation

#### Scenario: Captures are readable offline
- **WHEN** a capture is opened without the game
- **THEN** identifiers SHALL resolve to names from the capture's metadata

### Requirement: Buffering and loss policy
Trace events SHALL be written into **per-thread buffers** with no shared lock, drained by a
background consumer.

Channels SHALL declare a priority — critical, important, verbose, or sampled — and under buffer
pressure the system SHALL **drop by priority**, discarding verbose first and preserving crash
breadcrumbs, tick boundaries, and task lifecycle last.

**Dropped events SHALL be counted and reported**, so a gap in a capture is visible rather than
silently misleading.

A producer SHALL NEVER block waiting for a consumer. The real-time audio thread and the simulation
SHALL NEVER be stalled by diagnostics.

#### Scenario: Pressure degrades gracefully
- **WHEN** trace volume exceeds what the consumer drains
- **THEN** verbose channels SHALL be dropped, essential events SHALL survive, and the loss SHALL be
  reported

#### Scenario: Nothing blocks on the profiler
- **WHEN** no consumer is draining
- **THEN** producers SHALL continue at bounded cost, discarding by policy

### Requirement: Profiler views
The profiler SHALL provide, from the shared trace: thread and task timelines with nested scopes,
frame and tick boundaries, the **critical path** through the task graph, worker utilisation, queue
latency, blocked workers, and coroutine suspensions.

It SHALL provide subsystem views assembled from what each capability emits: ECS archetype, chunk and
query behaviour with structural change counts; memory by domain, tag, type, asset, and world cell;
GPU passes with queue, timing, barriers, and transient memory; residency and streaming with
requests, hits, evictions and churn; input latency from device event to presented frame; and
simulation ticks with hashes, command counts, and rollback activity.

The profiler SHALL present **totals and the critical path distinctly**, since a frame's duration is
its longest dependency chain rather than the sum of its work.

#### Scenario: The chain, not the sum
- **WHEN** a frame is long
- **THEN** the profiler SHALL show the critical path alongside total parallel work

#### Scenario: Subsystem detail without a second tool
- **WHEN** a developer investigates a streaming stall
- **THEN** streaming, task, memory, and input and output views SHALL be available on the same
  timeline

### Requirement: Structured logging
Logs SHALL be **structured**: a category, a severity, a message identifier, a source location, and
typed fields — not only a formatted string.

Categories SHALL be stable identifiers and extensible by plugins.

Structured fields SHALL make logs searchable, aggregatable, redactable, and exportable without
parsing text.

Formatting SHALL occur at presentation, not at emission, so a log that nobody reads costs almost
nothing.

#### Scenario: Logs are queryable
- **WHEN** a developer looks for every rejected ability activation for one participant
- **THEN** the query SHALL run over typed fields rather than over text

#### Scenario: Emission is cheap
- **WHEN** a verbose log is emitted and not consumed
- **THEN** no string SHALL be formatted

### Requirement: Assertions and health
The engine SHALL define assertion levels with distinct behaviour — checked, ensured, verified, and
fatal — and their behaviour per build configuration SHALL be declared rather than assumed.

The engine SHALL maintain a **health model** with severity levels, aggregating conditions that
subsystems report: frame and tick budget overruns, GPU overruns, memory pressure, streaming deadline
misses, packet loss, rollback frequency, task starvation, and determinism divergence.

Health SHALL be observable in one place, in every build type, so that "something is wrong" is
answerable without attaching a tool.

#### Scenario: One place to look
- **WHEN** a session behaves badly
- **THEN** the health model SHALL report which conditions are active and since when

#### Scenario: Assertion behaviour is declared
- **WHEN** a build configuration is chosen
- **THEN** the behaviour of each assertion level in it SHALL be documented rather than discovered

### Requirement: Rolling buffer and automatic capture
The engine SHALL maintain an **always-on rolling diagnostic buffer** of a configured recent duration
at low cost, holding trace events, logs, tick summaries, and health transitions.

Capture SHALL be **triggerable automatically** by declared conditions — a frame or tick exceeding a
threshold, a GPU overrun, an assertion, a health transition to critical — freezing the window before
and after the event and writing a capture artefact.

Manual capture SHALL also be available, and both SHALL produce the same artefact format.

A profiler SHALL NOT be required to be attached beforehand for a hitch to be diagnosable.

#### Scenario: The hitch that does not recur
- **WHEN** a rare frame spike occurs
- **THEN** the rolling buffer SHALL have captured the window automatically

#### Scenario: One artefact format
- **WHEN** a capture is produced automatically or manually
- **THEN** the artefact SHALL be the same format and openable by the same viewer

### Requirement: Crash artefacts
On a crash, fatal assertion, or unrecoverable fault, the engine SHALL write a **crash artefact**
containing: build identity, platform and system information, process and thread state, the
exception or signal, a stack trace with module identities and offsets, the module list, the trace
tail, breadcrumbs, the log tail, health state, and a reference to any reproduction artefact.

The artefact SHALL be **self-contained and symbol-independent**: it SHALL be usable on a machine with
no symbols, and symbolication SHALL occur later against the symbols archived by the build system.

Crash capture SHALL NOT require the editor, a network connection, or a debugger to be present.

The capture path SHALL be defensive: it SHALL assume the process is damaged and SHALL avoid
allocation, locks, and subsystem re-entry where possible.

#### Scenario: A player's crash is diagnosable
- **WHEN** a shipping build crashes on a player's machine
- **THEN** the artefact SHALL identify the build and carry offsets that symbolicate against archived
  symbols

#### Scenario: No editor required
- **WHEN** a dedicated server crashes
- **THEN** the artefact SHALL be written without any tool attached

### Requirement: Breadcrumbs
Subsystems SHALL be able to record **breadcrumbs**: durable markers of what was being done, written
so that they survive into the crash artefact even when the trace tail is lost.

The render graph SHALL record per-pass breadcrumbs so that a device loss identifies the last
executing pass, and the engine SHALL record breadcrumbs at coarse phase boundaries: tick, stage,
asset activation, level transition, and save.

Breadcrumbs SHALL be a bounded, low-cost mechanism, distinct from the verbose trace, precisely
because they must survive when the trace does not.

#### Scenario: The last thing it was doing
- **WHEN** a process dies without a usable stack
- **THEN** breadcrumbs SHALL identify the phase, tick, and pass it was in

### Requirement: Graphics device diagnostics
On device loss, device removal, or a graphics fault, the engine SHALL capture: the render graph as
built for the frame, the last submitted and last completed passes from breadcrumbs, pipeline and
shader identities, resource identities and their states, barrier history where available, queue
state, the frame identity, and any driver-reported reason.

"Device removed" alone SHALL NOT be considered a diagnosis.

Where the platform provides fault information — page faults, invalid access, timeouts — it SHALL be
included, and the artefact SHALL name the pass and pipeline most likely responsible.

#### Scenario: A device loss names a pass
- **WHEN** the device is lost
- **THEN** the artefact SHALL identify the last executing pass and its pipeline, not merely that the
  device was removed

### Requirement: Shader and material diagnostics
A shader or material failure SHALL report its **full lineage**: the material, the graph node, the
material intermediate representation, the generated source with location, the backend and its
message, and the permutation or pipeline state involved.

An opaque backend compiler log SHALL NOT be the diagnostic.

Where a failure occurs at runtime, the fallback behaviour applied SHALL be reported alongside the
cause.

#### Scenario: A failure points at a node
- **WHEN** a material fails to compile
- **THEN** the diagnostic SHALL name the graph node and the generated source location, not only the
  backend error

### Requirement: Reproduction artefacts
The engine SHALL support producing a **reproduction artefact** linking a crash or a reported defect
to the replay slice, checkpoints, external results, and state hashes recorded by
`replay-and-rollback`.

Given such an artefact, the engine SHALL be able to load it and advance to shortly before the
failure, so that a bug report carries its own reproduction.

The reproduction artefact SHALL be separate from the crash artefact and linked to it, since a crash
may have no reproduction and a reproduction may have no crash.

Where determinism is insufficient to reproduce exactly, the artefact SHALL state so rather than
implying fidelity it does not have.

#### Scenario: A bug report reproduces itself
- **WHEN** a tester reports a defect
- **THEN** the report SHALL be able to carry a reproduction artefact that replays the window

#### Scenario: Fidelity is honest
- **WHEN** the session's determinism profile cannot guarantee exact reproduction
- **THEN** the artefact SHALL say so

### Requirement: Privacy classification
Every field captured by diagnostics, logging, or crash reporting SHALL carry a **privacy
classification**: public, developer, potentially personal, sensitive, or secret.

Credentials, authentication tokens, private communications, and personal files SHALL NEVER be
captured automatically.

Redaction SHALL be enforceable because logging is structured: a field's classification determines
whether it is included in an artefact that leaves the machine.

Artefacts intended to leave a player's machine SHALL declare what classifications they may contain,
and a project SHALL be able to tighten that.

#### Scenario: Nothing sensitive leaves by default
- **WHEN** a crash artefact is prepared for upload
- **THEN** fields classified sensitive or secret SHALL be excluded

#### Scenario: Classification is enforceable
- **WHEN** a new field is logged
- **THEN** it SHALL carry a classification, and an unclassified field SHALL be reported

### Requirement: Remote and server diagnostics
Diagnostics SHALL be consumable **remotely**: an editor or tool on one machine observing a runtime on
a console, a mobile device, a dedicated server, or another machine.

The diagnostics backend SHALL NOT be owned by the editor. The editor SHALL be one client of a
transport that command-line tools and automated systems can also use.

Dedicated-server diagnostics SHALL be first-class and SHALL require no rendering, audio, or interface
code: tick rate, processor and memory use, connections, replication, artificial intelligence, world
streaming, and divergence.

#### Scenario: Profiling a console
- **WHEN** a defect appears only on a console
- **THEN** its trace, health, and captures SHALL be observable remotely through the same transport

#### Scenario: A headless server is observable
- **WHEN** a dedicated server runs
- **THEN** its diagnostics SHALL be available with no rendering or interface code present

### Requirement: Capture artefacts
Trace captures SHALL be written as a **chunked, compressed, streamable artefact** openable without
the game runtime, carrying its own metadata for identifier resolution and its build identity.

Capture artefacts, crash artefacts, and reproduction artefacts SHALL be distinct and mutually
linkable.

Artefacts SHALL be loadable partially, so a long capture can be opened and navigated without reading
all of it.

#### Scenario: A long capture opens quickly
- **WHEN** a multi-minute capture is opened
- **THEN** the viewer SHALL read its index and load regions on demand

### Requirement: Telemetry export
Aggregated metrics SHALL be exportable to an external system where a project chooses, and this SHALL
be **opt-in and separately configured** from local diagnostics.

Exported metrics SHALL respect privacy classification, and the set exported SHALL be declared rather
than being whatever happened to be recorded.

Useful aggregates SHALL include: crash and assertion rates by build, frame and tick budget
violations, memory pressure events, streaming deadline misses, save and migration failures, replay
corruption, desynchronisation frequency, and rollback statistics.

#### Scenario: Fleet-level signal
- **WHEN** a build is released
- **THEN** its crash rate, budget violations, and save failure rate SHALL be observable in aggregate
  if the project enables export

### Requirement: Diagnostics overhead
Overhead SHALL be bounded and declared: **shipping with minimal telemetry SHALL be well under one per
cent of frame time**, development with normal tracing SHALL be a small single-digit percentage, and
full instrumentation SHALL be explicitly higher.

There SHALL be no per-event heap allocation, no global lock in emission, and no unbounded growth in
any always-on buffer.

The engine SHALL be able to report the cost of its own diagnostics, so the measurement's cost is
itself measurable.

#### Scenario: Shipping telemetry is affordable
- **WHEN** minimal telemetry is enabled in a shipping build
- **THEN** its cost SHALL be within the declared bound and reportable

### Requirement: Forbidden diagnostics patterns
The following SHALL NOT appear, and each SHALL be checkable:

- A global lock taken per trace event
- Heap allocation or string formatting in the emission path
- Crash reporting that requires the editor, a debugger, or a network connection
- Critical breadcrumbs held only in a volatile verbose buffer
- Device-loss reporting that stops at the driver's message
- A subsystem defining its own incompatible trace format or capture file
- The profiler backend owned by the editor process
- Automatic capture of credentials, tokens, private communications, or personal files
- Silent event loss with no record that events were lost

#### Scenario: A proposal is checked
- **WHEN** a change would add a subsystem-specific profiler with its own capture file
- **THEN** it SHALL be flagged against this requirement
