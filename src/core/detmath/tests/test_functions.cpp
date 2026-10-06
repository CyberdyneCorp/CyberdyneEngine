// SPDX-License-Identifier: MIT
// THE TRANSCENDENTALS' EXACT PROPERTIES AND DEFINED EDGES. Tasks 1.4, 2.2 and 2.3; design §5 and
// §10.3.
//
// The error bounds are integration.detmath_vectors' (they need the oracle's files). What is here
// holds EXACTLY, for every input, because of how the reductions are built — so it is asserted with
// ==, never with a tolerance:
//
//   sin(a + quarter) == cos(a)       both cores come from one octant reduction
//   sin(-a) == -sin(a)               the magnitude is rounded before the sign is applied
//   atan2(-y, x) == -atan2(y, x)     the reflections after the rounding are integer operations
//   exp2(n) == 2^n, log2(2^n) == n   the integer part is a shift and the polynomial is exact at 0
//   sqrt is correctly rounded        decided from the exact remainder, never estimated
//
// plus monotonicity over sorted inputs, sin^2 + cos^2 within a stated tolerance, and the answer
// each function gives outside its domain.

#include "detmath_test.h"

#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>
#include <cy/test/test.h>

#include <algorithm>
#include <array>
#include <initializer_list>

namespace {

using cy::i64;
using cy::u32;
using cy::u64;
using cy::detmath::Angle;
using cy::detmath::Fixed;
using cy::detmath::WideFixed;
namespace dm = cy::detmath;

using cy::usize;

constexpr i64 kOne = Fixed::kOneRaw;

/// A spread of angles that includes every octant boundary and its neighbours.
[[nodiscard]] std::array<u32, 512> sample_angles(u64 seed) {
    std::array<u32, 512> angles{};
    cy::detmath_test::Rng rng(seed);
    for (u32& angle : angles) {
        angle = static_cast<u32>(rng.next() >> 32);
    }
    for (u32 octant = 0; octant < 8; ++octant) {
        angles[(octant * 3) + 0] = octant << 29;
        angles[(octant * 3) + 1] = (octant << 29) + 1;
        angles[(octant * 3) + 2] = (octant << 29) - 1;
    }
    return angles;
}

}  // namespace

CY_TEST_CASE("detmath: sine and cosine are one reduction, so their identities are exact") {
    for (const u32 raw : sample_angles(0xA9'0001ULL)) {
        const Angle a = Angle::from_raw(raw);
        CY_TEST_INFO("angle " << raw);
        CY_CHECK(dm::sin(a + Angle::quarter()) == dm::cos(a));
        CY_CHECK(dm::sin(-a) == -dm::sin(a));
        CY_CHECK(dm::cos(-a) == dm::cos(a));
        CY_CHECK(dm::sin(a + Angle::half()) == -dm::sin(a));
        const dm::SinCos both = dm::sincos(a);
        CY_CHECK(both.sin == dm::sin(a));
        CY_CHECK(both.cos == dm::cos(a));
    }
}

CY_TEST_CASE("detmath: sine and cosine are exact at the axes") {
    CY_CHECK(dm::sin(Angle{}) == Fixed::zero());
    CY_CHECK(dm::cos(Angle{}) == Fixed::one());
    CY_CHECK(dm::sin(Angle::quarter()) == Fixed::one());
    CY_CHECK(dm::cos(Angle::quarter()) == Fixed::zero());
    CY_CHECK(dm::sin(Angle::half()) == Fixed::zero());
    CY_CHECK(dm::cos(Angle::half()) == -Fixed::one());
    CY_CHECK(dm::sin(-Angle::quarter()) == -Fixed::one());
    CY_CHECK(dm::tan(Angle{}) == Fixed::zero());
    CY_CHECK(dm::tan(Angle::eighth()).raw >= kOne - 1);
    CY_CHECK(dm::tan(Angle::eighth()).raw <= kOne);
}

CY_TEST_CASE("detmath: sin^2 + cos^2 is within 4 ulp of one") {
    // Each of the two is within 1 ulp, and squaring a value at most 1 at most doubles that, so 4
    // ulp is the bound the declared ones imply rather than a figure chosen to pass.
    i64 worst = 0;
    for (const u32 raw : sample_angles(0xA9'0002ULL)) {
        const dm::SinCos both = dm::sincos(Angle::from_raw(raw));
        const i64 sum = (both.sin * both.sin + both.cos * both.cos).raw;
        worst = std::max(worst, sum > kOne ? sum - kOne : kOne - sum);
    }
    CY_TEST_MESSAGE("sin^2 + cos^2: worst " << worst << " ulp from one, bound 4");
    CY_CHECK_LE(worst, 4);
}

CY_TEST_CASE("detmath: tan saturates at its poles, toward the sign of the sine") {
    CY_CHECK(dm::tan(Angle::quarter()) == Fixed::max());
    CY_CHECK(dm::tan(-Angle::quarter()) == Fixed::min());
    CY_CHECK(dm::tan(Angle::from_raw((1U << 30) - 1)) > Fixed::from_int(100000000));
    CY_CHECK(dm::tan(Angle::from_raw((1U << 30) + 1)) < Fixed::from_int(-100000000));
}

CY_TEST_CASE("detmath: atan2 is exactly odd in y and exact on the axes and diagonals") {
    CY_CHECK_EQ(dm::atan2(Fixed::zero(), Fixed::zero()).raw, 0U);
    CY_CHECK_EQ(dm::atan2(Fixed::zero(), Fixed::one()).raw, 0U);
    CY_CHECK_EQ(dm::atan2(Fixed::one(), Fixed::zero()).raw, 1U << 30);
    CY_CHECK_EQ(dm::atan2(Fixed::zero(), -Fixed::one()).raw, 1U << 31);
    CY_CHECK_EQ(dm::atan2(-Fixed::one(), Fixed::zero()).raw, 3U << 30);
    CY_CHECK_EQ(dm::atan2(Fixed::one(), Fixed::one()).raw, 1U << 29);
    CY_CHECK_EQ(dm::atan2(Fixed::min(), Fixed::min()).raw, 5U << 29);
    CY_CHECK_EQ(dm::atan(Fixed::one()).raw, 1U << 29);
    cy::detmath_test::Rng rng(0xA9'0003ULL);
    for (int index = 0; index < 400; ++index) {
        const Fixed y = Fixed::from_raw(rng.scaled());
        const Fixed x = Fixed::from_raw(rng.scaled());
        CY_TEST_INFO("y " << y.raw << " x " << x.raw);
        if (y != Fixed::min()) {
            CY_CHECK(dm::atan2(-y, x) == -dm::atan2(y, x));
        }
        // Doubling both coordinates leaves the ratio, and so the angle, exactly unchanged.
        if (y.raw > INT64_MIN / 2 && y.raw < INT64_MAX / 2 && x.raw > INT64_MIN / 2 &&
            x.raw < INT64_MAX / 2) {
            CY_CHECK(dm::atan2(y << 1, x << 1) == dm::atan2(y, x));
        }
    }
}

CY_TEST_CASE("detmath: asin and acos clamp outside [-1, 1] and are exact at the ends") {
    CY_CHECK_EQ(dm::asin(Fixed::one()).raw, 1U << 30);
    CY_CHECK_EQ(dm::asin(-Fixed::one()).raw, 3U << 30);
    CY_CHECK_EQ(dm::asin(Fixed::zero()).raw, 0U);
    CY_CHECK_EQ(dm::acos(Fixed::one()).raw, 0U);
    CY_CHECK_EQ(dm::acos(-Fixed::one()).raw, 1U << 31);
    CY_CHECK_EQ(dm::acos(Fixed::zero()).raw, 1U << 30);
    CY_CHECK(dm::asin(Fixed::from_int(5)) == dm::asin(Fixed::one()));
    CY_CHECK(dm::acos(Fixed::min()) == dm::acos(-Fixed::one()));
    // asin(1/2) is 1/12 turn.
    const u32 twelfth = dm::asin(Fixed::half()).raw;
    CY_CHECK(twelfth - ((1ULL << 32) / 12) + 1U <= 2U);
}

CY_TEST_CASE("detmath: exp2 and log2 are exact at powers of two") {
    for (int n = -32; n <= 30; ++n) {
        CY_TEST_INFO("n " << n);
        const Fixed power = n >= 0 ? Fixed::from_raw(kOne << n) : Fixed::from_raw(kOne >> -n);
        CY_CHECK(dm::exp2(Fixed::from_int(n)) == power);
        CY_CHECK(dm::log2(power) == Fixed::from_int(n));
    }
    CY_CHECK(dm::exp(Fixed::zero()) == Fixed::one());
    CY_CHECK(dm::log(Fixed::one()) == Fixed::zero());
    CY_CHECK(dm::pow(Fixed::from_int(2), Fixed::from_int(10)) == Fixed::from_int(1024));
    CY_CHECK(dm::pow(Fixed::from_int(7), Fixed::zero()) == Fixed::one());
}

CY_TEST_CASE("detmath: exp2, exp and pow saturate above the range and reach zero below it") {
    CY_CHECK(dm::exp2(Fixed::from_int(31)) == Fixed::max());
    CY_CHECK(dm::exp2(Fixed::max()) == Fixed::max());
    // 2^-33 is exactly half an ulp, a tie, and ties round toward +infinity; 2^-34 is below it.
    CY_CHECK(dm::exp2(Fixed::from_int(-33)) == Fixed::epsilon());
    CY_CHECK(dm::exp2(Fixed::from_int(-34)) == Fixed::zero());
    CY_CHECK(dm::exp2(Fixed::min()) == Fixed::zero());
    CY_CHECK(dm::exp(Fixed::from_int(22)) == Fixed::max());
    CY_CHECK(dm::exp(Fixed::from_int(-23)) == Fixed::zero());
    CY_CHECK(dm::pow(Fixed::from_int(2), Fixed::from_int(40)) == Fixed::max());
    CY_CHECK(dm::pow(Fixed::from_int(2), Fixed::from_int(-40)) == Fixed::zero());
}

CY_TEST_CASE("detmath: the logarithms and pow answer a defined value outside their domain") {
    CY_CHECK(dm::log2(Fixed::zero()) == Fixed::min());
    CY_CHECK(dm::log2(-Fixed::one()) == Fixed::min());
    CY_CHECK(dm::log(Fixed::min()) == Fixed::min());
    CY_CHECK(dm::pow(Fixed::zero(), Fixed::one()) == Fixed::zero());
    CY_CHECK(dm::pow(-Fixed::one(), Fixed::one()) == Fixed::zero());
    CY_CHECK(dm::sqrt(Fixed::min()) == Fixed::zero());
}

CY_TEST_CASE("detmath: sqrt is correctly rounded") {
    CY_CHECK(dm::sqrt(Fixed::zero()) == Fixed::zero());
    CY_CHECK(dm::sqrt(Fixed::one()) == Fixed::one());
    CY_CHECK(dm::sqrt(Fixed::from_int(4)) == Fixed::from_int(2));
    CY_CHECK(dm::sqrt(Fixed::from_int(1 << 30)) == Fixed::from_int(1 << 15));
    // sqrt(2^-32) = 2^-16: an exact square below one ulp's square root.
    CY_CHECK_EQ(dm::sqrt(Fixed::epsilon()).raw, i64{1} << 16);
    // sqrt(2) = 1.41421356237309504880..., raw 6074000999.95..., which rounds up.
    CY_CHECK_EQ(dm::sqrt(Fixed::from_int(2)).raw, 6074001000);
    // sqrt(max) = sqrt(2^31 - 2^-32): raw 199032864766430.4..., which rounds down.
    CY_CHECK_EQ(dm::sqrt(Fixed::max()).raw, 199032864766430);
    // The defining property, over a sweep: r is the nearest integer to sqrt(raw * 2^32), so
    // (r - 1/2)^2 < n < (r + 1/2)^2, i.e. r^2 - r < n <= r^2 + r in integers.
    cy::detmath_test::Rng rng(0xA9'0004ULL);
    for (int index = 0; index < 300; ++index) {
        const i64 raw = rng.scaled() & INT64_MAX;
        const u64 root = static_cast<u64>(dm::sqrt(Fixed::from_raw(raw)).raw);
        const cy::detmath::U128 n = dm::wide::shl(dm::wide::from_u64(static_cast<u64>(raw)), 32);
        const cy::detmath::U128 square = dm::wide::mul_u64(root, root);
        CY_TEST_INFO("raw " << raw << " root " << root);
        if (root != 0) {  // (0 - 1/2)^2 < n holds for every n, including 0
            CY_CHECK(dm::wide::less(dm::wide::sub(square, dm::wide::from_u64(root)), n));
        }
        CY_CHECK_FALSE(dm::wide::less(dm::wide::add(square, dm::wide::from_u64(root)), n));
    }
}

CY_TEST_CASE("detmath: the wide square root takes a squared length a Fixed cannot hold") {
    const Fixed side = Fixed::from_int(3000000);
    const WideFixed length_squared =
        WideFixed::product(side, side) +
        WideFixed::product(Fixed::from_int(4000000), Fixed::from_int(4000000));
    CY_CHECK(dm::sqrt(length_squared) == Fixed::from_int(5000000));
    CY_CHECK(dm::sqrt(-WideFixed::from_fixed(Fixed::one())) == Fixed::zero());
    CY_CHECK(dm::sqrt(WideFixed::from_raw(cy::detmath::U128{~u64{0}, INT64_MAX})) == Fixed::max());
}

CY_TEST_CASE("detmath: sqrt, exp2, log2 and atan are monotonic over sorted inputs") {
    std::array<i64, 600> inputs{};
    cy::detmath_test::Rng rng(0xA9'0005ULL);
    for (i64& input : inputs) {
        input = rng.scaled();
    }
    std::ranges::sort(inputs);
    for (usize index = 1; index < inputs.size(); ++index) {
        const Fixed before = Fixed::from_raw(inputs[index - 1]);
        const Fixed after = Fixed::from_raw(inputs[index]);
        CY_TEST_INFO("between " << before.raw << " and " << after.raw);
        CY_CHECK(dm::sqrt(before) <= dm::sqrt(after));
        CY_CHECK(dm::exp2(before >> 26) <= dm::exp2(after >> 26));
        CY_CHECK(dm::atan(before).signed_turns() <= dm::atan(after).signed_turns());
        if (before.raw > 0) {
            CY_CHECK(dm::log2(before) <= dm::log2(after));
        }
    }
}
