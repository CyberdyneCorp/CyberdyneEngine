#pragma once
// The four separately exported rigs the retarget cases are written against, and the clips authored
// against each one's own joint indices. M8.d.
//
// WHY A SECOND FIXTURE BESIDE `fixture.h`. The twelve-joint biped there is the algebra's fixture:
// one arm, one leg, no left and right, and one skeleton reused as both sides of a retarget. The
// subject here is four SEPARATE EXPORTS of one character — the shape four Mixamo files have, where
// the file with the mesh carries a rig and each animation file carries another copy of it — so what
// has to be modelled is the ways two exports of one character differ: the namespace an exporter
// rewrites between them, the order it writes siblings in, and the proportions a different character
// was authored at. A rig with both arms and both legs is the minimum that can catch a mapping which
// puts the left leg's animation on the right leg, which is the failure a nearly-right
// correspondence produces.
//
// NOTHING HERE IS A MIXAMO FILE. The four sample exports live outside the repository — one of them
// is 16.7 MB — so these rigs are built in code with the shape and the joint vocabulary those files
// have: a rig whose root IS the hips, `Spine1` standing in for the chest, `_End` terminators, two
// finger joints per hand that no humanoid profile names, and a metre-scaled bind pose.

#include <cy/animation/clip.h>
#include <cy/animation/skeleton.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/values/name.h>

#include "fixture.h"

#include <cmath>
#include <string>
#include <string_view>

namespace cy::animation::testing::rigs {

/// The joints of the fixture rig, by role, so a case names a body part rather than an index — and
/// because the INDEX is exactly what differs between two exports, which is the point of the whole
/// exercise.
enum class RigJoint : u16 {
    Hips = 0,
    Spine,
    Chest,
    Neck,
    Head,
    HeadTop,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    LeftFinger1,
    LeftFinger2,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    RightFinger1,
    RightFinger2,
    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToeBase,
    LeftToeEnd,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToeBase,
    RightToeEnd,
    Count,
};

/// The row count as a size, for the tables below and for a loop over every role. `RigJoint` is a
/// scoped enum so that its `Hips` and `Spine` cannot collide with `fixture.h`'s twelve-joint biped,
/// which a case here builds beside these rigs.
inline constexpr usize kRoleCount = static_cast<usize>(RigJoint::Count);

/// Mixamo's own spelling, namespace excluded — the vocabulary all four sample files use.
inline const char* const kMixamoNames[kRoleCount] = {
    "Hips",           "Spine",           "Spine1",          "Neck",        "Head",
    "HeadTop_End",    "LeftShoulder",    "LeftArm",         "LeftForeArm", "LeftHand",
    "LeftHandIndex1", "LeftHandIndex2",  "RightShoulder",   "RightArm",    "RightForeArm",
    "RightHand",      "RightHandIndex1", "RightHandIndex2", "LeftUpLeg",   "LeftLeg",
    "LeftFoot",       "LeftToeBase",     "LeftToe_End",     "RightUpLeg",  "RightLeg",
    "RightFoot",      "RightToeBase",    "RightToe_End",
};

/// A different studio's vocabulary, sharing not one word with the above. The export that uses it is
/// the "entirely different naming conventions" scenario, and a correspondence derived from names
/// would find nothing between the two.
inline const char* const kStudioNames[kRoleCount] = {
    "pelvis",   "back_low",   "back_up", "collar",  "skull",   "skull_tip", "clav_l",
    "arm_up_l", "arm_lo_l",   "palm_l",  "dgt_l_a", "dgt_l_b", "clav_r",    "arm_up_r",
    "arm_lo_r", "palm_r",     "dgt_r_a", "dgt_r_b", "thigh_l", "shin_l",    "sole_l",
    "ball_l",   "ball_l_tip", "thigh_r", "shin_r",  "sole_r",  "ball_r",    "ball_r_tip",
};

/// One row of the rig: where a joint sits in its parent, which level of detail drops it, and which
/// standard humanoid joint it is.
///
/// The profile is authored here rather than derived from the name, because that derivation is the
/// IMPORTER's job (`cy/import/fbx_skeleton.h` does it and has its own suite) and a retarget case
/// that re-derived it would be testing two things at once.
struct RigRow {
    RigJoint role = RigJoint::Hips;
    RigJoint parent = RigJoint::Hips;
    bool is_root = false;
    Vec3 translation{0.0F, 0.0F, 0.0F};
    u8 dropped_at = kBoneLodLevels;
    /// `HumanoidJoint::Count` for a joint the standard vocabulary has no slot for — the finger
    /// joints and the `_End` terminators, which is what makes the difference between a
    /// joint-for-joint correspondence and a twenty-two joint one observable.
    HumanoidJoint standard = HumanoidJoint::Count;
};

/// The rig, in metres, in the proportions of a human of average height. The hips are the root, as
/// they are on a Mixamo rig, so the `Root` and `Hips` slots of the profile name one joint.
inline const RigRow kRigRows[kRoleCount] = {
    {RigJoint::Hips,
     RigJoint::Hips,
     true,
     {0.0F, 0.99F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::Hips},
    {RigJoint::Spine,
     RigJoint::Hips,
     false,
     {0.0F, 0.10F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::Spine},
    {RigJoint::Chest,
     RigJoint::Spine,
     false,
     {0.0F, 0.13F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::Chest},
    {RigJoint::Neck,
     RigJoint::Chest,
     false,
     {0.0F, 0.25F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::Neck},
    {RigJoint::Head,
     RigJoint::Neck,
     false,
     {0.0F, 0.10F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::Head},
    {RigJoint::HeadTop, RigJoint::Head, false, {0.0F, 0.18F, 0.0F}, 1, HumanoidJoint::Count},
    {RigJoint::LeftShoulder,
     RigJoint::Chest,
     false,
     {0.05F, 0.20F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::LeftShoulder},
    {RigJoint::LeftUpperArm,
     RigJoint::LeftShoulder,
     false,
     {0.13F, 0.0F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::LeftUpperArm},
    {RigJoint::LeftLowerArm,
     RigJoint::LeftUpperArm,
     false,
     {0.28F, 0.0F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::LeftLowerArm},
    {RigJoint::LeftHand,
     RigJoint::LeftLowerArm,
     false,
     {0.26F, 0.0F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::LeftHand},
    {RigJoint::LeftFinger1,
     RigJoint::LeftHand,
     false,
     {0.08F, 0.0F, 0.02F},
     1,
     HumanoidJoint::Count},
    {RigJoint::LeftFinger2,
     RigJoint::LeftFinger1,
     false,
     {0.04F, 0.0F, 0.0F},
     1,
     HumanoidJoint::Count},
    {RigJoint::RightShoulder,
     RigJoint::Chest,
     false,
     {-0.05F, 0.20F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::RightShoulder},
    {RigJoint::RightUpperArm,
     RigJoint::RightShoulder,
     false,
     {-0.13F, 0.0F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::RightUpperArm},
    {RigJoint::RightLowerArm,
     RigJoint::RightUpperArm,
     false,
     {-0.28F, 0.0F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::RightLowerArm},
    {RigJoint::RightHand,
     RigJoint::RightLowerArm,
     false,
     {-0.26F, 0.0F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::RightHand},
    {RigJoint::RightFinger1,
     RigJoint::RightHand,
     false,
     {-0.08F, 0.0F, 0.02F},
     1,
     HumanoidJoint::Count},
    {RigJoint::RightFinger2,
     RigJoint::RightFinger1,
     false,
     {-0.04F, 0.0F, 0.0F},
     1,
     HumanoidJoint::Count},
    {RigJoint::LeftUpperLeg,
     RigJoint::Hips,
     false,
     {0.09F, -0.06F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::LeftUpperLeg},
    {RigJoint::LeftLowerLeg,
     RigJoint::LeftUpperLeg,
     false,
     {0.0F, -0.42F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::LeftLowerLeg},
    {RigJoint::LeftFoot,
     RigJoint::LeftLowerLeg,
     false,
     {0.0F, -0.41F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::LeftFoot},
    {RigJoint::LeftToeBase,
     RigJoint::LeftFoot,
     false,
     {0.0F, -0.08F, 0.13F},
     kBoneLodLevels,
     HumanoidJoint::LeftToes},
    {RigJoint::LeftToeEnd,
     RigJoint::LeftToeBase,
     false,
     {0.0F, 0.0F, 0.07F},
     1,
     HumanoidJoint::Count},
    {RigJoint::RightUpperLeg,
     RigJoint::Hips,
     false,
     {-0.09F, -0.06F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::RightUpperLeg},
    {RigJoint::RightLowerLeg,
     RigJoint::RightUpperLeg,
     false,
     {0.0F, -0.42F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::RightLowerLeg},
    {RigJoint::RightFoot,
     RigJoint::RightLowerLeg,
     false,
     {0.0F, -0.41F, 0.0F},
     kBoneLodLevels,
     HumanoidJoint::RightFoot},
    {RigJoint::RightToeBase,
     RigJoint::RightFoot,
     false,
     {0.0F, -0.08F, 0.13F},
     kBoneLodLevels,
     HumanoidJoint::RightToes},
    {RigJoint::RightToeEnd,
     RigJoint::RightToeBase,
     false,
     {0.0F, 0.0F, 0.07F},
     1,
     HumanoidJoint::Count},
};

/// The order an exporter writes the rows in. Both are depth-first and both are parent-before-child;
/// they differ in which of the hips' three children is written first, which is enough to give every
/// joint below the hips a different index.
inline const RigJoint kSpineFirstOrder[kRoleCount] = {
    RigJoint::Hips,          RigJoint::Spine,         RigJoint::Chest,
    RigJoint::Neck,          RigJoint::Head,          RigJoint::HeadTop,
    RigJoint::LeftShoulder,  RigJoint::LeftUpperArm,  RigJoint::LeftLowerArm,
    RigJoint::LeftHand,      RigJoint::LeftFinger1,   RigJoint::LeftFinger2,
    RigJoint::RightShoulder, RigJoint::RightUpperArm, RigJoint::RightLowerArm,
    RigJoint::RightHand,     RigJoint::RightFinger1,  RigJoint::RightFinger2,
    RigJoint::LeftUpperLeg,  RigJoint::LeftLowerLeg,  RigJoint::LeftFoot,
    RigJoint::LeftToeBase,   RigJoint::LeftToeEnd,    RigJoint::RightUpperLeg,
    RigJoint::RightLowerLeg, RigJoint::RightFoot,     RigJoint::RightToeBase,
    RigJoint::RightToeEnd,
};

inline const RigJoint kLegsFirstOrder[kRoleCount] = {
    RigJoint::Hips,          RigJoint::LeftUpperLeg,  RigJoint::LeftLowerLeg,
    RigJoint::LeftFoot,      RigJoint::LeftToeBase,   RigJoint::LeftToeEnd,
    RigJoint::RightUpperLeg, RigJoint::RightLowerLeg, RigJoint::RightFoot,
    RigJoint::RightToeBase,  RigJoint::RightToeEnd,   RigJoint::Spine,
    RigJoint::Chest,         RigJoint::Neck,          RigJoint::Head,
    RigJoint::HeadTop,       RigJoint::LeftShoulder,  RigJoint::LeftUpperArm,
    RigJoint::LeftLowerArm,  RigJoint::LeftHand,      RigJoint::LeftFinger1,
    RigJoint::LeftFinger2,   RigJoint::RightShoulder, RigJoint::RightUpperArm,
    RigJoint::RightLowerArm, RigJoint::RightHand,     RigJoint::RightFinger1,
    RigJoint::RightFinger2,
};

/// How one export of the rig differs from another.
struct ExportShape {
    /// The joint vocabulary, and the namespace an exporter stamps on it. A re-export of one rig
    /// comes back with the namespace rewritten — `mixamorig:` becomes `mixamorig1:` — which is why
    /// nothing in the retarget path may compare a name.
    const char* const* names = kMixamoNames;
    const char* prefix = "mixamorig1:";
    /// Multiplies every bind translation: a character of different proportions.
    f32 scale = 1.0F;
    const RigJoint* order = kSpineFirstOrder;
    /// A joint above the hips that deforms nothing, which most rigs outside Mixamo have and a
    /// Mixamo rig does not. It takes the profile's `Root` slot, so the two rigs disagree about
    /// whether `Root` and `Hips` are one joint — the disagreement that decides which source joint
    /// the target's HIPS take their motion from.
    bool armature_root = false;
};

/// One export: the skeleton, the side table naming its standard joints, and where each role landed.
struct ExportedRig {
    explicit ExportedRig(Allocator& allocator) noexcept : skeleton(allocator) {}

    Skeleton skeleton;
    SkeletonProfile humanoid;
    u16 index[kRoleCount] = {};
    /// The deform-nothing joint above the hips, or `kInvalidJoint` when this export has none.
    u16 armature = kInvalidJoint;

    /// The index this export gave a role. Two exports answer differently, which is the whole
    /// problem.
    [[nodiscard]] u16 joint(RigJoint role) const noexcept {
        return index[static_cast<usize>(role)];
    }
};

[[nodiscard]] inline Status build_export(ExportedRig& rig, const ExportShape& shape) noexcept {
    for (u16& slot : rig.index) {
        slot = kInvalidJoint;
    }
    if (shape.armature_root) {
        const Expected<u16, Error> root =
            rig.skeleton.add_joint(Name::intern(std::string(shape.prefix) + "Armature"),
                                   kInvalidJoint, Transform::identity());
        if (!root) {
            return Status{make_unexpected(root.error())};
        }
        rig.armature = *root;
        rig.humanoid.map(HumanoidJoint::Root, *root);
    }
    for (usize position = 0; position < kRoleCount; ++position) {
        const RigRow& row = kRigRows[static_cast<usize>(shape.order[position])];
        const std::string name =
            std::string(shape.prefix) + shape.names[static_cast<usize>(row.role)];
        const u16 parent = row.is_root ? rig.armature : rig.index[static_cast<usize>(row.parent)];
        const Transform bind = Transform::from_translation(Vec3{row.translation.x * shape.scale,
                                                                row.translation.y * shape.scale,
                                                                row.translation.z * shape.scale});
        const Expected<u16, Error> joint =
            rig.skeleton.add_joint(Name::intern(name), parent, bind, row.dropped_at);
        if (!joint) {
            return Status{make_unexpected(joint.error())};
        }
        rig.index[static_cast<usize>(row.role)] = *joint;
        if (row.standard != HumanoidJoint::Count) {
            rig.humanoid.map(row.standard, *joint);
        }
    }
    if (!shape.armature_root) {
        // A rig with no root node of its own claims the `Root` slot for its own root joint — the
        // same claim `cy/import/fbx_skeleton.h` makes on a Mixamo rig, and what gives the root
        // chain something to derive a height scale from.
        rig.humanoid.map(HumanoidJoint::Root, rig.joint(RigJoint::Hips));
    }
    return rig.skeleton.finalize();
}

/// The animation authored onto one export, in that export's own joint indices.
struct ClipShape {
    f32 duration = 1.0F;
    f32 sample_rate = 30.0F;
    /// Metres travelled along −Z over the clip, and the hips' rise and fall, both in the source
    /// character's own proportions.
    f32 travel = 1.0F;
    f32 bob = 0.03F;
    f32 stride_radians = 0.7F;
    f32 arm_swing_radians = 0.5F;
    /// A curl on a finger joint no humanoid profile names. It crosses a joint-for-joint
    /// correspondence and does not cross a twenty-two joint one, which is the difference between
    /// them made measurable.
    f32 finger_curl_radians = 0.6F;
};

namespace detail {

[[nodiscard]] inline Status author_rotation(Clip& clip, u16 joint, const ClipShape& shape,
                                            Vec3 axis, f32 amplitude, f32 phase) noexcept {
    const auto frames = static_cast<u32>(shape.duration * shape.sample_rate) + 1U;
    Expected<u32, Error> track =
        clip.add_joint_track(TrackKind::Rotation, joint, Interpolation::Spherical);
    if (!track) {
        return Status{make_unexpected(track.error())};
    }
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 time = static_cast<f32>(frame) / shape.sample_rate;
        const f32 cycle = (time / shape.duration) * math::kTwoPi;
        const Quat rotation = Quat::from_axis_angle(axis, amplitude * std::sin(cycle + phase));
        if (Status added =
                clip.add_key(*track, time, Vec4{rotation.x, rotation.y, rotation.z, rotation.w});
            !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace detail

/// Author a walk-shaped clip against `rig`'s own joint indices and compress it.
///
/// The hips track carries the rig's OWN rest height, because that is what an exported clip holds: a
/// track is an absolute local transform, not an offset from a bind pose.
[[nodiscard]] inline Status author_clip(Clip& clip, const ExportedRig& rig, Name name,
                                        const ClipShape& shape) noexcept {
    clip.set_name(name);
    clip.set_duration(shape.duration);
    clip.set_loop_mode(LoopMode::Loop);
    clip.set_sample_rate_hint(shape.sample_rate);

    const auto frames = static_cast<u32>(shape.duration * shape.sample_rate) + 1U;
    const Vec3 rest = rig.skeleton.joints()[rig.joint(RigJoint::Hips)].bind_local.translation;
    Expected<u32, Error> hips = clip.add_joint_track(
        TrackKind::Translation, rig.joint(RigJoint::Hips), Interpolation::Linear);
    if (!hips) {
        return Status{make_unexpected(hips.error())};
    }
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 time = static_cast<f32>(frame) / shape.sample_rate;
        const f32 progress = time / shape.duration;
        const Vec4 value{rest.x, rest.y + (shape.bob * std::sin(progress * 2.0F * math::kTwoPi)),
                         rest.z - (shape.travel * progress), 0.0F};
        if (Status added = clip.add_key(*hips, time, value); !added) {
            return added;
        }
    }

    const Vec3 pitch{1.0F, 0.0F, 0.0F};
    if (Status authored = detail::author_rotation(clip, rig.joint(RigJoint::LeftUpperLeg), shape,
                                                  pitch, shape.stride_radians, 0.0F);
        !authored) {
        return authored;
    }
    if (Status authored = detail::author_rotation(clip, rig.joint(RigJoint::RightUpperLeg), shape,
                                                  pitch, shape.stride_radians, math::kPi);
        !authored) {
        return authored;
    }
    if (Status authored = detail::author_rotation(clip, rig.joint(RigJoint::LeftLowerArm), shape,
                                                  pitch, shape.arm_swing_radians, math::kPi);
        !authored) {
        return authored;
    }
    if (Status authored = detail::author_rotation(clip, rig.joint(RigJoint::RightLowerArm), shape,
                                                  pitch, shape.arm_swing_radians, 0.0F);
        !authored) {
        return authored;
    }
    if (Status authored = detail::author_rotation(clip, rig.joint(RigJoint::LeftFinger1), shape,
                                                  Vec3{0.0F, 0.0F, 1.0F}, shape.finger_curl_radians,
                                                  math::kHalfPi);
        !authored) {
        return authored;
    }
    return clip.compress(CompressionSettings{});
}

/// A joint's placement in model space under a local pose, walked to the root.
[[nodiscard]] inline Transform model_of(const Skeleton& skeleton, Span<const Transform> local,
                                        u16 joint) noexcept {
    Transform accumulated = Transform::identity();
    u16 cursor = joint;
    while (cursor != kInvalidJoint) {
        accumulated = local[cursor] * accumulated;
        cursor = skeleton.joints()[cursor].parent;
    }
    return accumulated;
}

}  // namespace cy::animation::testing::rigs
