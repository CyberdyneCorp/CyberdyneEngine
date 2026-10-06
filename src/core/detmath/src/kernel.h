#pragma once
// The transcendental kernels' shared arithmetic, private to the module. Design §4.1, "Kernel
// format": Q2.62 in an i64, range ±2, resolution 2^-62. Reduced arguments and polynomial values
// live in it; it never appears in a public signature.

#include <cy/core/detmath/generated/coefficients.h>
#include <cy/core/detmath/wide.h>

#include <cstddef>

namespace cy::detmath::inline CY_DETMATH_VARIANT::kernel {

/// 1.0 in Q2.62.
inline constexpr i64 kOne62 = i64{1} << 62;
/// The fractional bits of a Q2.62 value whose integer part is zero.
inline constexpr u64 kFraction62 = (u64{1} << 62) - 1;

/// `round(a * b / 2^62)`, ties toward +infinity: the product of two Q2.62 values.
[[nodiscard]] inline i64 mul62(i64 a, i64 b) noexcept {
    return wide::mul_shift(a, b, 62);
}

/// Horner's rule over a table stored low order first, in Q2.62. Every step rounds once, through
/// `mul62`; the generator measures the error of exactly this evaluation.
template <std::size_t N>
[[nodiscard]] inline i64 horner(const i64 (&coefficients)[N], i64 x) noexcept {
    i64 value = coefficients[N - 1];
    for (std::size_t index = N - 1; index > 0; --index) {
        value = static_cast<i64>(static_cast<u64>(mul62(value, x)) +
                                 static_cast<u64>(coefficients[index - 1]));
    }
    return value;
}

/// A Q2.62 magnitude rounded to Q32.32, ties toward +infinity, then given its sign. Rounding the
/// magnitude rather than the signed value is what makes the odd functions exactly odd.
[[nodiscard]] inline i64 to_fixed_signed(i64 value62) noexcept {
    const u64 magnitude =
        value62 < 0 ? u64{0} - static_cast<u64>(value62) : static_cast<u64>(value62);
    const auto rounded = static_cast<i64>((magnitude + (u64{1} << 29)) >> 30);
    return value62 < 0 ? -rounded : rounded;
}

/// The sine and cosine of an angle in Q2.62, signed, from one octant reduction.
struct SinCos62 {
    i64 sin = 0;
    i64 cos = 0;
};
[[nodiscard]] SinCos62 sincos62(u32 angle) noexcept;

/// `log2(x)` for a positive raw `x` as an integer exponent and a Q2.62 remainder in [-1/2, 1/2].
struct Log2Parts {
    i64 exponent = 0;
    i64 remainder62 = 0;
};
[[nodiscard]] Log2Parts log2_parts(i64 x) noexcept;

/// `2^(n + f)` for an integer `n` and a Q0.62 fraction `f` in [0, 1), as a `Fixed` raw: saturated
/// beyond the range (and counted), 0 below half an ulp.
[[nodiscard]] i64 exp2_parts(i64 n, u64 fraction62) noexcept;

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT::kernel
