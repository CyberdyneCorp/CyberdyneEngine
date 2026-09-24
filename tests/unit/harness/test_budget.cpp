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
#include <cstring>
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
    // Every platform this project builds on now has a per-thread CPU clock: Linux and macOS via
    // CLOCK_THREAD_CPUTIME_ID, Windows via QueryThreadCycleTime with a TSC calibration.
    CY_CHECK(cy::test::budget_measures_cpu_time());
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

// --- M11.c's fourth close: the fourth clock, time the case was blocked by the HOST ---------------

CY_TEST_CASE("harness: a thread's state is read after the LAST parenthesis of its stat line") {
    using cy::test::thread_state_from_stat;
    const auto state = [](const char* text) {
        return thread_state_from_stat(text, text == nullptr ? 0 : std::strlen(text));
    };

    CY_CHECK_EQ(state("4182225 (cat) R 4182220 4182225"), 'R');
    CY_CHECK_EQ(state("17 (cy_test_unit_) D 1 17 17 0 -1"), 'D');
    // A thread may call itself anything in sixteen bytes. A parser that stopped at the FIRST ')'
    // would read this sleeping thread as blocked on the disk and excuse its sleep.
    CY_CHECK_EQ(state("17 (x) D (y) S 1 17"), 'S');
    CY_CHECK_EQ(state("17 (a b)) R 1"), 'R');
    // Not a stat line, or cut short: no state rather than a guess.
    CY_CHECK_EQ(state(""), '\0');
    CY_CHECK_EQ(state("no parenthesis here"), '\0');
    CY_CHECK_EQ(state("17 (cut)"), '\0');
    CY_CHECK_EQ(state("17 (cut) "), '\0');
    CY_CHECK_EQ(state(nullptr), '\0');
}

CY_TEST_CASE("harness: the instrument says whether it can see the case blocked by the host") {
#if defined(__linux__)
    CY_CHECK(cy::test::budget_measures_host_blocking());
    // Cumulative and monotonic across the case, for the same reason as the runqueue clock.
    const unsigned long long first = cy::test::blocked_on_host_ns();
    const unsigned long long second = cy::test::blocked_on_host_ns();
    CY_CHECK_GE(second, first);
#else
    CY_CHECK_FALSE(cy::test::budget_measures_host_blocking());
    CY_CHECK_EQ(cy::test::blocked_on_host_ns(), 0ULL);
#endif
}

CY_TEST_CASE(
    "harness: a case that SLEEPS is not blocked by the host, so a sleep is still a stall") {
    // THE NEGATIVE CONTROL FOR THE FOURTH CLOCK. A sleep, a futex, a join and a pipe read are
    // interruptible sleeps; only a wait for the disk, a page fault or a kernel lock is not. If this
    // clock grew across a sleep, the guard would subtract the sleep and the ceiling would catch
    // nothing. Two milliseconds, the file's sanctioned exemption, which is two samples at the unit
    // tier's interval — enough for a clock that counted sleeping to count it.
    const unsigned long long blocked_before = cy::test::blocked_on_host_ns();
    const auto started = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const unsigned long long blocked = cy::test::blocked_on_host_ns() - blocked_before;

    const auto slept_ns =
        static_cast<unsigned long long>(std::chrono::nanoseconds(elapsed).count());
    CY_CHECK_GE(slept_ns, 1'500'000ULL);
    CY_CHECK_LT(blocked, slept_ns / 10);
}

// --- M11.c's fifth close: what of that clock the HOST accounts for ------------------------------

namespace {

/// A host-pressure reading of `some_ms` of PSI `some` and `nonidle_ms` of non-idle processor time,
/// at a tick resolution of 10 ms per field over seven fields.
cy::test::HostPressure pressure(unsigned long long some_ms, unsigned long long nonidle_ms) {
    cy::test::HostPressure reading;
    reading.available = true;
    reading.io_some_ns = some_ms * 1'000'000ULL;
    reading.nonidle_cpu_ns = nonidle_ms * 1'000'000ULL;
    reading.nonidle_resolution_ns = 70'000'000ULL;
    return reading;
}

constexpr unsigned long long kMs = 1'000'000ULL;

}  // namespace

CY_TEST_CASE("harness: a case that blocks ITSELF on a quiet host is excused nothing") {
    using cy::test::host_stall_allowance;

    // THE GATE'S PROBE, IN NUMBERS MEASURED ON THIS PROJECT'S HOST (300 ms windows): the parent of
    // a vfork is uninterruptible for the whole window, and the host's I/O pressure does not move.
    // `757b3d9` excused all 300 ms of it; the rule excuses none.
    CY_CHECK_EQ(host_stall_allowance(300 * kMs, 300 * kMs, 0, pressure(0, 0), pressure(0, 7200)),
                0ULL);

    // The case's OWN uncached reads and its own fsync do move the pressure — by at most its wait
    // weighted by its processor's share of the machine's non-idle time. With 24 busy processors:
    // 205 ms of reads moved it 7.9 ms, 288 ms of fsync 11.8 ms. Both are within the case's own
    // bound, so neither is excused.
    CY_CHECK_EQ(host_stall_allowance(300 * kMs, 205 * kMs, 0, pressure(0, 0), pressure(8, 7210)),
                0ULL);
    CY_CHECK_EQ(host_stall_allowance(300 * kMs, 288 * kMs, 0, pressure(0, 0), pressure(12, 7180)),
                0ULL);

    // On a QUIET host the case's processor is most of the non-idle time, and its own wait can be
    // most of the pressure: 290 ms of own reads with the machine otherwise idle moved it 290 ms.
    // Its bound is the whole of it, because no host is non-idle for less than the window.
    CY_CHECK_EQ(host_stall_allowance(300 * kMs, 290 * kMs, 0, pressure(0, 0), pressure(290, 150)),
                0ULL);
}

CY_TEST_CASE("harness: the allowance is at most the pressure OTHER tasks put on the host") {
    using cy::test::host_stall_allowance;
    using cy::test::stall_verdict;
    using cy::test::StallVerdict;

    // The host's pressure rose 500 ms over a 655 ms window on a 24-processor host, while the case
    // waited 600 ms: the case's own share is at most 600 × 655 / (15720 − 70) = 25.1 ms, so 474.9
    // ms of it is the rest of the host's, and that is what is excused.
    const unsigned long long excused =
        host_stall_allowance(655 * kMs, 600 * kMs, 0, pressure(1000, 0), pressure(1500, 15720));
    CY_CHECK_GT(excused, 474 * kMs);
    CY_CHECK_LT(excused, 475 * kMs);
    // The fourth close's numbers: 655.4 ms held against a 234.9 ms ceiling. With that much of the
    // host's pressure excused the case is the host's; with only the case's own wait it is not.
    CY_CHECK(stall_verdict(655 * kMs, excused, 234'900'000ULL) == StallVerdict::Contended);
    CY_CHECK(stall_verdict(655 * kMs, 0, 234'900'000ULL) == StallVerdict::Stalled);
    // THE RULE'S PRICE, stated as a check: the same case beside only moderate pressure — 300 ms of
    // the window — is excused 274.9 ms, which leaves 380 ms of its own, and it is a stall. Only
    // pressure the host shows is excused, however long the case itself waited.
    const unsigned long long moderate =
        host_stall_allowance(655 * kMs, 600 * kMs, 0, pressure(1000, 0), pressure(1300, 15720));
    CY_CHECK(stall_verdict(655 * kMs, moderate, 234'900'000ULL) == StallVerdict::Stalled);

    // Never more than the case waited: a case that took one 10 ms fault on a host under heavy
    // pressure is excused 10 ms, and a case that slept is excused nothing at all.
    CY_CHECK_EQ(host_stall_allowance(655 * kMs, 10 * kMs, 0, pressure(0, 0), pressure(600, 15720)),
                10 * kMs);
    CY_CHECK_EQ(host_stall_allowance(655 * kMs, 0, 0, pressure(0, 0), pressure(600, 15720)), 0ULL);

    // And the pressure is in PSI's own units — averaged over the processors, weighted by how busy
    // each was. The case and one other task each stalled for the whole 600 ms window on a host
    // whose 24 processors are all busy show as about a twenty-fourth of it each, 50 ms together;
    // the other task's twenty-fourth is all that is excused.
    const unsigned long long diluted =
        host_stall_allowance(600 * kMs, 600 * kMs, 0, pressure(0, 0), pressure(50, 14400));
    CY_CHECK_GT(diluted, 24 * kMs);
    CY_CHECK_LT(diluted, 25 * kMs);
}

CY_TEST_CASE("harness: without a pressure reading the allowance is ZERO, not unlimited") {
    using cy::test::host_stall_allowance;
    using cy::test::HostPressure;

    const HostPressure missing{};
    CY_CHECK_FALSE(missing.available);
    CY_CHECK_EQ(host_stall_allowance(655 * kMs, 600 * kMs, 0, missing, pressure(600, 15720)), 0ULL);
    CY_CHECK_EQ(host_stall_allowance(655 * kMs, 600 * kMs, 0, pressure(0, 0), missing), 0ULL);
    CY_CHECK_EQ(host_stall_allowance(655 * kMs, 600 * kMs, 0, missing, missing), 0ULL);
    // Readings out of order are not a measurement either.
    CY_CHECK_EQ(host_stall_allowance(655 * kMs, 600 * kMs, 0, pressure(600, 15720), pressure(0, 0)),
                0ULL);
    CY_CHECK_EQ(host_stall_allowance(655 * kMs, 600 * kMs, 0, pressure(0, 15720), pressure(600, 0)),
                0ULL);
    CY_CHECK_EQ(host_stall_allowance(0, 600 * kMs, 0, pressure(0, 0), pressure(600, 15720)), 0ULL);
}

CY_TEST_CASE("harness: this host's pressure is read, or said not to be") {
#if defined(__linux__)
    const cy::test::HostPressure first = cy::test::host_pressure_now();
    CY_CHECK_EQ(first.available, cy::test::budget_measures_host_pressure());
    if (first.available) {
        const cy::test::HostPressure second = cy::test::host_pressure_now();
        CY_CHECK_GE(second.io_some_ns, first.io_some_ns);
        CY_CHECK_GE(second.nonidle_cpu_ns, first.nonidle_cpu_ns);
        CY_CHECK_GT(first.nonidle_cpu_ns, 0ULL);
        CY_CHECK_GT(first.nonidle_resolution_ns, 0ULL);
    }
#else
    CY_CHECK_FALSE(cy::test::budget_measures_host_pressure());
    CY_CHECK_FALSE(cy::test::host_pressure_now().available);
#endif
}

// --- M11.c's sixth close: the case's own threads and children are the case, not the host --------

CY_TEST_CASE(
    "harness: the case's OWN threads and children are not the host, so they excuse nothing") {
    using cy::test::host_stall_allowance;
    using cy::test::stall_verdict;
    using cy::test::StallVerdict;

    // THE GATE'S SECOND PROBE, IN NUMBERS MEASURED ON THIS PROJECT'S HOST: a case held 300 ms by
    // its own vfork child while sixteen of its own threads read the disk with O_DIRECT, beside a
    // build keeping all 24 processors busy, against the gate's 100 ms ceiling. The sixteen
    // helpers were blocked about 285 ms each — 4,560 ms between them — and put 213 ms into the
    // host's pressure. With only the calling thread's share taken out, `4a1ad21` excused 200 ms
    // of it and reported the case `contended`. With the tree's share taken out, what is left is
    // the tick resolution's margin against the case — under 10 ms — and it is a stall.
    constexpr unsigned long long kProbeCeiling = 100 * kMs;
    const unsigned long long refuted =
        host_stall_allowance(300 * kMs, 300 * kMs, 0, pressure(0, 0), pressure(213, 7200));
    CY_CHECK_GT(refuted, 190 * kMs);
    CY_CHECK(stall_verdict(300 * kMs, refuted, kProbeCeiling) == StallVerdict::Contended);
    const unsigned long long tree =
        host_stall_allowance(300 * kMs, 300 * kMs, 4560 * kMs, pressure(0, 0), pressure(213, 7200));
    CY_CHECK_LT(tree, 10 * kMs);
    CY_CHECK(stall_verdict(300 * kMs, tree, kProbeCeiling) == StallVerdict::Stalled);

    // The same probe on a QUIET host: the tree's seventeen tasks are the whole of the non-idle
    // time, the pressure they made is the whole of the window, and `4a1ad21` excused 282 ms of
    // it. The tree's share leaves the resolution's margin again, and a stall.
    const unsigned long long quiet_refuted =
        host_stall_allowance(300 * kMs, 300 * kMs, 0, pressure(0, 0), pressure(300, 5100));
    CY_CHECK_GT(quiet_refuted, 280 * kMs);
    CY_CHECK(stall_verdict(300 * kMs, quiet_refuted, kProbeCeiling) == StallVerdict::Contended);
    const unsigned long long quiet_tree =
        host_stall_allowance(300 * kMs, 300 * kMs, 4560 * kMs, pressure(0, 0), pressure(300, 5100));
    CY_CHECK_LT(quiet_tree, 12 * kMs);
    CY_CHECK(stall_verdict(300 * kMs, quiet_tree, kProbeCeiling) == StallVerdict::Stalled);

    // A child PROCESS is the tree too: a case whose child wrote and fsynced for the window, alone
    // on the host, is the pressure it is looking at.
    CY_CHECK_EQ(
        host_stall_allowance(300 * kMs, 280 * kMs, 290 * kMs, pressure(0, 0), pressure(295, 600)),
        0ULL);
    CY_CHECK_GT(host_stall_allowance(300 * kMs, 280 * kMs, 0, pressure(0, 0), pressure(295, 600)),
                100 * kMs);

    // AND THE RULE'S OTHER HALF, which is why the tree is subtracted rather than made to forbid the
    // allowance outright: one worker thread's one-millisecond page fault beside the fourth close's
    // build is taken out — 0.04 ms of the pressure — and the case's 600 ms behind that build is
    // still excused. A tree that forbade the allowance would have failed this case as stalled.
    const unsigned long long worker = host_stall_allowance(
        655 * kMs, 600 * kMs, 1 * kMs, pressure(1000, 0), pressure(1500, 15720));
    CY_CHECK_GT(worker, 474 * kMs);
    CY_CHECK_LT(worker, 475 * kMs);
    CY_CHECK(stall_verdict(655 * kMs, worker, 234'900'000ULL) == StallVerdict::Contended);
}

CY_TEST_CASE("harness: the instrument says whether it can see the case's process tree") {
#if defined(__linux__)
    // This project's kernels have CONFIG_PROC_CHILDREN. Where a kernel does not, the census cannot
    // tell the case's children from other processes and the allowance is zero; that is asserted in
    // the integration suite, which can measure a window.
    CY_CHECK_EQ(cy::test::budget_measures_tree_blocking(),
                cy::test::budget_measures_host_blocking());
    // Cumulative and monotonic across the case, like the other clocks.
    const unsigned long long first = cy::test::tree_blocked_ns();
    const unsigned long long second = cy::test::tree_blocked_ns();
    CY_CHECK_GE(second, first);
#else
    CY_CHECK_FALSE(cy::test::budget_measures_tree_blocking());
    CY_CHECK_EQ(cy::test::tree_blocked_ns(), 0ULL);
#endif
}
