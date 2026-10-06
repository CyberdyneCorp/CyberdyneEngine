// THE CONVERSION BOUNDARY. Design §7: float into `Fixed` once, at cook, configuration or command
// creation; `Fixed` into float for presentation only, relative to the camera.
//
// Exactness, rounding and range, each asserted where it would be wrong if the rule were different:
// a dyadic value converts exactly, a value between two raws rounds to nearest with ties to EVEN
// (that is `nearbyint`'s rule, and the reason the conversion is itself the same everywhere), and a
// value beyond the range saturates rather than wrapping.

#include <cy/core/detmath/convert.h>
#include <cy/core/detmath/overflow.h>
#include <cy/test/test.h>

#include <initializer_list>
#include <limits>

namespace {

using cy::f32;
using cy::f64;
using cy::i64;
using cy::detmath::Fixed;
namespace dm = cy::detmath;

constexpr i64 kOne = Fixed::kOneRaw;
constexpr f64 kUlp = 1.0 / 4294967296.0;

}  // namespace

CY_TEST_CASE("detmath: a cooked float converts exactly when it is representable") {
    CY_CHECK_EQ(dm::from_f32_cooked(1.0F).raw, kOne);
    CY_CHECK_EQ(dm::from_f32_cooked(-0.5F).raw, -kOne / 2);
    CY_CHECK_EQ(dm::from_f32_cooked(0.0F).raw, 0);
    CY_CHECK_EQ(dm::from_f32_cooked(-0.0F).raw, 0);  // there is no negative zero to keep
    CY_CHECK_EQ(dm::from_f32_cooked(1048576.25F).raw, 1048576 * kOne + kOne / 4);
    // Every f32 with |v| < 2^31 whose lowest set bit is at least 2^-32 is exact (design §4.2).
    CY_CHECK_EQ(dm::from_f32_cooked(0x1p-32F).raw, 1);
    CY_CHECK_EQ(dm::from_f32_cooked(-0x1.fffffep30F).raw, -static_cast<i64>(0x1fffffeULL << 38));
    CY_CHECK_EQ(dm::from_f64_cooked(3.0 * kUlp).raw, 3);
}

CY_TEST_CASE("detmath: a cooked float between two raws rounds to nearest, ties to even") {
    CY_CHECK_EQ(dm::from_f64_cooked(0.5 * kUlp).raw, 0);  // tie, to even 0
    CY_CHECK_EQ(dm::from_f64_cooked(1.5 * kUlp).raw, 2);  // tie, to even 2
    CY_CHECK_EQ(dm::from_f64_cooked(2.5 * kUlp).raw, 2);  // tie, to even 2
    CY_CHECK_EQ(dm::from_f64_cooked(-1.5 * kUlp).raw, -2);
    CY_CHECK_EQ(dm::from_f64_cooked(0.75 * kUlp).raw, 1);
    CY_CHECK_EQ(dm::from_f64_cooked(0.25 * kUlp).raw, 0);
    CY_CHECK_EQ(dm::from_f32_cooked(0x1p-34F).raw, 0);
}

CY_TEST_CASE("detmath: a cooked float beyond the range saturates, and NaN is zero, both counted") {
    dm::OverflowGuard guard;
    CY_CHECK(dm::from_f64_cooked(2147483648.0) == Fixed::max());
    CY_CHECK(dm::from_f64_cooked(-2147483648.0) == Fixed::min());  // exactly min: in range
    CY_CHECK(dm::from_f64_cooked(-2147483649.0) == Fixed::min());
    CY_CHECK(dm::from_f32_cooked(std::numeric_limits<f32>::infinity()) == Fixed::max());
    CY_CHECK(dm::from_f32_cooked(-std::numeric_limits<f32>::infinity()) == Fixed::min());
    CY_CHECK(dm::from_f64_cooked(std::numeric_limits<f64>::quiet_NaN()) == Fixed::zero());
    if constexpr (dm::kCountsOverflow) {
        CY_CHECK_EQ(guard.overflows(), 5U);
    }
}

CY_TEST_CASE("detmath: presentation is relative to the origin, subtracted exactly first") {
    // A position two million metres out, one ulp from the camera: in f32 the absolute values are
    // indistinguishable, and the relative one is still exactly one ulp.
    const Fixed camera = Fixed::from_int(2000000);
    const Fixed unit = camera + Fixed::epsilon();
    CY_CHECK_EQ(dm::to_f64_relative(unit, camera), kUlp);
    CY_CHECK_EQ(dm::to_f32_relative(unit, camera), static_cast<f32>(kUlp));
    CY_CHECK_EQ(dm::to_f32_relative(camera, unit), -static_cast<f32>(kUlp));
    CY_CHECK_EQ(dm::to_f64_relative(Fixed::from_int(-3) + Fixed::half(), Fixed::zero()), -2.5);
    // Round trip through the cooked conversion is exact for every representable value.
    for (const i64 raw : {i64{1}, i64{-1}, kOne * 12345 + 678, -kOne * 99 - 1}) {
        CY_CHECK_EQ(
            dm::from_f64_cooked(dm::to_f64_relative(Fixed::from_raw(raw), Fixed::zero())).raw, raw);
    }
}
