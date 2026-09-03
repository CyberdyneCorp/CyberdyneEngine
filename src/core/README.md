# `src/core/` — layer 0

The foundation every other layer is written against. Core depends on the C++ standard library, the
platform it is running on, and nothing else in this repository.

**What belongs here**
- `base/` — type aliases, `cy::Expected<T, Error>`, the `Error` model, compile-time utilities
- `platform/` — the engine-owned `Platform` interface (process, time, dynamic libraries, filesystem
  entry points, crash-handler installation). Implementations live under `platform/<name>/`, never here.
- `diagnostics/` — assertions, structured logging, the trace timeline and its privacy classification
- Later milestones add memory and containers, math, jobs, and assets and I/O.

**What does not belong here**
- Anything that names an entity, a node, a server, or a window
- Any `#ifdef` on the host platform; platform-specific code lives under `platform/<name>/`
- Any third-party type in a public header

**Governed by**: `core-type-system`, `core-memory-and-containers`, `core-math`,
`core-jobs-and-concurrency`, `core-assets-and-io`, `core-platform-abstraction`,
`diagnostics-profiling-and-crash`.
