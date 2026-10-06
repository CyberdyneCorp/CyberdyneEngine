// SPDX-License-Identifier: MIT
#pragma once
// 128-bit integers in two 64-bit limbs, and the three ways this module multiplies and divides them.
//
// Design §4.4. A `Fixed` product is exact in 128 bits and a `Fixed` quotient divides a 96-bit
// numerator, so everything above this file rests on two primitives: a 64×64→128 multiply and a
// 128÷64 divide. Each has three implementations, and they must agree bit for bit:
//
//   reference     32-bit limbs and shift-and-subtract division: slow, obviously correct, and
//                 compiled on every toolchain. It is the one the tests trust.
//   long_division Knuth's algorithm D with 32-bit digits (Hacker's Delight `divlu`). It is the
//                 divide MSVC arm64 uses, and it is compiled and tested on every leg, so the path
//                 the one platform without a 128-bit divide relies on is not tested only there.
//   native        `unsigned __int128` on GCC and Clang; `_umul128` and `_udiv128` on MSVC x64;
//                 `__umulh` and the long division on MSVC arm64.
//
// `unit.detmath` compares native and long division against the reference over edge cases and a
// random sweep, on whichever leg runs it. An intrinsic that disagrees fails on the leg that uses
// it.
//
// Signed values are two's complement in the same two limbs. Only multiply and divide have more
// than one implementation: addition, shifts and comparison are written once, here, in 64-bit
// operations that mean the same thing on every compiler.
//
// The design table names `_mul128` for MSVC x64. The signed product is built here from the
// unsigned one (`mul_i64` below) on every path, so that only the unsigned primitive varies between
// toolchains; `_umul128` is that primitive.

#include <cy/core/base/types.h>
#include <cy/core/detmath/config.h>

#include <bit>

#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_ARM64))
#    include <intrin.h>
#endif

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// A 128-bit integer as two limbs. Signed operations read it as two's complement.
struct U128 {
    u64 lo = 0;  ///< Bits 0 to 63.
    u64 hi = 0;  ///< Bits 64 to 127; bit 63 of this limb is the sign of a signed value.

    friend constexpr bool operator==(const U128&, const U128&) = default;
};

/// The quotient and remainder of a 128-bit numerator divided by a 64-bit divisor.
struct DivResult {
    U128 quotient;      ///< Truncated; up to 128 bits.
    u64 remainder = 0;  ///< Less than the divisor.
};

namespace wide {

/// The value of `v`, sign-extended.
[[nodiscard]] constexpr U128 from_i64(i64 v) noexcept {
    return U128{static_cast<u64>(v), v < 0 ? ~u64{0} : u64{0}};
}

/// The value of `v`, zero-extended.
[[nodiscard]] constexpr U128 from_u64(u64 v) noexcept {
    return U128{v, 0};
}

/// `a + b`, modulo 2^128.
[[nodiscard]] constexpr U128 add(U128 a, U128 b) noexcept {
    U128 r{a.lo + b.lo, a.hi + b.hi};
    r.hi += r.lo < a.lo ? 1U : 0U;
    return r;
}

/// `a - b`, modulo 2^128.
[[nodiscard]] constexpr U128 sub(U128 a, U128 b) noexcept {
    U128 r{a.lo - b.lo, a.hi - b.hi};
    r.hi -= a.lo < b.lo ? 1U : 0U;
    return r;
}

/// `-a`, modulo 2^128.
[[nodiscard]] constexpr U128 negate(U128 a) noexcept {
    return sub(U128{}, a);
}

/// Whether `a`, read as two's complement, is negative.
[[nodiscard]] constexpr bool is_negative(U128 a) noexcept {
    return (a.hi >> 63) != 0;
}

/// `|a|` read as two's complement. The magnitude of -2^127 is 2^127, which is representable
/// unsigned.
[[nodiscard]] constexpr U128 magnitude(U128 a) noexcept {
    return is_negative(a) ? negate(a) : a;
}

/// `a << shift` for `shift` in [0, 127].
[[nodiscard]] constexpr U128 shl(U128 a, unsigned shift) noexcept {
    if (shift == 0) {
        return a;
    }
    if (shift >= 64) {
        return U128{0, a.lo << (shift - 64)};
    }
    return U128{a.lo << shift, (a.hi << shift) | (a.lo >> (64 - shift))};
}

/// `a >> shift`, filling with zeros, for `shift` in [0, 127].
[[nodiscard]] constexpr U128 shr(U128 a, unsigned shift) noexcept {
    if (shift == 0) {
        return a;
    }
    if (shift >= 64) {
        return U128{a.hi >> (shift - 64), 0};
    }
    return U128{(a.lo >> shift) | (a.hi << (64 - shift)), a.hi >> shift};
}

/// `a >> shift`, filling with the sign bit (floor division by 2^shift), for `shift` in [0, 127].
[[nodiscard]] constexpr U128 sar(U128 a, unsigned shift) noexcept {
    if (shift == 0) {
        return a;
    }
    const u64 fill = is_negative(a) ? ~u64{0} : u64{0};
    if (shift >= 64) {
        const u64 rest = shift == 64 ? a.hi : (a.hi >> (shift - 64)) | (fill << (128 - shift));
        return U128{rest, fill};
    }
    return U128{(a.lo >> shift) | (a.hi << (64 - shift)), (a.hi >> shift) | (fill << (64 - shift))};
}

/// Unsigned `a < b`.
[[nodiscard]] constexpr bool less(U128 a, U128 b) noexcept {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}

/// Signed `a < b`, reading both as two's complement.
[[nodiscard]] constexpr bool less_signed(U128 a, U128 b) noexcept {
    if (a.hi != b.hi) {
        return static_cast<i64>(a.hi) < static_cast<i64>(b.hi);
    }
    return a.lo < b.lo;
}

/// Whether `a`, read as two's complement, lies in the range of an `i64`.
[[nodiscard]] constexpr bool fits_i64(U128 a) noexcept {
    return a.hi == (static_cast<i64>(a.lo) < 0 ? ~u64{0} : u64{0});
}

/// The number of significant bits of `a`: 0 for zero, 128 when the top bit is set.
[[nodiscard]] constexpr unsigned bit_width(U128 a) noexcept {
    return a.hi != 0 ? 64U + static_cast<unsigned>(std::bit_width(a.hi))
                     : static_cast<unsigned>(std::bit_width(a.lo));
}

// --- The reference ------------------------------------------------------------------------------

namespace reference {

/// 64×64→128 from four 32×32→64 partial products. `cy::detail::mix` in
/// src/core/memory/include/cy/core/memory/hash.h computes the same product the same way on MSVC.
/// Out of line, so that it is compiled into every build of the kernel — including the
/// `-mgeneral-regs-only` one, where a floating-point shortcut in it would fail to compile.
[[nodiscard]] U128 mul_u64(u64 a, u64 b) noexcept;

/// 128÷64 by restoring shift-and-subtract, one quotient bit per step. `divisor` must not be zero.
[[nodiscard]] DivResult divrem(U128 numerator, u64 divisor) noexcept;

/// `floor(sqrt(value))`, one result bit per step.
[[nodiscard]] u64 isqrt(U128 value) noexcept;

}  // namespace reference

namespace long_division {

/// 128÷64 by two applications of Knuth's algorithm D on 32-bit digits. `divisor` must not be zero.
[[nodiscard]] DivResult divrem(U128 numerator, u64 divisor) noexcept;

}  // namespace long_division

// --- The native path ----------------------------------------------------------------------------

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
/// The name of the multiply this build uses, for test output and the cross-leg digest.
inline constexpr const char* kNativeMultiply = "msvc _umul128";
/// The name of the divide this build uses.
inline constexpr const char* kNativeDivide = "msvc _udiv128";
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64)
inline constexpr const char* kNativeMultiply = "msvc __umulh";
inline constexpr const char* kNativeDivide = "long division (32-bit digits)";
#elif defined(__SIZEOF_INT128__)
inline constexpr const char* kNativeMultiply = "unsigned __int128";
inline constexpr const char* kNativeDivide = "unsigned __int128";
#else
inline constexpr const char* kNativeMultiply = "reference (32-bit limbs)";
inline constexpr const char* kNativeDivide = "long division (32-bit digits)";
#endif

#if defined(__SIZEOF_INT128__) && !(defined(_MSC_VER) && !defined(__clang__))
__extension__ using NativeU128 = unsigned __int128;  ///< GCC and Clang's 128-bit integer.
#endif

/// 64×64→128, unsigned, by the fastest exact means this toolchain has.
[[nodiscard]] inline U128 mul_u64(u64 a, u64 b) noexcept {
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
    u64 high = 0;
    const u64 low = _umul128(a, b, &high);
    return U128{low, high};
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64)
    return U128{a * b, __umulh(a, b)};
#elif defined(__SIZEOF_INT128__)
    const NativeU128 product = static_cast<NativeU128>(a) * b;
    return U128{static_cast<u64>(product), static_cast<u64>(product >> 64)};
#else
    return reference::mul_u64(a, b);
#endif
}

/// 128÷64, unsigned, by the fastest exact means this toolchain has. `divisor` must not be zero.
[[nodiscard]] inline DivResult divrem(U128 numerator, u64 divisor) noexcept {
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
    // `_udiv128` faults when the quotient does not fit 64 bits, which is exactly when the high limb
    // is at least the divisor. Dividing the high limb first leaves a remainder below the divisor,
    // so the second step can never fault.
    const u64 quotient_hi = numerator.hi / divisor;
    u64 remainder = 0;
    const u64 quotient_lo = _udiv128(numerator.hi % divisor, numerator.lo, divisor, &remainder);
    return DivResult{U128{quotient_lo, quotient_hi}, remainder};
#elif defined(__SIZEOF_INT128__) && !(defined(_MSC_VER) && !defined(__clang__))
    const NativeU128 n = (static_cast<NativeU128>(numerator.hi) << 64) | numerator.lo;
    const NativeU128 q = n / divisor;
    return DivResult{U128{static_cast<u64>(q), static_cast<u64>(q >> 64)},
                     static_cast<u64>(n % divisor)};
#else
    return long_division::divrem(numerator, divisor);
#endif
}

/// The exact signed product of two `i64`, as a two's-complement 128-bit value.
///
/// Built from the unsigned product on every path: reading a negative operand as unsigned adds
/// 2^64 times the other operand to the true product, so that much is subtracted from the high limb.
[[nodiscard]] inline U128 mul_i64(i64 a, i64 b) noexcept {
    U128 product = mul_u64(static_cast<u64>(a), static_cast<u64>(b));
    if (a < 0) {
        product.hi -= static_cast<u64>(b);
    }
    if (b < 0) {
        product.hi -= static_cast<u64>(a);
    }
    return product;
}

/// `round(a * b / 2^shift)` with ties toward +infinity, wrapped to 64 bits, for `shift` in
/// [1, 63]. The kernel's one rounding multiply: `Fixed` multiplication is `shift` = 32, and the
/// polynomial evaluation in Q2.62 is `shift` = 62.
[[nodiscard]] inline i64 mul_shift(i64 a, i64 b, unsigned shift) noexcept {
    const U128 rounded = add(mul_i64(a, b), U128{u64{1} << (shift - 1), 0});
    return static_cast<i64>((rounded.lo >> shift) | (rounded.hi << (64 - shift)));
}

/// `floor(sqrt(value))` by a seeded Newton iteration and an exact correction. Agrees with
/// `reference::isqrt` for every input; `unit.detmath` checks that.
[[nodiscard]] u64 isqrt(U128 value) noexcept;

}  // namespace wide

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
