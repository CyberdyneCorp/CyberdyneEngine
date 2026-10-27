// SPDX-License-Identifier: MIT
#pragma once
// A pair of legs, a pelvis and a head: the skeleton foot placement and aim are written against,
// and the grounds the cases stand it on. Issue #92.

#include <cy/animation/foot_placement.h>
#include <cy/animation/skeleton.h>

#include "fixture.h"

namespace cy::animation::testing {

enum Legs : u16 {
    kLegRoot = 0,
    kPelvis,
    kSpineJoint,
    kHeadJoint,
    kHipL,
    kKneeL,
    kAnkleL,
    kHipR,
    kKneeR,
    kAnkleR,
    kLegJoints,
};

inline constexpr f32 kAnkleHeight = 0.08F;

/// A standing pair of legs, knees a touch bent as a stance is: the ankles 8 cm over the model's
/// ground plane, 0.2 m apart on x.
inline Status build_legs(Skeleton& skeleton, Quat hip_turn = Quat::identity()) noexcept {
    struct Row {
        const char* name = "";
        u16 parent = kInvalidJoint;
        Vec3 offset;
    };
    const Row rows[] = {
        {"root", kInvalidJoint, Vec3{0.0F, 0.0F, 0.0F}},
        {"pelvis", kLegRoot, Vec3{0.0F, 0.96F, 0.0F}},
        {"spine", kPelvis, Vec3{0.0F, 0.3F, 0.0F}},
        {"head", kSpineJoint, Vec3{0.0F, 0.4F, 0.0F}},
        {"l_hip", kPelvis, Vec3{-0.1F, -0.04F, 0.0F}},
        {"l_knee", kHipL, Vec3{0.0F, -0.42F, 0.04F}},
        {"l_ankle", kKneeL, Vec3{0.0F, -0.42F, -0.04F}},
        {"r_hip", kPelvis, Vec3{0.1F, -0.04F, 0.0F}},
        {"r_knee", kHipR, Vec3{0.0F, -0.42F, 0.04F}},
        {"r_ankle", kKneeR, Vec3{0.0F, -0.42F, -0.04F}},
    };
    for (const Row& row : rows) {
        Transform bind = Transform::from_translation(row.offset);
        if (row.parent == kPelvis && (row.offset.x != 0.0F)) {
            // A hip with a rotated frame, as an exported rig has: the leg below it is authored in
            // that frame, so its offsets turn with it.
            bind.rotation = hip_turn;
        }
        Expected<u16, Error> joint =
            skeleton.add_joint(Name::intern(row.name), row.parent, bind, kBoneLodLevels);
        if (!joint) {
            return Status{make_unexpected(joint.error())};
        }
    }
    return skeleton.finalize();
}

inline FootPlacementSetup legs_setup() noexcept {
    FootPlacementSetup setup;
    setup.pelvis = kPelvis;
    setup.legs[0] = IkChain{kHipL, kKneeL, kAnkleL, 0, 0, Vec3{0.0F, 0.0F, 1.0F}};
    setup.legs[1] = IkChain{kHipR, kKneeR, kAnkleR, 0, 0, Vec3{0.0F, 0.0F, 1.0F}};
    setup.ankle_height = kAnkleHeight;
    setup.smoothing = 0.0F;
    return setup;
}

/// A step: `low` for x below `edge`, `high` above it.
class StepGround final : public GroundProbe {
public:
    StepGround(f32 edge, f32 low, f32 high) noexcept : edge_(edge), low_(low), high_(high) {}
    [[nodiscard]] GroundHit cast_down(const GroundQuery& query) const noexcept override {
        GroundHit hit;
        const f32 height = query.origin.x < edge_ ? low_ : high_;
        const f32 drop = query.origin.y - height;
        if (drop < 0.0F || drop > query.length) {
            return hit;
        }
        hit.hit = true;
        hit.point = Vec3{query.origin.x, height, query.origin.z};
        return hit;
    }

private:
    f32 edge_;
    f32 low_;
    f32 high_;
};

}  // namespace cy::animation::testing
