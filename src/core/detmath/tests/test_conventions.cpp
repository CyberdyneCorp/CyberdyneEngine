// SPDX-License-Identifier: MIT
// THE CONVENTIONS, AS EXECUTABLE TESTS, FOR THE FIXED-POINT TYPES. Task 3.2, design §6.
//
// The same numeric consequences src/core/math/tests/test_conventions.cpp asserts for `Vec3` and
// `Quat`, asserted for `FixedVec3` and `FixedQuat`, so that both libraries name the same direction
// "forward" and the same sense "counter-clockwise". A unit converted from `f32` authoring data at
// load and simulated in `Fixed` would otherwise turn the wrong way with no number looking wrong.
//
// Where the fixed-point answer is exact it is spelled `==`. The quarter-turn rotations go through
// sin and cos of an eighth of a turn, which are rounded, so those are within a stated number of
// ulps; the core-math file states 1e-6 for the same identities in `f32`.

#include <cy/core/detmath/vec.h>
#include <cy/test/test.h>

namespace {

using cy::i64;
using cy::detmath::Angle;
using cy::detmath::Fixed;
using cy::detmath::FixedQuat;
using cy::detmath::FixedTransform;
using cy::detmath::FixedVec3;
namespace dm = cy::detmath;

/// Every quarter-turn identity below lands within this many ulps of its exact answer: two roundings
/// of sin and cos of an eighth of a turn, squared and doubled.
constexpr i64 kQuarterTurnUlps = 4;

[[nodiscard]] bool near(FixedVec3 a, FixedVec3 b, i64 ulps) noexcept {
    const auto within = [ulps](Fixed u, Fixed v) noexcept {
        const i64 difference = u.raw - v.raw;
        return difference <= ulps && difference >= -ulps;
    };
    return within(a.x, b.x) && within(a.y, b.y) && within(a.z, b.z);
}

}  // namespace

CY_TEST_CASE("detmath convention: the coordinate system is right-handed") {
    CY_CHECK(dm::cross(dm::fixed_axis_x(), dm::fixed_axis_y()) == dm::fixed_axis_z());
    CY_CHECK(dm::cross(dm::fixed_axis_y(), dm::fixed_axis_z()) == dm::fixed_axis_x());
    CY_CHECK(dm::cross(dm::fixed_axis_z(), dm::fixed_axis_x()) == dm::fixed_axis_y());
}

CY_TEST_CASE("detmath convention: Y is up and the identity transform faces -Z") {
    CY_CHECK(dm::fixed_forward() == (FixedVec3{Fixed::zero(), Fixed::zero(), -Fixed::one()}));
    CY_CHECK(FixedTransform::identity().forward() == dm::fixed_forward());
    CY_CHECK(dm::rotate(FixedQuat::identity(), dm::fixed_axis_y()) == dm::fixed_axis_y());
}

CY_TEST_CASE("detmath convention: rotation is counter-clockwise seen from the positive axis") {
    // A quarter turn about +Y takes +X to -Z: yawing left points forward where right used to be.
    const FixedQuat yaw = FixedQuat::from_axis_angle(dm::fixed_axis_y(), Angle::quarter());
    CY_CHECK(near(dm::rotate(yaw, dm::fixed_axis_x()), dm::fixed_forward(), kQuarterTurnUlps));

    // A quarter turn about +X takes +Y to +Z.
    const FixedQuat pitch = FixedQuat::from_axis_angle(dm::fixed_axis_x(), Angle::quarter());
    CY_CHECK(near(dm::rotate(pitch, dm::fixed_axis_y()), dm::fixed_axis_z(), kQuarterTurnUlps));

    // A quarter turn about +Z takes +X to +Y.
    const FixedQuat roll = FixedQuat::from_axis_angle(dm::fixed_axis_z(), Angle::quarter());
    CY_CHECK(near(dm::rotate(roll, dm::fixed_axis_x()), dm::fixed_axis_y(), kQuarterTurnUlps));

    // And the converse, so the case cannot pass by rotating nothing: a half turn about +Y sends +X
    // to -X, exactly.
    const FixedQuat about = FixedQuat::from_axis_angle(dm::fixed_axis_y(), Angle::half());
    CY_CHECK(dm::rotate(about, dm::fixed_axis_x()) == -dm::fixed_axis_x());
}

CY_TEST_CASE("detmath convention: A * B applies B first") {
    const FixedTransform move{FixedVec3{Fixed::from_int(10), Fixed::zero(), Fixed::zero()},
                              FixedQuat::identity()};
    const FixedTransform turn{FixedVec3::zero(),
                              FixedQuat::from_axis_angle(dm::fixed_axis_y(), Angle::quarter())};
    const FixedVec3 p = dm::fixed_axis_x();
    // move * turn: turn first — (1,0,0) becomes (0,0,-1) — then move: (10, 0, -1).
    CY_CHECK(near(dm::transform_point(move * turn, p),
                  FixedVec3{Fixed::from_int(10), Fixed::zero(), Fixed::from_int(-1)},
                  kQuarterTurnUlps));
    // turn * move: move first — (11,0,0) — then turn: (0, 0, -11). The quarter turn's error scales
    // with the 11 m lever arm.
    CY_CHECK(near(dm::transform_point(turn * move, p),
                  FixedVec3{Fixed::zero(), Fixed::zero(), Fixed::from_int(-11)},
                  11 * kQuarterTurnUlps));
}
