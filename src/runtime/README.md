# `src/runtime/` — layer 5

Engine bootstrap and subsystem wiring: the fixed initialisation order, its exact reverse for
shutdown, the fixed-tick loop, the deferred frame queue, and the commit boundary the simulation's
state becomes authoritative at.

**The runtime does not own the loop.** It exposes `tick()`; the `while (running)` lives in the host
under `platform/host/`, so a platform that drives frames itself — mobile, web — is not precluded.
This is `design.md` §3 and it is an M0 invariant that M2 did not regress. Nothing in this directory
calls a host loop, and nothing in it can: `cy_runtime` does not link one and does not know one
exists.

**What belongs here**: the startup and shutdown sequence, the frame's stage ordering, the wiring of
servers, modules, the ECS world and its scene façade.

**What does not belong here**: a main loop, a `main()`, and any platform call not routed through
`Platform` or `DisplayServer`.

## Files

| File | Task | What it owns |
|---|---|---|
| `runtime.h` | 3.4.1, 4.1.2 | the eleven stages, the journal, `tick()` |
| `servers.h` | 4.1.1 | `ServerKind`, `Server`, `NullServer`, `ServerRegistry` |
| `simulation.h` | 4.1.1–4.1.3, 4.2.3 | the world, the tree, the schedule, the tick pipeline |
| `frame_commands.h` | 4.1.3 | the frame-scoped deferred queue |
| `state_hash.h` | 4.2.6 | the hash walk over an `ecs::World` |
| `features.h` | 4.1.4 | the build's feature table, as data rather than macros |

**A note on the namespace.** `Runtime`, `RuntimeConfig` and `StartupStage` are `cy::`, which is
where M0 put them; everything M2 added is `cy::runtime::`, which is the convention every other
module follows (`cy::ecs`, `cy::scene`, `cy::determinism`). Moving the first three would rename them
in every sample, host and test that names one, for no benefit this milestone could point at, so the
split is recorded rather than fixed under a milestone whose subject is something else.

## The startup sequence

`engine-architecture` fixes eleven stages and requires teardown in exact reverse. All eleven are in
`kStages` in `src/runtime.cpp`, in the specification's order, and **every one of them is entered on
every run** — including the stages whose subsystem does not exist yet. A stage with no work is still
a stage: its position is fixed now, so the milestone that fills it fills a slot rather than choosing
a new order.

| # | Stage | State | Filled by |
|---:|---|---|---|
| 1 | `platform` | crash handler, trace open, base's two diagnostic seams | — |
| 2 | `core` | — | M1: allocators, jobs, type registry, VFS, configuration |
| 3 | `modules-core` | M1: `config::ModuleRegistry` at level Core | — |
| 4 | `display` | window creation, and the `GraphicsApi::None` surface seam | M3: the real surface |
| 5 | `servers` | **M2: backend selection through `ServerRegistry`** | M3, M4, M7: real servers |
| 6 | `modules-servers` | M1 | — |
| 7 | `ecs-scene` | **M2: the world and its node façade come up** | — |
| 8 | `modules-scene` | M1 | — |
| 9 | `scripting` | — | M4, M5 |
| 10 | `editor` | M1: modules at level Editor | M5: the editor itself |
| 11 | `boot` | **M2: registration closes; the frame clock's origin** | M6: the startup scene |

The order taken is recorded in a journal; `shutdown()` walks it backwards and the two are checked to
be exact reverses **in every configuration**, then asserted on top of that. A stage that fails
unwinds every stage already entered, in reverse, and returns an error after a diagnostic naming the
stage — `engine-architecture`, "Failure during startup unwinds cleanly".

`probe/startup_order_probe.cpp` prints both journals and exits;
`tests/smoke/test_startup_order.cpp` runs it a hundred times and requires all hundred to agree.
Separate processes rather than a loop: a fresh address space each run is what would expose an order
that depended on an allocation address or a static initialiser.

### Why the simulation and the server registry are the host's, not the runtime's

Both arrive through `RuntimeConfig` as pointers the host owns, exactly as
`config::ModuleRegistry` does, and both for the same reason: **registration happens before startup.**

A game registers its components and systems between constructing the `Simulation` and starting the
runtime, and a simulation the runtime constructed would give it nowhere to do that. A module
registered at level `Core` adds a physics backend that the `Servers` stage then chooses, which
`engine-architecture`'s "Module registers a backend" scenario requires to happen "before the runtime
constructs the physics server". Null for either leaves its stage empty and the sequence unchanged.

## The frame

```
tick():
  sample the monotonic clock              <- the ONE place wall time enters the simulation
  pump the display's events
  N × step()                              <- N from the clock, bounded by max_ticks_per_frame
  alpha = the accumulator's exact residue
  frame(alpha)                            <- the variable half
```

`step()` is `Simulation`'s and is `simulation-and-determinism`'s commit-boundary list in order:
ingest commands, run the four fixed stages, drain tasks, merge structural buffers, commit events,
commit. A consumer of authoritative state gets no hook anywhere else — that is what makes "one
moment, many consumers" true rather than aspirational, and a second call site would require an edit
to `simulation.cpp`.

`frame(alpha)` runs the other four stages and the frame flush point. The alpha is *handed to* the
variable half rather than fetched by it, so the seam M3 fills is already the right shape: transforms
are rendered at the interpolated pose using an alpha they were given.

### The tick rate is an exact rational

`RuntimeConfig::fixed_step_ns` is gone. 1/60 s is 16 666 666.66… ns, so a step in nanoseconds is
wrong by a third of a nanosecond per tick — 1.2 ms per hour, and
`simulation-and-determinism`'s no-drift scenario is a session that runs for hours. The rate is now a
`determinism::TickRate` numerator and denominator, and the accumulator holds nanoseconds *times the
numerator* so that subtracting a step is exact. `src/core/determinism/README.md` has the arithmetic.

`--fixed-step <n>` is `TickMode::FixedStep`: exactly *n* ticks per frame whatever the wall clock
says, which is what makes a recorded run reproduce on a machine of a different speed. The tick cap
still applies — a mode that stepped around the loop's safety property would be a mode whose runs do
not reproduce under a realtime one.

Exceeding the cap discards the excess with a counter and a trace record. Lengthening the step to
catch up is on `simulation-and-determinism`'s forbidden list, and there is no field in
`RuntimeConfig` that would let it.

A `CloseRequested` event calls `Platform::request_exit()`. The runtime records the intent; the host
observes it and returns from `main()`.

## Two things thinner than they look

* **`src/servers/` does not exist.** `servers.h` is the registry, the selection chain and the null
  implementation; every one of the seven slots runs `NullServer` because there is no backend to
  register until M3. That is deliberate rather than premature — the fallback chain is where a
  subsystem quietly becomes non-optional, and writing it before there is a backend with opinions is
  the only time it is cheap. What the header *does* guarantee today is that `Server` names no
  entity, node, world or script, so `engine-architecture`'s "the server SHALL never dereference an
  ECS entity or a scene node" is a property of the interface rather than a rule to remember.
* **The frame command queue is single-producer.** It is recorded into and drained on the tick
  thread, and it is not a lock-free multi-producer queue. Every operation it carries is structural
  at the scene level and `ecs::World` is single-threaded for structural change by construction, so a
  worker recording one would be recording work that cannot be applied until the flush anyway — what
  a worker records instead is an `ecs::CommandBuffer` entry, which *is* per-worker and *is* merged
  deterministically. The `order` key on every entry is the extension point, and it is the recording
  system's registration index and never a worker or thread identity.

**Governed by**: `engine-architecture` (deterministic startup and shutdown, main loop, server
architecture, the ECS/scene duality, the deferred command queue, build-time feature slicing),
`simulation-and-determinism` (the commit boundary, the clock), `core-platform-abstraction` (the
platform does not own the main loop).
