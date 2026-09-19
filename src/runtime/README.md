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

## M11.d: `engine-architecture` read requirement by requirement, and what the port cost

`engine-architecture` is claimed **Complete** at M11.d. It is one of the three rows this rung audits
that carry **no named blocker** — which means nothing has refused them rather than that nothing is
missing. Ten requirements, read against the tree.

| Requirement | Verdict | Evidence, and what is missing |
|---|---|---|
| Layered architecture | **satisfied, and enforced rather than advised** | `tools/layercheck/layercheck.py` behind `just quality-layers`, run in CI; the rule that refuses an SDL type above `platform/` and a graphics-API header above `src/backends/` is the same mechanism |
| C++20 as the implementation language | **satisfied** | concepts (`values/handle.h`), `Span`, designated initialisers in descriptor structs, `constexpr` tables, `<=>`, and `Expected<T, Error>` for fallible calls with assertions for programmer error |
| Server architecture | **PARTIAL, and it has been partial since M2** | `ServerRegistry`, the selection chain, `NullServer` and the handle semantics are all here and well tested (`test_servers.cpp`). **No production backend registers into it**: every `register_backend` call in the tree outside that test file belongs to the RHI's *own* backend registry, which is a different mechanism one layer down. `src/servers/` now holds seven real servers — render, physics, audio, input, camera, text, residency — and none of them is reached through this registry; `samples/03-first-light/main.cpp` says so in its own comment (*"wiring `cy::render::RenderServer` into `runtime::ServerRegistry` is the piece…"*). So "Backend is selectable" is exercised by test doubles, and `Runtime::enter_servers` resolves every kind to the null fallback in any real run. M3 was the milestone that was supposed to close this; it is eight rungs later and unchanged |
| ECS core with a scene-graph façade | **satisfied** | `src/ecs/` is the storage and `src/scene/` is the view (`node.h`, `coherence.h`, `propagation.h`); the coherence rules have their own module and their own suite |
| Module system | **satisfied at the mechanism, with one module in the tree** | `modules/example-null/module.json` carries every field the requirement names — name, description, layer, type, registration level, public and private dependencies, default-enabled, platforms, hot-reload — and discovery is from the manifest with no list to add to. `cmake/modules.cmake` refuses a module that links what its manifest does not declare. The residual is coverage, not design: one module exists, so "third-party module" and "module registers a backend" are demonstrated by the template rather than by a second author |
| Deterministic startup and shutdown | **satisfied** | the nine-stage table in `runtime.cpp`, torn down in exact reverse, with `unwind()` releasing what a failed stage had already entered; headless startup runs through `platform/headless/` |
| Main loop with fixed simulation and variable rendering | **satisfied** | zero or more fixed steps then one render, a bounded catch-up with the discarded time reported, interpolation for rendering, and `--fixed-step` |
| Deferred command queue | **satisfied** | `frame_commands.h` at the runtime level and `ecs/command_buffer.h` below it |
| Build-time feature slicing | **satisfied** | `cmake/features.cmake` defines a guard macro per option and records, per option, which milestone delivers it; `CY_DEDICATED_SERVER` is the requirement's own scenario as a single switch |
| Non-goals for the initial architecture | **satisfied** | the list is in the specification and every change to it has gone through the OpenSpec flow — M11.d's own scope change moved two whole sections into a new rung by that route rather than by narrowing a requirement in place |

### What the port cost above layer 3

`engine-architecture`'s claim at this rung is a **measurement, not a promise**: a second native
platform backend is the largest architectural stress this engine has had, and the number worth
recording is how much of `src/` above `platform/` and `src/backends/` had to change to absorb it.

The exit criterion for the native platform backend is stated as a diff — *"requiring no change in
`src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`"* — and the check that reads a changeset is
section 4's, not this file's. What this section records is the wider figure, over the whole of `src/`
above the two layers a port is allowed to touch.

**The method**, stated so the next port produces a comparable number:

    files changed under src/, excluding src/backends/, in the changeset that adds the backend —
    counted per directory, each non-zero count attributed to the change that forced it

**The measurement, taken over M11.d's own changeset** (`git diff <rung start>..` over `src/`,
excluding `src/backends/`, and attributed by subject rather than by author):

| Directory | Files changed | Attributed to |
|---|---:|---|
| `src/rendering/` | 17 | **the RHI interface**, not the port: `ImageLayout` → `ImageUse` and `queue_family` → `QueueOwner` in the render graph's compile, execute and visualise paths. Section 1's work, on Vulkan and null, before any new backend exists |
| `src/core/`, `src/ecs/`, `src/world/` | 12 | **this rung's own section 6**: the remote-file transport, the package-backed reload, and the three memory-attribution producers. Not port cost either |
| **Attributable to the native platform backend** | **0** | `platform/linux-native/` and `platform/stub/` are new directories; **nothing under `src/` changed to accommodate either** |

**Zero is the honest number, and it is the one the row is claimed on.** A second native `Platform`
and `DisplayServer` — a different windowing model, a different input source, a different surface
provider — was absorbed entirely below layer 3. `src/servers/` and `src/scene/` have no change of
any kind in this rung's changeset.

A non-zero count would not automatically have been a failure. A change under `src/rendering/` to
branch on a capability a new platform exposes is the abstraction working; a change under `src/core/`
is the abstraction being wrong, which is what `platform/README.md` already says and what section
4.2's criterion mechanises. What the table above shows is that the only non-zero rows belong to an
*interface* change made deliberately and to this section's own work — neither of which is the port
reaching upward.
