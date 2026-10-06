// SPDX-License-Identifier: MIT
// The correctly rounded square root. Design §5.2: "Integer square root of the 128-bit value
// raw << 32, rounded to nearest. Correctly rounded."
//
// Correct rounding is decided exactly, not estimated: with r = floor(sqrt(n)), the root rounds up
// when n - r^2 > r, because (r + 1/2)^2 = r^2 + r + 1/4 and n is an integer. There is never a tie.
// So the result does not depend on how `wide::isqrt` reaches its floor, only on that floor, and
// `unit.detmath` checks the floor against the one-bit-at-a-time reference.

#include <cy/core/detmath/functions.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

Fixed sqrt(WideFixed x) noexcept {
    if (x.negative()) {
        detail::overflowed("sqrt of a negative");
        return Fixed::zero();
    }
    u64 root = wide::isqrt(x.raw);
    const U128 remainder = wide::sub(x.raw, wide::mul_u64(root, root));
    if (wide::less(U128{root, 0}, remainder)) {
        ++root;
    }
    if (root > static_cast<u64>(INT64_MAX)) {
        detail::overflowed("sqrt beyond the range");
        return Fixed::max();
    }
    return Fixed{static_cast<i64>(root)};
}

Fixed sqrt(Fixed x) noexcept {
    if (x.raw < 0) {
        detail::overflowed("sqrt of a negative");
        return Fixed::zero();
    }
    return sqrt(WideFixed::from_fixed(x));
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
