## MODIFIED Requirements

### Requirement: Memory diagnostics
Development builds SHALL provide: per-tag and per-domain live bytes, peak bytes and allocation
counts; leak reporting at shutdown with the allocating call site; optional guard pages or red zones
around allocations; poisoning of freed and reset memory; double-free and generation validation; and
integration hooks for ASan, UBSan, and TSan.

All builds SHALL provide per-domain live and peak bytes, budget utilisation, pressure level and its
history, retirement queue depths, and pool and arena utilisation.

Allocation and free events, pressure transitions, and budget violations SHALL be emitted into the
**shared trace** defined in `diagnostics-profiling-and-crash`, so that a memory spike correlates with
the frame, task, asset, and streaming activity that caused it.

Call-stack capture for allocations SHALL be a **declared mode** — off, sampled, or full — rather than
an always-on cost, since full capture is affordable in development and not in shipping.

Reporting SHALL be attributable along the axes that answer real questions: by domain, by type, by
thread, by world cell, and by asset — so that "why is this region consuming this much" is
answerable.

**Telemetry SHALL exist before allocator optimisation.** Choosing or tuning allocators without
per-domain attribution is guesswork, and the ordering is a requirement rather than advice.

Allocations that are intentionally held for the process lifetime SHALL be taggable as such, so leak
reports distinguish them from defects.

#### Scenario: Leak report at shutdown
- **WHEN** the process exits with outstanding allocations in a tracked allocator
- **THEN** a report SHALL list them by tag with counts, sizes, and call sites, excluding
  allocations tagged as process-lifetime

#### Scenario: Sanitiser build
- **WHEN** the engine is built with `CY_SANITIZE=address`
- **THEN** custom allocators SHALL route through the sanitiser's interface so overflows and
  use-after-free are reported accurately

#### Scenario: Attribution answers a question
- **WHEN** a world region consumes unexpected memory
- **THEN** the report SHALL attribute it by domain, asset, and cell

#### Scenario: A spike has a cause on the timeline
- **WHEN** memory rises sharply during one frame
- **THEN** the allocations SHALL appear on the shared timeline alongside the activity that produced
  them
