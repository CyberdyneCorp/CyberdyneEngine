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
| 2.12 | Process-lifetime declarations               | `lifetime.h`, `lsan.supp`                                       |

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

3. **`tests/CMakeLists.txt` or `src/core/diagnostics/`** — the last of task 2.12. See below.

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

`src/core/diagnostics/` does not link `cy::core-memory`, so `claim_slot` has no way to reach the
declaration. Two ways to close it, either of which is a one-line change to a file this module does
not own:

* **Preferred** — `src/core/diagnostics/` links `cy::core-memory` (both are layer 0, so the layer
  check permits it) and `claim_slot` calls
  `cy::declare_process_lifetime(slot, sizeof(ThreadSlot), "trace.thread_slot")` and the same for the
  ring's buffer. The declaration then shows up in the engine's own report as well.
* **Otherwise** — `tests/CMakeLists.txt` sets `LSAN_OPTIONS=suppressions=<source>/src/core/memory/lsan.supp`
  on the suites that open a trace. `lsan.supp` names exactly two functions and nothing broader.

Verified here: with `LSAN_OPTIONS=suppressions=src/core/memory/lsan.supp`, all five diagnostics
integration binaries exit zero under `CY_SANITIZE=address`; without it, `cy_diag_test_trace` reports
`197376 byte(s) leaked in 6 allocation(s)`. LeakSanitizer's own `print_suppressions=1` confirms both
templates matched and nothing else did. The memory suites are green under ASan either way — they
have nothing to declare.

Note the spelling in `lsan.supp`: `claim_slot` carries no namespace because it has none in the
symbol — it is in an anonymous namespace, so the frame LeakSanitizer matches against is the bare
name, and `cy::diag::claim_slot` matches nothing.

## Sanitizers

| Build | Suites | Result |
|---|---|---|
| `CY_SANITIZE=address` | all four memory suites | clean; the diagnostics suites need `lsan.supp` above |
| `CY_SANITIZE=address,undefined` | all four memory suites | clean |
| `CY_SANITIZE=thread` | `unit.memory`, `memory_containers`, `memory_threads` | clean, under `setarch $(uname -m) -R` |

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
