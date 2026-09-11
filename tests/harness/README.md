# `tests/harness/`

The framework seam and the fixtures. Every test binary links `cy::test-harness`, and this is the
only directory permitted to name doctest.

| File | What it is |
|---|---|
| `include/cy/test/test.h` | `CY_TEST_CASE`, the assertions, and the budget guard. The one include a test needs. |
| `include/cy/test/fixtures.h` | The injectable fixtures: a deterministic clock, a seeded generator, a temporary directory. |
| `src/main.cpp` | doctest's `main`, so no test file carries one. |
| `src/budget.cpp` | The per-test budget check: the CPU clock, the wall-clock stall ceiling, and the contention clock that separates the two. |
| `src/fixtures.cpp` | The filesystem half of the fixtures. |

## Why a wrapper

`design.md` §5 chose doctest on a compile-time argument — an argument about today's numbers at
today's scale. The wrapper is what keeps that choice reversible: replacing the framework is a change
to this directory, not to every test in the tree. The seam is enforced twice, at configure time and
at run time, because a seam nobody checks is a seam that has already been crossed.

## The three clocks, and why the budget needs all of them

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

| Clock | Reads | Fails a case when |
|---|---|---|
| CPU | `CLOCK_THREAD_CPUTIME_ID` | the case spends more than its kind's budget, scaled to this machine |
| Wall | `steady_clock` | the case's own time over the window exceeds a hundred times that budget |
| Contention | `/proc/thread-self/schedstat` | never — it is subtracted, and `budget_measures_contention()` says whether it exists |

The decision is `stall_verdict()`, a pure function of three numbers, so the arithmetic is tested
without arranging for a machine to be busy. The two measurements it rests on are asserted separately:
`tests/unit/harness/test_budget.cpp` shows that a sleeping case accumulates no contention, and
`tests/integration/test_budget_contention.cpp` shows that a preempted one does.

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
