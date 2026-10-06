#pragma once
// The conversion boundary between floating point and `Fixed`. Design §7.
//
// FLOAT INTO FIXED HAPPENS ONCE, AND NEVER IN A TICK. The `*_cooked` functions are permitted at
// three moments only:
//
//   cook                   an authored float becomes the raw i64 a cooked asset stores
//   session configuration  configuration values, before tick 0, folded into the state hash
//   command creation       the issuing peer converts a float pick into the command payload
//
// Their names are the audit: `grep -rn "from_f32_cooked\|from_f64_cooked"` lists every entry
// point, and review keeps that list to those three places, as it does `bypass_classification()`.
//
// FIXED INTO FLOAT IS PRESENTATION ONLY. `to_f32_relative` subtracts in `Fixed` first, which is
// exact, so a value far from the origin loses nothing until the one final rounding, and the result
// is already camera-relative, which is what `core-math`'s large-world rule asks of rendering.
//
// This header and convert.cpp are the module's only floating point. convert.cpp is the one source
// excluded from the `-mgeneral-regs-only` build that proves the kernel contains none.

#include <cy/core/base/types.h>
#include <cy/core/detmath/config.h>
#include <cy/core/detmath/fixed.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// `value` as a `Fixed`: multiplied by 2^32 (exact) and rounded to nearest, ties to even, with
/// `std::nearbyint` in the default rounding mode — an operation IEEE 754 defines exactly, so the
/// conversion is itself the same everywhere. Out of range saturates; NaN gives 0. Both are counted.
/// Every `f32` with |value| < 2^31 whose lowest set bit is at least 2^-32 converts exactly.
[[nodiscard]] Fixed from_f32_cooked(f32 value) noexcept;
/// `value` as a `Fixed`, by the same rule as `from_f32_cooked`.
[[nodiscard]] Fixed from_f64_cooked(f64 value) noexcept;

/// `value - origin` as an `f64`: the subtraction in `Fixed` (exact unless it overflows, which is a
/// camera two billion metres from a unit), then one conversion, exact within ±2^21.
[[nodiscard]] f64 to_f64_relative(Fixed value, Fixed origin) noexcept;
/// `value - origin` as an `f32`, for presentation: the `f64` above, rounded once more.
[[nodiscard]] f32 to_f32_relative(Fixed value, Fixed origin) noexcept;

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
