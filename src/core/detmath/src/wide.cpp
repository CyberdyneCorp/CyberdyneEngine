// The out-of-line halves of wide.h: the reference divide and square root, the long division, and
// the fast integer square root. Design §4.4 and §5.2.
//
// Nothing here may touch a floating-point register. This translation unit is compiled a second time
// with `-mgeneral-regs-only` (src/core/detmath/tests/CMakeLists.txt), where a `double` anywhere in
// it is a compile error rather than a review comment.

#include <cy/core/detmath/wide.h>

#include <array>
#include <bit>

namespace cy::detmath::inline CY_DETMATH_VARIANT::wide {
namespace {

constexpr u64 kDigit = u64{1} << 32;
constexpr u64 kDigitMask = kDigit - 1;

/// `floor(sqrt(v))` for a 64-bit `v`, one bit at a time. Used only to build the seed table, at
/// compile time.
constexpr u64 isqrt_bitwise(u64 v) noexcept {
    u64 result = 0;
    u64 bit = u64{1} << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= result + bit) {
            v -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

/// For a normalised `m` in [2^62, 2^64), the seed is `ceil(sqrt((top + 1) * 2^56))` where `top` is
/// `m`'s leading eight bits. It is never below `sqrt(m)`, which is what Newton's iteration from
/// above needs, and it is within 2^-7 of it, so two iterations leave an error of a few units.
constexpr std::array<u64, 192> make_seeds() noexcept {
    std::array<u64, 192> seeds{};
    for (u64 top = 64; top < 256; ++top) {
        if (top == 255) {
            seeds[top - 64] = kDigit;  // sqrt(2^64), whose argument does not fit 64 bits
            continue;
        }
        const u64 bound = (top + 1) << 56;
        const u64 root = isqrt_bitwise(bound);
        seeds[top - 64] = root * root < bound ? root + 1 : root;
    }
    return seeds;
}

constexpr std::array<u64, 192> kSeeds = make_seeds();

/// `floor(sqrt(m))` for `m` in [2^62, 2^64). The result lies in [2^31, 2^32).
u64 isqrt_normalized(u64 m) noexcept {
    u64 y = kSeeds[(m >> 56) - 64];
    y = (y + m / y) >> 1;
    y = (y + m / y) >> 1;
    // Newton's iteration from above never falls below the floor, so the correction only descends.
    // y is at most 2^32, whose square needs the 128-bit product.
    while (less(U128{m, 0}, mul_u64(y, y))) {
        --y;
    }
    return y;
}

u64 isqrt64(u64 v) noexcept {
    if (v == 0) {
        return 0;
    }
    const unsigned shift = static_cast<unsigned>(std::countl_zero(v)) & ~1U;
    return isqrt_normalized(v << shift) >> (shift / 2);
}

/// Knuth's algorithm D for a two-digit-by-one quotient, 64-bit digits split into 32-bit halves:
/// `(high:low) / divisor` where `high < divisor`, so the quotient fits 64 bits. Hacker's Delight,
/// 2nd edition, figure 9-3 (`divlu`), with its gotos written as loops.
u64 divide_two_by_one(u64 high, u64 low, u64 divisor, u64& remainder) noexcept {
    const auto shift = static_cast<unsigned>(std::countl_zero(divisor));
    divisor <<= shift;
    const u64 divisor_hi = divisor >> 32;
    const u64 divisor_lo = divisor & kDigitMask;

    const u64 numerator_32 = (high << shift) | (shift == 0 ? 0 : low >> (64 - shift));
    const u64 numerator_10 = low << shift;
    const u64 numerator_1 = numerator_10 >> 32;
    const u64 numerator_0 = numerator_10 & kDigitMask;

    u64 quotient_1 = numerator_32 / divisor_hi;
    u64 estimate = numerator_32 - quotient_1 * divisor_hi;
    while (quotient_1 >= kDigit || quotient_1 * divisor_lo > ((estimate << 32) | numerator_1)) {
        --quotient_1;
        estimate += divisor_hi;
        if (estimate >= kDigit) {
            break;
        }
    }

    const u64 numerator_21 = (numerator_32 << 32) + numerator_1 - quotient_1 * divisor;
    u64 quotient_0 = numerator_21 / divisor_hi;
    estimate = numerator_21 - quotient_0 * divisor_hi;
    while (quotient_0 >= kDigit || quotient_0 * divisor_lo > ((estimate << 32) | numerator_0)) {
        --quotient_0;
        estimate += divisor_hi;
        if (estimate >= kDigit) {
            break;
        }
    }

    remainder = ((numerator_21 << 32) + numerator_0 - quotient_0 * divisor) >> shift;
    return (quotient_1 << 32) + quotient_0;
}

}  // namespace

DivResult reference::divrem(U128 numerator, u64 divisor) noexcept {
    const U128 wide_divisor{divisor, 0};
    U128 quotient{};
    U128 remainder{};
    for (int bit = 127; bit >= 0; --bit) {
        remainder = shl(remainder, 1);
        const u64 incoming =
            bit >= 64 ? (numerator.hi >> (bit - 64)) & 1U : (numerator.lo >> bit) & 1U;
        remainder.lo |= incoming;
        if (!less(remainder, wide_divisor)) {
            remainder = sub(remainder, wide_divisor);
            if (bit >= 64) {
                quotient.hi |= u64{1} << (bit - 64);
            } else {
                quotient.lo |= u64{1} << bit;
            }
        }
    }
    return DivResult{quotient, remainder.lo};
}

u64 reference::isqrt(U128 value) noexcept {
    U128 result{};
    U128 bit{0, u64{1} << 62};  // 2^126, the largest power of four below 2^128
    while (less(value, bit)) {
        bit = shr(bit, 2);
    }
    while (bit != U128{}) {
        const U128 trial = add(result, bit);
        if (!less(value, trial)) {
            value = sub(value, trial);
            result = add(shr(result, 1), bit);
        } else {
            result = shr(result, 1);
        }
        bit = shr(bit, 2);
    }
    return result.lo;
}

DivResult long_division::divrem(U128 numerator, u64 divisor) noexcept {
    const u64 quotient_hi = numerator.hi / divisor;
    u64 remainder = 0;
    const u64 quotient_lo =
        divide_two_by_one(numerator.hi % divisor, numerator.lo, divisor, remainder);
    return DivResult{U128{quotient_lo, quotient_hi}, remainder};
}

u64 isqrt(U128 value) noexcept {
    if (value.hi == 0) {
        return isqrt64(value.lo);
    }
    // Normalise by an even shift so the high limb is at least 2^62; the root then shifts back by
    // half of it, and floor(floor(sqrt(m)) / 2^k) is floor(sqrt(m / 4^k)).
    const unsigned shift = static_cast<unsigned>(std::countl_zero(value.hi)) & ~1U;
    const U128 m = shl(value, shift);
    const u64 y = isqrt_normalized(m.hi);

    // One Newton step from y * 2^32, which is below the root: r = y 2^32 + (m - y^2 2^64) / (y
    // 2^33). The step lands at or above the root, by a few units at most.
    const U128 residual{m.lo, m.hi - y * y};
    const u64 step = shr(residual, 33).lo / y;
    u64 root = (y << 32) + step;
    if (root < (y << 32)) {
        root = ~u64{0};  // the estimate passed 2^64; the root itself is below it
    }
    while (less(m, mul_u64(root, root))) {
        --root;
    }
    while (root != ~u64{0} && !less(m, mul_u64(root + 1, root + 1))) {
        ++root;
    }
    return root >> (shift / 2);
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT::wide
