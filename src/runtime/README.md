# `src/runtime/` — layer 5

Engine bootstrap and subsystem wiring: the fixed initialisation order, its exact reverse for
shutdown, and `Runtime::tick()`.

**The runtime does not own the loop.** It exposes `tick()`; the `while (running)` lives in the host
under `platform/host/`, so a platform that drives frames itself — mobile, web — is not precluded.
This is `design.md` §3 and it is an M0 invariant. Nothing in this directory calls a host loop, and
nothing in it can: `cy_runtime` does not link one and does not know one exists.

**What belongs here**: the startup and shutdown sequence, the frame's stage ordering, the wiring of
servers and modules.

**What does not belong here**: a main loop, a `main()`, and any platform call not routed through
`Platform` or `DisplayServer`.

## The startup sequence

`engine-architecture` fixes eleven stages and requires teardown in exact reverse. All eleven are in
`kStages` in `src/runtime.cpp`, in the specification's order, and **every one of them is entered on
every run** — including the stages whose subsystem does not exist yet. A stage with no work is still
a stage: its position is fixed now, so the milestone that fills it fills a slot rather than choosing
a new order.

| # | Stage | M0 | Filled by |
|---:|---|---|---|
| 1 | `platform` | crash handler, trace open, base's two diagnostic seams | — |
| 2 | `core` | — | M1: allocators, jobs, type registry, VFS, configuration |
| 3 | `modules-core` | — | M1 |
| 4 | `display` | window creation, and the `GraphicsApi::None` surface seam | M3: the real surface |
| 5 | `servers` | — | M2, M3 |
| 6 | `modules-servers` | — | M2 |
| 7 | `ecs-scene` | — | M2 |
| 8 | `modules-scene` | — | M2 |
| 9 | `scripting` | — | M4, M5 |
| 10 | `editor` | — | M5 |
| 11 | `boot` | the frame clock's origin | M2: the startup scene |

The order taken is recorded in a journal; `shutdown()` walks it backwards and the two are checked to
be exact reverses **in every configuration**, then asserted on top of that. A stage that fails
unwinds every stage already entered, in reverse, and returns an error after a diagnostic naming the
stage — `engine-architecture`, "Failure during startup unwinds cleanly".

`probe/startup_order_probe.cpp` prints both journals and exits;
`tests/smoke/test_startup_order.cpp` runs it a hundred times and requires all hundred to agree
(task 3.4.4). Separate processes rather than a loop: a fresh address space each run is what would
expose an order that depended on an allocation address or a static initialiser.

## The frame

`tick()` samples the monotonic clock, pumps the display's events, runs zero or more fixed simulation
steps, and computes the interpolation alpha. The steps are empty — the ECS is M2 — but the
accumulator, the `max_ticks_per_frame` cap and the discard of excess time are here from M0, because
they are the loop's only defence against a death spiral and they are four lines.

A `CloseRequested` event calls `Platform::request_exit()`. The runtime records the intent; the host
observes it and returns from `main()`.

**Governed by**: `engine-architecture` (deterministic startup and shutdown, main loop),
`core-platform-abstraction` (the platform does not own the main loop).
