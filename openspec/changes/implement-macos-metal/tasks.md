# Tasks

## 1. Native build and resource foundation

- [x] 1.1 Identify the local Metal device and argument-buffer tier; record actual API results in design.md.
- [x] 1.2 Compile the existing seed against the Apple SDK and record which assertions were exercised.
- [ ] 1.3 Repair macOS configure/link prerequisites with regression coverage; build the Metal target with warnings as errors.
- [ ] 1.4 Implement the native device and resource lifetime foundation; verify handle reuse, deferred destruction, and mapped readback.

## 2. Native execution

- [ ] 2.1 Implement shader, pipeline, descriptor, draw, and compute paths; verify pixel and buffer readbacks.
- [ ] 2.2 Implement queue timelines, transient heaps, memoryless attachments, and Tier 2 argument buffers; verify on the named Apple GPU with validation enabled.
- [ ] 2.3 Integrate the display-server surface and render graph; capture actual presentation and report golden-image deltas.

## 3. macOS editor transport

- [x] 3.1 Implement native shared-surface publishing and synchronized import behind the existing transport abstraction; verify producer/consumer ownership across a bounded ring.
- [x] 3.2 Enable and test the Darwin runtime-control bridge; exercise pick identity, resize, disconnect, and restart across real processes.
- [x] 3.3 Capture an engine frame in the macOS editor and record latency/frame pacing with the device and transport named.

## 4. Integration handoff

- [x] 4.1 Run native conformance and relevant editor tests, update backend documentation, and report unresolved scope to the roadmap owner without editing their milestone status.
