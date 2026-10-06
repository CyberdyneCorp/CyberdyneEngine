// SPDX-License-Identifier: MIT
// Sine, cosine and tangent of a binary angle; arctangent, arcsine and arccosine to one. Design
// §5.2.
//
// The octant is the angle's top three bits, so the range reduction is exact: there is no
// multiplication by 2/pi to round, which is the reason `Angle` exists. Within an octant the reduced
// argument t = x / 2^29 lies in [0, 1] and the generated polynomials evaluate
//
//     sin(pi/4 t) = t P_sin(t^2)        cos(pi/4 t) = 1 + t^2 P_cos(t^2)
//
// in Q2.62. Odd octants are reflected (x = 2^29 - rest), and each octant then takes the sine or the
// cosine core with a sign. Because both cores come from one reduction and the reflection is exact,
// `sin(a + quarter) == cos(a)` and `sin(-a) == -sin(a)` hold for every angle, not approximately.
//
// The arctangent reduces by octant too: the signs and the comparison of |y| with |x| choose it,
// min/max becomes a ratio in [0, 1], and a ratio above tan(pi/8) is taken through
// atan(r) = pi/4 + atan((r - 1) / (r + 1)) to [-tan(pi/8), 0]. The polynomial returns TURNS, so the
// result is rounded once to an `Angle` and reflected by exact integer operations.

#include "kernel.h"

#include <cy/core/detmath/functions.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {
namespace kernel {
namespace {

/// Which core each octant's sine and cosine come from, and their signs (design §5.2).
struct Octant {
    bool swap;             ///< The sine comes from the cosine core, and the cosine from the sine.
    bool sine_negative;    ///< The sine is negated.
    bool cosine_negative;  ///< The cosine is negated.
};

constexpr Octant kOctants[8] = {
    {false, false, false},  // [0, 45)    sin r,      cos r
    {true, false, false},   // [45, 90)   cos(45-r),  sin(45-r)
    {true, false, true},    // [90, 135)  cos r,     -sin r
    {false, false, true},   // [135, 180) sin(45-r), -cos(45-r)
    {false, true, true},    // [180, 225) -sin r,    -cos r
    {true, true, true},     // [225, 270) -cos(45-r), -sin(45-r)
    {true, true, false},    // [270, 315) -cos r,     sin r
    {false, true, false},   // [315, 360) -sin(45-r), cos(45-r)
};

constexpr u32 kOctantBits = 29;
constexpr u32 kOctantMask = (u32{1} << kOctantBits) - 1;

}  // namespace

/// The arctangent, in turns, of a Q2.62 ratio in [0, 1]: an angle in [0, 1/8] turn, in Q2.62.
i64 atan_turns62(i64 ratio62) noexcept {
    i64 z = ratio62;
    i64 base = 0;
    if (ratio62 > coefficients::kTanPiOver8Q62) {
        // atan(r) = pi/4 + atan((r - 1) / (r + 1)), and pi/4 is an eighth of a turn.
        const U128 numerator = wide::shl(wide::from_u64(static_cast<u64>(kOne62 - ratio62)), 62);
        const u64 denominator = static_cast<u64>(kOne62) + static_cast<u64>(ratio62);
        z = -static_cast<i64>(wide::divrem(numerator, denominator).quotient.lo);
        base = i64{1} << 59;
    }
    return base + mul62(horner(coefficients::kAtan, mul62(z, z)), z);
}

SinCos62 sincos62(u32 angle) noexcept {
    const u32 octant = angle >> kOctantBits;
    const u32 rest = angle & kOctantMask;
    const u32 x = (octant & 1U) != 0 ? (u32{1} << kOctantBits) - rest : rest;
    const auto t = static_cast<i64>(u64{x} << 33);  // x / 2^29 in Q2.62; 1.0 at the octant's end
    const i64 u = mul62(t, t);
    const i64 sine_core = mul62(horner(coefficients::kSin, u), t);
    const i64 cosine_core = kOne62 + mul62(horner(coefficients::kCos, u), u);

    const Octant& rule = kOctants[octant];
    const i64 sine = rule.swap ? cosine_core : sine_core;
    const i64 cosine = rule.swap ? sine_core : cosine_core;
    return SinCos62{rule.sine_negative ? -sine : sine, rule.cosine_negative ? -cosine : cosine};
}

}  // namespace kernel

Fixed sin(Angle a) noexcept {
    return Fixed{kernel::to_fixed_signed(kernel::sincos62(a.raw).sin)};
}

Fixed cos(Angle a) noexcept {
    return Fixed{kernel::to_fixed_signed(kernel::sincos62(a.raw).cos)};
}

SinCos sincos(Angle a) noexcept {
    const kernel::SinCos62 both = kernel::sincos62(a.raw);
    return SinCos{Fixed{kernel::to_fixed_signed(both.sin)},
                  Fixed{kernel::to_fixed_signed(both.cos)}};
}

Fixed tan(Angle a) noexcept {
    const kernel::SinCos62 both = kernel::sincos62(a.raw);
    if (both.cos == 0) {
        detail::overflowed("tan at a pole");
        return both.sin >= 0 ? Fixed::max() : Fixed::min();
    }
    const u64 sine =
        both.sin < 0 ? u64{0} - static_cast<u64>(both.sin) : static_cast<u64>(both.sin);
    const u64 cosine =
        both.cos < 0 ? u64{0} - static_cast<u64>(both.cos) : static_cast<u64>(both.cos);
    const bool negative = (both.sin < 0) != (both.cos < 0);
    const U128 quotient = wide::divrem(wide::shl(wide::from_u64(sine), 32), cosine).quotient;
    if (quotient.hi != 0 || quotient.lo > static_cast<u64>(INT64_MAX)) {
        detail::overflowed("tan beyond the range");
        return negative ? Fixed::min() : Fixed::max();
    }
    const auto magnitude = static_cast<i64>(quotient.lo);
    return Fixed{negative ? -magnitude : magnitude};
}

Angle atan2(Fixed y, Fixed x) noexcept {
    if (x.raw == 0 && y.raw == 0) {
        return Angle{};
    }
    const u64 ax = x.raw < 0 ? u64{0} - static_cast<u64>(x.raw) : static_cast<u64>(x.raw);
    const u64 ay = y.raw < 0 ? u64{0} - static_cast<u64>(y.raw) : static_cast<u64>(y.raw);
    const bool swap = ay > ax;
    const u64 numerator = swap ? ax : ay;
    const u64 denominator = swap ? ay : ax;
    if (denominator == 0) {
        return Angle{};  // unreachable: both zero returned above; stated for the analyser
    }
    const auto ratio = static_cast<i64>(
        wide::divrem(wide::shl(wide::from_u64(numerator), 62), denominator).quotient.lo);

    const i64 turns62 = kernel::atan_turns62(ratio);
    auto angle = static_cast<u32>(static_cast<u64>(turns62 + (i64{1} << 29)) >> 30);
    if (swap) {
        angle = (u32{1} << 30) - angle;
    }
    if (x.raw < 0) {
        angle = (u32{1} << 31) - angle;
    }
    if (y.raw < 0) {
        angle = 0U - angle;
    }
    return Angle{angle};
}

Angle atan(Fixed x) noexcept {
    return atan2(x, Fixed::one());
}

namespace {

/// `x` clamped to [-1, 1], and `sqrt(1 - x^2)` of the clamped value through the wide square root:
/// 1 - x^2 is exact in Q64.64, so the only rounding is the root's own.
struct UnitLeg {
    Fixed clamped;
    Fixed other;
};

UnitLeg unit_leg(Fixed x) noexcept {
    Fixed clamped = x;
    if (x > Fixed::one()) {
        detail::overflowed("asin/acos outside [-1, 1]");
        clamped = Fixed::one();
    } else if (x < -Fixed::one()) {
        detail::overflowed("asin/acos outside [-1, 1]");
        clamped = -Fixed::one();
    }
    const WideFixed one_minus_square =
        WideFixed::from_fixed(Fixed::one()) - WideFixed::product(clamped, clamped);
    return UnitLeg{clamped, sqrt(one_minus_square)};
}

}  // namespace

Angle asin(Fixed x) noexcept {
    const UnitLeg leg = unit_leg(x);
    return atan2(leg.clamped, leg.other);
}

Angle acos(Fixed x) noexcept {
    const UnitLeg leg = unit_leg(x);
    return atan2(leg.other, leg.clamped);
}

Angle Angle::from_radians(Fixed radians) noexcept {
    const U128 product = wide::mul_i64(radians.raw, coefficients::kInvTwoPiQ64);
    return Angle{static_cast<u32>(wide::add(product, U128{u64{1} << 63, 0}).hi)};
}

Fixed Angle::radians() const noexcept {
    return Fixed{wide::mul_shift(static_cast<i64>(raw), coefficients::kTwoPiQ60, 60)};
}

Fixed Angle::signed_radians() const noexcept {
    return Fixed{
        wide::mul_shift(static_cast<i64>(static_cast<i32>(raw)), coefficients::kTwoPiQ60, 60)};
}

Fixed pi() noexcept {
    return Fixed{coefficients::kPiQ32};
}

Fixed two_pi() noexcept {
    return Fixed{coefficients::kTwoPiQ32};
}

Fixed half_pi() noexcept {
    return Fixed{coefficients::kHalfPiQ32};
}

Fixed e() noexcept {
    return Fixed{coefficients::kEQ32};
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
