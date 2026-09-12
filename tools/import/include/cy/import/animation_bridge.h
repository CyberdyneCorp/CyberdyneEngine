#ifndef CY_IMPORT_ANIMATION_BRIDGE_H
#define CY_IMPORT_ANIMATION_BRIDGE_H
// The one file that names both an imported record and the animation runtime. M8.d.
//
// ================================================================================================
// WHY THIS IS A HEADER AND NOT A TRANSLATION UNIT OF `cy_import`
// ================================================================================================
//
// Step 7 — the skeleton — deliberately needs no animation runtime, so that its cooked bytes are the
// same whether or not `CY_ANIMATION` is on; `cy/import/fbx_skeleton.h` argues that at length. The
// conversion INTO the runtime obviously does need it, and putting that conversion in a translation
// unit of `cy_import` would put the dependency back where the record just removed it.
//
// A header costs nothing until somebody includes it, and whoever includes it has already linked the
// runtime — the animation-aware half of a cook step, an editor, or the suite that proves the record
// loads. So the dependency is the INCLUDER's, which is exactly where it belongs, and this file
// compiles in a build where `cy::animation` exists and is invisible in one where it does not. (In a
// build with `CY_ANIMATION` on, `cy_import` does link the runtime for step 8's sake and defines
// `CY_IMPORT_ANIMATION` — this file needs neither, which is the point.)
//
// WHAT THIS IS NOT: a shipped path. Nothing in the engine loads a cooked skeleton yet — there is no
// runtime asset loader for `cy::animation::Skeleton` anywhere in the tree. This file is the loop
// that turns the record into the runtime object, written once here rather than three times in three
// callers, and `tools/import/tests/test_fbx_skeleton.cpp` is its first caller.

#include <cy/animation/skeleton.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/values/name.h>
#include <cy/import/fbx_skeleton.h>

namespace cy::import {

// The record duplicates two constants of the animation runtime, because tools/ may not include it
// (above). These are what keep the duplication from drifting: a joint cap or a bone-LOD depth that
// moved on one side and not the other stops the build here, at the only place that can see both.
static_assert(kHumanoidJointCount == static_cast<u16>(animation::HumanoidJoint::Count),
              "the importer's humanoid slot count is cy::animation::HumanoidJoint's");
static_assert(kBoneLodLevels == animation::kBoneLodLevels,
              "the importer's bone level count is cy::animation::kBoneLodLevels");
static_assert(kMaxSkeletonJoints == animation::kMaxJoints,
              "the importer's joint cap is the pose mask's");

/// Build the runtime skeleton from an imported record, and its humanoid profile beside it.
///
/// A loop with no decisions in it, which is the property `fbx_skeleton.h` shapes the record for:
/// the record's joints are already parent-before-child, already within the joint cap, and already
/// nested-subset in their bone levels, so every refusal `add_joint` and `finalize` can make is one
/// the importer has already made on the source. A failure here therefore means the record is
/// corrupt, not that the rig is unusual.
///
/// `out` is finalized on success — `bind_model()`, `inverse_bind()` and `retained()` are populated
/// and the skeleton is ready for `AnimationRig::bind`.
[[nodiscard]] inline Status build_runtime_skeleton(const ImportedSkeleton& skeleton,
                                                   animation::Skeleton& out,
                                                   animation::SkeletonProfile& profile) noexcept {
    if (skeleton.joints.empty()) {
        return fail(ErrorCode::InvalidArgument, "a skeleton with no joints deforms nothing");
    }
    for (const ImportedJoint& joint : skeleton.joints) {
        const u16 parent =
            joint.parent < 0 ? animation::kInvalidJoint : static_cast<u16>(joint.parent);
        const Expected<u16, Error> added =
            out.add_joint(Name::intern(joint.name), parent, joint.bind_local, joint.dropped_at);
        if (!added) {
            return make_unexpected(added.error());
        }
    }
    for (u16 standard = 0; standard < kHumanoidJointCount; ++standard) {
        const i32 joint = skeleton.humanoid.resolve(standard);
        if (joint >= 0) {
            profile.map(static_cast<animation::HumanoidJoint>(standard), static_cast<u16>(joint));
        }
    }
    return out.finalize();
}

}  // namespace cy::import

#endif  // CY_IMPORT_ANIMATION_BRIDGE_H
