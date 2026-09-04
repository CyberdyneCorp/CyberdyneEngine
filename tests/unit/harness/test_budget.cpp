// The budget guard's own instrument, and the regression for task 1.1.
//
// The guard is the one thing the wrapper adds to doctest, and through M2 it measured the wrong
// quantity: wall clock, which is a property of the machine as much as of the test. `four-profiles`
// — an exit criterion of two milestones and a permanent gate — failed about one Debug run in thirty
// because of it. The first case below is that flake, reduced to something deterministic: it fails
// under a wall-clock budget and passes under a CPU one, so the fix cannot be undone silently.

#include <cy/test/test.h>

#include <chrono>
#include <cstdlib>
#include <thread>

CY_TEST_CASE("harness: the budget is CPU time, so a descheduled case is not a failing case") {
    // Two milliseconds of sleep: twice the unit budget in wall clock, and no CPU at all. That is
    // exactly the shape of the flake — a case doing a fifth of a millisecond of work stretched to
    // 4.1 ms of wall clock by twenty-four spinning threads beside it — with the machine's load
    // replaced by something a test can rely on.
    //
    // THE ASSERTION IS THAT THIS CASE PASSES. The budget guard reports as an ordinary check failure
    // at this file and line, so a guard measuring wall clock fails the case and a guard measuring
    // CPU time does not; there is nothing further to write. The elapsed check below only guards the
    // premise, so that a platform whose sleep returns immediately reports that rather than passing
    // vacuously.
    //
    // It is also the one sanctioned sleep in the unit suite. `testing-and-quality` puts sleeping in
    // tests/integration/ or above, and the exemption is narrow: this is the harness measuring its
    // own instrument, and two milliseconds is fifty times below the stall ceiling that would
    // otherwise catch it.
    const auto started = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CY_CHECK_GE(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(), 1500);
}

CY_TEST_CASE("harness: the instrument says which clock it is") {
    // Not an assertion that the clock is a CPU clock — that is a platform's answer, not a test's.
    // What is asserted is that the harness states it, because the stall ceiling is applied only
    // where the budget is CPU time and a test that assumed otherwise would be asserting nothing.
#if defined(_WIN32)
    CY_CHECK_FALSE(cy::test::budget_measures_cpu_time());
#else
    CY_CHECK(cy::test::budget_measures_cpu_time());
#endif
}

CY_TEST_CASE("harness: the budget scale is whatever the environment asked for, zero included") {
    // `CY_TEST_BUDGET_SCALE=0` switches the check off. It is a documented value — `just
    // test-sanitize` used to export it — so the harness's own suite must not assert it away: a test
    // that requires a positive scale is a test that fails under the one setting that says "do not
    // measure me", which is how `just test-sanitize --tests .` became unusable at M2. That is task
    // 1.5's half of this file.
    const double scale = cy::test::budget_scale();
    CY_CHECK_GE(scale, 0.0);

    const char* requested = std::getenv("CY_TEST_BUDGET_SCALE");
    if (requested == nullptr || *requested == '\0') {
        // Nothing asked, so the default applies: relaxed under a sanitizer, one otherwise. Both are
        // positive, which is the property that matters — the check is live unless it was switched
        // off deliberately.
        CY_CHECK_GT(scale, 0.0);
        return;
    }
    const double parsed = std::strtod(requested, nullptr);
    if (parsed >= 0.0) {
        CY_CHECK_EQ(scale, doctest::Approx(parsed));
    }
}

CY_TEST_CASE("harness: the suite's budget is the one the taxonomy gives its kind") {
    // A unit suite, so one millisecond. This restates tests/CMakeLists.txt on purpose: the budget
    // reaches the binary as a compiled-in definition, and a suite declared outside cy_add_test()
    // would silently get the header's default instead.
    CY_CHECK_EQ(CY_TEST_BUDGET_NS, 1000000ULL);
    CY_CHECK_GT(cy::test::kStallMultiplier, 1ULL);
}
