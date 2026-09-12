#include <cy/animation/retarget_build.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::animation {
namespace {

constexpr usize kStandardCount = static_cast<usize>(HumanoidJoint::Count);

/// The chain of each standard joint, in `HumanoidJoint` order. A table rather than a switch so that
/// a joint added to the enum without a chain stops the build on the static assert below instead of
/// falling through a `default:` into the root chain, which is the one chain that carries
/// translation and therefore the worst place for a joint to land by accident.
constexpr Chain kChainOfStandard[] = {
    Chain::Root,      // Root
    Chain::Root,      // Hips
    Chain::Spine,     // Spine
    Chain::Spine,     // Chest
    Chain::Neck,      // Neck
    Chain::Head,      // Head
    Chain::LeftArm,   // LeftShoulder
    Chain::LeftArm,   // LeftUpperArm
    Chain::LeftArm,   // LeftLowerArm
    Chain::LeftArm,   // LeftHand
    Chain::RightArm,  // RightShoulder
    Chain::RightArm,  // RightUpperArm
    Chain::RightArm,  // RightLowerArm
    Chain::RightArm,  // RightHand
    Chain::LeftLeg,   // LeftUpperLeg
    Chain::LeftLeg,   // LeftLowerLeg
    Chain::LeftLeg,   // LeftFoot
    Chain::LeftLeg,   // LeftToes
    Chain::RightLeg,  // RightUpperLeg
    Chain::RightLeg,  // RightLowerLeg
    Chain::RightLeg,  // RightFoot
    Chain::RightLeg,  // RightToes
};

static_assert(sizeof(kChainOfStandard) / sizeof(kChainOfStandard[0]) == kStandardCount,
              "every standard humanoid joint belongs to exactly one chain");

/// The marker `assign_chains` leaves on a joint no standard slot named, before the inheritance pass
/// fills it in. `Chain::Count` would do, but a value outside the enum says "not yet" rather than
/// naming a chain that does not exist.
constexpr u8 kUnassignedChain = 0xFFU;

/// Add one pair to the profile. Pairs go in one at a time rather than a chain at a time because
/// `map_chain` takes any length and the caller then needs no buffer — and a buffer sized for the
/// joint cap is a kilobyte of stack for a cook-time call that does not need it.
[[nodiscard]] Status map_one(RetargetProfile& profile, Chain chain, u16 source,
                             u16 target) noexcept {
    const JointPair pair{source, target};
    return profile.map_chain(chain, Span<const JointPair>(&pair, 1));
}

/// Whether either joint of a candidate pair is already spoken for.
///
/// EITHER, not both, and that is the rule this function exists to state. Two rigs need not agree on
/// how many joints a body part is: one resolves `Root` and `Hips` to a single joint because its
/// hips ARE its root, and the next resolves them to two because it carries a joint above the hips
/// that deforms nothing. A joint that is already paired takes no second pair, so the slot that
/// arrives later maps nothing rather than overwriting — which, with the pairing order below, is
/// what keeps the target's hips driven by the source's hips in all four combinations of the two
/// rigs.
[[nodiscard]] bool claimed(const JointPair* written, u32 count, u16 source, u16 target) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (written[index].source == source || written[index].target == target) {
            return true;
        }
    }
    return false;
}

/// The standard slots in the order they are paired: `Hips` first, `Root` LAST, everything else in
/// `HumanoidJoint` order.
///
/// The root slot goes last because it is the one whose meaning differs between rigs — on a rig with
/// no root node it names the hips, and on a rig with one it names a joint that never moves. Pairing
/// it first would let it claim the target's hips and leave the character standing on the spot while
/// every other joint animated; pairing it last means it only takes a joint the hips did not.
[[nodiscard]] HumanoidJoint slot_at(usize position) noexcept {
    return static_cast<HumanoidJoint>((position + 1) % kStandardCount);
}

/// The chain of every joint: the one its nearest humanoid-mapped ancestor-or-self belongs to, and
/// the root chain for a joint with no mapped joint above it at all.
///
/// One forward pass is enough for the inheritance because `Skeleton::add_joint` refuses a parent
/// index that is not smaller than the child's, so a parent's chain is always already decided.
void assign_chains(const Skeleton& skeleton, const SkeletonProfile& humanoid,
                   Span<u8> chains) noexcept {
    for (u16 joint = 0; joint < skeleton.joint_count(); ++joint) {
        chains[joint] = kUnassignedChain;
    }
    // In `HumanoidJoint` order, and the first slot to claim a joint keeps it: a rig whose hips are
    // its root resolves both `Root` and `Hips` to one joint, and both say the root chain anyway.
    for (usize standard = 0; standard < kStandardCount; ++standard) {
        const u16 joint = humanoid.resolve(static_cast<HumanoidJoint>(standard));
        if (joint >= skeleton.joint_count() || chains[joint] != kUnassignedChain) {
            continue;
        }
        chains[joint] = static_cast<u8>(kChainOfStandard[standard]);
    }
    for (u16 joint = 0; joint < skeleton.joint_count(); ++joint) {
        if (chains[joint] != kUnassignedChain) {
            continue;
        }
        const u16 parent = skeleton.joints()[joint].parent;
        chains[joint] = parent == kInvalidJoint ? static_cast<u8>(Chain::Root) : chains[parent];
    }
}

/// Whether the two side tables read the two skeletons the same way: every standard slot resolves to
/// the same index on both, mapped or unmapped.
[[nodiscard]] bool profiles_agree(const SkeletonProfile& source,
                                  const SkeletonProfile& target) noexcept {
    for (usize standard = 0; standard < kStandardCount; ++standard) {
        const auto slot = static_cast<HumanoidJoint>(standard);
        if (source.resolve(slot) != target.resolve(slot)) {
            return false;
        }
    }
    return true;
}

/// The worst of the two rest placements' three disagreements, folded into `match`.
void widen_rest_difference(RigMatch& match, const Transform& source,
                           const Transform& target) noexcept {
    match.worst_rest_translation =
        math::max(match.worst_rest_translation, length(source.translation - target.translation));
    match.worst_rest_rotation_degrees =
        math::max(match.worst_rest_rotation_degrees,
                  math::degrees(angle_between(source.rotation, target.rotation)));
    for (usize axis = 0; axis < 3; ++axis) {
        match.worst_rest_scale =
            math::max(match.worst_rest_scale, std::fabs(source.scale[axis] - target.scale[axis]));
    }
}

}  // namespace

Chain chain_of(HumanoidJoint standard) noexcept {
    const auto index = static_cast<usize>(standard);
    return index < kStandardCount ? kChainOfStandard[index] : Chain::Root;
}

RigMatch compare_rigs(const Skeleton& source, const Skeleton& target,
                      const RigMatchTolerance& tolerance) noexcept {
    RigMatch match;
    const u16 shared = math::min(source.joint_count(), target.joint_count());
    match.congruent = source.joint_count() == target.joint_count() && source.joint_count() != 0;
    for (u16 joint = 0; joint < shared; ++joint) {
        const Joint& from = source.joints()[joint];
        const Joint& onto = target.joints()[joint];
        if (from.parent != onto.parent) {
            match.congruent = false;
            if (match.first_difference == kInvalidJoint) {
                match.first_difference = joint;
            }
        }
        widen_rest_difference(match, from.bind_local, onto.bind_local);
    }
    if (source.joint_count() != target.joint_count() && match.first_difference == kInvalidJoint) {
        // The hierarchies agree as far as the shorter one goes, so the first difference is where it
        // stops.
        match.first_difference = shared;
    }
    match.same_rest_pose = match.congruent &&
                           match.worst_rest_translation <= tolerance.translation &&
                           match.worst_rest_rotation_degrees <= tolerance.rotation_degrees &&
                           match.worst_rest_scale <= tolerance.scale;
    return match;
}

Status map_humanoid_chains(RetargetProfile& profile, const SkeletonProfile& source,
                           const SkeletonProfile& target, RetargetBuildReport& report) noexcept {
    report.correspondence = Correspondence::Humanoid;
    JointPair written[kStandardCount];
    u32 count = 0;
    for (usize position = 0; position < kStandardCount; ++position) {
        const HumanoidJoint slot = slot_at(position);
        const u16 from = source.resolve(slot);
        const u16 onto = target.resolve(slot);
        if (from == kInvalidJoint || onto == kInvalidJoint) {
            if (from != kInvalidJoint || onto != kInvalidJoint) {
                ++report.unpaired_standard;
            }
            continue;
        }
        if (claimed(written, count, from, onto)) {
            continue;
        }
        if (Status mapped = map_one(profile, chain_of(slot), from, onto); !mapped) {
            return mapped;
        }
        written[count] = JointPair{from, onto};
        ++count;
        ++report.pairs;
    }
    return ok();
}

Status map_congruent_joints(RetargetProfile& profile, const Skeleton& source,
                            const Skeleton& target, const SkeletonProfile& humanoid,
                            RetargetBuildReport& report) noexcept {
    if (!compare_rigs(source, target).congruent) {
        return fail(ErrorCode::InvalidArgument,
                    "a joint-for-joint correspondence is authored over two skeletons that are the "
                    "same hierarchy; these two are not, so joint N means a different bone on each "
                    "side");
    }
    const Skeleton& skeleton = source;
    if (skeleton.joint_count() > kMaxJoints) {
        return fail(ErrorCode::InvalidArgument,
                    "a skeleton with more joints than the pose mask's cap");
    }
    u8 chains[kMaxJoints] = {};
    assign_chains(skeleton, humanoid, Span<u8>(chains, kMaxJoints));
    report.correspondence = Correspondence::Congruent;
    for (u16 joint = 0; joint < skeleton.joint_count(); ++joint) {
        if (Status mapped = map_one(profile, static_cast<Chain>(chains[joint]), joint, joint);
            !mapped) {
            return mapped;
        }
        ++report.pairs;
    }
    return ok();
}

Status build_retarget_profile(const Skeleton& source, const SkeletonProfile& source_humanoid,
                              const Skeleton& target, const SkeletonProfile& target_humanoid,
                              RetargetProfile& profile, RetargetBuildReport& report) noexcept {
    report = RetargetBuildReport{};
    if (!source.finalized() || !target.finalized()) {
        return fail(ErrorCode::InvalidArgument,
                    "a correspondence is authored between two finalized skeletons: the rest poses "
                    "it reconciles are what finalize() computes");
    }

    // Two rigs of the same shape whose profiles disagree about which joint is the left hand are not
    // two exports of one rig, whatever the shape says, so the general mapping takes them.
    const bool joint_for_joint = compare_rigs(source, target).congruent &&
                                 source_humanoid.mapped_count() != 0 &&
                                 profiles_agree(source_humanoid, target_humanoid);
    if (Status authored =
            joint_for_joint
                ? map_congruent_joints(profile, source, target, source_humanoid, report)
                : map_humanoid_chains(profile, source_humanoid, target_humanoid, report);
        !authored) {
        return authored;
    }
    if (report.pairs == 0) {
        return fail(
            ErrorCode::InvalidArgument,
            "no joint of these two skeletons corresponds to another: they are not the same "
            "hierarchy and their humanoid profiles name no joint in common. A profile built "
            "from nothing retargets every frame of every clip to the target's bind pose");
    }

    if (Status built = profile.build(source, target, report.retarget); !built) {
        return built;
    }
    for (u16 joint = 0; joint < target.joint_count(); ++joint) {
        bool carries_translation = false;
        if (!profile.maps_target(joint, carries_translation)) {
            ++report.target_joints_at_rest;
        }
    }
    return ok();
}

}  // namespace cy::animation
