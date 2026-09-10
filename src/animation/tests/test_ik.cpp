// The constraint framework: declared reads and writes, conflict detection at setup, the two-bone
// solver and its weight, and the solvers this milestone refuses by name. M8.b task 5.3.

#include <cy/animation/ik.h>
#include <cy/test/test.h>

#include "fixture.h"

#include <cmath>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

namespace {

[[nodiscard]] Vec3 model_position(const Skeleton& skeleton, u16 joint,
                                  Span<const Transform> local) noexcept {
    Transform accumulated = Transform::identity();
    u16 cursor = joint;
    while (cursor != kInvalidJoint) {
        accumulated = local[cursor] * accumulated;
        cursor = skeleton.joints()[cursor].parent;
    }
    return accumulated.translation;
}

[[nodiscard]] IkChain arm_chain() noexcept {
    IkChain chain;
    chain.root = kShoulder;
    chain.mid = kUpperArm;
    chain.tip = kLowerArm;
    chain.pole = Vec3{0.0F, 0.0F, 1.0F};
    return chain;
}

}  // namespace

CY_TEST_CASE("ik: two constraints writing one joint at one order are reported at setup") {
    ConstraintDecl aim;
    aim.name = Name::intern("aim");
    aim.writes.set(kChest);
    aim.writes.set(kHead);
    aim.reads.set(kSpine);

    ConstraintDecl look;
    look.name = Name::intern("look_at");
    look.writes.set(kHead);
    look.order = 0;

    const ConstraintDecl both[] = {aim, look};
    Array<ConstraintConflict> conflicts(allocator());
    ConflictReport report;
    CY_REQUIRE(
        detect_conflicts(Span<const ConstraintDecl>(both, 2), conflicts, report).has_value());
    CY_CHECK_EQ(report.examined, 2U);
    CY_REQUIRE_EQ(report.conflicts, 1U);
    CY_CHECK_EQ(conflicts[0].first, Name::intern("aim"));
    CY_CHECK_EQ(conflicts[0].second, Name::intern("look_at"));
    CY_CHECK_EQ(conflicts[0].joint, static_cast<u16>(kHead));

    // Declaring an order resolves it, which is the fix the report is asking for.
    ConstraintDecl ordered = look;
    ordered.order = 1;
    const ConstraintDecl separated[] = {aim, ordered};
    conflicts.clear();
    CY_REQUIRE(
        detect_conflicts(Span<const ConstraintDecl>(separated, 2), conflicts, report).has_value());
    CY_CHECK_EQ(report.conflicts, 0U);
}

CY_TEST_CASE("ik: a two-bone chain reaches a target it can reach, and points at one it cannot") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    Array<Transform> local(allocator());
    CY_REQUIRE(local.resize(kJointCount).has_value());
    skeleton.reference_pose(local.span());

    const Vec3 rest_tip = model_position(skeleton, kLowerArm, local.span());
    const Vec3 shoulder = model_position(skeleton, kShoulder, local.span());
    // Somewhere inside the chain's reach: half a metre from the shoulder, forward and down.
    const Vec3 target = shoulder + Vec3{-0.35F, -0.25F, 0.15F};

    Expected<bool, Error> reached =
        solve_two_bone(skeleton, arm_chain(), target, 1.0F, local.span());
    CY_REQUIRE(reached.has_value());
    CY_CHECK(*reached);
    const Vec3 solved = model_position(skeleton, kLowerArm, local.span());
    CY_CHECK_LT(length(solved - target), length(rest_tip - target));
    CY_CHECK_LT(length(solved - target), 0.05F);

    // A target beyond the chain's length straightens it toward the target rather than failing.
    skeleton.reference_pose(local.span());
    const Vec3 far_target = shoulder + Vec3{-5.0F, 0.0F, 0.0F};
    Expected<bool, Error> unreachable =
        solve_two_bone(skeleton, arm_chain(), far_target, 1.0F, local.span());
    CY_REQUIRE(unreachable.has_value());
    CY_CHECK_FALSE(*unreachable);
}

CY_TEST_CASE("ik: a constraint at zero weight changes nothing, and fades in between") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    Array<Transform> local(allocator());
    Array<Transform> untouched(allocator());
    CY_REQUIRE(local.resize(kJointCount).has_value());
    CY_REQUIRE(untouched.resize(kJointCount).has_value());
    skeleton.reference_pose(local.span());
    skeleton.reference_pose(untouched.span());

    const Vec3 shoulder = model_position(skeleton, kShoulder, local.span());
    const Vec3 target = shoulder + Vec3{-0.35F, -0.25F, 0.15F};

    CY_REQUIRE(solve_two_bone(skeleton, arm_chain(), target, 0.0F, local.span()).has_value());
    for (u16 joint = 0; joint < kJointCount; ++joint) {
        CY_CHECK(nearly_equal(local[joint], untouched[joint], 1e-6F));
    }

    // Half weight lands between the rest pose and the full solve rather than at either.
    CY_REQUIRE(solve_two_bone(skeleton, arm_chain(), target, 0.5F, local.span()).has_value());
    const f32 half = length(model_position(skeleton, kLowerArm, local.span()) - target);
    skeleton.reference_pose(local.span());
    const f32 none = length(model_position(skeleton, kLowerArm, local.span()) - target);
    CY_REQUIRE(solve_two_bone(skeleton, arm_chain(), target, 1.0F, local.span()).has_value());
    const f32 full = length(model_position(skeleton, kLowerArm, local.span()) - target);
    CY_CHECK_LT(half, none);
    CY_CHECK_LT(full, half);
}

CY_TEST_CASE("ik: a chain that is not a chain is refused rather than solved into nonsense") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    Array<Transform> local(allocator());
    CY_REQUIRE(local.resize(kJointCount).has_value());
    skeleton.reference_pose(local.span());

    IkChain broken = arm_chain();
    broken.mid = kFoot;  // not the root's child
    CY_CHECK_FALSE(solve_two_bone(skeleton, broken, Vec3{}, 1.0F, local.span()).has_value());

    IkChain absent = arm_chain();
    absent.tip = 200;
    CY_CHECK_FALSE(solve_two_bone(skeleton, absent, Vec3{}, 1.0F, local.span()).has_value());

    IkChain empty;
    CY_CHECK_FALSE(solve_two_bone(skeleton, empty, Vec3{}, 1.0F, local.span()).has_value());
}

CY_TEST_CASE("ik: look-at points a joint at a target and fades with its weight") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    Array<Transform> local(allocator());
    CY_REQUIRE(local.resize(kJointCount).has_value());
    skeleton.reference_pose(local.span());

    const Quat rest = local[kHead].rotation;
    CY_REQUIRE(
        solve_look_at(skeleton, kHead, Vec3{5.0F, 1.8F, 0.0F}, 0.0F, local.span()).has_value());
    CY_CHECK(same_rotation(local[kHead].rotation, rest, 1e-6F));

    CY_REQUIRE(
        solve_look_at(skeleton, kHead, Vec3{5.0F, 1.8F, 0.0F}, 1.0F, local.span()).has_value());
    CY_CHECK_FALSE(same_rotation(local[kHead].rotation, rest, 1e-3F));
}

CY_TEST_CASE("ik: the solvers this milestone does not have are refused by name") {
    // "full-body IK SHALL solve multiple effectors ... as one pose problem" — a chain solver is not
    // that, and answering a caller with one would be worse than answering with nothing.
    const Status full_body = solve_unimplemented(UnimplementedSolver::FullBodyIk);
    CY_REQUIRE_FALSE(full_body.has_value());
    CY_CHECK(full_body.error().code == ErrorCode::NotImplemented);
    CY_CHECK_FALSE(solve_unimplemented(UnimplementedSolver::Fabrik).has_value());
    CY_CHECK_FALSE(solve_unimplemented(UnimplementedSolver::Ccd).has_value());
    CY_CHECK_FALSE(solve_unimplemented(UnimplementedSolver::SplineIk).has_value());
    CY_CHECK_FALSE(solve_unimplemented(UnimplementedSolver::SpringBones).has_value());
    CY_CHECK_FALSE(solve_unimplemented(UnimplementedSolver::FootPlacement).has_value());
}
