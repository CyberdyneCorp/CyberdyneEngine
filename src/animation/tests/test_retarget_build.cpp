// Authoring a retarget profile's joint correspondence: the two ways two exports of one character
// can correspond, and the refusal that stands between a caller and a profile that maps nothing.
// M8.d.
//
// The case these are written for is four Mixamo exports of one character. Three of them carry an
// animation and another copy of the rig; one carries the mesh and the skin. What every case here
// asserts about is the bridge from "a clip authored against THAT export's indices" to "a pose on
// THIS character's skeleton".

#include <cy/animation/retarget_build.h>
#include <cy/core/memory/array.h>
#include <cy/test/test.h>

#include "fixture.h"
#include "rig_exports.h"

#include <cmath>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;
using namespace cy::animation::testing::rigs;

namespace {

/// The export that carries the mesh and the skin — the character every clip has to end up on.
ExportShape character_shape() noexcept {
    return ExportShape{};
}

/// The same rig, re-exported: the exporter stamped a different namespace on every joint and changed
/// nothing else. This is what `Breathing Idle.fbx` is beside `Walking.fbx`.
ExportShape re_export_shape() noexcept {
    ExportShape shape;
    shape.prefix = "mixamorig:";
    return shape;
}

/// An export of the same body plan that writes the legs before the spine, so every joint below the
/// hips has a different index.
ExportShape reordered_shape() noexcept {
    ExportShape shape;
    shape.order = kLegsFirstOrder;
    return shape;
}

/// The same body plan at a third again the height: a taller character, joint for joint.
ExportShape tall_shape() noexcept {
    ExportShape shape;
    shape.scale = 1.3F;
    return shape;
}

/// Another studio's character: its own joint vocabulary, a third again the height, and a root joint
/// above the hips that this character's rig does not have.
ExportShape foreign_shape() noexcept {
    ExportShape shape;
    shape.names = kStudioNames;
    shape.prefix = "";
    shape.scale = 1.3F;
    shape.armature_root = true;
    return shape;
}

[[nodiscard]] Status pose_buffers(const Skeleton& skeleton, Array<Transform>& out) noexcept {
    if (Status sized = out.resize(skeleton.joint_count()); !sized) {
        return sized;
    }
    skeleton.reference_pose(out.span());
    return ok();
}

/// The largest angle by which any joint's local rotation differs from the skeleton's rest pose.
[[nodiscard]] f32 worst_departure_from_rest(const Skeleton& skeleton,
                                            Span<const Transform> local) noexcept {
    f32 worst = 0.0F;
    for (u16 joint = 0; joint < skeleton.joint_count(); ++joint) {
        worst = math::max(
            worst, math::degrees(angle_between(local[joint].rotation,
                                               skeleton.joints()[joint].bind_local.rotation)));
    }
    return worst;
}

}  // namespace

// --- The chain table -----------------------------------------------------------------------------

CY_TEST_CASE("retarget build: every standard joint is filed under the chain of its body part") {
    // The table is what decides where translation crosses, so a joint that landed in the root chain
    // by accident would move the character rather than pose it.
    CY_CHECK_EQ(chain_of(HumanoidJoint::Root), Chain::Root);
    CY_CHECK_EQ(chain_of(HumanoidJoint::Hips), Chain::Root);
    CY_CHECK_EQ(chain_of(HumanoidJoint::Spine), Chain::Spine);
    CY_CHECK_EQ(chain_of(HumanoidJoint::Chest), Chain::Spine);
    CY_CHECK_EQ(chain_of(HumanoidJoint::Neck), Chain::Neck);
    CY_CHECK_EQ(chain_of(HumanoidJoint::Head), Chain::Head);
    CY_CHECK_EQ(chain_of(HumanoidJoint::LeftShoulder), Chain::LeftArm);
    CY_CHECK_EQ(chain_of(HumanoidJoint::LeftHand), Chain::LeftArm);
    CY_CHECK_EQ(chain_of(HumanoidJoint::RightHand), Chain::RightArm);
    CY_CHECK_EQ(chain_of(HumanoidJoint::LeftToes), Chain::LeftLeg);
    CY_CHECK_EQ(chain_of(HumanoidJoint::RightUpperLeg), Chain::RightLeg);

    // Left and right never share a chain, which is the mapping error a mirrored rig invites.
    for (u8 standard = 0; standard < static_cast<u8>(HumanoidJoint::Count); ++standard) {
        const Chain chain = chain_of(static_cast<HumanoidJoint>(standard));
        CY_CHECK(static_cast<u8>(chain) < static_cast<u8>(Chain::Count));
    }
}

// --- Two exports of one rig ----------------------------------------------------------------------

CY_TEST_CASE("retarget build: two exports of one rig match although no joint name is shared") {
    ExportedRig character(allocator());
    ExportedRig re_exported(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(re_exported, re_export_shape()).has_value());

    // The namespace is the half an exporter rewrites, so name equality is the one signal that fails
    // on two exports of ONE rig. Every joint of the character is missing from the re-export by
    // name.
    for (u16 joint = 0; joint < character.skeleton.joint_count(); ++joint) {
        CY_CHECK_EQ(re_exported.skeleton.find(character.skeleton.joints()[joint].name),
                    kInvalidJoint);
    }

    const RigMatch match = compare_rigs(character.skeleton, re_exported.skeleton);
    CY_CHECK(match.congruent);
    CY_CHECK(match.same_rest_pose);
    CY_CHECK_EQ(match.first_difference, kInvalidJoint);
    CY_CHECK_LT(match.worst_rest_translation, 1e-6F);
}

CY_TEST_CASE("retarget build: the correspondence over two exports of one rig is joint for joint") {
    ExportedRig character(allocator());
    ExportedRig re_exported(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(re_exported, re_export_shape()).has_value());

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    CY_REQUIRE(build_retarget_profile(re_exported.skeleton, re_exported.humanoid,
                                      character.skeleton, character.humanoid, profile, report)
                   .has_value());

    CY_CHECK_EQ(report.correspondence, Correspondence::Congruent);
    CY_CHECK_EQ(report.pairs, static_cast<u32>(kRoleCount));
    // Nothing is left at rest: the fingers and the `_End` terminators cross too, and those are the
    // joints a twenty-two slot humanoid mapping cannot carry.
    CY_CHECK_EQ(report.target_joints_at_rest, 0U);
    CY_CHECK_NEAR(report.retarget.height_scale, 1.0F, 1e-4);
}

CY_TEST_CASE("retarget build: a pose crosses two exports of one rig unchanged") {
    ExportedRig character(allocator());
    ExportedRig source(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(source, re_export_shape()).has_value());

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    CY_REQUIRE(build_retarget_profile(source.skeleton, source.humanoid, character.skeleton,
                                      character.humanoid, profile, report)
                   .has_value());

    Array<Transform> posed(allocator());
    Array<Transform> retargeted(allocator());
    CY_REQUIRE(pose_buffers(source.skeleton, posed).has_value());
    CY_REQUIRE(pose_buffers(character.skeleton, retargeted).has_value());

    posed[source.joint(RigJoint::LeftUpperLeg)].rotation =
        Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, 0.6F);
    posed[source.joint(RigJoint::RightLowerArm)].rotation =
        Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, -0.4F);
    posed[source.joint(RigJoint::LeftFinger1)].rotation =
        Quat::from_axis_angle(Vec3{0.0F, 0.0F, 1.0F}, 0.9F);
    posed[source.joint(RigJoint::Hips)].translation.z -= 0.75F;

    CY_REQUIRE(
        profile.retarget_pose(source.skeleton, character.skeleton, posed.span(), retargeted.span())
            .has_value());

    // Same rig, same rest: every joint lands where the source put it, finger included.
    for (usize role = 0; role < kRoleCount; ++role) {
        const auto part = static_cast<RigJoint>(role);
        const Transform from = model_of(source.skeleton, posed.span(), source.joint(part));
        const Transform onto =
            model_of(character.skeleton, retargeted.span(), character.joint(part));
        CY_CHECK_NEAR(onto.translation.x, from.translation.x, 1e-3);
        CY_CHECK_NEAR(onto.translation.y, from.translation.y, 1e-3);
        CY_CHECK_NEAR(onto.translation.z, from.translation.z, 1e-3);
        CY_CHECK_LT(math::degrees(angle_between(onto.rotation, from.rotation)), 0.1F);
    }

    // And it is emphatically not the bind pose, which is what a correspondence that mapped nothing
    // would have produced without failing.
    CY_CHECK_GT(worst_departure_from_rest(character.skeleton, retargeted.span()), 20.0F);
}

// --- Exports that are not the same rig -----------------------------------------------------------

CY_TEST_CASE("retarget build: an export that orders its joints differently maps by chain") {
    ExportedRig character(allocator());
    ExportedRig reordered(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(reordered, reordered_shape()).has_value());

    // Same body, same names, different indices — the hazard, because `AnimationRig::bind` compares
    // joint COUNTS and these two agree on that.
    CY_CHECK_EQ(character.skeleton.joint_count(), reordered.skeleton.joint_count());
    CY_CHECK_NE(character.joint(RigJoint::LeftUpperLeg), reordered.joint(RigJoint::LeftUpperLeg));

    const RigMatch match = compare_rigs(character.skeleton, reordered.skeleton);
    CY_CHECK_FALSE(match.congruent);
    CY_CHECK_NE(match.first_difference, kInvalidJoint);

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    CY_REQUIRE(build_retarget_profile(reordered.skeleton, reordered.humanoid, character.skeleton,
                                      character.humanoid, profile, report)
                   .has_value());
    CY_CHECK_EQ(report.correspondence, Correspondence::Humanoid);
    // Twenty-two standard slots, of which `Root` and `Hips` name one joint on a rig whose hips are
    // its root, so twenty-one distinct pairs.
    CY_CHECK_EQ(report.pairs, 21U);
    CY_CHECK_EQ(report.unpaired_standard, 0U);
    CY_CHECK_EQ(report.target_joints_at_rest, static_cast<u32>(kRoleCount) - 21U);
}

CY_TEST_CASE("retarget build: a reordered export's left leg drives the left leg") {
    ExportedRig character(allocator());
    ExportedRig reordered(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(reordered, reordered_shape()).has_value());

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    CY_REQUIRE(build_retarget_profile(reordered.skeleton, reordered.humanoid, character.skeleton,
                                      character.humanoid, profile, report)
                   .has_value());

    Array<Transform> posed(allocator());
    Array<Transform> retargeted(allocator());
    CY_REQUIRE(pose_buffers(reordered.skeleton, posed).has_value());
    CY_REQUIRE(pose_buffers(character.skeleton, retargeted).has_value());
    posed[reordered.joint(RigJoint::LeftUpperLeg)].rotation =
        Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, 0.8F);
    CY_REQUIRE(
        profile
            .retarget_pose(reordered.skeleton, character.skeleton, posed.span(), retargeted.span())
            .has_value());

    // The left foot swings and the right foot does not. Reading the clip at face value would have
    // moved whichever joint happens to sit at the source's left-leg index on this skeleton, which
    // here is a joint of the spine.
    const f32 left =
        model_of(character.skeleton, retargeted.span(), character.joint(RigJoint::LeftFoot))
            .translation.z;
    const f32 right =
        model_of(character.skeleton, retargeted.span(), character.joint(RigJoint::RightFoot))
            .translation.z;
    CY_CHECK_GT(std::fabs(left), 0.2F);
    CY_CHECK_LT(std::fabs(right), 1e-3F);
}

CY_TEST_CASE("retarget build: a taller character of the same shape maps joint for joint") {
    ExportedRig character(allocator());
    ExportedRig tall(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(tall, tall_shape()).has_value());

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    CY_REQUIRE(build_retarget_profile(tall.skeleton, tall.humanoid, character.skeleton,
                                      character.humanoid, profile, report)
                   .has_value());
    // Same hierarchy, different proportions: the joints still correspond one for one, and it is the
    // rest-pose reconciliation and the height scale — not a smaller mapping — that absorb the
    // difference in size.
    CY_CHECK_EQ(report.correspondence, Correspondence::Congruent);
    // The hips of a character a third again as tall come down by that third.
    CY_CHECK_NEAR(report.retarget.height_scale, 1.0F / 1.3F, 1e-3);

    Array<Transform> posed(allocator());
    Array<Transform> retargeted(allocator());
    CY_REQUIRE(pose_buffers(tall.skeleton, posed).has_value());
    CY_REQUIRE(pose_buffers(character.skeleton, retargeted).has_value());
    // A stride of 1.3 m for the tall character is 1.0 m for this one: the same fraction of its own
    // gait, which is what keeps a foot on the ground rather than sliding.
    posed[tall.joint(RigJoint::Hips)].translation.z -= 1.3F;
    CY_REQUIRE(
        profile.retarget_pose(tall.skeleton, character.skeleton, posed.span(), retargeted.span())
            .has_value());
    const Vec3 hips = retargeted[character.joint(RigJoint::Hips)].translation;
    CY_CHECK_NEAR(hips.z, -1.0F, 1e-2);
    CY_CHECK_NEAR(hips.y, 0.99F, 1e-2);
}

CY_TEST_CASE("retarget build: a source with a root joint above its hips still drives the hips") {
    // THE MAPPING ERROR THIS CASE EXISTS FOR. Most rigs carry a deform-nothing joint above the hips
    // and a Mixamo rig does not, so one profile resolves `Root` and `Hips` to two joints and the
    // other resolves both to one. Pair the slots in enum order and `Root` claims the character's
    // hips first — the hips then take an armature's transform, which never moves, and the character
    // walks on the spot while every other joint animates. Pairing the hips first is what stops it.
    ExportedRig character(allocator());
    ExportedRig foreign(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(foreign, foreign_shape()).has_value());
    CY_REQUIRE_NE(foreign.armature, kInvalidJoint);
    CY_CHECK_NE(foreign.humanoid.resolve(HumanoidJoint::Root),
                foreign.humanoid.resolve(HumanoidJoint::Hips));
    CY_CHECK_EQ(character.humanoid.resolve(HumanoidJoint::Root),
                character.humanoid.resolve(HumanoidJoint::Hips));

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    CY_REQUIRE(build_retarget_profile(foreign.skeleton, foreign.humanoid, character.skeleton,
                                      character.humanoid, profile, report)
                   .has_value());
    CY_CHECK_EQ(report.correspondence, Correspondence::Humanoid);
    // Twenty-two slots, and the `Root` pair is the one that finds the character's hips already
    // spoken for.
    CY_CHECK_EQ(report.pairs, 21U);
    CY_CHECK_EQ(profile.source_of(character.joint(RigJoint::Hips)), foreign.joint(RigJoint::Hips));
    // The height scale still comes out, because the hips pair is the root chain's.
    CY_CHECK_NEAR(report.retarget.height_scale, 1.0F / 1.3F, 1e-3);

    Array<Transform> posed(allocator());
    Array<Transform> retargeted(allocator());
    CY_REQUIRE(pose_buffers(foreign.skeleton, posed).has_value());
    CY_REQUIRE(pose_buffers(character.skeleton, retargeted).has_value());
    posed[foreign.joint(RigJoint::Hips)].translation.z -= 1.3F;
    CY_REQUIRE(
        profile.retarget_pose(foreign.skeleton, character.skeleton, posed.span(), retargeted.span())
            .has_value());
    const Vec3 hips = retargeted[character.joint(RigJoint::Hips)].translation;
    CY_CHECK_NEAR(hips.z, -1.0F, 1e-2);
    CY_CHECK_NEAR(hips.y, 0.99F, 1e-2);
}

// --- The refusal ---------------------------------------------------------------------------------

CY_TEST_CASE("retarget build: a correspondence that would map nothing is refused, not built") {
    // Two skeletons with no humanoid profile between them and no shape in common. A profile built
    // from no pairs retargets every frame of every clip to the target's bind pose — a character
    // standing still while the animation plays, and no error anywhere. That is the failure this
    // refusal exists for.
    Skeleton plain(allocator());
    CY_REQUIRE(build_biped(plain).has_value());
    ExportedRig character(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());

    const SkeletonProfile unmapped;
    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    const Status built = build_retarget_profile(plain, unmapped, character.skeleton,
                                                character.humanoid, profile, report);
    CY_CHECK_FALSE(built.has_value());
    CY_CHECK_EQ(report.pairs, 0U);
    CY_CHECK_FALSE(profile.built());
}

CY_TEST_CASE("retarget build: an unfinalized skeleton is refused before anything is mapped") {
    ExportedRig character(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    Skeleton half_built(allocator());
    CY_REQUIRE(half_built.add_joint(Name::intern("hips"), kInvalidJoint, Transform::identity())
                   .has_value());

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    CY_CHECK_FALSE(build_retarget_profile(half_built, character.humanoid, character.skeleton,
                                          character.humanoid, profile, report)
                       .has_value());
    CY_CHECK_EQ(report.pairs, 0U);
}
