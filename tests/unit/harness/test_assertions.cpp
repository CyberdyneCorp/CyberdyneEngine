// The wrapper's vocabulary, exercised. Every macro a test may use is used here, so that replacing
// the framework beneath cy/test/test.h has one place that says what "replaced correctly" means.

#include <cy/test/test.h>

#include <cstdint>
#include <limits>

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
    CY_CHECK_NEAR(1000.25F, 1000.0F, 0.25F);
    CY_CHECK_NEAR_REL(1000.5, 1000.0, 1e-3);
}

// CY_CHECK_NEAR expands to `(value) == cy::test::near(expected, tolerance)`, so asserting on that
// expression is asserting on the macro. These are the regressions for the macro having been
// doctest's Approx, whose epsilon is relative: `tolerance * (1 + max(|value|, |expected|))`.
CY_TEST_CASE("harness: CY_CHECK_NEAR's tolerance is absolute, and Approx's was not") {
    // A thousand metres off by half a metre, against a tolerance of a hundredth. The old macro
    // accepted it — shown here with the exact expression it expanded to — and the new one refuses.
    CY_CHECK(1000.5 == doctest::Approx(1000.0).epsilon(0.01));
    CY_CHECK_FALSE(1000.5 == cy::test::near(1000.0, 0.01));

    // Below one the relative epsilon still admitted up to twice the tolerance.
    CY_CHECK(0.515 == doctest::Approx(0.5).epsilon(0.01));
    CY_CHECK_FALSE(0.515 == cy::test::near(0.5, 0.01));

    // The tolerance is inclusive, on either side, in float as in double.
    CY_CHECK(1.25 == cy::test::near(1.0, 0.25));
    CY_CHECK(0.75 == cy::test::near(1.0, 0.25));
    CY_CHECK(-1.5F == cy::test::near(-1.0F, 0.5F));
    CY_CHECK_FALSE(1.2500001 == cy::test::near(1.0, 0.25));
}

CY_TEST_CASE("harness: a near comparison refuses NaN and admits an exact infinity") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CY_CHECK_FALSE(nan == cy::test::near(0.0, 1e30));
    CY_CHECK_FALSE(0.0 == cy::test::near(nan, 1e30));
    CY_CHECK_FALSE(nan == cy::test::near_relative(1.0, 1.0));
    CY_CHECK(inf == cy::test::near(inf, 0.0));
    CY_CHECK_FALSE(inf == cy::test::near(1e300, 1e300));
    CY_CHECK(2.0 == cy::test::near(2.0, 0.0));
}

CY_TEST_CASE("harness: CY_CHECK_NEAR_REL scales its tolerance by the larger magnitude") {
    CY_CHECK(1010.0 == cy::test::near_relative(1000.0, 0.01));
    CY_CHECK_FALSE(1010.5 == cy::test::near_relative(1000.0, 0.01));
    CY_CHECK(-990.0 == cy::test::near_relative(-1000.0, 0.01));
    // No `1 +` in the scale: relative to zero only zero is close.
    CY_CHECK_FALSE(1e-9 == cy::test::near_relative(0.0, 0.5));
    CY_CHECK(0.0 == cy::test::near_relative(0.0, 0.0));
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
