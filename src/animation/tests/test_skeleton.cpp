// The skeleton: parent-before-child order, one-pass model space, bone levels of detail, skinning
// matrices, the humanoid profile and animated bounds. M8.b task 5.1.

#include <cy/animation/skeleton.h>
#include <cy/test/test.h>

#include "fixture.h"

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

CY_TEST_CASE("skeleton: a joint's parent must already be in the array, so evaluation is one pass") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(
        skeleton.add_joint(Name::intern("root"), kInvalidJoint, Transform::identity()).has_value());
    // A forward reference would make model-space resolution a graph walk rather than a loop.
    CY_CHECK_FALSE(skeleton.add_joint(Name::intern("child"), 5, Transform::identity()).has_value());
    CY_CHECK_FALSE(skeleton.add_joint(Name::intern("self"), 1, Transform::identity()).has_value());
    CY_CHECK(skeleton.add_joint(Name::intern("hips"), 0, Transform::identity()).has_value());
}

CY_TEST_CASE("skeleton: local space becomes model space in one forward iteration") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    CY_REQUIRE_EQ(skeleton.joint_count(), static_cast<u16>(kJointCount));

    // The bind pose, resolved by finalize(), is the same answer to_model() gives for it.
    Array<Transform> local(allocator());
    Array<Transform> model(allocator());
    CY_REQUIRE(local.resize(kJointCount).has_value());
    CY_REQUIRE(model.resize(kJointCount).has_value());
    skeleton.reference_pose(local.span());
    skeleton.to_model(local.span(), skeleton.retained(0), model.span());

    CY_CHECK_NEAR(model[kHips].translation.y, 1.0F, 1e-5);
    CY_CHECK_NEAR(model[kChest].translation.y, 1.8F, 1e-5);
    CY_CHECK_NEAR(model[kFoot].translation.y, 0.0F, 1e-5);
    for (u16 joint = 0; joint < kJointCount; ++joint) {
        CY_CHECK(nearly_equal(model[joint], skeleton.bind_model()[joint], 1e-5F));
    }
}

CY_TEST_CASE("skeleton: a bone level of detail drops detail joints first and keeps the rest") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());

    // Level 0 is the whole skeleton; the finger goes at 1 and the head at 2.
    CY_CHECK_EQ(skeleton.retained_count(0), static_cast<u32>(kJointCount));
    CY_CHECK(skeleton.retained(0).test(kFinger));
    CY_CHECK_EQ(skeleton.retained_count(1), static_cast<u32>(kJointCount) - 1U);
    CY_CHECK_FALSE(skeleton.retained(1).test(kFinger));
    CY_CHECK(skeleton.retained(1).test(kHead));
    CY_CHECK_EQ(skeleton.retained_count(2), static_cast<u32>(kJointCount) - 2U);
    CY_CHECK_FALSE(skeleton.retained(2).test(kHead));
    // The legs are never dropped: a walk cycle at any distance still needs them.
    CY_CHECK(skeleton.retained(2).test(kFoot));
}

CY_TEST_CASE("skeleton: a joint may not survive a level its parent is dropped at") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(skeleton.add_joint(Name::intern("root"), kInvalidJoint, Transform::identity(), 1)
                   .has_value());
    // The child claims to survive level 2 while its parent is gone from level 1: the retained sets
    // would not be nested, and the child would be parented to nothing.
    CY_REQUIRE(skeleton.add_joint(Name::intern("child"), 0, Transform::identity(), 3).has_value());
    const Status refused = skeleton.finalize();
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CY_CHECK_FALSE(skeleton.finalized());
}

CY_TEST_CASE(
    "skeleton: the bind pose skins to the identity, which is what makes it the rest pose") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());

    Array<Mat4> matrices(allocator());
    CY_REQUIRE(matrices.resize(kJointCount).has_value());
    skeleton.to_skinning(skeleton.bind_model(), skeleton.retained(0), matrices.span());
    for (u16 joint = 0; joint < kJointCount; ++joint) {
        for (usize column = 0; column < 4; ++column) {
            for (usize row = 0; row < 4; ++row) {
                const f32 expected = column == row ? 1.0F : 0.0F;
                CY_CHECK_NEAR(matrices[joint].at(row, column), expected, 1e-4);
            }
        }
    }
}

CY_TEST_CASE("skeleton: a humanoid profile addresses joints whatever the rig called them") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton, "Bip01_").has_value());

    SkeletonProfile profile;
    CY_CHECK_EQ(profile.resolve(HumanoidJoint::Hips), kInvalidJoint);
    profile.map(HumanoidJoint::Hips, skeleton.find(Name::intern("Bip01_hips")));
    profile.map(HumanoidJoint::Head, skeleton.find(Name::intern("Bip01_head")));
    profile.map(HumanoidJoint::LeftFoot, skeleton.find(Name::intern("Bip01_foot")));

    CY_CHECK_EQ(profile.resolve(HumanoidJoint::Hips), static_cast<u16>(kHips));
    CY_CHECK_EQ(profile.resolve(HumanoidJoint::Head), static_cast<u16>(kHead));
    CY_CHECK_EQ(profile.resolve(HumanoidJoint::Neck), kInvalidJoint);
    CY_CHECK_EQ(profile.mapped_count(), 3U);
    // The skeleton itself carries no humanoid knowledge: it stays lean.
    CY_CHECK_EQ(skeleton.find(Name::intern("hips")), kInvalidJoint);
}

CY_TEST_CASE("skeleton: animated bounds come from the bone transforms, not from the bind pose") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());

    Array<Transform> local(allocator());
    Array<Transform> model(allocator());
    CY_REQUIRE(local.resize(kJointCount).has_value());
    CY_REQUIRE(model.resize(kJointCount).has_value());
    skeleton.reference_pose(local.span());
    skeleton.to_model(local.span(), skeleton.retained(0), model.span());
    const Aabb rest = skeleton.skinned_bounds(model.span());
    CY_REQUIRE_FALSE(rest.is_empty());

    // Raise the whole character by two metres at the root and the box follows it.
    local[kRoot].translation = Vec3{0.0F, 2.0F, 0.0F};
    skeleton.to_model(local.span(), skeleton.retained(0), model.span());
    const Aabb raised = skeleton.skinned_bounds(model.span());
    CY_CHECK_NEAR(raised.min.y - rest.min.y, 2.0F, 1e-4);
    CY_CHECK_NEAR(raised.max.y - rest.max.y, 2.0F, 1e-4);
}
