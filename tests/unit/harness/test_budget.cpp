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

CY_TEST_CASE("harness: the budget is calibrated against the machine it is running on") {
    // REGRESSION, M8.a's gate, and the general form of a failure that has been recurring since M4.
    //
    // `CY_TEST_BUDGET_NS` is written as a duration, and a duration is not a property of a test — it
    // is a property of the test AND the machine AND the optimiser AND whatever else is running. Two
    // gates measured that from opposite ends. M7's found the same suite taking 1.05 ms alone and
    // 0.20 ms beside four spinners, because this governor idles at 800 MHz and an IDLE machine is
    // therefore the harness's worst case. M8.a's found sixteen unit cases failing in the Debug
    // configuration in directories with no modifications at all, and five of the seven offending
    // suites passing standalone and failing only inside a 57-suite run.
    //
    // So the budget is scaled by a reference workload measured in this process. What this case
    // asserts is the property that makes that safe rather than merely convenient: THE SCALE NEVER
    // TIGHTENS THE BUDGET. A machine faster than the reference does not earn a stricter budget than
    // the suite was written against, because a check that gets stricter on good hardware is a check
    // that starts failing for reasons nobody introduced.
    if (std::getenv("CY_TEST_BUDGET_SCALE") != nullptr) {
        return;  // an explicit override owns the scale outright, including values below one
    }
    CY_CHECK_GE(cy::test::budget_scale(), 1.0);
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

// --- M9 TASK 7.5b: the stall ceiling, and the clock that tells waiting from preemption -----------

CY_TEST_CASE("harness: the instrument says whether it can tell contention from blocking") {
    // The same shape as the case above about the CPU clock, and for the same reason: the stall
    // ceiling subtracts contention only where contention is measurable, so a test asserting about
    // that subtraction has to assert that the instrument exists first.
#if defined(__linux__)
    CY_CHECK(cy::test::budget_measures_contention());
    // Cumulative and monotonic: the guard takes a difference across the case, and a clock that ran
    // backwards would excuse a stall that never happened.
    const unsigned long long first = cy::test::contended_ns();
    const unsigned long long second = cy::test::contended_ns();
    CY_CHECK_GE(second, first);
#else
    CY_CHECK_FALSE(cy::test::budget_measures_contention());
    CY_CHECK_EQ(cy::test::contended_ns(), 0ULL);
#endif
}

CY_TEST_CASE("harness: a case that BLOCKS accumulates no contention, so a sleep is still a stall") {
    // THE NEGATIVE CONTROL FOR THE EXCUSE, and the reason the subtraction is sound rather than
    // convenient. If sleeping accumulated runqueue wait, the guard would subtract a sleep's own
    // duration and the stall ceiling would stop catching the thing it exists to catch.
    //
    // Two milliseconds of sleep: the file's sanctioned exemption, for the same reason as the first
    // case. What is asserted is that the WAIT clock barely moves across it while the wall clock
    // moves by the whole sleep — so wall-minus-contention is still about the sleep's length, and a
    // case that sleeps past its ceiling still fails.
    const unsigned long long contended_before = cy::test::contended_ns();
    const auto started = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const unsigned long long contended = cy::test::contended_ns() - contended_before;

    const auto slept_ns =
        static_cast<unsigned long long>(std::chrono::nanoseconds(elapsed).count());
    CY_CHECK_GE(slept_ns, 1'500'000ULL);
    if (cy::test::budget_measures_contention()) {
        // A tenth of the sleep is generous: what is being ruled out is contention accounting for
        // the sleep, which would be all of it. On an idle machine this difference is zero.
        CY_CHECK_LT(contended, slept_ns / 10);
    }
}

CY_TEST_CASE("harness: the stall verdict separates a busy machine from a waiting case") {
    using cy::test::stall_verdict;
    using cy::test::StallVerdict;

    constexpr unsigned long long kCeiling = 100'000'000ULL;  // 100 ms: a unit budget times 100

    // Inside the ceiling: nothing to decide, whatever the contention was.
    CY_CHECK(stall_verdict(kCeiling - 1, 0, kCeiling) == StallVerdict::Fine);
    CY_CHECK(stall_verdict(kCeiling, kCeiling, kCeiling) == StallVerdict::Fine);
    // No ceiling — a budget of zero switches the check off — is not a stall.
    CY_CHECK(stall_verdict(10 * kCeiling, 0, 0) == StallVerdict::Fine);

    // THE FLAKE: 150 ms of wall clock of which 130 ms was spent wanting a core. Twenty milliseconds
    // is the case's own, which is inside the ceiling, so the machine was slow and the case was not.
    CY_CHECK(stall_verdict(150'000'000ULL, 130'000'000ULL, kCeiling) == StallVerdict::Contended);

    // THE DEFECT THE CEILING EXISTS FOR: 150 ms of wall clock and no contention at all, which is
    // what a sleep, a blocking read or a join looks like. Still a stall.
    CY_CHECK(stall_verdict(150'000'000ULL, 0, kCeiling) == StallVerdict::Stalled);
    // And contention that does not account for the excess does not excuse it either.
    CY_CHECK(stall_verdict(300'000'000ULL, 100'000'000ULL, kCeiling) == StallVerdict::Stalled);

    // A clock that ran backwards, or a contention figure larger than the window, saturates at zero
    // rather than wrapping to something enormous.
    CY_CHECK(stall_verdict(150'000'000ULL, 400'000'000ULL, kCeiling) == StallVerdict::Contended);
}
