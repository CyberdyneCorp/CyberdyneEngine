// The conversion boundary. Design §7: the module's only floating point, and the one source the
// `-mgeneral-regs-only` build of the kernel leaves out.

#include <cy/core/detmath/convert.h>

#include <cmath>

namespace cy::detmath::inline CY_DETMATH_VARIANT {
namespace {

/// 2^63 as a double: exactly representable, and the first value that does not fit an i64.
constexpr f64 kTwoTo63 = 9223372036854775808.0;
/// 2^32, the scale of a raw value.
constexpr f64 kTwoTo32 = 4294967296.0;

}  // namespace

Fixed from_f64_cooked(f64 value) noexcept {
    if (std::isnan(value)) {
        detail::overflowed("from_f64_cooked of NaN");
        return Fixed::zero();
    }
    // Scaling by a power of two is exact unless it overflows, and an overflow saturates below.
    const f64 scaled = std::nearbyint(value * kTwoTo32);
    if (scaled >= kTwoTo63) {
        detail::overflowed("from_f64_cooked beyond the range");
        return Fixed::max();
    }
    if (scaled < -kTwoTo63) {
        detail::overflowed("from_f64_cooked beyond the range");
        return Fixed::min();
    }
    return Fixed{static_cast<i64>(scaled)};
}

Fixed from_f32_cooked(f32 value) noexcept {
    // Widening f32 to f64 is exact, so this is one rounding, not two.
    return from_f64_cooked(static_cast<f64>(value));
}

f64 to_f64_relative(Fixed value, Fixed origin) noexcept {
    return static_cast<f64>((value - origin).raw) / kTwoTo32;
}

f32 to_f32_relative(Fixed value, Fixed origin) noexcept {
    return static_cast<f32>(to_f64_relative(value, origin));
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
