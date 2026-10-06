// THE ARITHMETIC RULES, CASE BY CASE. Tasks 1.2 and 1.4, design §4.3.
//
// Each rule of the table is a case here, asserted on the values where an implementation that broke
// it would differ: the rounding ties for `*`, the sign of a truncated quotient, the two
// division-by-zero answers, the wrap at each end of the range, and the saturation of a narrowing.
// The golden vectors (integration.detmath_vectors) hold the same rules over thousands of inputs;
// these are the ones a reader can check by hand.

#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/overflow.h>
#include <cy/test/test.h>

#include <cstring>
#include <initializer_list>

namespace {

using cy::i32;
using cy::i64;
using cy::u32;
using cy::u64;
using cy::detmath::Angle;
using cy::detmath::Fixed;
using cy::detmath::Fixed16;
using cy::detmath::OverflowGuard;
using cy::detmath::U128;
using cy::detmath::WideFixed;

constexpr i64 kOne = Fixed::kOneRaw;

[[nodiscard]] Fixed raw(i64 value) noexcept {
    return Fixed::from_raw(value);
}

}  // namespace

CY_TEST_CASE("detmath: addition and subtraction wrap through u64") {
    CY_CHECK_EQ((Fixed::max() + Fixed::epsilon()).raw, INT64_MIN);
    CY_CHECK_EQ((Fixed::min() - Fixed::epsilon()).raw, INT64_MAX);
    CY_CHECK_EQ((-Fixed::min()).raw, INT64_MIN);
    CY_CHECK_EQ((Fixed::one() + Fixed::one()).raw, 2 * kOne);
    CY_CHECK_EQ((Fixed::from_int(-3) - Fixed::from_int(4)).raw, -7 * kOne);
    CY_CHECK_EQ(Fixed::from_int(INT32_MIN).raw, INT64_MIN);
    CY_CHECK_EQ(Fixed::from_int(INT32_MAX).raw, i64{INT32_MAX} * kOne);
}

CY_TEST_CASE("detmath: multiplication rounds to nearest with ties toward +infinity") {
    // A raw product of exactly k + 1/2 ulp is a tie. Toward +infinity means +0.5 -> 1, +2.5 -> 3,
    // -0.5 -> 0, -2.5 -> -2. Round-half-even would give 0, 2, 0, -2 and round-half-away 1, 3, -1,
    // -3, so each of the three plausible alternatives is wrong on at least one of these.
    const Fixed half_ulp = raw(i64{1} << 31);
    CY_CHECK_EQ((raw(1) * half_ulp).raw, 1);
    CY_CHECK_EQ((raw(5) * half_ulp).raw, 3);
    CY_CHECK_EQ((raw(-1) * half_ulp).raw, 0);
    CY_CHECK_EQ((raw(-5) * half_ulp).raw, -2);
    // Not a tie: just below and above half.
    CY_CHECK_EQ((raw(1) * raw((i64{1} << 31) - 1)).raw, 0);
    CY_CHECK_EQ((raw(1) * raw((i64{1} << 31) + 1)).raw, 1);
    // Exact products are exact.
    CY_CHECK_EQ((Fixed::from_int(-3) * Fixed::from_int(7)).raw, -21 * kOne);
    CY_CHECK_EQ((Fixed::half() * Fixed::half()).raw, kOne / 4);
    CY_CHECK_EQ((Fixed::epsilon() * Fixed::epsilon()).raw, 0);
}

CY_TEST_CASE("detmath: multiplication wraps the rounded product to 64 bits") {
    // max * max is about 2^62 in value: its raw is the low 64 bits of the rounded product.
    CY_CHECK_EQ((Fixed::max() * Fixed::max()).raw, static_cast<i64>(0xFFFF'FFFF'0000'0000ULL));
    CY_CHECK_EQ((Fixed::min() * -Fixed::one()).raw, INT64_MIN);
    CY_CHECK_EQ((Fixed::min() * Fixed::min()).raw, 0);
}

CY_TEST_CASE("detmath: division truncates toward zero and wraps") {
    // 1/3 and -1/3 truncate to the same magnitude: toward zero, not toward -infinity.
    const i64 third = (Fixed::one() / Fixed::from_int(3)).raw;
    CY_CHECK_EQ(third, 0x5555'5555);
    CY_CHECK_EQ((-Fixed::one() / Fixed::from_int(3)).raw, -third);
    CY_CHECK_EQ((Fixed::one() / Fixed::from_int(-3)).raw, -third);
    CY_CHECK_EQ((raw(7) / raw(2 * kOne)).raw, 3);
    CY_CHECK_EQ((raw(-7) / raw(2 * kOne)).raw, -3);
    CY_CHECK_EQ((Fixed::from_int(6) / Fixed::from_int(-2)).raw, -3 * kOne);
    // The quotient of min by -epsilon is 2^95: the low 64 bits of it are 0.
    CY_CHECK_EQ((Fixed::min() / raw(-1)).raw, 0);
    CY_CHECK_EQ((Fixed::min() / -Fixed::one()).raw, INT64_MIN);
}

CY_TEST_CASE("detmath: division by zero gives max for a >= 0 and min for a < 0, in every build") {
    CY_CHECK(Fixed::one() / Fixed::zero() == Fixed::max());
    CY_CHECK(Fixed::zero() / Fixed::zero() == Fixed::max());
    CY_CHECK(-Fixed::epsilon() / Fixed::zero() == Fixed::min());
    CY_CHECK(Fixed::min() / Fixed::zero() == Fixed::min());
}

CY_TEST_CASE("detmath: shifts are arithmetic and comparison is the raw integer's") {
    CY_CHECK_EQ((raw(-1) >> 1).raw, -1);  // floor, not truncation
    CY_CHECK_EQ((raw(-3) >> 1).raw, -2);
    CY_CHECK_EQ((Fixed::one() << 3).raw, 8 * kOne);
    CY_CHECK(Fixed::min() < Fixed::zero());
    CY_CHECK(raw(-1) < raw(0));
    CY_CHECK(Fixed::one() == Fixed::from_int(1));
    CY_CHECK_EQ(raw(-1).floor(), -1);
    CY_CHECK_EQ(raw(-kOne - 1).fraction().raw, kOne - 1);
}

CY_TEST_CASE("detmath: the saturating forms clamp where the operators wrap") {
    CY_CHECK(cy::detmath::saturating_add(Fixed::max(), Fixed::one()) == Fixed::max());
    CY_CHECK(cy::detmath::saturating_add(Fixed::min(), -Fixed::one()) == Fixed::min());
    CY_CHECK(cy::detmath::saturating_sub(Fixed::min(), Fixed::one()) == Fixed::min());
    CY_CHECK(cy::detmath::saturating_sub(Fixed::max(), -Fixed::one()) == Fixed::max());
    CY_CHECK(cy::detmath::saturating_mul(Fixed::max(), Fixed::max()) == Fixed::max());
    CY_CHECK(cy::detmath::saturating_mul(Fixed::max(), Fixed::min()) == Fixed::min());
    CY_CHECK(cy::detmath::saturating_mul(Fixed::half(), Fixed::half()) ==
             Fixed::half() * Fixed::half());
}

CY_TEST_CASE("detmath: Fixed16 narrows by rounding ties toward +infinity and saturating") {
    // Fixed16's ulp is 2^16 Fixed ulps, so a raw of 2^15 is exactly half of one.
    CY_CHECK_EQ(Fixed16::narrow(raw(i64{1} << 15)).raw, 1);
    CY_CHECK_EQ(Fixed16::narrow(raw(-(i64{1} << 15))).raw, 0);
    CY_CHECK_EQ(Fixed16::narrow(raw(3 * (i64{1} << 15))).raw, 2);
    CY_CHECK_EQ(Fixed16::narrow(raw(-3 * (i64{1} << 15))).raw, -1);
    CY_CHECK_EQ(Fixed16::narrow(raw((i64{1} << 15) - 1)).raw, 0);
    CY_CHECK_EQ(Fixed16::narrow(Fixed::max()).raw, INT32_MAX);
    CY_CHECK_EQ(Fixed16::narrow(Fixed::min()).raw, INT32_MIN);
    // Widening is exact, and narrowing a widened value gives it back.
    for (const i32 value : {0, 1, -1, INT32_MAX, INT32_MIN, 12345, -98765}) {
        CY_CHECK_EQ(Fixed16::narrow(Fixed16::from_raw(value).widen()).raw, value);
    }
}

CY_TEST_CASE("detmath: angles wrap exactly and scale through 128 bits") {
    CY_CHECK_EQ((Angle::half() + Angle::half()).raw, 0U);
    CY_CHECK_EQ((-Angle::quarter()).raw, 3U << 30);
    CY_CHECK_EQ((Angle::from_raw(1) - Angle::from_raw(2)).raw, 0xFFFF'FFFFU);
    CY_CHECK_EQ((Angle::quarter() * Fixed::from_int(2)).raw, 1U << 31);
    CY_CHECK_EQ((Angle::quarter() * Fixed::from_int(5)).raw, 1U << 30);  // 5/4 turn wraps to 1/4
    CY_CHECK_EQ((Angle::quarter() * -Fixed::one()).raw, 3U << 30);
    // A tie in the scale rounds toward +infinity, as `*` does.
    CY_CHECK_EQ((Angle::from_raw(1) * Fixed::half()).raw, 1U);
    CY_CHECK_EQ((Angle::from_raw(3) * Fixed::half()).raw, 2U);
    // Turns are the fractional bits, exactly.
    CY_CHECK_EQ(Angle::from_turns(Fixed::from_int(7) + Fixed::half()).raw, 1U << 31);
    CY_CHECK_EQ(Angle::from_turns(-Fixed::half() >> 1).raw, 3U << 30);
    CY_CHECK_EQ(Angle::quarter().turns().raw, kOne / 4);
    CY_CHECK_EQ(Angle::from_raw(3U << 30).signed_turns().raw, -kOne / 4);
}

CY_TEST_CASE("detmath: radians and angles convert through the generated constants") {
    CY_CHECK_EQ(Angle::from_radians(cy::detmath::pi()).raw, 1U << 31);
    CY_CHECK_EQ(Angle::from_radians(cy::detmath::half_pi()).raw, 1U << 30);
    CY_CHECK_EQ(Angle::from_radians(-cy::detmath::half_pi()).raw, 3U << 30);
    CY_CHECK_EQ(Angle::from_radians(cy::detmath::two_pi()).raw, 0U);
    CY_CHECK(Angle::half().radians() == cy::detmath::pi());
    CY_CHECK(Angle::quarter().radians() == cy::detmath::half_pi());
    CY_CHECK(Angle::half().signed_radians() == -cy::detmath::pi());
    CY_CHECK(Angle::from_raw(3U << 30).signed_radians() == -cy::detmath::half_pi());
}

CY_TEST_CASE("detmath: a product of two Fixed is exact in WideFixed, and narrows like *") {
    const WideFixed square = WideFixed::product(Fixed::max(), Fixed::max());
    CY_CHECK(square.raw == cy::detmath::wide::mul_u64(INT64_MAX, INT64_MAX));
    CY_CHECK(WideFixed::product(Fixed::min(), Fixed::one()) == WideFixed::from_fixed(Fixed::min()));
    // Distances beyond 2^15.5 overflow as a Fixed square and still compare correctly here.
    const Fixed far = Fixed::from_int(100000);
    const Fixed farther = far + Fixed::epsilon();
    CY_CHECK(WideFixed::product(far, far) < WideFixed::product(farther, farther));
    CY_CHECK(-WideFixed::product(far, far) < WideFixed{});
    // narrow() is `*`'s rounding, so a product narrowed is the product.
    for (const i64 a : {i64{5}, i64{-5}, kOne + 3, -kOne / 3}) {
        for (const i64 b : {i64{1} << 31, kOne / 7, -(kOne * 3)}) {
            CY_CHECK(WideFixed::product(raw(a), raw(b)).narrow() == raw(a) * raw(b));
        }
    }
    CY_CHECK(square.saturating_narrow() == Fixed::max());
    CY_CHECK((-square).saturating_narrow() == Fixed::min());
}

CY_TEST_CASE("detmath: an overflow is counted and the first site named, in development builds") {
    OverflowGuard guard;
    const Fixed wrapped = Fixed::max() + Fixed::one();
    const Fixed quotient = Fixed::one() / Fixed::zero();
    const Fixed root = cy::detmath::sqrt(-Fixed::one());
    // The arithmetic is the same whether or not anything counts it.
    CY_CHECK_EQ(wrapped.raw, INT64_MIN + kOne - 1);
    CY_CHECK(quotient == Fixed::max());
    CY_CHECK(root == Fixed::zero());
    if constexpr (cy::detmath::kCountsOverflow) {
        CY_CHECK_EQ(guard.overflows(), 3U);
        CY_CHECK(std::strcmp(guard.first_site(), "Fixed +") == 0);
        {
            // Guards nest: the inner one counts, and the outer one is untouched until it ends.
            OverflowGuard inner;
            (void)(Fixed::min() - Fixed::one());
            CY_CHECK_EQ(inner.overflows(), 1U);
            CY_CHECK(std::strcmp(inner.first_site(), "Fixed -") == 0);
        }
        CY_CHECK_EQ(guard.overflows(), 3U);
        (void)(Fixed::max() * Fixed::max());
        CY_CHECK_EQ(guard.overflows(), 4U);
    } else {
        // Shipping: "compiles to nothing". The guard exists and counts nothing.
        CY_CHECK_EQ(guard.overflows(), 0U);
    }
    // In-range arithmetic counts nothing in any build.
    guard.clear();
    (void)(Fixed::from_int(3) * Fixed::from_int(-4) + Fixed::one() / Fixed::from_int(7));
    CY_CHECK_EQ(guard.overflows(), 0U);
    CY_CHECK(std::strcmp(guard.first_site(), "") == 0);
}

static_assert(sizeof(Fixed) == 8, "a Fixed is its i64, so a FixedVec3 is 24 bytes (design §4.2)");
static_assert(sizeof(Fixed16) == 4, "a Fixed16 is its i32");
static_assert(sizeof(Angle) == 4, "an Angle is its u32");
static_assert(sizeof(WideFixed) == 16, "a WideFixed is two limbs");
static_assert((Fixed::one() + Fixed::one()).raw == 2 * kOne, "+ is usable in constant expressions");
static_assert(Fixed16::narrow(Fixed::half()).widen() == Fixed::half(), "narrow is constexpr");
static_assert(sizeof(u32) == 4 && sizeof(u64) == 8 && sizeof(U128) == 16);
