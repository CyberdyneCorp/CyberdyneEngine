#pragma once
// The chain mapping the retarget cases share, and the model-space height they measure a contact
// with. Two suites use it — the unit one that maps a pose and the integration one that bakes a clip
// — so it lives beside the fixture rather than being copied into both.

#include <cy/animation/retarget.h>

#include "fixture.h"

namespace cy::animation::testing {

/// Map the fixture biped onto itself, chain by chain. Both skeletons have the same joint indices
/// and, deliberately, no name in common: the mapping is by chain semantics, which is the whole
/// point of the profile.
inline Status map_biped(RetargetProfile& profile) noexcept {
    const JointPair root[] = {{kRoot, kRoot}, {kHips, kHips}};
    const JointPair spine[] = {{kSpine, kSpine}, {kChest, kChest}};
    const JointPair head[] = {{kHead, kHead}};
    const JointPair arm[] = {
        {kShoulder, kShoulder}, {kUpperArm, kUpperArm}, {kLowerArm, kLowerArm}};
    const JointPair leg[] = {{kUpperLeg, kUpperLeg}, {kLowerLeg, kLowerLeg}, {kFoot, kFoot}};
    if (Status mapped = profile.map_chain(Chain::Root, Span<const JointPair>(root, 2)); !mapped) {
        return mapped;
    }
    if (Status mapped = profile.map_chain(Chain::Spine, Span<const JointPair>(spine, 2)); !mapped) {
        return mapped;
    }
    if (Status mapped = profile.map_chain(Chain::Head, Span<const JointPair>(head, 1)); !mapped) {
        return mapped;
    }
    if (Status mapped = profile.map_chain(Chain::LeftArm, Span<const JointPair>(arm, 3)); !mapped) {
        return mapped;
    }
    return profile.map_chain(Chain::LeftLeg, Span<const JointPair>(leg, 3));
}

/// A joint's height in model space under a local pose. A foot contact is this number being zero.
[[nodiscard]] inline f32 model_height(const Skeleton& skeleton, Span<const Transform> local,
                                      u16 joint) noexcept {
    Transform accumulated = Transform::identity();
    u16 cursor = joint;
    while (cursor != kInvalidJoint) {
        accumulated = local[cursor] * accumulated;
        cursor = skeleton.joints()[cursor].parent;
    }
    return accumulated.translation.y;
}

}  // namespace cy::animation::testing
