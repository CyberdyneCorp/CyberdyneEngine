# `src/core/jobs` — the job system, coroutines, and the concurrency vocabulary

Layer 0, target `cy::core-jobs`, headers `<cy/core/jobs/*.h>`, namespace `cy::jobs`.
Governed by `core-jobs-and-concurrency`, which reaches **Working at M1** and **Complete at M11.d**.

**This README was written at M11.d and its subject is the Complete-grade audit below.** The module
had no README before — every other module under `src/core/` has one — so what is here is the map a
reader needs and then the row read requirement by requirement, rather than a retrospective of ten
milestones this file was not present for.

## The map, in one paragraph per file

* `job_system.h` — the one `JobSystem`. Per-worker deques with work stealing, five priority classes,
  named jobs, explicit dependencies, `wait`/`wait_all` that **participate** rather than block, a
  deterministic mode, a chaos mode, and the watchdogs that report a blocked worker, an overlong task
  and a task that has not observed its cancellation.
* `async.h` — `AsyncService`, the one thread where blocking is legal. A worker that needs a file
  read submits it here and goes back to its deque; the completion releases a gated job. Timers live
  here too, because a third dedicated thread would be a thread that sleeps.
* `coroutine.h` — C++20 coroutines as the asynchronous model, with frames from per-worker slabs and
  symmetric transfer so a chain of a thousand awaits is not a stack a thousand deep. A fiber runtime
  is a non-goal and there is no stack switching anywhere in the file.
* `parallel.h` — `parallel_for`, `parallel_reduce`, `parallel_scan`, `parallel_sort`, with a fixed
  combination order so a floating-point reduction is reproducible.
* `schedule.h` + `access.h` — declared access sets (`Read`/`Write`/`Exclude` per component, plus
  resources and events), the conflict graph built from them, and the deterministic order.
* `cancellation.h`, `context.h`, `scratch.h` — the task context: worker index, scratch allocator,
  cancellation token.
* `sync.h` — `Thread`, `Mutex`, `RecursiveMutex`, `RwLock`, `Semaphore`, `ConditionVariable`,
  `SpinLock`, `Event`, `Atomic<T>`, `AtomicFlag`, `AtomicRefCount`.
* `command_queue.h`, `double_buffer.h` — the two cross-thread mechanisms the specification prefers
  to locks.
* `thread_role.h` — the seven roles and the assertion macro. **See the audit: it has no caller.**
* `diagnostics.h` — job statistics, the shared-trace events, and the **critical path**.

## M11.d: `core-jobs-and-concurrency` read requirement by requirement

Sixteen requirements, read at Complete grade. `unit.jobs` and its siblings carry **107 cases** over
thirteen source files, which is why most rows below are one line: the mechanism exists and a case
runs it. The rows worth a reader's time are the three that are not.

| Requirement | Verdict | Evidence, and what is missing |
|---|---|---|
| Single job system owns all worker threads | **satisfied** | `JobSystem`; worker count defaults to `hardware_concurrency() - 1` (`job_system.cpp:237`); `submit`, `submit_parallel_for`, dependency spans, `wait`/`wait_all`, five priorities, named jobs, per-worker deques with stealing. A waiting worker **participates**, which is what makes recursive submission deadlock-free |
| Thread roles | **PARTIAL, and this is the finding of this audit** | the mechanism is complete — seven roles, `set_thread_role`, `require_thread_role`, `CY_ASSERT_THREAD_ROLE`, a violation counter. **Nothing outside this module calls any of it.** Two roles are ever set, both from here: `Worker` (`job_system.cpp:791`) and `AssetIo` (`async.cpp:104`). Main, Simulation, Render and Audio are never declared by the runtime, the renderer or the audio backend, and `CY_ASSERT_THREAD_ROLE` has **zero call sites** in `src/` outside this module. So the requirement's own scenario — "RHI object touched off the render thread → a development-build assertion fires naming the role" — cannot fire: no thread claims the role and no RHI entry point checks one |
| Parallel system execution is safe by construction | **satisfied** | `access.h` declarations, the per-stage conflict graph in `schedule.cpp`, undeclared access caught in development builds, structural changes recorded into per-thread command buffers and applied at the flush point |
| Task context | **satisfied** | worker index, scratch allocator, cancellation token on every task; task records and coroutine frames from per-worker slabs, with `coroutine_frame_stats()` reporting the fallback count so "scheduling does not allocate" is a number |
| Coroutines are the asynchronous model | **satisfied** | `Task<T>`, symmetric transfer, slab frames, `-fno-exceptions`-clean allocation failure. Awaitable: another task, an `AsyncService` completion, a gated `FenceSignal` (the GPU-fence shape), a timer, a cancellation |
| Workers never block on I/O or the GPU | **satisfied** | `begin_blocking_region` **fails on a worker thread and counts the refusal in every configuration**, so the rule is enforced rather than asserted out of the build that would discover it; `AsyncService` is the alternative; the blocked-worker watchdog names the task |
| Cancellation | **satisfied** | cooperative tokens, propagation to children, and the unresponsive-cancellation watchdog with its own counter (`unresponsive_cancellations`) |
| Priority classes, fairness, and deadlines | **satisfied** | five classes exactly as specified, anti-starvation in the scheduler, deadline hints that deterministic mode ignores |
| Long-running work is chunked | **satisfied at the mechanism, untested above it** | `Background`/`Idle` priorities, cancellation, and the overlong-task watchdog are all here; whether a *particular* long job (cooking, navmesh build, light bake) is actually chunked is that module's property and not this one's |
| Deterministic scheduling mode | **satisfied** | fixed topological order, fixed partitioning, ordering keys that exclude worker identity, a single-threaded deterministic mode, and a chaos mode |
| Deterministic parallel primitives | **satisfied** | the four primitives, fixed combination order, per-worker buffers committed in declared order |
| Synchronisation primitives | **satisfied** | every type the requirement names is in `sync.h` |
| Command queue for single-threaded servers | **satisfied** | `command_queue.h`: by-value trivially relocatable arguments, inline limit with arena spill, fire-and-forget and synchronous submission |
| Double buffering across thread boundaries | **satisfied** | `double_buffer.h` with an explicit swap point |
| Frame pacing and synchronisation points | **partial, and by module boundary rather than by omission** | the stage flush points are the scheduler's and exist; `frames_in_flight` and the fence wait are the RHI's (`backends/rhi/device.h`), which is where the requirement's only scenario lives. Nothing in this module can assert it |
| Concurrency diagnostics | **partial** | job statistics, per-job durations, queue depths, steal counts, the shared-trace events and **the critical path with its constituent tasks** are all implemented and tested. Two pieces are not this module's and are not done: the **visual** frame timeline is a viewer over the trace, and **TSan integration** is a build option (`cmake/sanitizers.cmake`) rather than anything here. The throughput thresholds this module's own benchmarks assert on remain loose — `docs/roadmap/open-debts.md` has carried that since M2 and it is unchanged |

### The one thing a later milestone should do here

**Give the thread-role mechanism its producers and its first consumer.** The cheapest honest version
is four lines and one assertion: the runtime declares `Main` (and `Simulation` where they share a
thread) in `Runtime::startup`, the render thread declares `Render` where one exists, the audio
backend declares `Audio`, and one RHI entry point — command recording is the natural one — calls
`CY_ASSERT_THREAD_ROLE(ThreadRole::Render, ...)`. Until then the module ships a rule it can state
and cannot enforce, which is the same shape as an attribution axis nobody pushes and a report
nobody calls.
