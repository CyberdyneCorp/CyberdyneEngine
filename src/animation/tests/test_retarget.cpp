// Retargeting by semantic chains: two skeletons that share no joint name and half the height, the
// contact tolerance, and the offline bake. M8.b task 5.3.

#include <cy/animation/retarget.h>
#include <cy/test/test.h>

#include "fixture.h"
#include "retarget_chains.h"

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

CY_TEST_CASE("retarget: chains map two skeletons that share no joint name at all") {
    Skeleton source(allocator());
    Skeleton target(allocator());
    CY_REQUIRE(build_biped(source, "Bip01_").has_value());
    CY_REQUIRE(build_biped(target, "mixamo:", 0.5F).has_value());

    // Not one name is shared, which is the case name matching would fail on.
    CY_CHECK_EQ(target.find(Name::intern("Bip01_hips")), kInvalidJoint);
    CY_CHECK_EQ(source.find(Name::intern("mixamo:hips")), kInvalidJoint);

    RetargetProfile profile(allocator());
    CY_REQUIRE(map_biped(profile).has_value());
    RetargetReport report;
    CY_REQUIRE(profile.build(source, target, report).has_value());
    CY_CHECK_EQ(report.chains_mapped, 5U);
    CY_CHECK_EQ(report.joints_mapped, 11U);
    CY_CHECK_NEAR(report.height_scale, 0.5F, 1e-4);
}

CY_TEST_CASE("retarget: a foot on the ground stays on the ground at half the height") {
    Skeleton source(allocator());
    Skeleton target(allocator());
    CY_REQUIRE(build_biped(source, "a_").has_value());
    CY_REQUIRE(build_biped(target, "b_", 0.5F).has_value());
    RetargetProfile profile(allocator());
    CY_REQUIRE(map_biped(profile).has_value());
    RetargetReport report;
    CY_REQUIRE(profile.build(source, target, report).has_value());

    Array<Transform> source_local(allocator());
    Array<Transform> target_local(allocator());
    CY_REQUIRE(source_local.resize(kJointCount).has_value());
    CY_REQUIRE(target_local.resize(kJointCount).has_value());

    // The rest pose has both characters' feet at y = 0.
    source.reference_pose(source_local.span());
    target.reference_pose(target_local.span());
    CY_REQUIRE(profile.retarget_pose(source, target, source_local.span(), target_local.span())
                   .has_value());
    CY_CHECK_NEAR(model_height(target, target_local.span(), kFoot), 0.0F, 1e-4);

    // Walking forward two metres moves a half-height character one, so the same number of strides
    // covers the same fraction of its own gait.
    source_local[kRoot].translation = Vec3{0.0F, 0.0F, -2.0F};
    target.reference_pose(target_local.span());
    CY_REQUIRE(profile.retarget_pose(source, target, source_local.span(), target_local.span())
                   .has_value());
    CY_CHECK_NEAR(target_local[kRoot].translation.z, -1.0F, 1e-4);
    CY_CHECK_NEAR(model_height(target, target_local.span(), kFoot), 0.0F, 1e-4);

    // A rotated leg transfers as a rotation, so the target's foot rises by ITS proportion of the
    // source's — which is what "preserve foot contact within tolerance" means for a shorter rig.
    source_local[kRoot].translation = Vec3{0.0F, 0.0F, 0.0F};
    source_local[kUpperLeg].rotation = Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, 0.6F);
    target.reference_pose(target_local.span());
    CY_REQUIRE(profile.retarget_pose(source, target, source_local.span(), target_local.span())
                   .has_value());
    const f32 lifted_source = model_height(source, source_local.span(), kFoot);
    const f32 lifted_target = model_height(target, target_local.span(), kFoot);
    CY_CHECK_GT(lifted_source, 0.05F);
    CY_CHECK_NEAR(lifted_target, lifted_source * 0.5F, 1e-3);
}
