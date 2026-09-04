// cy/test/test.h — the test vocabulary. The only file in the repository that includes doctest.
//
// Task 4.1.1, design.md §5. doctest is the framework because it compiles roughly an order of
// magnitude faster than the alternative, and `testing-and-quality` budgets thousands of unit tests
// at under a millisecond each. That argument is about today's numbers, so the framework is reached
// through one wrapper: replacing it is a change to this header and to tests/harness/, not to every
// test in the tree. tests/CMakeLists.txt fails the configure if any other test source includes
// doctest directly, so the seam cannot decay quietly.
//
// The wrapper is thin on purpose. It renames what doctest already does well and adds exactly one
// thing doctest does not do: it enforces the taxonomy's per-test budget, because a budget nothing
// measures is an aspiration. That budget is CPU time rather than wall clock, which is task 1.1 and
// is argued where BudgetGuard is declared.

#ifndef CY_TEST_TEST_H
#define CY_TEST_TEST_H

#include <doctest/doctest.h>

// The budget for one test case, in nanoseconds of the case's own CPU time. tests/CMakeLists.txt
// defines it per suite from the taxonomy in `testing-and-quality`: 1 ms for a unit test, 1 s for an
// integration test, 30 s for a smoke test. The default here is the strictest of the three, so a
// test built outside that machinery is held to the tightest budget rather than to none.
#ifndef CY_TEST_BUDGET_NS
#    define CY_TEST_BUDGET_NS 1000000ULL
#endif

namespace cy::test {

/// How much wall clock a case within its budget may still hold the suite for, as a multiple of the
/// budget. See BudgetGuard: the budget is CPU time, and this is the separate limit that keeps
/// "a unit test does not sleep, block or wait on a thread" a checked property rather than a
/// comment.
///
/// A hundred, because the two limits must not be confusable. A case that spends 1 ms of CPU and
/// 100 ms of wall clock is waiting on something by a factor no amount of machine load explains: the
/// measurement that motivated this file saw a 0.2 ms case stretch to 4.1 ms of wall clock under
/// 24-way oversubscription, which is 25x below this ceiling.
inline constexpr unsigned long long kStallMultiplier = 100;

/// Fails the surrounding test case when its body costs more than its suite's budget.
///
/// Constructed by CY_TEST_CASE, so every test in the tree is measured. A failure is reported as an
/// ordinary check failure at the test's own file and line — it names the budget, the measurement
/// and the recipe that owns the suite, because "which suite is this test in?" is the first question
/// an over-budget test raises.
///
/// --- THE CLOCK IS THE CASE'S OWN CPU TIME, AND THAT IS TASK 1.1 -------------------------------
///
/// Through M2 the budget was wall clock, and `four-profiles` — an exit criterion of two milestones
/// and a permanent gate, so a pull request is exposed to it three times over — failed about one
/// Debug run in thirty for it. The failure was measured, not guessed: `ctest -L unit` in the Debug
/// tree fails when the host is busy and passes when it is idle (0 failures in 100 idle runs; 10 in
/// 30 with 24 spinning threads beside it), and the case it lands on is whichever one happened to be
/// descheduled — `unit.values`' generation-table case measured 4.134 ms of wall clock against a
/// 1 ms budget while doing about 0.2 ms of work.
///
/// That is a defect in the instrument. The taxonomy's question is "what does this test cost?", and
/// the answer must not change with what else the machine is doing, or the gate reports on the
/// build agent rather than on the change. So the budget is the CPU time the case's own thread
/// consumed. Load is invisible to it; the work is not, so the split that moved seven scene and
/// reflection cases into the integration suite at M2's close stands on exactly the same numbers.
///
/// The fix deliberately is NOT any of: shrinking a case (the case is not the problem), raising the
/// budget (a budget raised until a flake hides measures nothing), or scaling it under load (which
/// is the same thing with an extra variable).
///
/// WHAT THE CPU CLOCK DOES NOT SEE, AND WHAT COVERS IT. Time the case spends waiting — a sleep, a
/// blocking read, a lock, a thread it joined — costs its thread no CPU, and work handed to the job
/// system is charged to the workers rather than to the case. A pure CPU budget would therefore let
/// a unit test sleep for a second, and `testing-and-quality` places sleeping and I/O in
/// tests/integration/ or above. `kStallMultiplier` is the second limit that keeps that checkable:
/// a case within its CPU budget that holds the suite for more than a hundred times it is reported
/// as stalled, and the message says "waiting" rather than "slow", because that is what it is.
///
/// The budget is scaled by the CY_TEST_BUDGET_SCALE environment variable, and defaults to a relaxed
/// scale under a sanitizer, where a five- to twenty-fold slowdown is the tool working correctly
/// rather than the test regressing. CY_TEST_BUDGET_SCALE=0 disables both checks.
class BudgetGuard {
public:
    BudgetGuard(const char* name, unsigned long long budget_ns, const char* file, int line);
    ~BudgetGuard();

    BudgetGuard(const BudgetGuard&) = delete;
    BudgetGuard& operator=(const BudgetGuard&) = delete;

private:
    const char* name_;
    const char* file_;
    int line_;
    unsigned long long budget_ns_;
    unsigned long long started_cpu_ns_;
    unsigned long long started_wall_ns_;
};

/// The scale applied to every budget, resolved once from the environment. Exposed so that a test of
/// the harness can state what it is running under rather than guess.
double budget_scale();

/// True where the budget is measured as CPU time. False on a platform with no usable per-thread CPU
/// clock — Windows, whose thread times are updated on the scheduler's quantum — where the guard
/// falls back to wall clock and the stall ceiling is not applied. Exposed so that a test of the
/// harness states which instrument it is asserting about instead of assuming one.
bool budget_measures_cpu_time() noexcept;

}  // namespace cy::test

// --- Test declaration ---------------------------------------------------------------------------

#define CY_TEST_CONCAT_IMPL(a, b) a##b
#define CY_TEST_CONCAT(a, b) CY_TEST_CONCAT_IMPL(a, b)

#ifdef __COUNTER__
#    define CY_TEST_UNIQUE(prefix) CY_TEST_CONCAT(prefix, __COUNTER__)
#else
#    define CY_TEST_UNIQUE(prefix) CY_TEST_CONCAT(prefix, __LINE__)
#endif

// The body is a separate function so that the budget guard brackets it exactly: the guard is
// constructed before the first statement and destroyed after the last, including on an early
// return. Subcases still work — doctest tracks them on a stack, so a CY_TEST_SUBCASE inside the
// body function, or inside a helper it calls, behaves as it does inside a bare test case.
#define CY_TEST_CASE_IMPL(name, fn)                                                      \
    static void fn();                                                                    \
    DOCTEST_TEST_CASE(name) {                                                            \
        ::cy::test::BudgetGuard cy_test_budget_guard_{name, CY_TEST_BUDGET_NS, __FILE__, \
                                                      __LINE__};                         \
        fn();                                                                            \
    }                                                                                    \
    static void fn()

/// Declare a test case. `CY_TEST_CASE("name") { ... }`
#define CY_TEST_CASE(name) CY_TEST_CASE_IMPL(name, CY_TEST_UNIQUE(cy_test_body_))

/// A named section of a test case, re-entered once per leaf. State declared before it is rebuilt
/// for each, which is how a fixture is shared without being shared between runs.
#define CY_TEST_SUBCASE(name) DOCTEST_SUBCASE(name)

/// Group the test cases that follow under a name, for `--test-suite=` selection.
#define CY_TEST_SUITE(name) DOCTEST_TEST_SUITE(name)

// --- Assertions ---------------------------------------------------------------------------------
//
// CY_CHECK records a failure and carries on; CY_REQUIRE stops the test, because what follows it
// would be meaningless. Under -fno-exceptions a failed CY_REQUIRE aborts the process rather than
// unwinding — that is doctest's DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS behaviour, set in
// cmake/dependencies.cmake, and it is the right trade: the alternative is a precondition failure
// that silently continues into the code it was guarding. It does mean a failed CY_REQUIRE takes the
// rest of the binary's test cases with it, so guard preconditions with it and assert results with
// CY_CHECK.

#define CY_CHECK(...) DOCTEST_CHECK(__VA_ARGS__)
#define CY_CHECK_FALSE(...) DOCTEST_CHECK_FALSE(__VA_ARGS__)
#define CY_CHECK_EQ(...) DOCTEST_CHECK_EQ(__VA_ARGS__)
#define CY_CHECK_NE(...) DOCTEST_CHECK_NE(__VA_ARGS__)
#define CY_CHECK_LT(...) DOCTEST_CHECK_LT(__VA_ARGS__)
#define CY_CHECK_LE(...) DOCTEST_CHECK_LE(__VA_ARGS__)
#define CY_CHECK_GT(...) DOCTEST_CHECK_GT(__VA_ARGS__)
#define CY_CHECK_GE(...) DOCTEST_CHECK_GE(__VA_ARGS__)

#define CY_REQUIRE(...) DOCTEST_REQUIRE(__VA_ARGS__)
#define CY_REQUIRE_FALSE(...) DOCTEST_REQUIRE_FALSE(__VA_ARGS__)
#define CY_REQUIRE_EQ(...) DOCTEST_REQUIRE_EQ(__VA_ARGS__)
#define CY_REQUIRE_NE(...) DOCTEST_REQUIRE_NE(__VA_ARGS__)

/// Floating-point comparison with an explicit tolerance. There is no default tolerance: the value
/// that is close enough is a property of what is being measured, not of the framework.
#define CY_CHECK_NEAR(value, expected, tolerance) \
    DOCTEST_CHECK((value) == doctest::Approx(expected).epsilon(tolerance))

/// Record a message in the test's output without asserting anything.
#define CY_TEST_MESSAGE(...) DOCTEST_MESSAGE(__VA_ARGS__)

/// Fail the current test case with a message, and continue.
#define CY_TEST_FAIL_CHECK(...) DOCTEST_FAIL_CHECK(__VA_ARGS__)

/// Fail the current test case with a message, and stop it.
#define CY_TEST_FAIL(...) DOCTEST_FAIL(__VA_ARGS__)

#endif  // CY_TEST_TEST_H
