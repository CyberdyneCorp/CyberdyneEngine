# `tests/`

The test taxonomy `testing-and-quality` defines. Each class is one recipe with a stated duration;
the fast set staying fast is a requirement, not an aspiration.

| Directory | Recipe | Budget per test | Contains |
|---|---|---|---|
| `unit/<module>/` | `just test-unit` | 1 ms CPU | Sub-millisecond tests of one unit, no I/O, no window |
| `integration/` | `just test-integration` | 1 s CPU | Several subsystems together, and anything that does I/O |
| `smoke/` | `just test-smoke` | 30 s CPU | The samples, run headless, asserted to exit cleanly |
| `render/` | — | 5 s | Golden-image and render-seam checks; wired when the renderer lands at M3 |
| `determinism/` | — | 10 s | Fixed-seed replay equivalence; wired when simulation lands at M9 |

`harness/` is not a suite: it is the framework seam and the fixtures every suite links.

## The budget is enforced, not documented

`tests/CMakeLists.txt` holds the table above as data. `cy_add_test(NAME <n> KIND <kind> SOURCES ...)`
reads it for two things: the per-test budget it compiles into the binary, where
`cy::test::BudgetGuard` fails any test case that outlives it, and the CTest timeout it gives the
suite. A suite therefore cannot be declared without a budget, and a test that has quietly grown into
the next class up says so on the run that made it slow.

**The budget is the case's own CPU time, not wall clock.** A wall-clock budget measures the machine
as much as the test: through M2 it made `four-profiles` fail about one Debug run in thirty, on
whichever case happened to be descheduled — measured at 0 failures in 100 runs on an idle host and
10 in 30 with twenty-four spinning threads beside it. A CPU budget answers the question the taxonomy
actually asks, "what does this test cost?", and answers it the same on a busy agent and an idle
laptop. `tests/unit/harness/test_budget.cpp` is the regression, and
`tests/harness/include/cy/test/test.h` carries the argument in full.

A CPU budget alone would let a unit test sleep, so there is a second limit: a case that stays within
its budget but holds the suite for more than **a hundred times** it is reported as *stalled* rather
than as slow, because that is what waiting on a sleep, a lock, a read or a joined thread looks like
— and `testing-and-quality` puts all four in `integration/` or above.

`CY_TEST_BUDGET_SCALE` relaxes both limits for one run — it defaults to 20 under a sanitizer, where
the slowdown is the tool working — and `CY_TEST_BUDGET_SCALE=0` switches them off.

## Writing a test

```cpp
#include <cy/test/test.h>

CY_TEST_CASE("clock: time moves only when the test moves it") {
    cy::test::DeterministicClock clock;
    clock.advance_ns(1500);
    CY_CHECK_EQ(clock.now_ns(), 1500ULL);
}
```

doctest is the framework, reached through the `CY_TEST_CASE` wrapper so it is replaceable — see
`design.md` §5. A test written against doctest's own macros bypasses that seam, so it is rejected at
configure time by `tests/CMakeLists.txt` and again at run time by
`tests/integration/test_wrapper_seam.cpp`, which sees a file added since the last configure.

`CY_CHECK` records a failure and carries on. `CY_REQUIRE` stops the test — and, under
`-fno-exceptions`, aborts the process, taking the rest of the binary's cases with it. Guard
preconditions with it; assert results with `CY_CHECK`.

## Running

```
just test-unit                     # build, then the unit suite
just test-unit -R harness          # everything after --profile goes to ctest
just test-integration
just test-all                      # every suite, each reported separately
CY_TEST_JUNIT=results.xml just test-all     # machine-readable results for CI
```

Every bug fix carries a regression test.

**Governed by**: `testing-and-quality`.
