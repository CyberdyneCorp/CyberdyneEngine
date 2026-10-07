// SPDX-License-Identifier: MIT
// VECTORS, ROTATIONS AND SHAPES OVER `Fixed`. Task 3.1, design §6.
//
// Each case pins a stated rounding on a value where a different rule would give a different raw:
// the exact Q64.64 products (a `Fixed` product would have rounded them), the correctly rounded
// lengths, the truncating `normalize`, the exact overlap decisions at the touching boundary, and
// the long-division `ratio_unit`.

#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/overflow.h>
#include <cy/core/detmath/shapes.h>
#include <cy/core/detmath/vec.h>
#include <cy/test/test.h>

namespace {

using cy::i64;
using cy::u64;
using cy::detmath::Angle;
using cy::detmath::Fixed;
using cy::detmath::Fixed16Vec3;
using cy::detmath::FixedAabb;
using cy::detmath::FixedCapsule2D;
using cy::detmath::FixedCircle;
using cy::detmath::FixedQuat;
using cy::detmath::FixedTransform;
using cy::detmath::FixedVec2;
using cy::detmath::FixedVec3;
using cy::detmath::OverflowGuard;
using cy::detmath::Rot2;
using cy::detmath::U128;
using cy::detmath::WideFixed;
namespace dm = cy::detmath;

constexpr i64 kOne = Fixed::kOneRaw;

[[nodiscard]] Fixed raw(i64 value) noexcept {
    return Fixed::from_raw(value);
}
[[nodiscard]] Fixed whole(cy::i32 value) noexcept {
    return Fixed::from_int(value);
}
[[nodiscard]] i64 ulps(Fixed a, Fixed b) noexcept {
    const i64 difference = a.raw - b.raw;
    return difference < 0 ? -difference : difference;
}

}  // namespace

CY_TEST_CASE("detmath geometry: dot, cross and squared length are exact in Q64.64") {
    // 2^-32 * 2^-32 is 2^-64: a Fixed product rounds it to 0 (the tie rounds up only at 2^-33),
    // and the wide one keeps it.
    const FixedVec2 tiny{raw(1), raw(0)};
    CY_CHECK(dm::dot(tiny, tiny) == WideFixed::from_raw(U128{1, 0}));
    CY_CHECK_EQ((tiny.x * tiny.x).raw, 0);

    const FixedVec2 a{whole(3), whole(-2)};
    const FixedVec2 b{whole(5), whole(7)};
    CY_CHECK(dm::dot(a, b) == WideFixed::from_fixed(whole(1)));
    CY_CHECK(dm::cross(a, b) == WideFixed::from_fixed(whole(31)));
    CY_CHECK(dm::cross(b, a) == WideFixed::from_fixed(whole(-31)));
    CY_CHECK(dm::length_squared(a) == WideFixed::from_fixed(whole(13)));

    // Beyond 181 m a `Fixed` square would still fit, but beyond 46 341 m it wraps; the wide one
    // does not.
    const FixedVec2 far{whole(100000), whole(100000)};
    CY_CHECK(!dm::length_squared(far).negative());
    CY_CHECK_EQ(dm::length_squared(far).saturating_narrow().raw, Fixed::max().raw);
}

CY_TEST_CASE("detmath geometry: length and distance are correctly rounded") {
    CY_CHECK_EQ(dm::length(FixedVec2{whole(3), whole(4)}).raw, 5 * kOne);
    CY_CHECK_EQ(dm::length(FixedVec3{whole(2), whole(3), whole(6)}).raw, 7 * kOne);
    CY_CHECK_EQ(dm::length(FixedVec2{whole(1), whole(1)}).raw, dm::sqrt(whole(2)).raw);
    CY_CHECK_EQ(dm::distance(FixedVec2{whole(1), whole(1)}, FixedVec2{whole(4), whole(5)}).raw,
                5 * kOne);
    CY_CHECK_EQ(dm::distance(FixedVec3{whole(1), whole(1), whole(1)},
                             FixedVec3{whole(3), whole(4), whole(7)})
                    .raw,
                7 * kOne);
}

CY_TEST_CASE("detmath geometry: normalize truncates, and the zero vector is zero and counted") {
    const FixedVec2 unit = dm::normalize(FixedVec2{whole(3), whole(4)});
    CY_CHECK_EQ(unit.x.raw, (whole(3) / whole(5)).raw);
    CY_CHECK_EQ(unit.y.raw, (whole(4) / whole(5)).raw);

    const FixedVec3 axis = dm::normalize(FixedVec3{whole(0), whole(-9), whole(0)});
    CY_CHECK(axis == (FixedVec3{whole(0), whole(-1), whole(0)}));

    OverflowGuard guard;
    CY_CHECK(dm::normalize(FixedVec2::zero()) == FixedVec2::zero());
    CY_CHECK(dm::normalize(FixedVec3::zero()) == FixedVec3::zero());
    if constexpr (dm::kCountsOverflow) {
        CY_CHECK_EQ(guard.overflows(), 2U);
    }
}

CY_TEST_CASE("detmath geometry: clamp_length leaves a short vector alone and caps a long one") {
    const FixedVec2 short_one{whole(3), whole(4)};
    CY_CHECK(dm::clamp_length(short_one, whole(5)) == short_one);
    // 30 * 5 / 50 and 40 * 5 / 50 are exact: the scaling goes through the 128-bit product.
    const FixedVec2 capped = dm::clamp_length(FixedVec2{whole(30), whole(40)}, whole(5));
    CY_CHECK(capped == (FixedVec2{whole(3), whole(4)}));
    const FixedVec2 far = dm::clamp_length(FixedVec2{whole(600), whole(0)}, whole(4));
    CY_CHECK(far == (FixedVec2{whole(4), whole(0)}));
    CY_CHECK(dm::clamp_length(FixedVec2{whole(1), whole(0)}, Fixed::zero()) == FixedVec2::zero());
}

CY_TEST_CASE("detmath geometry: mul_div scales through the exact product and truncates once") {
    // 3 * 5 / 7: the quotient of the exact product, not of a rounded 15.
    CY_CHECK_EQ(dm::mul_div(whole(3), whole(5), whole(7)).raw,
                static_cast<i64>((u64{15} << 32) / 7));
    CY_CHECK_EQ(dm::mul_div(whole(-3), whole(5), whole(7)).raw,
                -static_cast<i64>((u64{15} << 32) / 7));
    CY_CHECK_EQ(dm::mul_div(whole(3), whole(-5), whole(-7)).raw,
                static_cast<i64>((u64{15} << 32) / 7));
    // A product far beyond the Fixed range is fine when the quotient is not.
    CY_CHECK_EQ(dm::mul_div(whole(1 << 30), whole(1 << 30), whole(1 << 30)).raw, i64{1} << 62);
    OverflowGuard guard;
    CY_CHECK_EQ(dm::mul_div(whole(2), whole(3), Fixed::zero()).raw, Fixed::max().raw);
    CY_CHECK_EQ(dm::mul_div(whole(-2), whole(3), Fixed::zero()).raw, Fixed::min().raw);
    if constexpr (dm::kCountsOverflow) {
        CY_CHECK_EQ(guard.overflows(), 2U);
    }
}

CY_TEST_CASE("detmath geometry: the 3D cross product rounds each component once") {
    CY_CHECK(dm::cross(dm::fixed_axis_x(), dm::fixed_axis_y()) == dm::fixed_axis_z());
    // The z component of (2^-32, 0, 0) x (0, y, 0) is the exact product 2^-32 y, which is below
    // one ulp for y < 1: it is rounded once, to nearest with ties toward +infinity.
    const FixedVec3 a{raw(1), raw(0), raw(0)};
    const FixedVec3 b{raw(0), raw(kOne / 2 - 1), raw(0)};
    CY_CHECK_EQ(dm::cross(a, b).z.raw, 0);
    const FixedVec3 c{raw(1), raw(0), raw(0)};
    const FixedVec3 d{raw(0), raw(kOne / 2), raw(0)};
    CY_CHECK_EQ(dm::cross(c, d).z.raw, 1);  // the tie
    const FixedVec3 e{raw(1), raw(0), raw(0)};
    const FixedVec3 f{raw(0), raw(kOne / 2 + kOne / 4 + kOne), raw(0)};
    CY_CHECK_EQ(dm::cross(e, f).z.raw, 2);
}

CY_TEST_CASE("detmath geometry: Fixed16Vec3 widens exactly and narrows by the Fixed16 rule") {
    const FixedVec3 value{raw(3 * kOne + (kOne >> 16)), raw(-kOne), raw(kOne >> 17)};
    const Fixed16Vec3 stored = Fixed16Vec3::narrow(value);
    CY_CHECK_EQ(stored.x.raw, (3 << 16) + 1);
    CY_CHECK_EQ(stored.y.raw, -(1 << 16));
    CY_CHECK_EQ(stored.z.raw, 1);  // half a Q16.16 ulp: the tie goes toward +infinity
    CY_CHECK(Fixed16Vec3::narrow(stored.widen()) == stored);
}

CY_TEST_CASE("detmath geometry: Rot2 turns counter-clockwise and its inverse undoes it") {
    const Rot2 quarter = Rot2::from_angle(Angle::quarter());
    CY_CHECK_EQ(quarter.cos.raw, 0);
    CY_CHECK_EQ(quarter.sin.raw, kOne);
    CY_CHECK(dm::rotate(quarter, FixedVec2{whole(1), whole(0)}) == (FixedVec2{whole(0), whole(1)}));
    CY_CHECK(dm::unrotate(quarter, FixedVec2{whole(0), whole(1)}) ==
             (FixedVec2{whole(1), whole(0)}));

    const Rot2 turn = Rot2::from_angle(Angle::from_raw(0x2345'6789U));
    const FixedVec2 v{whole(7), whole(-3)};
    const FixedVec2 back = dm::unrotate(turn, dm::rotate(turn, v));
    CY_CHECK(ulps(back.x, v.x) <= 8);
    CY_CHECK(ulps(back.y, v.y) <= 8);
}

CY_TEST_CASE("detmath geometry: quaternion composition stays unit length and applies b first") {
    const FixedQuat yaw = FixedQuat::from_axis_angle(dm::fixed_axis_y(), Angle::quarter());
    const FixedQuat pitch = FixedQuat::from_axis_angle(dm::fixed_axis_x(), Angle::quarter());
    const FixedQuat both = yaw * pitch;
    const WideFixed norm =
        dm::dot(FixedVec3{both.x, both.y, both.z}, FixedVec3{both.x, both.y, both.z}) +
        WideFixed::product(both.w, both.w);
    CY_CHECK(ulps(norm.narrow(), Fixed::one()) <= 4);

    // pitch first takes +Y to +Z, then yaw takes +Z to +X.
    const FixedVec3 moved = dm::rotate(both, dm::fixed_axis_y());
    CY_CHECK(ulps(moved.x, Fixed::one()) <= 8);
    CY_CHECK(ulps(moved.y, Fixed::zero()) <= 8);
    CY_CHECK(ulps(moved.z, Fixed::zero()) <= 8);

    // A half turn about +Y is exactly (0, 1, 0, 0): the half angle is a quarter, whose sine and
    // cosine are exact.
    CY_CHECK(FixedQuat::from_axis_angle(dm::fixed_axis_y(), Angle::half()) ==
             (FixedQuat{Fixed::zero(), Fixed::one(), Fixed::zero(), Fixed::zero()}));

    OverflowGuard guard;
    CY_CHECK(dm::normalize(FixedQuat{Fixed::zero(), Fixed::zero(), Fixed::zero(), Fixed::zero()}) ==
             FixedQuat::identity());
    if constexpr (dm::kCountsOverflow) {
        CY_CHECK_EQ(guard.overflows(), 1U);
    }
}

CY_TEST_CASE("detmath geometry: a transform rotates, then translates, and composes b first") {
    const FixedTransform move{FixedVec3{whole(10), whole(0), whole(0)}, FixedQuat::identity()};
    const FixedTransform turn{FixedVec3::zero(),
                              FixedQuat::from_axis_angle(dm::fixed_axis_y(), Angle::quarter())};
    const FixedVec3 p{whole(1), whole(0), whole(0)};

    // move * turn: turn first, (1,0,0) -> (0,0,-1), then move: (10, 0, -1).
    const FixedVec3 a = dm::transform_point(move * turn, p);
    CY_CHECK(ulps(a.x, whole(10)) <= 8);
    CY_CHECK(ulps(a.z, whole(-1)) <= 8);
    // turn * move: move first, (11,0,0), then turn: (0, 0, -11).
    const FixedVec3 b = dm::transform_point(turn * move, p);
    CY_CHECK(ulps(b.x, whole(0)) <= 64);
    CY_CHECK(ulps(b.z, whole(-11)) <= 64);
}

CY_TEST_CASE("detmath geometry: ratio_unit is a long division clamped to [0, 1]") {
    const WideFixed three = WideFixed::from_fixed(whole(3));
    CY_CHECK_EQ(dm::ratio_unit(WideFixed::from_fixed(whole(1)), three).raw,
                static_cast<i64>((u64{1} << 32) / 3));
    CY_CHECK_EQ(dm::ratio_unit(WideFixed::from_fixed(whole(2)), three).raw,
                static_cast<i64>((u64{2} << 32) / 3));
    CY_CHECK_EQ(dm::ratio_unit(three, three).raw, kOne);
    CY_CHECK_EQ(dm::ratio_unit(WideFixed::from_fixed(whole(4)), three).raw, kOne);
    CY_CHECK_EQ(dm::ratio_unit(WideFixed::from_fixed(whole(-1)), three).raw, 0);
    CY_CHECK_EQ(dm::ratio_unit(three, WideFixed{}).raw, 0);
    // Values that do not fit a Fixed at all: 2^40 / 2^41 is still exactly a half.
    const WideFixed big = WideFixed::product(whole(1 << 20), whole(1 << 20));
    const WideFixed bigger = big + big;
    CY_CHECK_EQ(dm::ratio_unit(big, bigger).raw, kOne / 2);
}

CY_TEST_CASE("detmath geometry: the closest point on a segment clamps to its ends") {
    const FixedVec2 a{whole(0), whole(0)};
    const FixedVec2 b{whole(10), whole(0)};
    // The parameter 0.4 is truncated to 2^-32, so the point falls short of 4 toward `a` by less
    // than |b - a| = 10 ulps — and by the same amount on every peer.
    const FixedVec2 middle = dm::closest_point_on_segment(a, b, FixedVec2{whole(4), whole(3)});
    CY_CHECK(middle.x <= whole(4));
    CY_CHECK(ulps(middle.x, whole(4)) < 10);
    CY_CHECK(middle.y == whole(0));
    // A parameter that is exact in binary lands exactly.
    CY_CHECK(dm::closest_point_on_segment(a, FixedVec2{whole(8), whole(0)},
                                          FixedVec2{whole(2), whole(-5)}) ==
             (FixedVec2{whole(2), whole(0)}));
    CY_CHECK(dm::closest_point_on_segment(a, b, FixedVec2{whole(-4), whole(3)}) == a);
    CY_CHECK(dm::closest_point_on_segment(a, b, FixedVec2{whole(14), whole(-3)}) == b);
    CY_CHECK(dm::closest_point_on_segment(a, a, FixedVec2{whole(3), whole(3)}) == a);
}

CY_TEST_CASE("detmath geometry: shapes that touch exactly do not overlap") {
    const FixedCircle left{FixedVec2{whole(0), whole(0)}, whole(1)};
    const FixedCircle touching{FixedVec2{whole(2), whole(0)}, whole(1)};
    const FixedCircle one_ulp_in{FixedVec2{raw(2 * kOne - 1), whole(0)}, whole(1)};
    CY_CHECK(!left.overlaps(touching));
    CY_CHECK(left.overlaps(one_ulp_in));
    CY_CHECK(left.contains(FixedVec2{whole(1), whole(0)}));
    CY_CHECK(!left.contains(FixedVec2{raw(kOne + 1), whole(0)}));

    const FixedCapsule2D wall{FixedVec2{whole(0), whole(5)}, FixedVec2{whole(10), whole(5)},
                              whole(1)};
    CY_CHECK(!wall.overlaps(FixedCircle{FixedVec2{whole(5), whole(3)}, whole(1)}));
    CY_CHECK(wall.overlaps(FixedCircle{FixedVec2{whole(5), raw(3 * kOne + 1)}, whole(1)}));
    CY_CHECK(wall.contains(FixedVec2{whole(11), whole(5)}));
    CY_CHECK(!wall.contains(FixedVec2{whole(11), raw(5 * kOne + 1)}));

    const FixedAabb box{FixedVec3{whole(0), whole(0), whole(0)},
                        FixedVec3{whole(1), whole(1), whole(1)}};
    const FixedAabb beside{FixedVec3{whole(1), whole(0), whole(0)},
                           FixedVec3{whole(2), whole(1), whole(1)}};
    const FixedAabb into{FixedVec3{raw(kOne - 1), whole(0), whole(0)},
                         FixedVec3{whole(2), whole(1), whole(1)}};
    CY_CHECK(!box.overlaps(beside));
    CY_CHECK(box.overlaps(into));
    CY_CHECK(box.contains(FixedVec3{whole(1), whole(1), whole(1)}));
    CY_CHECK(!box.contains(FixedVec3{raw(kOne + 1), whole(1), whole(1)}));
}
