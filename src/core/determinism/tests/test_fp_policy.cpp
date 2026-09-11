// M9 TASK 2.3 — the floating-point policy, and the twelve replacements.
//
// Three things are checked and they are different in kind:
//
//   1. THE TABLE IS THE SPIKE'S. All thirteen disqualified functions are present and classified
//      `NotCorrectlyRounded`; the eleven exact ones and the eleven correctly-rounded ones are too.
//      A table that quietly lost a row would let a forbidden call through the lint.
//   2. THE PERMISSION RULE FOLLOWS THE PROFILE. `SamePlatform` may use this libm's
//      correctly-rounded set; `CrossPlatform` and `Lockstep` may not, because that set is a
//      property of glibc 2.39 rather than of IEEE-754.
//   3. THE REPLACEMENTS ARE ACCURATE. Compared against `<cmath>` in ulps at a spread of sample
//      points. The bound is stated per function rather than as one number, because `cbrt` after two
//      Newton steps and `log10` through a reciprocal-log multiply are not the same claim.
//
// WHAT THIS SUITE DELIBERATELY DOES NOT CLAIM. It does not assert that `fp::acos(k)` folded equals
// `fp::acos(k)` at run time, because on this compiler at `-O0` nothing is folded and the case would
// pass without testing anything. The fold-stability argument is structural — every replacement is
// built from operations with a unique right answer — and the thing that keeps it true is the lint's
// forbidden-call rule plus `-ffp-contract=off`, both of which ARE checked.

#include <cy/core/determinism/fp_policy.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstring>

namespace {

using namespace cy;
using namespace cy::determinism;

[[nodiscard]] const FloatFunction& row(const char* name) noexcept {
    static const FloatFunction kMissing{};
    const FloatFunction* found = find_float_function(name);
    return found != nullptr ? *found : kMissing;
}

/// A spread rather than a grid: the endpoints an implementation gets wrong, a few ordinary values,
/// and one value either side of each branch the implementations take.
constexpr f64 kUnitPoints[] = {-0.999999, -0.9, -0.5, -0.25, -1e-8,   0.0,
                               1e-8,      0.25, 0.5,  0.9,   0.999999};
constexpr f64 kPositivePoints[] = {1e-300, 1e-8, 0.001, 0.1, 0.5, 1.0, 1.5, 2.0, 10.0, 1e8, 1e100};
constexpr f64 kWidePoints[] = {-40.0, -20.0, -1.5, -1.0, -0.5, -1e-6, 0.0,
                               1e-6,  0.5,   1.0,  1.5,  20.0, 40.0};

}  // namespace

CY_TEST_CASE("determinism: the float policy table is the spike's, row for row") {
    // The thirteen. Named here rather than counted, so that losing one is a failure that says
    // which.
    const char* forbidden[] = {"acos", "acosh", "asin",  "asinh", "atan2", "atanh", "cbrt",
                               "cosh", "expm1", "log10", "log1p", "sinh",  "tgamma"};
    u32 not_correctly_rounded = 0;
    for (const char* name : forbidden) {
        CY_CHECK(row(name).classification == FloatClass::NotCorrectlyRounded);
        CY_CHECK(is_forbidden_on_authoritative_path(name));
    }
    // Twelve of the thirteen ship a replacement; `tgamma` deliberately does not — it is forbidden
    // because it was measured wrong, not because a simulation needs it.
    u32 with_replacement = 0;
    for (const FloatFunction& entry : float_function_policy()) {
        if (entry.classification == FloatClass::NotCorrectlyRounded) {
            ++not_correctly_rounded;
            if (entry.replacement_available) {
                ++with_replacement;
            }
        }
    }
    CY_CHECK_EQ(not_correctly_rounded, 13U);
    CY_CHECK_EQ(with_replacement, 12U);
    CY_CHECK_FALSE(row("tgamma").replacement_available);

    const char* exact[] = {"sqrt",      "fabs", "floor", "ceil",      "trunc",   "round",
                           "nearbyint", "fma",  "fmod",  "remainder", "copysign"};
    for (const char* name : exact) {
        CY_CHECK(row(name).classification == FloatClass::ExactByIeee754);
        CY_CHECK_FALSE(is_forbidden_on_authoritative_path(name));
    }
    const char* correctly_rounded[] = {"exp", "exp2", "log",  "log2", "sin", "cos",
                                       "tan", "atan", "tanh", "erf",  "pow"};
    for (const char* name : correctly_rounded) {
        CY_CHECK(row(name).classification == FloatClass::CorrectlyRoundedHere);
    }
    // A function nobody measured has no opinion, and "no opinion" is not "permitted".
    CY_CHECK(find_float_function("j0") == nullptr);
    CY_CHECK_FALSE(is_forbidden_on_authoritative_path("j0"));
}

CY_TEST_CASE("determinism: permission follows the profile, and CrossPlatform gets the exact set") {
    // ReplayStable permits everything: a replay is replayed by the same binary against the same C
    // library, so a folded constant that is wrong in the same way twice reproduces.
    CY_CHECK(is_permitted_under("acos", DeterminismProfile::ReplayStable));
    CY_CHECK(is_permitted_under("j0", DeterminismProfile::None));

    CY_CHECK(is_permitted_under("sqrt", DeterminismProfile::SamePlatform));
    CY_CHECK(is_permitted_under("exp", DeterminismProfile::SamePlatform));
    CY_CHECK_FALSE(is_permitted_under("acos", DeterminismProfile::SamePlatform));

    // The line this project must not cross: `exp` is correctly rounded in glibc 2.39 on x86-64 and
    // that is a statement about one C library. A CrossPlatform claim may not rest on it.
    CY_CHECK(is_permitted_under("sqrt", DeterminismProfile::CrossPlatform));
    CY_CHECK_FALSE(is_permitted_under("exp", DeterminismProfile::CrossPlatform));
    CY_CHECK_FALSE(is_permitted_under("exp", DeterminismProfile::Lockstep));
    // And an unmeasured function is refused rather than waved through under any strict profile.
    CY_CHECK_FALSE(is_permitted_under("j0", DeterminismProfile::SamePlatform));
}

CY_TEST_CASE("determinism: ulp_distance orders the reals and saturates on a NaN") {
    CY_CHECK_EQ(ulp_distance(1.0, 1.0), u64{0});
    CY_CHECK_EQ(ulp_distance(0.0, -0.0), u64{0});  // compare equal, so not a divergence
    CY_CHECK_EQ(ulp_distance(1.0, std::nextafter(1.0, 2.0)), u64{1});
    CY_CHECK_EQ(ulp_distance(-1.0, std::nextafter(-1.0, -2.0)), u64{1});
    // Across zero: one step down from the smallest positive is the smallest negative.
    const f64 tiny = std::nextafter(0.0, 1.0);
    CY_CHECK_EQ(ulp_distance(tiny, -tiny), u64{2});
    CY_CHECK_EQ(ulp_distance(1.0, std::nan("")), ~u64{0});

    CY_CHECK_EQ(ulp_distance(1.0F, 1.0F), u64{0});
    CY_CHECK_EQ(ulp_distance(1.0F, std::nextafterf(1.0F, 2.0F)), u64{1});
}

CY_TEST_CASE("determinism: the twelve replacements agree with <cmath> to a stated bound") {
    // Each bound is this implementation's own claim, checked. They are not all the same number and
    // pretending they were would be a claim about `cbrt` that only holds for `acos`.
    u64 worst_acos = 0;
    u64 worst_asin = 0;
    for (const f64 x : kUnitPoints) {
        worst_acos = worst_acos > ulp_distance(fp::acos(x), std::acos(x))
                         ? worst_acos
                         : ulp_distance(fp::acos(x), std::acos(x));
        worst_asin = worst_asin > ulp_distance(fp::asin(x), std::asin(x))
                         ? worst_asin
                         : ulp_distance(fp::asin(x), std::asin(x));
        CY_CHECK_LE(ulp_distance(fp::atanh(x), std::atanh(x)), u64{4});
        CY_CHECK_LE(ulp_distance(fp::log1p(x), std::log1p(x)), u64{4});
    }
    CY_CHECK_LE(worst_acos, u64{4});
    CY_CHECK_LE(worst_asin, u64{4});
    CY_CHECK_EQ(fp::acos(1.0), 0.0);
    CY_CHECK_EQ(fp::asin(0.0), 0.0);

    for (const f64 x : kPositivePoints) {
        CY_CHECK_LE(ulp_distance(fp::cbrt(x), std::cbrt(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::cbrt(-x), std::cbrt(-x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::log10(x), std::log10(x)), u64{4});
        if (x >= 1.0) {
            CY_CHECK_LE(ulp_distance(fp::acosh(x), std::acosh(x)), u64{4});
        }
        CY_CHECK_LE(ulp_distance(fp::asinh(x), std::asinh(x)), u64{4});
        CY_CHECK_LE(ulp_distance(fp::asinh(-x), std::asinh(-x)), u64{4});
    }
    CY_CHECK_EQ(fp::cbrt(0.0), 0.0);
    CY_CHECK_EQ(fp::acosh(1.0), 0.0);

    for (const f64 x : kWidePoints) {
        CY_CHECK_LE(ulp_distance(fp::sinh(x), std::sinh(x)), u64{4});
        CY_CHECK_LE(ulp_distance(fp::cosh(x), std::cosh(x)), u64{4});
        CY_CHECK_LE(ulp_distance(fp::expm1(x), std::expm1(x)), u64{4});
    }
    CY_CHECK_EQ(fp::sinh(0.0), 0.0);
    CY_CHECK_EQ(fp::cosh(0.0), 1.0);
    CY_CHECK_EQ(fp::expm1(0.0), 0.0);

    // atan2 over every quadrant and both axes, including the branch cut, where the sign of a zero
    // decides the answer and a comparison against zero would get it wrong.
    const f64 pairs[][2] = {{1, 1},  {1, -1}, {-1, -1}, {-1, 1},         {0, 1},
                            {0, -1}, {1, 0},  {-1, 0},  {1e-300, 1e300}, {1e300, 1e-300}};
    for (const auto& pair : pairs) {
        CY_CHECK_LE(ulp_distance(fp::atan2(pair[0], pair[1]), std::atan2(pair[0], pair[1])),
                    u64{4});
    }
    CY_CHECK_EQ(fp::atan2(-0.0, -1.0), -std::atan2(0.0, -1.0));
}

CY_TEST_CASE("determinism: the float overloads round once, through double") {
    // Computing in `double` and rounding once is both more accurate than a `float` evaluation and,
    // more to the point here, one rounding rather than a chain of them.
    const f32 points[] = {-0.75F, -0.25F, 0.0F, 0.25F, 0.75F};
    for (const f32 x : points) {
        CY_CHECK_LE(ulp_distance(fp::acos(x), std::acos(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::asin(x), std::asin(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::atanh(x), std::atanh(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::sinh(x), std::sinh(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::cosh(x), std::cosh(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::expm1(x), std::expm1(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::log1p(x), std::log1p(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::cbrt(x), std::cbrt(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::asinh(x), std::asinh(x)), u64{2});
        CY_CHECK_LE(ulp_distance(fp::atan2(x, 1.0F), std::atan2(x, 1.0F)), u64{2});
    }
    CY_CHECK_LE(ulp_distance(fp::acosh(2.0F), std::acosh(2.0F)), u64{2});
    CY_CHECK_LE(ulp_distance(fp::log10(1000.0F), std::log10(1000.0F)), u64{2});
}
