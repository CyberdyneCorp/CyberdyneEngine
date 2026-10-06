// SPDX-License-Identifier: MIT
// THE 128-BIT PATHS AGREE WITH THE REFERENCE. Task 1.3, design §4.4.
//
// "The portable reference is compiled on every leg. The unit suite checks that the selected path
// matches it, bit for bit, over the golden vectors and a random sweep. An intrinsic that disagrees
// fails on the leg that uses it." The golden vectors are integration.detmath_vectors'; this is the
// edge cases and a sweep sized for a unit budget. integration.detmath_vectors runs a longer one.
//
// The long division is checked on every leg too, although only MSVC arm64 uses it: a path that one
// platform relies on and only that platform tests is a path whose first failure is a desync.

#include "detmath_test.h"

#include <cy/core/detmath/wide.h>
#include <cy/test/test.h>

#include <cstring>
#include <initializer_list>
#include <string>

namespace {

using cy::i64;
using cy::u64;
using cy::detmath::DivResult;
using cy::detmath::U128;
namespace wide = cy::detmath::wide;

constexpr u64 kAll = ~u64{0};

const u64 kEdges[] = {0,
                      1,
                      2,
                      3,
                      0x7FFF'FFFFU,
                      0x8000'0000U,
                      0xFFFF'FFFFU,
                      0x1'0000'0000U,
                      0x1'0000'0001U,
                      (u64{1} << 62),
                      (u64{1} << 63) - 1,
                      u64{1} << 63,
                      kAll - 1,
                      kAll};

[[nodiscard]] bool same(const DivResult& a, const DivResult& b) noexcept {
    return a.quotient == b.quotient && a.remainder == b.remainder;
}

}  // namespace

CY_TEST_CASE("detmath: the native multiply is the reference multiply on the edges") {
    CY_TEST_MESSAGE("native multiply: " << std::string(wide::kNativeMultiply));
    for (const u64 a : kEdges) {
        for (const u64 b : kEdges) {
            CY_TEST_INFO("a " << a << " b " << b);
            CY_CHECK(wide::mul_u64(a, b) == wide::reference::mul_u64(a, b));
        }
    }
    // The largest product, written out: (2^64 - 1)^2 = 2^128 - 2^65 + 1.
    CY_CHECK(wide::reference::mul_u64(kAll, kAll) == (U128{1, kAll - 1}));
}

CY_TEST_CASE("detmath: the native multiply is the reference multiply over a sweep") {
    cy::detmath_test::Rng rng(0x5EED'0001ULL);
    for (int index = 0; index < 2000; ++index) {
        const u64 a = static_cast<u64>(rng.scaled());
        const u64 b = static_cast<u64>(rng.scaled());
        CY_TEST_INFO("a " << a << " b " << b);
        CY_CHECK(wide::mul_u64(a, b) == wide::reference::mul_u64(a, b));
    }
}

CY_TEST_CASE("detmath: the signed product is the two's complement of the exact product") {
    CY_CHECK(wide::mul_i64(-1, -1) == (U128{1, 0}));
    CY_CHECK(wide::mul_i64(-1, 1) == (U128{kAll, kAll}));
    CY_CHECK(wide::mul_i64(INT64_MIN, INT64_MIN) == (U128{0, u64{1} << 62}));
    CY_CHECK(wide::mul_i64(INT64_MIN, -1) == (U128{u64{1} << 63, 0}));
    CY_CHECK(wide::mul_i64(INT64_MAX, INT64_MIN) ==
             wide::negate(U128{u64{1} << 63, (u64{1} << 62) - 1}));
}

CY_TEST_CASE("detmath: the native and long divisions are the reference division on the edges") {
    CY_TEST_MESSAGE("native divide: " << std::string(wide::kNativeDivide));
    for (const u64 hi : kEdges) {
        for (const u64 divisor : kEdges) {
            if (divisor == 0) {
                continue;
            }
            const U128 numerator{0x0123'4567'89AB'CDEFULL ^ hi, hi};
            CY_TEST_INFO("hi " << hi << " divisor " << divisor);
            const DivResult expected = wide::reference::divrem(numerator, divisor);
            CY_CHECK(same(wide::divrem(numerator, divisor), expected));
            CY_CHECK(same(wide::long_division::divrem(numerator, divisor), expected));
        }
    }
}

CY_TEST_CASE("detmath: the native and long divisions are the reference division over a sweep") {
    cy::detmath_test::Rng rng(0x5EED'0002ULL);
    for (int index = 0; index < 150; ++index) {
        const U128 numerator{rng.next(), static_cast<u64>(rng.scaled())};
        u64 divisor = static_cast<u64>(rng.scaled());
        divisor = divisor == 0 ? 1 : divisor;
        CY_TEST_INFO("numerator " << numerator.hi << ":" << numerator.lo << " divisor " << divisor);
        const DivResult expected = wide::reference::divrem(numerator, divisor);
        CY_CHECK(same(wide::divrem(numerator, divisor), expected));
        CY_CHECK(same(wide::long_division::divrem(numerator, divisor), expected));
    }
}

CY_TEST_CASE("detmath: the reference division is division") {
    // The reference is what the others are held to, so it is held to arithmetic: q d + r == n and
    // r < d, for numerators whose quotient needs all 128 bits.
    cy::detmath_test::Rng rng(0x5EED'0003ULL);
    for (int index = 0; index < 100; ++index) {
        const U128 numerator{rng.next(), rng.next()};
        const u64 divisor = (rng.next() >> (rng.next() & 63U)) | 1U;
        const DivResult result = wide::reference::divrem(numerator, divisor);
        CY_CHECK(result.remainder < divisor);
        const U128 low = wide::reference::mul_u64(result.quotient.lo, divisor);
        const U128 high = wide::reference::mul_u64(result.quotient.hi, divisor);
        CY_CHECK(high.hi == 0);
        const U128 product = wide::add(low, U128{0, high.lo});
        CY_CHECK(wide::add(product, U128{result.remainder, 0}) == numerator);
    }
}

CY_TEST_CASE("detmath: the fast integer square root is the reference one") {
    cy::detmath_test::Rng rng(0x5EED'0004ULL);
    for (const u64 hi : kEdges) {
        for (const u64 lo : {u64{0}, u64{1}, kAll}) {
            const U128 value{lo, hi};
            CY_TEST_INFO("value " << hi << ":" << lo);
            CY_CHECK_EQ(wide::isqrt(value), wide::reference::isqrt(value));
        }
    }
    for (int index = 0; index < 150; ++index) {
        const U128 value{rng.next(), static_cast<u64>(rng.scaled()) >> 1};
        CY_TEST_INFO("value " << value.hi << ":" << value.lo);
        CY_CHECK_EQ(wide::isqrt(value), wide::reference::isqrt(value));
    }
}

CY_TEST_CASE("detmath: the square root's floor is exact at squares and either side of them") {
    // Where an approximation is most likely to land one off: r^2 - 1, r^2 and r^2 + 2r (the last
    // value whose floor is still r).
    cy::detmath_test::Rng rng(0x5EED'0005ULL);
    for (int index = 0; index < 300; ++index) {
        const u64 root = rng.next() >> (rng.next() & 63U);
        const U128 square = wide::mul_u64(root, root);
        CY_TEST_INFO("root " << root);
        CY_CHECK_EQ(wide::isqrt(square), root);
        if (root != 0) {
            CY_CHECK_EQ(wide::isqrt(wide::sub(square, U128{1, 0})), root - 1);
        }
        const U128 last = wide::add(square, wide::add(U128{root, 0}, U128{root, 0}));
        if (root != kAll) {
            CY_CHECK_EQ(wide::isqrt(last), root);
        }
    }
    CY_CHECK_EQ(wide::isqrt(U128{kAll, kAll}), kAll);
}

CY_TEST_CASE("detmath: shifts and comparisons read two's complement") {
    const U128 minus_one{kAll, kAll};
    CY_CHECK(wide::sar(minus_one, 100) == minus_one);
    CY_CHECK(wide::sar(U128{0, u64{1} << 63}, 64) == (U128{u64{1} << 63, kAll}));
    CY_CHECK(wide::sar(U128{0, u64{1} << 63}, 127) == minus_one);
    CY_CHECK(wide::shr(U128{0, u64{1} << 63}, 127) == (U128{1, 0}));
    CY_CHECK(wide::shl(U128{1, 0}, 127) == (U128{0, u64{1} << 63}));
    CY_CHECK(wide::less_signed(minus_one, U128{}));
    CY_CHECK_FALSE(wide::less(minus_one, U128{}));
    CY_CHECK(wide::fits_i64(wide::from_i64(INT64_MIN)));
    CY_CHECK_FALSE(wide::fits_i64(U128{u64{1} << 63, 0}));
    CY_CHECK_EQ(wide::mul_shift(3, i64{1} << 61, 62), 2);    // 1.5 ties up
    CY_CHECK_EQ(wide::mul_shift(-3, i64{1} << 61, 62), -1);  // -1.5 ties up, toward +infinity
}
