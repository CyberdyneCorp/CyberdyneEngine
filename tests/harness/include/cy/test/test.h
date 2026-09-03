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
// measures is an aspiration.

#ifndef CY_TEST_TEST_H
#define CY_TEST_TEST_H

#include <doctest/doctest.h>

// The wall-clock budget for one test case, in nanoseconds. tests/CMakeLists.txt defines it per
// suite from the taxonomy in `testing-and-quality`: 1 ms for a unit test, 1 s for an integration
// test, 30 s for a smoke test. The default here is the strictest of the three, so a test built
// outside that machinery is held to the tightest budget rather than to none.
#ifndef CY_TEST_BUDGET_NS
#    define CY_TEST_BUDGET_NS 1000000ULL
#endif

namespace cy::test {

/// Fails the surrounding test case when its body outlives the suite's budget.
///
/// Constructed by CY_TEST_CASE, so every test in the tree is measured. A failure is reported as an
/// ordinary check failure at the test's own file and line — it names the budget, the measurement
/// and the recipe that owns the suite, because "which suite is this test in?" is the first question
/// an over-budget test raises.
///
/// The budget is scaled by the CY_TEST_BUDGET_SCALE environment variable, and defaults to a relaxed
/// scale under a sanitizer, where a five- to twenty-fold slowdown is the tool working correctly
/// rather than the test regressing. CY_TEST_BUDGET_SCALE=0 disables the check.
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
    unsigned long long started_ns_;
};

/// The scale applied to every budget, resolved once from the environment. Exposed so that a test of
/// the harness can state what it is running under rather than guess.
double budget_scale();

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
