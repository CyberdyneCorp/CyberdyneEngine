#pragma once
// Square root and the transcendentals, each with a declared error bound. Design §5.
//
// Every function here is computed from integer instructions: a range reduction that is exact or
// stated, a polynomial whose coefficients are the generated integers in
// generated/coefficients.h, and one final rounding. Nothing comes from a C library or from a
// compiler's choices, so the result is the same bits on every leg.
//
// THE BOUNDS are against the exact real function OF THE QUANTISED INPUT (a function cannot be more
// accurate than its input). "ulp" is one unit of the output: 2^-32 for `Fixed`, 2^-32 turn for
// `Angle`. `integration.detmath_vectors` asserts each one against an mpmath oracle
// (tools/detmath/gen_oracle.py) and prints the worst error it found beside it.
//
//   sqrt             <= 0.5 ulp: correctly rounded
//   sin, cos         <= 1 ulp
//   tan              <= 2 ulp where |tan| <= 1; relative <= 2^-30 above that
//   atan, atan2      <= 1 ulp of Angle
//   asin, acos       <= 2 ulp of Angle for |x| <= 1 - 2^-16; <= 2^-15 turn nearer to ±1
//   exp2             the larger of 1 ulp and a relative 2^-36
//   log2             <= 2 ulp
//   exp              the larger of 2 ulp and a relative 2^-34
//   log              <= 3 ulp
//   pow              the larger of 2 ulp and a relative 2^-34 + |y| 2^-58 (design §5.2 offers it
//                    for x > 0 only; the |y| term is the log2 kernel's own error, amplified)
//
// OUT OF DOMAIN, every function answers something defined and reports it to the thread's
// `OverflowGuard`: `sqrt` of a negative is 0; `log2`, `log` of a non-positive is `Fixed::min()`;
// `pow` of a non-positive base is 0; `asin`/`acos` clamp to [-1, 1]; `exp2`, `exp`, `pow` and `tan`
// saturate at the range; `atan2(0, 0)` is 0 and is not an error.

#include <cy/core/detmath/config.h>
#include <cy/core/detmath/fixed.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// `sqrt(x)`, correctly rounded. A negative `x` gives 0 and is counted.
[[nodiscard]] Fixed sqrt(Fixed x) noexcept;
/// `sqrt(x)` of a Q64.64 value, correctly rounded, so a squared length never has to be a `Fixed`.
/// A negative `x` gives 0, and a root beyond `Fixed::max()` saturates; both are counted.
[[nodiscard]] Fixed sqrt(WideFixed x) noexcept;

/// The sine and the cosine of one angle, from one reduction.
struct SinCos {
    Fixed sin;  ///< `sin(angle)`.
    Fixed cos;  ///< `cos(angle)`.
};

/// `sin(a)`. Exactly odd: `sin(-a) == -sin(a)`, and `sin(a + quarter) == cos(a)` for every `a`.
[[nodiscard]] Fixed sin(Angle a) noexcept;
/// `cos(a)`.
[[nodiscard]] Fixed cos(Angle a) noexcept;
/// `sin(a)` and `cos(a)`, bit-identical to calling the two separately.
[[nodiscard]] SinCos sincos(Angle a) noexcept;
/// `tan(a)`, truncated toward zero. At a pole, and wherever the quotient leaves the range, it
/// saturates toward the sign of the sine and is counted.
[[nodiscard]] Fixed tan(Angle a) noexcept;

/// `atan(x)`, in (-quarter, quarter) read as a signed angle.
[[nodiscard]] Angle atan(Fixed x) noexcept;
/// The angle of the vector (x, y), counter-clockwise from +x. `atan2(0, 0)` is 0. Exactly odd in y:
/// `atan2(-y, x) == -atan2(y, x)`.
[[nodiscard]] Angle atan2(Fixed y, Fixed x) noexcept;
/// `asin(x)` through `atan2(x, sqrt(1 - x^2))`. `x` outside [-1, 1] is clamped and counted.
[[nodiscard]] Angle asin(Fixed x) noexcept;
/// `acos(x)` through `atan2(sqrt(1 - x^2), x)`. `x` outside [-1, 1] is clamped and counted.
[[nodiscard]] Angle acos(Fixed x) noexcept;

/// `2^x`. Beyond `Fixed::max()` it saturates and is counted; below half an ulp it is 0.
[[nodiscard]] Fixed exp2(Fixed x) noexcept;
/// `log2(x)`. A non-positive `x` gives `Fixed::min()` and is counted.
[[nodiscard]] Fixed log2(Fixed x) noexcept;
/// `e^x`, as `exp2(x log2 e)` with the product kept at 2^-62.
[[nodiscard]] Fixed exp(Fixed x) noexcept;
/// `ln(x)`, as `log2(x) ln 2` with the logarithm kept at 2^-62. A non-positive `x` gives
/// `Fixed::min()` and is counted.
[[nodiscard]] Fixed log(Fixed x) noexcept;
/// `x^y` for `x > 0`, as `exp2(y log2 x)`. A non-positive `x` gives 0 and is counted.
[[nodiscard]] Fixed pow(Fixed x, Fixed y) noexcept;

/// The constant pi, correctly rounded.
[[nodiscard]] Fixed pi() noexcept;
/// The constant 2 pi, correctly rounded.
[[nodiscard]] Fixed two_pi() noexcept;
/// The constant pi / 2, correctly rounded.
[[nodiscard]] Fixed half_pi() noexcept;
/// The constant e, correctly rounded.
[[nodiscard]] Fixed e() noexcept;

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
