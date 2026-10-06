// exp2 and log2, and exp, log and pow built on them. Design §5.2.
//
// exp2: the integer part of x is a shift and the fractional part f in [0, 1) goes to the generated
// polynomial 2^f = 1 + f P(f) in Q2.62. The value lies in [1, 2), so one shift and one rounding
// place it in Q32.32.
//
// log2: `std::countl_zero` finds the exponent exactly. The mantissa is moved to [1/sqrt 2, sqrt 2)
// by an EXACT test — x^2 >= 2^(2p + 1) in 128 bits rather than a comparison with a rounded sqrt 2 —
// and s = (m - 1)/(m + 1) is formed from the integers themselves, so the only roundings are the
// one division and the polynomial log2((1 + s)/(1 - s)) = 4 s P(s^2).
//
// exp, log and pow do not round through a `Fixed` in the middle. exp keeps x log2(e) at 2^-62
// before splitting it, and log keeps log2(x) at 2^-62 before multiplying by ln 2: rounding the
// intermediate to Q32.32 would cost up to half an ulp of the argument, which exp then multiplies by
// the result.

#include "kernel.h"

#include <cy/core/detmath/functions.h>

#include <bit>

namespace cy::detmath::inline CY_DETMATH_VARIANT {
namespace kernel {

i64 exp2_parts(i64 n, u64 fraction62) noexcept {
    const auto f = static_cast<i64>(fraction62);
    const i64 value = kOne62 + mul62(horner(coefficients::kExp2, f), f);  // [2^62, 2^63)
    if (n >= 31) {
        detail::overflowed("exp2 beyond the range");
        return INT64_MAX;
    }
    const i64 shift = 30 - n;
    if (shift >= 64) {
        return 0;
    }
    if (shift == 0) {
        return value;
    }
    // value < 2^63, so adding half of 2^shift cannot carry out of 64 bits.
    const u64 half = u64{1} << (shift - 1);
    return static_cast<i64>((static_cast<u64>(value) + half) >> shift);
}

Log2Parts log2_parts(i64 x) noexcept {
    const auto value = static_cast<u64>(x);
    const auto top = static_cast<unsigned>(63 - std::countl_zero(value));  // 2^top <= x
    // x >= sqrt(2) 2^top, decided exactly: x^2 >= 2^(2 top + 1).
    const U128 square = wide::mul_u64(value, value);
    const U128 threshold = wide::shl(wide::from_u64(1), 2 * top + 1);
    const bool upper = !wide::less(square, threshold);

    // m = x / 2^top in [1, sqrt 2), or x / 2^(top + 1) in [1/sqrt 2, 1). Both sums fit 64 bits:
    // x < 2^63 and 2^(top + 1) <= 2^63.
    const u64 power = u64{1} << (upper ? top + 1 : top);
    const u64 numerator = upper ? power - value : value - power;
    const u64 denominator = value + power;
    const auto s = static_cast<i64>(
        wide::divrem(wide::shl(wide::from_u64(numerator), 62), denominator).quotient.lo);

    const i64 magnitude = wide::mul_shift(horner(coefficients::kLog2, mul62(s, s)), s, 60);
    const i64 exponent = static_cast<i64>(top) - (upper ? 31 : 32);
    return Log2Parts{exponent, upper ? -magnitude : magnitude};
}

}  // namespace kernel

Fixed exp2(Fixed x) noexcept {
    const u64 fraction = static_cast<u64>(x.raw) & 0xFFFF'FFFFU;
    return Fixed{kernel::exp2_parts(x.raw >> 32, fraction << 30)};
}

Fixed log2(Fixed x) noexcept {
    if (x.raw <= 0) {
        detail::overflowed("log2 of a non-positive");
        return Fixed::min();
    }
    const kernel::Log2Parts parts = kernel::log2_parts(x.raw);
    return Fixed{static_cast<i64>(static_cast<u64>(parts.exponent) << 32) +
                 ((parts.remainder62 + (i64{1} << 29)) >> 30)};
}

Fixed exp(Fixed x) noexcept {
    // x log2(e) at 2^-62: the Q32.32 × Q2.62 product is Q.94, rounded once to Q.62.
    const U128 product = wide::mul_i64(x.raw, coefficients::kLog2eQ62);
    const U128 scaled = wide::sar(wide::add(product, U128{u64{1} << 31, 0}), 32);
    const auto whole = static_cast<i64>(wide::sar(scaled, 62).lo);
    return Fixed{kernel::exp2_parts(whole, scaled.lo & kernel::kFraction62)};
}

Fixed log(Fixed x) noexcept {
    if (x.raw <= 0) {
        detail::overflowed("log of a non-positive");
        return Fixed::min();
    }
    const kernel::Log2Parts parts = kernel::log2_parts(x.raw);
    // ln x = (exponent + remainder) ln 2, every term at 2^-62 before the one final rounding.
    const U128 whole = wide::mul_i64(parts.exponent, coefficients::kLn2Q62);
    const i64 rest = kernel::mul62(parts.remainder62, coefficients::kLn2Q62);
    const U128 total = wide::add(whole, wide::from_i64(rest));
    return Fixed{static_cast<i64>(wide::sar(wide::add(total, U128{u64{1} << 29, 0}), 30).lo)};
}

Fixed pow(Fixed x, Fixed y) noexcept {
    if (x.raw <= 0) {
        detail::overflowed("pow of a non-positive base");
        return Fixed::zero();
    }
    const kernel::Log2Parts parts = kernel::log2_parts(x.raw);
    // y log2(x) at 2^-62: y × exponent exactly, then y × remainder rounded once from Q.94.
    const U128 whole = wide::shl(wide::mul_i64(y.raw, parts.exponent), 30);
    const U128 rest =
        wide::sar(wide::add(wide::mul_i64(y.raw, parts.remainder62), U128{u64{1} << 31, 0}), 32);
    const U128 scaled = wide::add(whole, rest);
    const U128 integer = wide::sar(scaled, 62);
    if (!wide::fits_i64(integer)) {
        // |y log2 x| beyond 2^63: far outside the range in either direction.
        if (wide::is_negative(integer)) {
            return Fixed::zero();
        }
        detail::overflowed("exp2 beyond the range");
        return Fixed::max();
    }
    return Fixed{kernel::exp2_parts(static_cast<i64>(integer.lo), scaled.lo & kernel::kFraction62)};
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
