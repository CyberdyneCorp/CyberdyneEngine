# `tests/harness/`

The framework seam and the fixtures. Every test binary links `cy::test-harness`, and this is the
only directory permitted to name doctest.

| File | What it is |
|---|---|
| `include/cy/test/test.h` | `CY_TEST_CASE`, the assertions, and the budget guard. The one include a test needs. |
| `include/cy/test/fixtures.h` | The injectable fixtures: a deterministic clock, a seeded generator, a temporary directory. |
| `src/main.cpp` | doctest's `main`, so no test file carries one. |
| `src/budget.cpp` | The per-test budget check: the CPU clock, the wall-clock stall ceiling, and the contention clock that separates the two. |
| `src/host_blocking.cpp` | The fourth clock: a sampler that sees the case's thread blocked by the host — on the disk or a page fault — and why it is the instrument used. |
| `src/fixtures.cpp` | The filesystem half of the fixtures. |

## Why a wrapper

`design.md` §5 chose doctest on a compile-time argument — an argument about today's numbers at
today's scale. The wrapper is what keeps that choice reversible: replacing the framework is a change
to this directory, not to every test in the tree. The seam is enforced twice, at configure time and
at run time, because a seam nobody checks is a seam that has already been crossed.

## The four clocks, and why the budget needs all of them

A case is timed by **CPU time**, which is the budget, and by **wall clock**, which is the stall
ceiling — a hundred times the budget, there to catch a case that is waiting rather than working: a
sleep, a blocking read, a lock, a thread it joined. M9's closing gate added a third, and task 7.5b
says why: wall clock is a property of the machine as much as of the test, and the ledger's own load
was tripping the ceiling. Three wall-clock-bound suites each failed once across three full ledger
runs and each passed three to five times in isolation on an idle machine.

`testing-and-quality` already had the answer — "a case that exceeds its budget only under load SHALL
be reported as a case to reclassify rather than failing the build outright" — and no tolerance could
deliver it, because a case descheduled by forty spinning compilers and a case sleeping on a lock look
identical in wall clock.

They do not look identical in `/proc/thread-self/schedstat`, whose second field counts the
nanoseconds a thread spent **runnable and not running**. Preemption grows it; blocking does not. So
the ceiling is applied to wall clock *minus* contention, a case over the ceiling on a busy machine is
reported on stderr and counted by `contended_cases()`, and a case that slept past it still fails.

M11.c's fourth close found the half that clock cannot see: a machine busy with **I/O** rather than
CPU. `unit.determinism`'s first case held the suite for 655 ms with 0.21 ms of CPU and *zero*
runqueue wait beside a heavy build, and passed three runs in three alone. A thread waiting for the
disk — a read, or a major page fault on code a build pushed out of the page cache — is not runnable,
so it accumulates no runqueue wait. It is, however, in an **uninterruptible** sleep (`D`), which a
sleep, a futex, a join and a pipe read are not (`S`). So a sampler thread reads the case thread's
state from `/proc/self/task/<tid>/stat` every one to five milliseconds while the case runs, and the
time it sees in `D` is subtracted too. `host_blocking.cpp` records why the other instruments were
rejected — delay accounting is off by default since Linux 5.14 and needs root to enable, and
`/proc/pressure/io` is neither per thread nor proportional to one thread's wait — and the limits of
this one: it is a sample, `D` also covers kernel locks, and a case's own synchronous disk I/O is
excused with the host's.

| Clock | Reads | Fails a case when |
|---|---|---|
| CPU | `CLOCK_THREAD_CPUTIME_ID` | the case spends more than its kind's budget, scaled to this machine |
| Wall | `steady_clock` | the case's own time over the window exceeds a hundred times that budget |
| Contention | `/proc/thread-self/schedstat` | never — it is subtracted, and `budget_measures_contention()` says whether it exists |
| Host blocking | `/proc/self/task/<tid>/stat`, sampled | never — it is subtracted, and `budget_measures_host_blocking()` says whether it exists |

The decision is `stall_verdict()`, a pure function of three numbers — wall clock, the host's share
(runqueue wait plus host blocking) and the ceiling — so the arithmetic is tested without arranging
for a machine to be busy. The measurements it rests on are asserted separately:
`tests/unit/harness/test_budget.cpp` shows that a sleeping case accumulates neither contention nor
host blocking, and `tests/integration/test_budget_contention.cpp` shows that a preempted case
accumulates contention, that a case waiting on uncached reads or on major page faults accumulates
host blocking and is excused, and that a case which sleeps while another thread keeps the same disk
busy, or which burns its own CPU, accumulates none and is still a stall.

## What the harness does not have yet

`testing-and-quality` names a fuller set than this: scene and world fixtures, a mock platform and
display server, an in-memory filesystem mount, network condition simulation, image comparison, and
state hashing. Each is a mock or a comparison of an interface that does not exist yet, and a mock
written before its interface is a guess that has to be rewritten.

| Fixture | Arrives with |
|---|---|
| Mock `Platform` and `DisplayServer` | M0's headless implementations, once `core-platform-abstraction` has landed |
| World and scene fixtures | M2, with the ECS and the scene graph |
| In-memory filesystem mount | M2, with the asset pipeline's virtual filesystem |
| Image comparison | M3, with the renderer and `tests/render/` |
| State hashing, network conditions | M9, with determinism and replication |

**Governed by**: `testing-and-quality`.
