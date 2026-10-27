// SPDX-License-Identifier: MIT
// Foot placement, the two-bone solver it stands on and the aim chain. Issue #92.
//
// UNIT: each case builds a nine-joint pair of legs and solves it a handful of times against a
// plane or a step answered in code, touching no world and no file.

#include <cy/animation/foot_placement.h>
#include <cy/animation/ik.h>
#include <cy/test/test.h>

#include "legs_fixture.h"

#include <cmath>
#include <cstring>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

namespace {

[[nodiscard]] Transform model(const Skeleton& skeleton, u16 joint, Span<const Transform> local) {
    return joint_model(skeleton, joint, local);
}

struct Legs2 {
    Legs2() noexcept : skeleton(allocator()), local(allocator()) {}
    [[nodiscard]] Status build(Quat hip_turn = Quat::identity()) noexcept {
        if (Status built = build_legs(skeleton, hip_turn); !built) {
            return built;
        }
        if (Status sized = local.resize(kLegJoints); !sized) {
            return sized;
        }
        skeleton.reference_pose(local.span());
        return ok();
    }
    Skeleton skeleton;
    Array<Transform> local;
};

constexpr f32 kDegree = 0.0174532925F;

}  // namespace

CY_TEST_CASE("ik: a two-bone chain under a rotated parent reaches its target exactly") {
    // REGRESSION. The solver applied its knee bend about a MODEL-space axis to the knee's LOCAL
    // rotation, which is only the same thing when every joint above it is unrotated. Under a hip
    // turned 90 degrees, as an exported rig's often is, the knee bent out of the leg's plane and
    // the ankle missed by centimetres.
    Legs2 legs;
    CY_REQUIRE(
        legs.build(Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, 90.0F * kDegree)).has_value());
    const IkChain leg = legs_setup().legs[0];
    const Vec3 hip = model(legs.skeleton, kHipL, legs.local.span()).translation;
    const Vec3 target = hip + Vec3{0.05F, -0.7F, 0.15F};
    Expected<bool, Error> reached =
        solve_two_bone(legs.skeleton, leg, target, 1.0F, legs.local.span());
    CY_REQUIRE(reached.has_value());
    CY_CHECK(*reached);
    const Vec3 ankle = model(legs.skeleton, kAnkleL, legs.local.span()).translation;
    CY_CHECK_LT(length(ankle - target), 1e-3F);
    // The bone lengths are kept: a solve rotates, it never stretches.
    const Vec3 knee = model(legs.skeleton, kKneeL, legs.local.span()).translation;
    CY_CHECK_LT(std::fabs(length(knee - hip) - length(Vec3{0.0F, -0.42F, 0.04F})), 1e-4F);
    CY_CHECK_LT(std::fabs(length(ankle - knee) - length(Vec3{0.0F, -0.42F, -0.04F})), 1e-4F);
    // And the knee goes where the pole says, forward.
    CY_CHECK_GT(knee.z - hip.z, 0.0F);
}

CY_TEST_CASE("foot placement: on a slope both feet stand on the ground and turn to it") {
    Legs2 legs;
    CY_REQUIRE(legs.build().has_value());
    // 20 degrees rising towards +x: the left foot (x = -0.1) is over lower ground than the
    // character's root, the right one over higher.
    const f32 slope = 20.0F * kDegree;
    const Vec3 normal{-std::sin(slope), std::cos(slope), 0.0F};
    const PlaneGround ground(Vec3{0.0F, 0.0F, 0.0F}, normal);
    FootPlacementContext context;
    FootPlacementState state;
    FootPlacementReport report;
    CY_REQUIRE(solve_foot_placement(legs.skeleton, legs_setup(), context, ground, 1.0F, state,
                                    legs.local.span(), &report)
                   .has_value());
    CY_REQUIRE(report.solved);
    CY_CHECK(report.hit[0]);
    CY_CHECK(report.hit[1]);
    CY_CHECK(report.reached[0]);
    CY_CHECK(report.reached[1]);
    // THE HIPS DROP by the lower foot's offset so that leg can reach.
    CY_CHECK_LT(report.pelvis, -0.02F);
    CY_CHECK_EQ(report.pelvis, report.offset[0]);

    for (const u16 ankle : {static_cast<u16>(kAnkleL), static_cast<u16>(kAnkleR)}) {
        const Transform placed = model(legs.skeleton, ankle, legs.local.span());
        const f32 ground_height = std::tan(slope) * placed.translation.x;
        // PLANTED: the ankle is its own height above the slope along the slope's normal, to the
        // millimetre.
        const f32 above = (placed.translation.y - ground_height) * std::cos(slope);
        CY_CHECK_LT(std::fabs(above - kAnkleHeight), 1e-3F);
        // And the sole faces the slope: the foot's up is the ground's normal within a degree.
        const Vec3 up = placed.rotation * Vec3{0.0F, 1.0F, 0.0F};
        CY_CHECK_GT(dot(normalize(up), normal), std::cos(1.0F * kDegree));
    }

    // WITHOUT IT, the reference pose floats one foot and sinks the other by centimetres.
    Legs2 unplaced;
    CY_REQUIRE(unplaced.build().has_value());
    for (const u16 ankle : {static_cast<u16>(kAnkleL), static_cast<u16>(kAnkleR)}) {
        const Vec3 at = model(unplaced.skeleton, ankle, unplaced.local.span()).translation;
        const f32 above = (at.y - (std::tan(slope) * at.x)) * std::cos(slope);
        CY_CHECK_GT(std::fabs(above - kAnkleHeight), 0.02F);
    }
}

CY_TEST_CASE("foot placement: on a stair one foot is raised and the hips stay") {
    Legs2 legs;
    CY_REQUIRE(legs.build().has_value());
    // The character stands on the lower step with its right foot over an 18 cm riser.
    const StepGround stairs(0.0F, 0.0F, 0.18F);
    FootPlacementContext context;
    FootPlacementState state;
    FootPlacementReport report;
    CY_REQUIRE(solve_foot_placement(legs.skeleton, legs_setup(), context, stairs, 1.0F, state,
                                    legs.local.span(), &report)
                   .has_value());
    CY_CHECK_EQ(report.pelvis, 0.0F);
    CY_CHECK_LT(std::fabs(report.offset[1] - 0.18F), 1e-6F);
    const Vec3 left = model(legs.skeleton, kAnkleL, legs.local.span()).translation;
    const Vec3 right = model(legs.skeleton, kAnkleR, legs.local.span()).translation;
    CY_CHECK_LT(std::fabs(left.y - kAnkleHeight), 1e-3F);
    CY_CHECK_LT(std::fabs(right.y - (0.18F + kAnkleHeight)), 1e-3F);

    // On the upper step with the left foot hanging over the lower one, the hips drop the riser.
    Legs2 upper;
    CY_REQUIRE(upper.build().has_value());
    FootPlacementContext raised;
    raised.model_to_world = Transform::from_translation(Vec3{0.0F, 0.18F, 0.0F});
    FootPlacementState fresh;
    CY_REQUIRE(solve_foot_placement(upper.skeleton, legs_setup(), raised, stairs, 1.0F, fresh,
                                    upper.local.span(), &report)
                   .has_value());
    CY_CHECK_LT(std::fabs(report.pelvis + 0.18F), 1e-6F);
    const Vec3 hanging = model(upper.skeleton, kAnkleL, upper.local.span()).translation;
    CY_CHECK_LT(std::fabs((hanging.y + 0.18F) - kAnkleHeight), 1e-3F);
}

CY_TEST_CASE("foot placement: at zero weight the pose is untouched bit for bit") {
    Legs2 legs;
    CY_REQUIRE(legs.build().has_value());
    Array<Transform> before(allocator());
    CY_REQUIRE(before.resize(kLegJoints).has_value());
    legs.skeleton.reference_pose(before.span());
    const StepGround stairs(0.0F, 0.0F, 0.18F);
    FootPlacementState state;
    state.primed = true;
    FootPlacementReport report;
    CY_REQUIRE(solve_foot_placement(legs.skeleton, legs_setup(), FootPlacementContext{}, stairs,
                                    0.0F, state, legs.local.span(), &report)
                   .has_value());
    CY_CHECK(same_bits(legs.local.data(), before.data(), sizeof(Transform) * kLegJoints));
    CY_CHECK_FALSE(report.solved);
    // The follower starts again from the ground when it is faded back in.
    CY_CHECK_FALSE(state.primed);
}

CY_TEST_CASE("foot placement: smoothing follows simulation time, not the number of solves") {
    const StepGround stairs(0.0F, 0.0F, 0.18F);
    FootPlacementSetup setup = legs_setup();
    setup.smoothing = 0.1F;

    // Primed on flat ground, then the step appears under the right foot.
    const auto settle = [&](FootPlacementState& state) {
        Legs2 legs;
        CY_REQUIRE(legs.build().has_value());
        const PlaneGround flat(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
        CY_REQUIRE(solve_foot_placement(legs.skeleton, setup, FootPlacementContext{}, flat, 1.0F,
                                        state, legs.local.span())
                       .has_value());
    };
    FootPlacementState once;
    FootPlacementState twice;
    settle(once);
    settle(twice);

    FootPlacementContext tenth;
    tenth.elapsed = 0.1F;
    FootPlacementContext twentieth;
    twentieth.elapsed = 0.05F;
    {
        Legs2 legs;
        CY_REQUIRE(legs.build().has_value());
        CY_REQUIRE(
            solve_foot_placement(legs.skeleton, setup, tenth, stairs, 1.0F, once, legs.local.span())
                .has_value());
    }
    for (u32 solve = 0; solve < 2; ++solve) {
        Legs2 legs;
        CY_REQUIRE(legs.build().has_value());
        CY_REQUIRE(solve_foot_placement(legs.skeleton, setup, twentieth, stairs, 1.0F, twice,
                                        legs.local.span())
                       .has_value());
    }
    // One time constant: 1 - 1/e of the way to the riser, however it was divided.
    CY_CHECK_LT(std::fabs(once.offset[1] - (0.18F * (1.0F - std::exp(-1.0F)))), 1e-5F);
    CY_CHECK_LT(std::fabs(once.offset[1] - twice.offset[1]), 1e-5F);
}

CY_TEST_CASE("ik: an aim chain points its last joint at the target and fades with its weight") {
    Legs2 legs;
    CY_REQUIRE(legs.build().has_value());
    AimChain chain;
    chain.joints[0] = kSpineJoint;
    chain.joints[1] = kHeadJoint;
    chain.count = 2;
    chain.axis = Vec3{0.0F, 0.0F, 1.0F};
    const Vec3 target{1.5F, 1.9F, 2.0F};
    Array<Transform> before(allocator());
    CY_REQUIRE(before.resize(kLegJoints).has_value());
    legs.skeleton.reference_pose(before.span());

    CY_REQUIRE(solve_aim(legs.skeleton, chain, target, 0.0F, legs.local.span()).has_value());
    CY_CHECK(same_bits(legs.local.data(), before.data(), sizeof(Transform) * kLegJoints));

    CY_REQUIRE(solve_aim(legs.skeleton, chain, target, 1.0F, legs.local.span()).has_value());
    const Transform head = model(legs.skeleton, kHeadJoint, legs.local.span());
    const Vec3 forward = normalize(head.rotation * chain.axis);
    CY_CHECK_GT(dot(forward, normalize(target - head.translation)), std::cos(1.0F * kDegree));
    // The turn is shared: the spine turned as well as the head.
    CY_CHECK_FALSE(legs.local[kSpineJoint].rotation == before[kSpineJoint].rotation);

    // A target behind the character is turned towards by at most `max_angle`.
    legs.skeleton.reference_pose(legs.local.span());
    chain.max_angle = 0.5F;
    CY_REQUIRE(solve_aim(legs.skeleton, chain, Vec3{0.0F, 1.7F, -3.0F}, 1.0F, legs.local.span())
                   .has_value());
    const Vec3 turned =
        normalize(model(legs.skeleton, kHeadJoint, legs.local.span()).rotation * chain.axis);
    CY_CHECK_LT(std::acos(math::clamp(dot(turned, Vec3{0.0F, 0.0F, 1.0F}), -1.0F, 1.0F)),
                0.5F + 1e-3F);
}
