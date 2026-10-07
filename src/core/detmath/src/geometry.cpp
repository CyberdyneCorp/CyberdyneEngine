// SPDX-License-Identifier: MIT
// Vectors, rotations and shapes over `Fixed`. Design §6.
//
// A kernel source: it is compiled again with `-mgeneral-regs-only`, so a float here fails the
// build. Every rounding is the one vec.h and shapes.h state; most outputs are an exact Q64.64 sum
// narrowed once, which is why the helpers below take products rather than `Fixed` operands.

#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/shapes.h>
#include <cy/core/detmath/vec.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

namespace {

/// `a b + c d`, rounded once.
[[nodiscard]] Fixed sum_of_products(Fixed a, Fixed b, Fixed c, Fixed d) noexcept {
    return (WideFixed::product(a, b) + WideFixed::product(c, d)).narrow();
}

/// `a b - c d`, rounded once.
[[nodiscard]] Fixed difference_of_products(Fixed a, Fixed b, Fixed c, Fixed d) noexcept {
    return (WideFixed::product(a, b) - WideFixed::product(c, d)).narrow();
}

}  // namespace

Fixed mul_div(Fixed a, Fixed b, Fixed c) noexcept {
    const U128 product = wide::mul_i64(a.raw, b.raw);
    if (c.raw == 0) {
        detail::overflowed("mul_div by zero");
        return wide::is_negative(product) ? Fixed::min() : Fixed::max();
    }
    // The Q64.64 product over a Q32.32 divisor is the Q32.32 quotient.
    const u64 divisor = c.raw < 0 ? u64{0} - static_cast<u64>(c.raw) : static_cast<u64>(c.raw);
    U128 quotient = wide::divrem(wide::magnitude(product), divisor).quotient;
    if (wide::is_negative(product) != (c.raw < 0)) {
        quotient = wide::negate(quotient);
    }
    if (!wide::fits_i64(quotient)) {
        detail::overflowed("mul_div");
    }
    return Fixed{static_cast<i64>(quotient.lo)};
}

// --- FixedVec2
// ------------------------------------------------------------------------------------

Fixed length(FixedVec2 v) noexcept {
    return sqrt(length_squared(v));
}

Fixed distance(FixedVec2 a, FixedVec2 b) noexcept {
    return sqrt(distance_squared(a, b));
}

FixedVec2 normalize(FixedVec2 v) noexcept {
    const Fixed magnitude = length(v);
    if (magnitude.raw == 0) {
        detail::overflowed("normalize of the zero vector");
        return FixedVec2::zero();
    }
    return {v.x / magnitude, v.y / magnitude};
}

FixedVec2 clamp_length(FixedVec2 v, Fixed limit) noexcept {
    if (length_squared(v) <= WideFixed::product(limit, limit)) {
        return v;
    }
    // `magnitude > limit >= 0`, so each scaled component is smaller than the original.
    const Fixed magnitude = length(v);
    return {mul_div(v.x, limit, magnitude), mul_div(v.y, limit, magnitude)};
}

// --- FixedVec3
// ------------------------------------------------------------------------------------

FixedVec3 cross(FixedVec3 a, FixedVec3 b) noexcept {
    return {difference_of_products(a.y, b.z, a.z, b.y), difference_of_products(a.z, b.x, a.x, b.z),
            difference_of_products(a.x, b.y, a.y, b.x)};
}

Fixed length(FixedVec3 v) noexcept {
    return sqrt(length_squared(v));
}

Fixed distance(FixedVec3 a, FixedVec3 b) noexcept {
    return sqrt(length_squared(b - a));
}

FixedVec3 normalize(FixedVec3 v) noexcept {
    const Fixed magnitude = length(v);
    if (magnitude.raw == 0) {
        detail::overflowed("normalize of the zero vector");
        return FixedVec3::zero();
    }
    return {v.x / magnitude, v.y / magnitude, v.z / magnitude};
}

// --- Rot2
// -----------------------------------------------------------------------------------------

Rot2 Rot2::from_angle(Angle angle) noexcept {
    const SinCos both = sincos(angle);
    return Rot2{both.cos, both.sin};
}

FixedVec2 rotate(Rot2 r, FixedVec2 v) noexcept {
    return {difference_of_products(r.cos, v.x, r.sin, v.y),
            sum_of_products(r.sin, v.x, r.cos, v.y)};
}

FixedVec2 unrotate(Rot2 r, FixedVec2 v) noexcept {
    return {sum_of_products(r.cos, v.x, r.sin, v.y),
            difference_of_products(r.cos, v.y, r.sin, v.x)};
}

// --- FixedQuat
// ------------------------------------------------------------------------------------

FixedQuat FixedQuat::from_axis_angle(FixedVec3 axis, Angle angle) noexcept {
    const SinCos half = sincos(Angle::from_raw(angle.raw >> 1));
    return FixedQuat{axis.x * half.sin, axis.y * half.sin, axis.z * half.sin, half.cos};
}

FixedQuat normalize(FixedQuat q) noexcept {
    const WideFixed squared = WideFixed::product(q.x, q.x) + WideFixed::product(q.y, q.y) +
                              WideFixed::product(q.z, q.z) + WideFixed::product(q.w, q.w);
    const Fixed magnitude = sqrt(squared);
    if (magnitude.raw == 0) {
        detail::overflowed("normalize of the zero quaternion");
        return FixedQuat::identity();
    }
    return FixedQuat{q.x / magnitude, q.y / magnitude, q.z / magnitude, q.w / magnitude};
}

FixedQuat operator*(FixedQuat a, FixedQuat b) noexcept {
    const auto p = [](Fixed u, Fixed v) noexcept { return WideFixed::product(u, v); };
    const FixedQuat product{
        (p(a.w, b.x) + p(a.x, b.w) + p(a.y, b.z) - p(a.z, b.y)).narrow(),
        (p(a.w, b.y) - p(a.x, b.z) + p(a.y, b.w) + p(a.z, b.x)).narrow(),
        (p(a.w, b.z) + p(a.x, b.y) - p(a.y, b.x) + p(a.z, b.w)).narrow(),
        (p(a.w, b.w) - p(a.x, b.x) - p(a.y, b.y) - p(a.z, b.z)).narrow(),
    };
    return normalize(product);
}

FixedVec3 rotate(FixedQuat q, FixedVec3 v) noexcept {
    const FixedVec3 u{q.x, q.y, q.z};
    const FixedVec3 t = cross(u, v) * Fixed::from_int(2);
    return v + t * q.w + cross(u, t);
}

// --- FixedTransform
// -------------------------------------------------------------------------------

FixedVec3 FixedTransform::forward() const noexcept {
    return rotate(rotation, fixed_forward());
}

FixedVec3 transform_point(const FixedTransform& t, FixedVec3 p) noexcept {
    return rotate(t.rotation, p) + t.translation;
}

FixedTransform operator*(const FixedTransform& a, const FixedTransform& b) noexcept {
    return FixedTransform{transform_point(a, b.translation), a.rotation * b.rotation};
}

// --- Shapes
// ---------------------------------------------------------------------------------------

Fixed ratio_unit(WideFixed numerator, WideFixed denominator) noexcept {
    if (numerator.raw == U128{} || numerator.negative() || denominator.negative() ||
        denominator.raw == U128{}) {
        return Fixed::zero();
    }
    if (!(numerator < denominator)) {
        return Fixed::one();
    }
    // 0 < numerator < denominator < 2^127, so the remainder doubled stays below 2^128 unsigned.
    U128 remainder = numerator.raw;
    i64 quotient = 0;
    for (int bit = 0; bit < Fixed::kFractionBits; ++bit) {
        remainder = wide::shl(remainder, 1);
        quotient <<= 1;
        if (!wide::less(remainder, denominator.raw)) {
            remainder = wide::sub(remainder, denominator.raw);
            quotient |= 1;
        }
    }
    return Fixed::from_raw(quotient);
}

FixedVec2 closest_point_on_segment(FixedVec2 a, FixedVec2 b, FixedVec2 point) noexcept {
    const FixedVec2 along = b - a;
    const Fixed t = ratio_unit(dot(point - a, along), length_squared(along));
    return a + along * t;
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
