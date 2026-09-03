# `tests/`

The test taxonomy `testing-and-quality` defines. Each class is one recipe with a stated duration;
the fast set staying fast is a requirement, not an aspiration.

| Directory | Recipe | Budget per test | Contains |
|---|---|---|---|
| `unit/<module>/` | `just test-unit` | 1 ms | Sub-millisecond tests of one unit, no I/O, no window |
| `integration/` | `just test-integration` | 1 s | Several subsystems together, and anything that does I/O |
| `smoke/` | `just test-smoke` | 30 s | The samples, run headless, asserted to exit cleanly |
| `render/` | — | 5 s | Golden-image and render-seam checks; wired when the renderer lands at M3 |
| `determinism/` | — | 10 s | Fixed-seed replay equivalence; wired when simulation lands at M9 |

`harness/` is not a suite: it is the framework seam and the fixtures every suite links.

## The budget is enforced, not documented

`tests/CMakeLists.txt` holds the table above as data. `cy_add_test(NAME <n> KIND <kind> SOURCES ...)`
reads it for two things: the per-test budget it compiles into the binary, where
`cy::test::BudgetGuard` fails any test case that outlives it, and the CTest timeout it gives the
suite. A suite therefore cannot be declared without a budget, and a test that has quietly grown into
the next class up says so on the run that made it slow.

`CY_TEST_BUDGET_SCALE` relaxes the budget for one run — it defaults to 20 under a sanitizer, where
the slowdown is the tool working — and `CY_TEST_BUDGET_SCALE=0` switches the check off.

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
