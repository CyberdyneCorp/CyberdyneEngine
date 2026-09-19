# `src/core/memory/` — layer 0

The allocator interface every engine allocation passes through, the domains and budget tree that
account for it, the containers written against it, and the storage machinery M2's ECS is built on.

**Governed by** `core-memory-and-containers`. **Milestone** M1, tasks 2.1 – 2.12.

## What is here

| Task | What                                        | Header                                                          |
|------|---------------------------------------------|-----------------------------------------------------------------|
| 2.1  | Allocator interface and the concrete allocators | `allocator.h`, `system_allocator.h`, `arena.h`, `pool.h`, `slab.h`, `chunk_allocator.h`, `tracking_allocator.h` |
| 2.1  | Allocator propagation                       | `scope.h`                                                       |
| 2.2  | Memory domains and the budget tree          | `domain.h`, `budget.h`                                          |
| 2.3  | Pressure levels and the declared response   | `pressure.h`                                                    |
| 2.4  | Sequence and associative containers         | `array.h`, `ring_buffer.h`, `sparse_set.h`, `intrusive_list.h`, `hash_map.h`, `flat_map.h`, `hash.h`, `relocatable.h` |
| 2.5  | Handle pools                                | `handle_pool.h`                                                 |
| 2.6  | Chunked storage                             | `chunk_storage.h`                                               |
| 2.7  | Scratch and frame memory                    | `frame_memory.h`                                                |
| 2.8  | Retirement and frame epochs                 | `epoch.h`                                                       |
| 2.9  | Virtual reservation, ownership conventions  | `virtual_memory.h`, `ownership.h`                               |
| 2.10 | The general heap, by measurement            | `bench/heap_pattern.cpp`, and the numbers below                 |
| 2.11 | Memory diagnostics on the shared trace      | `diagnostics.h`                                                 |
| 2.11 | AddressSanitizer integration for the custom allocators | `sanitizer.h`                                        |
| 2.12 | Process-lifetime declarations               | `lifetime.h`, and `src/core/diagnostics/src/lifetime.h`         |
| M7 3.1 | Attribution by type, thread, world cell and asset | `attribution.h`, `TrackingAllocator::report_attribution`, `memory_log_attribution_report` |

`memory.h` includes all of it and is for a subsystem's own translation unit; a header that needs one
container should include that container.

## The three decisions worth reading before the code

**Failure is a null pointer at the allocator and a `cy::Expected` at the container.** The
specification says the allocator returns null and the caller surfaces an error, so `Allocator`
returns `void*` and `Array<T>::push_back` returns `cy::Status`. Growth is `[[nodiscard]]` everywhere,
which the engine's `-Werror` turns into a compile error when it is ignored.

**Accounting happens where memory is taken from the platform, not where it is handed out.** A
`SystemAllocator` records every block against its domain. An arena records its *reservation*, once,
and its bump allocations are free — which is both why the hot path costs nothing to attribute and
why the domain report says what a frame arena actually costs the process rather than the sum of
temporaries that were never separately resident.

**There is no virtual call on a hot allocation path.** `Allocator`'s public entry points are
non-virtual wrappers over protected virtuals, and every concrete allocator publishes its own
non-virtual, inline fast path: `ArenaAllocator::bump`, `StackAllocator::push`,
`PoolAllocator<T>::acquire`, `SlabAllocator::take`, `ChunkAllocator::acquire`,
`VirtualArena::bump`. Code with the concrete type in hand calls those.

## Chunked storage is memory, not an ECS

`chunk_storage.h` is the machinery `ecs-core` will be built on at M2, and it deliberately knows
nothing about components, archetypes or queries (design.md §7). The vocabulary is one level down:
an archetype's component set is a `ChunkLayout`'s **columns**, a component type is a `ColumnSpec` —
a size and an alignment, with no name and no type — and an entity id is the **key**, whose size the
layout is told and whose meaning it is not.

Do not add a component, an archetype or a query to this directory. That split is what keeps
`ecs-core` about entities rather than about memory.

## The general heap: the measurement, and the decision

Task 2.10, and `core-memory-and-containers` "General heap is an integration decided by measurement".

### How it was measured

`bench/heap_pattern.cpp` runs four workloads on a deterministic allocation sequence, so two runs
allocate in exactly the same order and differ only in the heap underneath:

| Workload | What it is |
|---|---|
| `general-churn` | 512 live blocks of 16–512 bytes per thread, freed in a shuffled order, four threads. The pattern a general heap is actually judged on. |
| `frame-arena` | The same number of allocations from a per-frame arena, reset every 512. What the engine does instead. |
| `pool-churn` | Fixed-size objects through `PoolAllocator`, acquired and released. |
| `mixed-frame` | One frame's shape: four large heap blocks, 64 pooled objects and 2,000 arena allocations, repeated. |

Every block is written to after it is handed out, because an allocator that returns cold pages is
not free and a benchmark that never writes hides that.

The candidate is compared by running **the same binary** under `LD_PRELOAD`, so the two runs differ
in nothing but the heap. Numbers below are from this machine — Linux 6.8, x86-64, GCC 13.3, the
`release` profile (`Shipping`: `-O3`, LTO) — as the best of five runs, which is the figure least
contaminated by scheduler noise.

### The numbers

Best and median of sixteen interleaved runs (`cy_memory_heap_pattern 1 4`: four threads), the two
heaps alternating so that any drift over the measurement lands on both. Lower is better.

| Workload | glibc 2.39 best / median | mimalloc 2.1.7 best / median | Difference (best) |
|---|---|---|---|
| `general-churn` | 65.77 / 89.84 ns/op | 64.60 / 73.61 ns/op | mimalloc 1.8% faster |
| `frame-arena`   |  3.00 /  4.76 ns/op |  3.33 /  4.44 ns/op | glibc 11.0% faster |
| `pool-churn`    |  1.48 /  1.69 ns/op |  1.49 /  1.68 ns/op | glibc 0.7% faster |
| `mixed-frame`   |  5.96 / 12.96 ns/op |  4.99 /  5.32 ns/op | mimalloc 16.3% faster |

**Read the second row first.** `frame-arena` makes exactly one heap call — the arena's reservation —
and 800,000 bump allocations. It CANNOT depend on the heap underneath, so the 11% between its two
columns is the measurement's noise floor and nothing else. Every difference in the table is at or
below that figure, in both directions.

The noise is the environment's and it is large: these runs were taken on a 24-core machine carrying
a load average near 27 from concurrent builds, and run-to-run spread within one heap reached 40% on
`general-churn`. Interleaving and best-of-sixteen is what makes the comparison meaningful at all,
and it is still only meaningful to about ten per cent. An earlier set taken at a load of about 10
gave the same shape with a tighter floor: `general-churn` 59.76 against 57.49, `frame-arena` 0.76
against 0.72 — a 3.8% candidate advantage against a 5.3% noise floor. **The conclusion did not
depend on which set was used**, which is the only reason it is reported from a loaded machine at
all.

**Repeated independently.** The comparison was run again from scratch on a quieter machine (load
average about 6, same binary, same mimalloc build), sixteen interleaved rounds at scale 2:

| Workload | glibc 2.39 best / median | mimalloc 2.1.7 best / median | Difference (best) |
|---|---|---|---|
| `general-churn` | 56.97 / 66.42 ns/op | 54.46 / 61.20 ns/op | mimalloc 4.4% faster |
| `frame-arena`   |  0.77 /  0.81 ns/op |  0.72 /  0.74 ns/op | mimalloc 6.5% faster |
| `pool-churn`    |  0.76 /  1.31 ns/op |  0.76 /  1.31 ns/op | even |
| `mixed-frame`   |  3.37 /  3.40 ns/op |  2.07 /  3.46 ns/op | mimalloc 38.6% faster |

Read `frame-arena` first again: 6.5% on a workload that makes ONE heap call is the floor, and the
4.4% on `general-churn` is under it. Same shape, same conclusion, from a third independent set. An
eight-round set at scale 1 taken minutes earlier put the floor higher still — `frame-arena` 11.3%,
`pool-churn` 18.6% — against 12.9% on `general-churn`. The figure that moves between sets is the
noise, not the difference.

The candidate is mimalloc 2.1.7, commit `8c532c32c3c96e5ba1f2283e032f69ead8add00f`, built Release
and loaded with `LD_PRELOAD` so that the two runs differ in nothing else. It is the allocator with
the strongest published results on multi-threaded small-block churn, which is the workload a general
heap is judged on, so it is the candidate most likely to beat the platform.

### The decision

**The platform allocator is retained. No dependency is added.**

The candidate's advantage on `general-churn` — 1.8% at best here, 3.8% on the quieter set — is
smaller than this measurement's own noise floor, and the two workloads that cannot depend on the
heap at all move by more than it does.

`mixed-frame` reads as a 16% win for the candidate and is the row to be most careful with, because
it is the one that looks like an argument. It is one frame's shape, and 2,064 of its 2,072
allocations per frame come from the arena and the pool: only eight reach the heap. Eight calls at
sixty-five nanoseconds is half a microsecond against a sixteen-millisecond frame — three hundredths
of one per cent — so a 16% difference in that row cannot be those eight calls, and its own spread
says what it is: glibc's median is more than twice its best. The row is measuring the machine, not
the heap.

That is the result design.md §6 anticipated: "If the platform allocator wins, that is the result — no
dependency is added, and the interface makes the decision reversible." It is a decision about THIS
engine's allocation pattern, not a claim about allocators: the reason the heap does not matter is
that `core-memory-and-containers` requires arenas, scratch, pools and chunks to cover the per-frame
pattern, and they do. An engine that reached the general heap per entity would get a different
answer from the same benchmark, which is the point of running it rather than choosing.

**What would change it.** Three things, each of which should re-run this benchmark rather than
argue:
* A real frame at M3, with a renderer and streaming in it, allocating in a shape this synthetic
  pattern does not predict.
* A platform whose heap is worse than glibc's. Windows and macOS are UNMEASURED — this is Linux
  only, and `core-memory-and-containers` asks for a benchmark "on target platforms", plural.
* A quiet machine. A 5% noise floor cannot resolve a 4% difference; a dedicated runner could.

**Recorded limitation.** Adopting a third-party heap needs a pinned entry in `deps/manifest.toml`,
which this module does not own. Had the measurement gone the other way, that entry — and not this
paragraph — would be the deliverable.

### If the decision is revisited

`bench/heap_pattern.cpp` is the benchmark to run, and the comparison is one `LD_PRELOAD` away. The
allocator interface is what makes the choice reversible: `SystemAllocator::heap_allocate` and
`heap_free` in `src/system_allocator.cpp` are the only two functions that name the platform heap, so
adopting a third-party allocator is a change to those two and an entry in `deps/manifest.toml`.

## Where this is thinner than the specification

Recorded rather than glossed, because M1's closing task asks what is thinner than the specification
and this is the module's own answer.

* ~~**Attribution has three axes of the five the specification names.**~~ **Closed at M7** (task
  3.1). `attribution.h` adds the other four — by type, by thread, by world cell, by asset — and
  `TrackingAllocator::report_attribution` groups live bytes by any of them.

  The axes are OPAQUE INTEGERS, and that is the layering rather than a shortcut: this module is
  below `cy::core-values`, `cy::core-reflect` and `cy::world`, so it cannot name `AssetId`, a
  reflected `TypeId` or a `CellId`, and the module that owns an identity is the one that pushes it
  through `MemoryAttributionScope`. A scope MERGES over the enclosing one, so a cell activation, a
  mesh loader inside it and a container inside that each add one axis without knowing the other two.
  The thread axis is the reverse — captured rather than declared, because asking a caller for it is
  asking it to get it wrong.

  A report says what it could not fit: `distinct_keys` against `rows`, plus `unreported_bytes` and
  `unattributed_bytes`, so `reported + unreported + unattributed == live_bytes()` and a reader can
  tell "nobody declared this axis" from "the table was too small".
* **Capture is a call SITE, not a call STACK.** `CaptureMode::Off / Sampled / Full` is the declared
  mode the specification asks for, and it selects how often a record is kept, but the record holds
  the file, function and line pushed by `CY_ALLOCATION_SITE` rather than an unwound stack. A real
  stack needs the platform's unwinder, which is `src/core/platform/`, not this module.
* **`MemoryDomain::Gpu` is budgeted and nothing reports into it.** "GPU memory SHALL participate in
  the same pressure model" is structurally satisfied — the domain, its budget and its pressure
  contribution all exist — and untested against a real allocator until M3.
  **Closed at M11.d**: `VulkanDevice` and `NullDevice` both report their device-heap level into the
  domain now; see the M11.d section at the end of this file.
* **Paged subsystems reduce together** is the residency layer's scenario and is M6. `PressureMonitor`
  broadcasts the level every one of them will respond to; nothing here weighs their reductions
  against each other, and nothing should.
* **Windows and macOS are UNVERIFIED.** `src/virtual_memory_windows.cpp` has never been compiled;
  everything else is standard C++ or POSIX. Linux only on this machine.

## What another agent must do — files this module does not own

1. **`src/core/CMakeLists.txt` registration order.** This module depends publicly on
   `cy::core-values` (handle pools compose `cy::GenerationTable` rather than reimplementing the
   generation scheme — `<cy/core/values/handle.h>` names task 2.5 from the other side) and privately
   on `cy::core-diagnostics` (task 2.11 puts the counters on the M0 trace). `add_subdirectory(memory)`
   currently runs before both. CMake resolves `cy::*` at generate time so the link is correct either
   way, but that file's comment calls its list a dependency order. Suggested order:
   `base, math, reflect, values, platform, diagnostics, memory, jobs, assets, config`.

2. **`just/diagnose.just`** should gain a `diagnose-memory` recipe running
   `cy_memory_heap_pattern`, in the same shape as `diagnose-overhead`. The target is built by
   `CY_BUILD_TESTS` and is at `<build>/src/core/memory/cy_memory_heap_pattern`.

3. **`tests/CMakeLists.txt` or `src/core/diagnostics/`** — the last of task 2.12, and the nine
   suites it still leaves red under `CY_SANITIZE=address`: `integration.values_diagnostics`,
   `integration.jobs_diagnostics`, the five `diagnostics.*` suites that open a trace, and
   `smoke.empty_sample`. One `LSAN_OPTIONS` line in `tests/CMakeLists.txt` closes all nine; two
   calls to `cy::declare_process_lifetime` in `claim_slot` close them properly. See below.

## Task 2.12: the trace's process-lifetime rings

The mechanism is `cy::declare_process_lifetime(pointer, bytes, tag)` in `lifetime.h`. One call tells
both detectors: the entry goes into the registry the engine's own leak report reads (so
`TrackingAllocator::report_leaks` counts it as intentional and excludes it), and, in a build with
LeakSanitizer, `__lsan_ignore_object` is called for the same pointer. It names one pointer of a
known size with a tag — a declaration, not a suppression.

**What is still not adopted, and why this module cannot adopt it.** The allocation LeakSanitizer
reports is `new (std::nothrow) ThreadSlot()` in `src/core/diagnostics/src/trace.cpp` (`claim_slot`),
with the ring buffer under it from `ThreadRing::initialize`. Slots outlive the threads that used
them on purpose. They look unreachable at exit because `System::slots` is a `std::vector` inside a
function-local static, whose destructor runs before LeakSanitizer's check.

`src/core/diagnostics/` does not link `cy::core-memory` — it sits *below* it, since memory reports
onto the trace — so `claim_slot` cannot reach this module's declaration without closing a cycle.

**Closed, in the module that allocates.** `src/core/diagnostics/src/lifetime.h` carries the tool half
of this mechanism: `cy::diag::declare_process_lifetime(pointer)`, three lines around
`__lsan_ignore_object`, empty in an uninstrumented binary. `ThreadRing::initialize` declares its
buffer and `claim_slot` declares the slot, both at the allocation site. Nothing is suppressed by
pattern, so a second ring allocated every frame by a defect is still a finding, and no test needs an
`LSAN_OPTIONS` of its own. The suppression file this section used to describe is gone.

The general mechanism in this module, `cy::declare_process_lifetime(pointer, bytes, tag)`, remains
the one to use anywhere above layer 0's diagnostics floor: it tells LeakSanitizer *and* the engine's
own leak report, so a declared allocation is attributable in a build with no sanitizer at all.

Verified: the whole 47-test suite under `CY_SANITIZE=address,undefined` with `detect_leaks=1`. Before
the declaration, thirteen suites were red — `integration.values_diagnostics`,
`integration.jobs_diagnostics`, `integration.assets_io`, `integration.assets_loading`, the seven
`diagnostics.*` suites that open a trace, `smoke.empty_sample` and `smoke.headless_host` — every one
reporting the same two frames, `cy::diag::ThreadRing::initialize` and `claim_slot`. After it, none.

## Sanitizers

| Build | Suites | Result |
|---|---|---|
| `CY_SANITIZE=address` | all four memory suites | clean |
| `CY_SANITIZE=address,undefined` | all four memory suites | clean |
| `CY_SANITIZE=thread` | all four memory suites | clean, under `setarch $(uname -m) -R` |

### The allocators route through the tool's interface

`sanitizer.h`, and the "Sanitiser build" scenario of the diagnostics requirement. AddressSanitizer
knows about the blocks the platform heap hands out; an arena takes one such block and carves ten
thousand allocations from it, so without help the whole region is one valid object and a write off
the end of a bump allocation is not a finding. `poison_memory` / `unpoison_memory` mark what an
allocator owns but has not handed out, and `ArenaAllocator`, `StackAllocator`, `SlabAllocator`,
`PoolAllocator` and `ChunkAllocator` call them: the alignment padding between two bump allocations,
everything past a reset, a pool block on the free list and a chunk on the free list are all
use-after-poison rather than silently valid memory. Every function compiles to nothing in a binary
that is not instrumented, so the calls on the hot paths cost the shipping build nothing.

`VirtualArena` is deliberately excluded, and the header says why: its bytes are mapped rather than
allocated, and shadow state for an address range that is later unmapped is not reliably reset for
the next mapping.

`tests/test_allocators.cpp` asserts the shadow state directly through `memory_is_poisoned` rather
than by provoking an abort, so the case runs in every build and needs no second process. It found
one thing immediately: the "use after reset" case in `test_lifetimes.cpp` read the arena's byte
pattern back, which is itself the use-after-reset the tool now reports, so that case asks the shadow
under ASan and reads the byte pattern otherwise.

### Defects the sanitizers found

Two defects were found by running them and are fixed here, each with a regression test:

* `Array::erase` closed its gap with the same helper `Array` uses to grow — a single `memcpy`, on
  ranges that overlap. AddressSanitizer reported `memcpy-param-overlap`. Fixed by
  `detail::relocate_overlapping`; the regression case erases from a 64-element array so the overlap
  is longer than one element.
* `TrackingAllocator`'s double-free detection read the freed block's header, which is memory the
  upstream allocator has taken back — a genuine use-after-free that ASan reported. Fixed by a
  bounded ring of recently freed pointers, checked before anything is dereferenced. Its one limit is
  documented at the declaration.

`RefCounted::release_ref` acquires with a load on its own counter rather than
`std::atomic_thread_fence`, because GCC's ThreadSanitizer rejects the fence outright
(`-Werror=tsan`), which would have put this module's suites out of reach of the tool most likely to
find a bug in it.

## M11.d: the attribution axes have producers, and the larger finding underneath them

`core-memory-and-containers` is claimed **Complete** at M11.d. Its one named absence was that the
four attribution axes M7 built had **no producer**: the only files naming `MemoryAttributionScope`
were its own header, source, test and README, so in any running engine every axis reported its live
bytes as `unattributed_bytes` and nothing else.

### The producers, and where they are

The axis is opaque here by design — this module is layer 0 and cannot name an `AssetId`, a `CellId`
or a reflected `TypeId` — so **the module that owns the identity is the one that pushes it**. Three
now do:

| Axis | Producer | Why that call and not another |
|---|---|---|
| Asset | `AssetSystem`'s read stage and its decompress-and-publish stage (`asset_system.cpp`) | the two stages that allocate an asset's bytes. Pushed twice because they run on different threads and a scope is thread-local |
| World cell | `CellActivation::advance()` and `::publish()` (`src/world/src/activation.cpp`) | private staging is where a cell's memory is spent — decoded columns, overlay patches, entity lists — and publication grows the chunks that hold it |
| Type | `World::ensure_sparse_store()` (`src/ecs/src/world.cpp`) | a sparse component's side table holds exactly **one** component type, so it can honestly say which. The key is the *reflected* `TypeId`, not the world-local `ComponentTypeId`, so two worlds' reports join |

**An archetype chunk deliberately pushes nothing, and that is a measurement rather than an
omission.** A chunk holds a component *set*; charging its bytes to any one member would be a number
that reads as a fact and is not. The type axis therefore covers sparse storage and not chunk
storage, and a reader of a type report should know that before drawing a conclusion from it.

The tests are the producers' tests and not the mechanism's: *"a sparse component's side table is
attributed to its reflected type"* (`unit.ecs`) and *"cell activation attributes its staging to the
cell that spent it"* (`integration.world_activation`) run the ordinary write paths on a
`TrackingAllocator` and read the axis back. The assertion that matters in both is
**`unattributed_bytes == 0`**: before these scopes existed every one of those bytes was in that
figure and none was in a row. Both cases were verified against the defect — with the scope removed,
the cell case fails on `unattributed_bytes` (17 824 bytes unattributed) and the type case on the row
being absent.

### The finding underneath: attribution has no producer *and* no consumer

Closing the producers exposed the bigger half, which no requirement text names and which is the same
firewall shape M8.c found in two other modules:

* **Nothing in the engine installs a `TrackingAllocator`.** It is the only allocator that records an
  attribution — `SystemAllocator` records the domain and nothing else — and every reference to it
  outside this module is in a test. So a scope pushed in a shipping or development *engine* is
  recorded by nobody today; it is recorded the moment something wraps an allocator, which is what
  the two new tests do.
* **`memory_log_attribution_report`, `memory_log_report`, `memory_log_leak_report`,
  `memory_trace_report`, `memory_trace_pressure`, `memory_trace_budget_violation` and
  `memory_trace_eviction` have no caller outside this module's own tests.** The requirement's
  "Allocation and free events, pressure transitions, and budget violations SHALL be emitted into the
  shared trace" is therefore implemented and unreached: the functions exist, they work, and no frame
  loop, tool or `just diagnose-*` recipe calls one.
* **`MemoryDomain::Gpu` was budgeted and no backend reported a byte into it — closed here for the
  two backends that exist.** `budget.cpp` gives it 768 MiB soft on desktop and 192 MiB hard on the
  constrained profile, and until M11.d the budget was compared against zero. `VulkanDevice` and
  `NullDevice` now record their device-heap level into the domain from `publish_memory_pressure()`,
  as a delta against the level they last reported, so `live_bytes` tracks the device rather than
  accumulating. The producer's test is *"publishing memory pressure reports the device heap into
  MemoryDomain::Gpu"* (`unit.rhi`), which brackets one call that allocates nothing — so the
  difference it measures cannot be host memory — and it was verified against the defect: with the
  reporting disabled, the released-heap assertion fails. **Metal and D3D12 owe the same two blocks
  when they are written**, which is M11.d.5's, and this paragraph is where that rung should read the
  shape from.

What closing this needs, stated so the next reader does not re-derive it: one place in the runtime
that owns a `TrackingAllocator` over the process allocator in development builds, and one caller per
report — a `just diagnose-memory` recipe and a frame-loop hook are the two obvious ones. It is a
change to `src/runtime/` and to `just/diagnose.just`, not to this module.

### The row at Complete grade

| Requirement | Verdict | Evidence, and what is missing |
|---|---|---|
| Allocator interface | **satisfied** | `allocator.h`: allocate/deallocate/reallocate with alignment, domain and tag on every allocator, no virtual dispatch on the arena's hot path |
| Memory domains | **satisfied** | `domain.h` + `DomainStats` in every build; the hierarchy is tested for containment |
| Memory budget tree | **satisfied** | `budget.h`, per-platform profiles, over-subscription refused at startup |
| Memory pressure levels | **satisfied, with one caller** | `pressure.h` and its history; `update_memory_pressure()` is called from the two RHI devices and from nowhere else, so the level moves only when a graphics backend is up |
| Allocator propagation | **satisfied** | `scope.h`'s thread-local stack; containers default to `current_allocator()` |
| Sequence containers | **satisfied** | `Array`, `RingBuffer`, `FixedArray`, `relocatable.h`'s trait, no copy-on-write |
| Associative containers | **satisfied** | `FlatMap` and `HashMap` with deterministic iteration order for serialization |
| Handle pools | **satisfied** | `handle_pool.h`, generational, pointer-stable under growth |
| Chunked component storage | **satisfied** | `chunk_storage.h`, and `src/ecs/` allocates every chunk through it rather than owning a second allocator |
| Scratch and frame memory | **satisfied** | `frame_memory.h`, `arena.h`; poisoned on reset in development builds |
| Retirement and frame epochs | **satisfied** | `epoch.h`, one mechanism with several consumers, depth reported |
| Virtual address reservation | **satisfied** | `virtual_memory.h`, POSIX and Windows implementations both present |
| Reference-counted shared data | **satisfied** | `ownership.h`'s `Ref`/`RefCounted` with a release policy; the asset system is its heaviest user |
| Ownership conventions | **satisfied** | `UniquePtr` with allocator-aware deleters; the `mount_owned` note in `vfs.h` is the convention's sharpest edge |
| General heap is an integration decided by measurement | **satisfied** | the measurement and its numbers are above in this file; `bench/heap_pattern.cpp` is what makes a regression visible |
| Memory diagnostics | **partial, and the axes are only half of why** | per-tag and per-domain figures, leak reporting with call sites, red zones, poisoning, double-free detection, sampled call-stack capture and the four attribution axes all exist and are tested. **The axes now have producers, and `MemoryDomain::Gpu` now has one** (above). What is still unmet: nothing installs a tracking allocator, and no report has a caller outside tests |
