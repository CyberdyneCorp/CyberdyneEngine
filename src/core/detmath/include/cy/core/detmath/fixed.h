#pragma once
// The scalar formats of the deterministic math module, and their arithmetic. Design §4.
//
//   Fixed      Q32.32 in an i64. ALL authoritative arithmetic: positions, velocities, times,
//              distances, costs. Range ±2^31, resolution 2^-32.
//   Fixed16    Q16.16 in an i32. STORAGE ONLY: widened to Fixed on load, never computed with.
//   Angle      a binary angle in a u32, a full turn being 2^32. Wrapping is free and exact, which
//              is what makes the trigonometric range reduction exact.
//   WideFixed  Q64.64 in 128 bits. Intermediate only: exact products, and sums of them, so that a
//              squared distance never overflows and two of them always compare correctly.
//
// THE RULES (design §4.3) ARE THE SPECIFICATION. Every implementation — the intrinsic paths, the
// portable limbs, tools/detmath/model.py and, later, Swift — produces these bits:
//
//   + - and unary -   two's-complement wrapping, computed in u64. Never undefined behaviour.
//   *                 the exact 128-bit product, rounded to nearest with ties toward +infinity
//                     (add 2^31, then floor-shift by 32), wrapped to 64 bits.
//   /                 the exact quotient of (a << 32) / b, truncated toward zero, wrapped. Division
//                     by zero gives Fixed::max() for a >= 0 and Fixed::min() for a < 0, in every
//                     build.
//   >>                arithmetic (floor), as C++20 defines it for signed values.
//   comparison        integer comparison of the raw value. No NaN, no infinity, no negative zero.
//   Fixed -> Fixed16  round to nearest, ties toward +infinity; out of range saturates.
//   Angle             u32 wrapping. Angle × Fixed scales through 128 bits and wraps.
//
// A wrap, a saturation and a division by zero are reported to the thread's `OverflowGuard` in
// development builds (overflow.h) and compute exactly the same thing either way.

#include <cy/core/base/types.h>
#include <cy/core/detmath/config.h>
#include <cy/core/detmath/overflow.h>
#include <cy/core/detmath/wide.h>

#include <compare>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

// --- Fixed
// ----------------------------------------------------------------------------------------

/// A Q32.32 fixed-point number: `raw / 2^32`. The type of every authoritative quantity in a
/// `CrossPlatform` or `Lockstep` session.
struct Fixed {
    /// The fractional bits.
    static constexpr int kFractionBits = 32;
    /// The raw value of 1.0.
    static constexpr i64 kOneRaw = i64{1} << kFractionBits;

    /// The value times 2^32. Public, because it is what is hashed, serialised and sent across the
    /// ABI, and a type that hid it would be converted through something less exact.
    i64 raw = 0;

    /// The value whose raw representation is `raw`.
    [[nodiscard]] static constexpr Fixed from_raw(i64 raw) noexcept { return Fixed{raw}; }
    /// An integer. Every `i32` is exact.
    [[nodiscard]] static constexpr Fixed from_int(i32 value) noexcept {
        return Fixed{static_cast<i64>(static_cast<u64>(static_cast<i64>(value)) << kFractionBits)};
    }
    /// 0.
    [[nodiscard]] static constexpr Fixed zero() noexcept { return Fixed{0}; }
    /// 1.
    [[nodiscard]] static constexpr Fixed one() noexcept { return Fixed{kOneRaw}; }
    /// 1/2.
    [[nodiscard]] static constexpr Fixed half() noexcept { return Fixed{kOneRaw / 2}; }
    /// The smallest positive value, 2^-32.
    [[nodiscard]] static constexpr Fixed epsilon() noexcept { return Fixed{1}; }
    /// The largest value, 2^31 - 2^-32.
    [[nodiscard]] static constexpr Fixed max() noexcept { return Fixed{INT64_MAX}; }
    /// The smallest value, -2^31.
    [[nodiscard]] static constexpr Fixed min() noexcept { return Fixed{INT64_MIN}; }

    /// The largest integer not above the value.
    [[nodiscard]] constexpr i64 floor() const noexcept { return raw >> kFractionBits; }
    /// The fractional part, in [0, 1).
    [[nodiscard]] constexpr Fixed fraction() const noexcept { return Fixed{raw & (kOneRaw - 1)}; }

    /// Raw comparison: total, because there is no NaN.
    friend constexpr auto operator<=>(const Fixed&, const Fixed&) = default;
};

/// `a + b`, wrapped.
[[nodiscard]] constexpr Fixed operator+(Fixed a, Fixed b) noexcept {
    const auto sum = static_cast<i64>(static_cast<u64>(a.raw) + static_cast<u64>(b.raw));
    if (((a.raw ^ sum) & (b.raw ^ sum)) < 0) {
        detail::overflowed("Fixed +");
    }
    return Fixed{sum};
}

/// `a - b`, wrapped.
[[nodiscard]] constexpr Fixed operator-(Fixed a, Fixed b) noexcept {
    const auto difference = static_cast<i64>(static_cast<u64>(a.raw) - static_cast<u64>(b.raw));
    if (((a.raw ^ b.raw) & (a.raw ^ difference)) < 0) {
        detail::overflowed("Fixed -");
    }
    return Fixed{difference};
}

/// `-a`, wrapped: `-Fixed::min()` is `Fixed::min()`.
[[nodiscard]] constexpr Fixed operator-(Fixed a) noexcept {
    if (a.raw == INT64_MIN) {
        detail::overflowed("Fixed unary -");
    }
    return Fixed{static_cast<i64>(u64{0} - static_cast<u64>(a.raw))};
}

/// `a * b`: exact product, rounded to nearest with ties toward +infinity, wrapped.
[[nodiscard]] inline Fixed operator*(Fixed a, Fixed b) noexcept {
    const U128 rounded = wide::add(wide::mul_i64(a.raw, b.raw), U128{u64{1} << 31, 0});
    const U128 shifted = wide::sar(rounded, 32);
    if (!wide::fits_i64(shifted)) {
        detail::overflowed("Fixed *");
    }
    return Fixed{static_cast<i64>(shifted.lo)};
}

/// `a / b`: `(a << 32) / b` truncated toward zero, wrapped. Division by zero gives `max()` for
/// `a >= 0` and `min()` for `a < 0`.
[[nodiscard]] inline Fixed operator/(Fixed a, Fixed b) noexcept {
    if (b.raw == 0) {
        detail::overflowed("Fixed / by zero");
        return a.raw >= 0 ? Fixed::max() : Fixed::min();
    }
    const U128 numerator = wide::shl(wide::from_i64(a.raw), 32);
    const u64 divisor = b.raw < 0 ? u64{0} - static_cast<u64>(b.raw) : static_cast<u64>(b.raw);
    U128 quotient = wide::divrem(wide::magnitude(numerator), divisor).quotient;
    if ((a.raw < 0) != (b.raw < 0)) {
        quotient = wide::negate(quotient);
    }
    if (!wide::fits_i64(quotient)) {
        detail::overflowed("Fixed /");
    }
    return Fixed{static_cast<i64>(quotient.lo)};
}

/// `a << shift`, wrapped, for `shift` in [0, 63].
[[nodiscard]] constexpr Fixed operator<<(Fixed a, int shift) noexcept {
    return Fixed{static_cast<i64>(static_cast<u64>(a.raw) << shift)};
}

/// `a >> shift`: arithmetic, so it rounds toward -infinity. `shift` in [0, 63].
[[nodiscard]] constexpr Fixed operator>>(Fixed a, int shift) noexcept {
    return Fixed{a.raw >> shift};
}

/// `a += b`.
constexpr Fixed& operator+=(Fixed& a, Fixed b) noexcept {
    return a = a + b;
}
/// `a -= b`.
constexpr Fixed& operator-=(Fixed& a, Fixed b) noexcept {
    return a = a - b;
}
/// `a *= b`.
inline Fixed& operator*=(Fixed& a, Fixed b) noexcept {
    return a = a * b;
}
/// `a /= b`.
inline Fixed& operator/=(Fixed& a, Fixed b) noexcept {
    return a = a / b;
}

/// `|a|`, wrapped: `abs(Fixed::min())` is `Fixed::min()` and is counted.
[[nodiscard]] constexpr Fixed abs(Fixed a) noexcept {
    return a.raw < 0 ? -a : a;
}

/// `a + b`, clamped to the range instead of wrapping. For gameplay clamps that want saturation;
/// the operators wrap (design §4.3).
[[nodiscard]] constexpr Fixed saturating_add(Fixed a, Fixed b) noexcept {
    const auto sum = static_cast<i64>(static_cast<u64>(a.raw) + static_cast<u64>(b.raw));
    if (((a.raw ^ sum) & (b.raw ^ sum)) < 0) {
        return a.raw < 0 ? Fixed::min() : Fixed::max();
    }
    return Fixed{sum};
}

/// `a - b`, clamped to the range instead of wrapping.
[[nodiscard]] constexpr Fixed saturating_sub(Fixed a, Fixed b) noexcept {
    const auto difference = static_cast<i64>(static_cast<u64>(a.raw) - static_cast<u64>(b.raw));
    if (((a.raw ^ b.raw) & (a.raw ^ difference)) < 0) {
        return a.raw < 0 ? Fixed::min() : Fixed::max();
    }
    return Fixed{difference};
}

/// `a * b`, rounded as `*` rounds, clamped to the range instead of wrapping.
[[nodiscard]] inline Fixed saturating_mul(Fixed a, Fixed b) noexcept {
    const U128 rounded = wide::add(wide::mul_i64(a.raw, b.raw), U128{u64{1} << 31, 0});
    const U128 shifted = wide::sar(rounded, 32);
    if (!wide::fits_i64(shifted)) {
        return wide::is_negative(shifted) ? Fixed::min() : Fixed::max();
    }
    return Fixed{static_cast<i64>(shifted.lo)};
}

// --- Fixed16
// --------------------------------------------------------------------------------------

/// A Q16.16 storage form: range ±32 768, resolution 2^-16. Never computed with: `widen()` it.
///
/// Design §4.2: `Fixed` × `Fixed` is safe to ±2^31, but Q16.16's own `x * x` overflows at 181, so a
/// squared distance in this format would overflow beyond 181 m. It exists for dense per-element
/// data whose range fits, at half the bytes.
struct Fixed16 {
    /// The value times 2^16.
    i32 raw = 0;

    /// The value whose raw representation is `raw`.
    [[nodiscard]] static constexpr Fixed16 from_raw(i32 raw) noexcept { return Fixed16{raw}; }

    /// `value`, rounded to nearest with ties toward +infinity. Out of range saturates and is
    /// counted.
    [[nodiscard]] static constexpr Fixed16 narrow(Fixed value) noexcept {
        // floor((raw + 2^15) / 2^16) without forming raw + 2^15, which could overflow.
        const i64 rounded = (value.raw >> 16) + ((value.raw >> 15) & 1);
        if (rounded > INT32_MAX) {
            detail::overflowed("Fixed16 narrow");
            return Fixed16{INT32_MAX};
        }
        if (rounded < INT32_MIN) {
            detail::overflowed("Fixed16 narrow");
            return Fixed16{INT32_MIN};
        }
        return Fixed16{static_cast<i32>(rounded)};
    }

    /// The same value as a `Fixed`. Exact.
    [[nodiscard]] constexpr Fixed widen() const noexcept {
        return Fixed{static_cast<i64>(static_cast<u64>(static_cast<i64>(raw)) << 16)};
    }

    /// Raw comparison.
    friend constexpr auto operator<=>(const Fixed16&, const Fixed16&) = default;
};

// --- Angle
// ----------------------------------------------------------------------------------------

/// A binary angle: `raw / 2^32` of a turn, counter-clockwise. Arithmetic wraps, exactly, which is
/// what a heading wants and what makes the octant reduction in `sin` exact.
struct Angle {
    /// The angle in units of 2^-32 turn.
    u32 raw = 0;

    /// The angle whose raw representation is `raw`.
    [[nodiscard]] static constexpr Angle from_raw(u32 raw) noexcept { return Angle{raw}; }
    /// A quarter turn, 90 degrees.
    [[nodiscard]] static constexpr Angle quarter() noexcept { return Angle{u32{1} << 30}; }
    /// Half a turn, 180 degrees.
    [[nodiscard]] static constexpr Angle half() noexcept { return Angle{u32{1} << 31}; }
    /// An eighth of a turn, 45 degrees.
    [[nodiscard]] static constexpr Angle eighth() noexcept { return Angle{u32{1} << 29}; }

    /// `turns` modulo one turn. Exact: the fractional bits of a Q32.32 number ARE a binary angle.
    [[nodiscard]] static constexpr Angle from_turns(Fixed turns) noexcept {
        return Angle{static_cast<u32>(static_cast<u64>(turns.raw))};
    }
    /// The angle in turns, in [0, 1). Exact.
    [[nodiscard]] constexpr Fixed turns() const noexcept { return Fixed{static_cast<i64>(raw)}; }
    /// The angle in turns, in [-1/2, 1/2). Exact.
    [[nodiscard]] constexpr Fixed signed_turns() const noexcept {
        return Fixed{static_cast<i64>(static_cast<i32>(raw))};
    }

    /// `radians` modulo one turn, rounded to the nearest 2^-32 turn (ties toward +infinity).
    [[nodiscard]] static Angle from_radians(Fixed radians) noexcept;
    /// The angle in radians, in [0, 2 pi), rounded to the nearest 2^-32.
    [[nodiscard]] Fixed radians() const noexcept;
    /// The angle in radians, in [-pi, pi), rounded to the nearest 2^-32.
    [[nodiscard]] Fixed signed_radians() const noexcept;

    /// Raw equality. Angles are not ordered: on a circle, "less than" has no meaning that survives
    /// wrapping, so a caller compares `signed_turns()` of a difference instead.
    friend constexpr bool operator==(const Angle&, const Angle&) = default;
};

/// `a + b`, modulo one turn.
[[nodiscard]] constexpr Angle operator+(Angle a, Angle b) noexcept {
    return Angle{static_cast<u32>(a.raw + b.raw)};
}
/// `a - b`, modulo one turn.
[[nodiscard]] constexpr Angle operator-(Angle a, Angle b) noexcept {
    return Angle{static_cast<u32>(a.raw - b.raw)};
}
/// `-a`, modulo one turn.
[[nodiscard]] constexpr Angle operator-(Angle a) noexcept {
    return Angle{static_cast<u32>(0U - a.raw)};
}

/// `a * k` through the exact 128-bit product, rounded to the nearest 2^-32 turn (ties toward
/// +infinity), modulo one turn. Wrapping is the meaning of an angle, so it is not counted.
[[nodiscard]] inline Angle operator*(Angle a, Fixed k) noexcept {
    const U128 rounded =
        wide::add(wide::mul_i64(static_cast<i64>(a.raw), k.raw), U128{u64{1} << 31, 0});
    return Angle{static_cast<u32>(rounded.lo >> 32)};
}

// --- WideFixed
// ------------------------------------------------------------------------------------

/// A Q64.64 value in 128 bits: `raw / 2^64`. The type of a product of two `Fixed`, and of a dot
/// product or squared length, so that comparing two distances cannot overflow (design §6).
struct WideFixed {
    /// The value times 2^64, two's complement.
    U128 raw;

    /// The value whose raw representation is `raw`.
    [[nodiscard]] static constexpr WideFixed from_raw(U128 raw) noexcept { return WideFixed{raw}; }
    /// `value`, exactly.
    [[nodiscard]] static constexpr WideFixed from_fixed(Fixed value) noexcept {
        return WideFixed{wide::shl(wide::from_i64(value.raw), 32)};
    }
    /// `a * b`, exactly: the product of two Q32.32 raws is the Q64.64 raw of the product.
    [[nodiscard]] static WideFixed product(Fixed a, Fixed b) noexcept {
        return WideFixed{wide::mul_i64(a.raw, b.raw)};
    }

    /// Whether the value is below zero.
    [[nodiscard]] constexpr bool negative() const noexcept { return wide::is_negative(raw); }

    /// The nearest `Fixed`, ties toward +infinity, wrapped. A wrap is counted.
    [[nodiscard]] constexpr Fixed narrow() const noexcept {
        const U128 shifted = wide::sar(wide::add(raw, U128{u64{1} << 31, 0}), 32);
        if (!wide::fits_i64(shifted)) {
            detail::overflowed("WideFixed narrow");
        }
        return Fixed{static_cast<i64>(shifted.lo)};
    }

    /// The nearest `Fixed`, ties toward +infinity, clamped to the range instead of wrapping.
    [[nodiscard]] constexpr Fixed saturating_narrow() const noexcept {
        const U128 shifted = wide::sar(wide::add(raw, U128{u64{1} << 31, 0}), 32);
        if (!wide::fits_i64(shifted)) {
            return wide::is_negative(shifted) ? Fixed::min() : Fixed::max();
        }
        return Fixed{static_cast<i64>(shifted.lo)};
    }

    /// Raw equality.
    friend constexpr bool operator==(const WideFixed&, const WideFixed&) = default;
    /// Signed comparison: total.
    friend constexpr std::strong_ordering operator<=>(const WideFixed& a,
                                                      const WideFixed& b) noexcept {
        if (a.raw == b.raw) {
            return std::strong_ordering::equal;
        }
        return wide::less_signed(a.raw, b.raw) ? std::strong_ordering::less
                                               : std::strong_ordering::greater;
    }
};

/// `a + b`, modulo 2^128. A sum of products of in-range `Fixed` values cannot get near the limit.
[[nodiscard]] constexpr WideFixed operator+(WideFixed a, WideFixed b) noexcept {
    return WideFixed{wide::add(a.raw, b.raw)};
}
/// `a - b`, modulo 2^128.
[[nodiscard]] constexpr WideFixed operator-(WideFixed a, WideFixed b) noexcept {
    return WideFixed{wide::sub(a.raw, b.raw)};
}
/// `-a`, modulo 2^128.
[[nodiscard]] constexpr WideFixed operator-(WideFixed a) noexcept {
    return WideFixed{wide::negate(a.raw)};
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
