# `tests/harness/`

The framework seam and the fixtures. Every test binary links `cy::test-harness`, and this is the
only directory permitted to name doctest.

| File | What it is |
|---|---|
| `include/cy/test/test.h` | `CY_TEST_CASE`, the assertions, and the budget guard. The one include a test needs. |
| `include/cy/test/fixtures.h` | The injectable fixtures: a deterministic clock, a seeded generator, a temporary directory. |
| `src/main.cpp` | doctest's `main`, so no test file carries one. |
| `src/budget.cpp` | The per-test budget check: the CPU clock, the wall-clock stall ceiling, and the contention clock that separates the two. |
| `src/host_blocking.cpp` | The fourth clock: a sampler that sees the case's thread in an uninterruptible wait, and the allowance that excuses only the part of it the host's I/O pressure accounts for. |
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
state from `/proc/self/task/<tid>/stat` every one to five milliseconds while the case runs, and
counts the time it sees in `D`. `host_blocking.cpp` records why the other instruments were rejected
as that clock — delay accounting is off by default since Linux 5.14 and needs root to enable, and
`/proc/pressure/io` is neither per thread nor proportional to one thread's wait.

M11.c's fifth close found that clock cannot be subtracted as it stands. `D` says the kernel made the
case wait, not what for: a case whose **own** `vfork` child held it 300 ms on an idle host read
`D` for all of it, and was reported `contended:` and passed. So is a case's own `fsync` or its own
uncached read. The owner's rule is that the ceiling may excuse only waiting the **host** causes, so
the guard excuses `host_stall_allowance()`: the smaller of the case's `D` time and the host-wide I/O
pressure (`/proc/pressure/io`'s `some` total) over the case that other tasks account for. The
case's own wait raises that total too, by at most its wait weighted by its processor's share of the
machine's non-idle time (read from `/proc/stat` over the same window), and that much is taken out
first. The consequences, which `host_blocking.cpp` spells out:

- a case that blocks itself on a quiet host is excused nothing, and fails as `stalled:`;
- a case whose wait sat behind other processes' I/O is excused at most what they were stalled, in
  PSI's units — averaged over the processors, so a machine that is CPU-bound but lightly I/O-bound
  excuses little;
- the bound is on how long the host was stalled, not on which wait: a case that blocks itself while
  other processes happen to be stalled is excused up to their stall time;
- the pressure files are read only by a case that has run an eighth of its ceiling (20 to 100 ms),
  and at its end only if it is over the ceiling — PSI's weights are whole jiffies, and frequent
  reads lose precision — so the waiting before that baseline is never excusable;
- no `/proc/pressure/io` or no `/proc/stat`: the allowance is **zero**, never unlimited.

| Clock | Reads | Fails a case when |
|---|---|---|
| CPU | `CLOCK_THREAD_CPUTIME_ID` | the case spends more than its kind's budget, scaled to this machine |
| Wall | `steady_clock` | the case's own time over the window exceeds a hundred times that budget |
| Contention | `/proc/thread-self/schedstat` | never — it is subtracted, and `budget_measures_contention()` says whether it exists |
| Host blocking | `/proc/self/task/<tid>/stat`, sampled | never — it bounds the allowance, and `budget_measures_host_blocking()` says whether it exists |
| Host I/O pressure | `/proc/pressure/io` and `/proc/stat`, around long cases | never — it bounds the allowance, and `budget_measures_host_pressure()` says whether it exists |

The decision is `stall_verdict()`, a pure function of three numbers — wall clock, the host's share
(runqueue wait plus the allowance) and the ceiling — and the allowance is `host_stall_allowance()`,
a pure function of the window, the `D` time and two pressure readings, so the arithmetic of both is
tested without arranging for a machine to be busy. The measurements they rest on are asserted
separately: `tests/unit/harness/test_budget.cpp` shows that a sleeping case accumulates neither
contention nor host blocking, and that without a pressure reading nothing is excused.
`tests/integration/test_budget_contention.cpp` shows that a preempted case accumulates contention;
that a case blocked by its own `vfork` child, its own uncached reads or its own major page faults is
seen as blocked but excused nothing; that a case which sleeps, holds a mutex or burns its CPU
accumulates no blocking and is still a stall, the mutex even while other processes press the disk;
and that a case whose disk wait sat behind other processes writing and fsyncing is excused. It also
runs `tests/integration/stall_probe.cpp` — the gate's probe, real `CY_TEST_CASE`s — as a child
process and reads the verdict the guard printed: the vfork case and the held mutex fail as
`stalled:`, the spin as `over budget:`.

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
