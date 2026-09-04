// The wrapper's vocabulary, exercised. Every macro a test may use is used here, so that replacing
// the framework beneath cy/test/test.h has one place that says what "replaced correctly" means.

#include <cy/test/test.h>

#include <cstdint>

namespace {

// Arithmetic with no dependencies, so that these tests measure the harness and nothing else.
constexpr std::uint32_t triangular(std::uint32_t n) {
    return n * (n + 1) / 2;
}

}  // namespace

CY_TEST_CASE("harness: a passing check reports nothing") {
    CY_CHECK(true);
    CY_CHECK_FALSE(false);
    CY_CHECK_EQ(triangular(4), 10U);
    CY_CHECK_NE(triangular(4), 11U);
    CY_CHECK_LT(triangular(3), triangular(4));
    CY_CHECK_LE(triangular(4), triangular(4));
    CY_CHECK_GT(triangular(5), triangular(4));
    CY_CHECK_GE(triangular(4), triangular(4));
}

CY_TEST_CASE("harness: a requirement guards what follows it") {
    const std::uint32_t total = triangular(10);
    CY_REQUIRE(total > 0U);
    CY_REQUIRE_EQ(total, 55U);
    CY_REQUIRE_NE(total, 0U);
    CY_REQUIRE_FALSE(total == 0U);
    CY_CHECK_EQ(total % 5U, 0U);
}

CY_TEST_CASE("harness: a floating-point comparison states its tolerance") {
    const double third = 1.0 / 3.0;
    CY_CHECK_NEAR(third * 3.0, 1.0, 1e-12);
}

CY_TEST_CASE("harness: a subcase re-enters the case with fresh state") {
    std::uint32_t counter = 0;

    CY_TEST_SUBCASE("one increment") {
        counter += 1;
        CY_CHECK_EQ(counter, 1U);
    }
    CY_TEST_SUBCASE("the other branch starts from the same state") {
        counter += 2;
        CY_CHECK_EQ(counter, 2U);
    }
}

CY_TEST_CASE("harness: every test case is measured against a budget") {
    // The budget itself is a compile-time constant the suite's KIND selects. What it measures, how
    // it is scaled, and why it is CPU time rather than wall clock are test_budget.cpp's, which is
    // where the guard's own regressions live; this only asserts that a case here carries one.
    CY_CHECK_EQ(CY_TEST_BUDGET_NS, 1000000ULL);
    CY_CHECK_GE(cy::test::budget_scale(), 0.0);
}
