#ifndef CY_IMPORT_FBX_SKELETON_H
#define CY_IMPORT_FBX_SKELETON_H
// Step 7 of a model import: the skeleton. M8.d.
//
// `asset-import-pipeline` — "Model import", step 7: "Import skeletons, derive bone LOD levels, and
// remap to a `SkeletonProfile` if configured". `fbx.h` and `gltf.h` both said this step was absent
// because "animation-and-skinning reaches Working at M8 and there is nothing to import a skeleton
// INTO before it". M8.b landed the runtime — `cy/animation/skeleton.h` — so the blocker is gone and
// this file is the bridge.
//
// ================================================================================================
// WHY THE RECORD IS THE IMPORTER'S OWN, AND NOT `cy::animation::Skeleton`
// ================================================================================================
//
// STEP 7 PRODUCES THE SAME BYTES IN EVERY BUILD CONFIGURATION, and that is the whole argument.
// `-D CY_ANIMATION=OFF` removes src/animation/ from the build; a step that authored a
// `cy::animation::Skeleton` directly would therefore produce a skeleton in one configuration and
// nothing in the other, and a setting that changes the cooked bytes without reaching the derivation
// key is the one defect `options.h` says a cook cache cannot survive. A record of names, parents,
// bind poses and bone levels needs no runtime to write, so step 7 declines the dependency and keeps
// one answer.
//
// (Step 8 — `cy/import/fbx_clip.h` — made the other choice, and had to: a clip's error-bounded
// codec IS `cy::animation::Clip`, so there is nothing to compress against when the runtime is
// absent, and that step reports itself as not reached in an `OFF` build. The difference between the
// two steps is what each one needs, not two opinions about the same question.)
//
// So this file follows the precedent `gltf.h` sets for the cooked mesh: the importer defines its
// own compact record, and the runtime form is built from it by whoever has the runtime. The record
// is deliberately shaped so that building a `cy::animation::Skeleton` from it is a loop with no
// decisions in it — one `ImportedJoint` carries exactly the four arguments of
// `Skeleton::add_joint(name, parent, bind_local, dropped_at)` and nothing else, and the humanoid
// table beside them is exactly `SkeletonProfile`'s shape. `cy/import/animation_bridge.h` is that
// loop, and it is the file that includes `cy/animation/skeleton.h`.
//
// ================================================================================================
// THE THREE INVARIANTS THIS FILE EXISTS TO SATISFY
// ================================================================================================
//
// `skeleton.h` enforces all three as REFUSALS rather than trusting its caller, so an importer that
// gets any of them wrong produces a record nothing can load:
//
//   1. PARENT BEFORE CHILD. `add_joint` refuses a parent index that is not strictly smaller than
//      the joint's own. The joints here are emitted by an explicit depth-first walk from the scene
//      root — the same walk `fbx.cpp` uses for step 10 — rather than from `scene->nodes`, whose
//      order ufbx documents nowhere. It happens to be parent-before-child in every Mixamo file
//      measured, and resting correctness on that would be resting it on an implementation detail.
//
//   2. AT MOST 256 JOINTS. `graph::pose::kMaxJoints` is a fixed `u64[4]` mask and `add_joint`
//      returns `OutOfRange` past it. A rig over the cap is reported as a warning naming the count
//      and produces no skeleton, because truncating a hierarchy silently is worse than not
//      importing it.
//
//   3. BONE LEVELS ARE NESTED SUBSETS. `finalize()` refuses a child that survives a level its
//      parent is dropped at. The bone-LOD heuristic below therefore clamps every joint's level to
//      its parent's in one forward pass, which makes the rule hold whatever the heuristic decides.
//
// ================================================================================================
// THE COORDINATE AND UNIT CONVENTION IS ufbx's, NOT A SECOND ONE
// ================================================================================================
//
// `fbx.cpp` asks ufbx for `ufbx_axes_right_handed_y_up`, `target_unit_meters = 1.0` and
// `SPACE_CONVERSION_MODIFY_GEOMETRY`, so by the time a scene reaches this file every node's
// `local_transform` is already in the engine's right-handed, Y-up, metre space. A bind pose is read
// from that same `local_transform` — the identical field step 10 reads for its node table — so the
// skeleton and the hierarchy cannot disagree about where a joint is.
//
// Measured on `Walking.fbx`, whose 65 bones carry a real bind pose: the file's own bind pose
// (`ufbx_get_bone_pose`, `bone_to_parent`) and `local_transform` agree to 6e-7 in translation and
// 5e-9 in quaternion dot. Reading the pose instead would buy nothing and would introduce a second
// convention, and ufbx's own header warns that `bone_to_parent` is APPROXIMATED from world
// transforms because FBX stores only world.
//
// The `scale` option and the `source-up-axis` override are applied exactly as `fbx.cpp` applies
// them to a node: the scale multiplies translation, and a `z-up` override is one rotation on the
// ROOTS which the hierarchy carries down — never a rotation per joint.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/import/importer.h>
#include <cy/import/model.h>

#include <string>
#include <string_view>
#include <vector>

// ufbx is PRIVATE to `cy_import` (tools/import/CMakeLists.txt), so a consumer of this header must
// not be made to name a ufbx type. The scene is taken by an opaque reference and only
// `src/fbx_skeleton.cpp`, which includes <ufbx.h>, ever looks inside it.
struct ufbx_scene;

namespace cy::import {

/// The cooked skeleton payload's format version. Moved when the layout changes.
inline constexpr u32 kCookedSkeletonVersion = 1;

/// The prefix a skeleton sub-asset's name carries.
///
/// A naming convention rather than an `assets::AssetKind`, for the reason `importer.h` gives for
/// `kCollisionSubAssetPrefix`: a kind is persistent — "an enumerator is added at the end and never
/// renumbered" — and adding one makes every consumer that switches on kind grow a case. The kind is
/// `Animation`, which is the module that loads it; the prefix is what tells a skeleton from a clip
/// and what a cook profile can select on. `animation-and-skinning` does list Skeleton as its own
/// asset type, so an `AssetKind::Skeleton` may yet be worth its cost — and the day it is, this
/// prefix stays and only the kind moves.
inline constexpr std::string_view kSkeletonSubAssetPrefix = "skeleton/";

/// How many standard humanoid joints there are: `cy::animation::HumanoidJoint::Count`.
inline constexpr u16 kHumanoidJointCount = 22;

/// A joint that maps onto no standard humanoid joint.
inline constexpr u16 kUnmappedHumanoidJoint = 0xFFFFU;

/// The engine's standard humanoid joints, IN THE ORDER `cy::animation::HumanoidJoint` declares
/// them and spelled the way `animation::humanoid_joint_name` spells them.
///
/// Duplicated rather than included because tools/ may not link the animation runtime (see the top
/// of this file). It is duplication with a guard on it: `cy/import/animation_bridge.h`
/// static-asserts the count, and `test_fbx_skeleton.cpp` asserts every spelling against
/// `humanoid_joint_name()` — so the day somebody inserts a joint into that enum, a test fails
/// naming the slot rather than a character silently acquiring an elbow where its shoulder was.
inline constexpr std::string_view kHumanoidJointNames[kHumanoidJointCount] = {
    "Root",          "Hips",          "Spine",         "Chest",         "Neck",
    "Head",          "LeftShoulder",  "LeftUpperArm",  "LeftLowerArm",  "LeftHand",
    "RightShoulder", "RightUpperArm", "RightLowerArm", "RightHand",     "LeftUpperLeg",
    "LeftLowerLeg",  "LeftFoot",      "LeftToes",      "RightUpperLeg", "RightLowerLeg",
    "RightFoot",     "RightToes",
};

/// How many bone levels of detail a skeleton declares: `cy::animation::kBoneLodLevels`. A joint
/// whose `dropped_at` is this number survives every level.
inline constexpr u8 kBoneLodLevels = 4;

/// The joint cap a pose mask imposes: `cy::animation::kMaxJoints`, which is
/// `cy::graph::pose::kMaxJoints`.
inline constexpr u32 kMaxSkeletonJoints = 256;

/// One joint of an imported skeleton: exactly the arguments of `Skeleton::add_joint`.
struct ImportedJoint {
    /// The source node's own name, unmodified — namespace prefix and all. It is what the runtime
    /// interns and what a clip's tracks are resolved against, so it is NOT the stripped form the
    /// humanoid mapping matches on.
    std::string name;
    /// The index of the parent joint, always smaller than this joint's own, or -1 for a root.
    i32 parent = -1;
    /// The rest placement, in the parent's space, in metres.
    Transform bind_local;
    /// The FIRST bone level of detail at which this joint is dropped; `kBoneLodLevels` means never.
    u8 dropped_at = kBoneLodLevels;
};

/// The humanoid profile, by `cy::animation::HumanoidJoint` ordinal: the joint index playing that
/// standard part, or -1.
///
/// A SIDE TABLE AND NOT A FIELD ON THE JOINT, which is the shape `cy::animation::SkeletonProfile`
/// has and for the reason it has it: two standard joints may resolve to ONE joint. A Mixamo rig has
/// no dedicated root node — the hips ARE the root — so `Root` and `Hips` both answer with the same
/// index, and a per-joint field could only have said one of them.
struct HumanoidProfile {
    HumanoidProfile() noexcept {
        for (i32& joint : joints) {
            joint = -1;
        }
    }

    /// Zeroed in the declaration and then filled with the -1 sentinel by the constructor above.
    /// The two together rather than either alone: zero is a VALID joint index — the root of every
    /// rig — so `{}` cannot be the unmapped value, and the declaration's initialiser is what keeps
    /// the field initialised on any path a future constructor might add.
    i32 joints[kHumanoidJointCount] = {};

    void map(u16 standard, i32 joint) noexcept {
        if (standard < kHumanoidJointCount) {
            joints[standard] = joint;
        }
    }

    [[nodiscard]] i32 resolve(u16 standard) const noexcept {
        return standard < kHumanoidJointCount ? joints[standard] : -1;
    }

    [[nodiscard]] u32 mapped_count() const noexcept {
        u32 mapped = 0;
        for (const i32 joint : joints) {
            mapped += joint >= 0 ? 1U : 0U;
        }
        return mapped;
    }
};

/// One imported skeleton.
struct ImportedSkeleton {
    /// The sub-asset name: `kSkeletonSubAssetPrefix` followed by the root joint's name with any
    /// namespace stripped. Derived from something an ARTIST controls rather than from the file's
    /// ordering, which is what `importer.h` requires of a name an `AssetId` is bound to.
    std::string name;
    std::vector<ImportedJoint> joints;
    HumanoidProfile humanoid;

    [[nodiscard]] bool empty() const noexcept { return joints.empty(); }

    /// The index of the joint with this source name, or -1. Linear, like `Skeleton::find`, and for
    /// the same reason: a rig is tens of joints and a side table would be a second thing to keep in
    /// step. It is what an animation importer resolves a track's target node through.
    [[nodiscard]] i32 find(std::string_view joint_name) const noexcept;
};

/// How step 7 is configured. Every field is the value of an option `fbx.cpp` already declares, so
/// nothing here can change the output without reaching the derivation key.
struct SkeletonImportOptions {
    /// The uniform scale applied after the file's own unit conversion — the `scale` option.
    f32 scale = 1.0f;
    /// Empty for `auto`, which is ufbx's own conversion and the ordinary case. "z-up" makes this
    /// importer rotate the ROOT joints, exactly as `fbx.cpp` rotates the root nodes.
    std::string_view up_override;
};

/// Extract the skeleton from a loaded ufbx scene, and emit it as a sub-asset.
///
/// The joint set is every node carrying a bone attribute, every node a skin cluster deforms
/// through, and every ancestor of those up to (but not including) the scene root — an ancestor's
/// transform is part of its descendants' bind pose, so dropping it would move the rig.
///
/// A scene with no bones and no skin is not an error and not a warning: it is a static model, and
/// `out` is left empty. A rig over `kMaxSkeletonJoints` is a warning naming the count, and likewise
/// produces nothing. Everything else that can go wrong here is the machine's, and comes back as a
/// returned error.
///
/// `skeleton` is filled whether or not a sub-asset was emitted, so an animation importer can
/// resolve its tracks against the same joint indices this record numbers.
[[nodiscard]] Status import_fbx_skeleton(const ufbx_scene& scene,
                                         const SkeletonImportOptions& options, SubAssetNames& names,
                                         ImportResult& out, ImportedSkeleton& skeleton) noexcept;

/// Write the record: a header, then one variable-length entry per joint, little-endian.
///
/// A pure function of the skeleton, so two cooks of one source produce identical bytes — which is
/// `asset-import-pipeline`'s determinism requirement and what the content-addressed cook cache
/// rests on. The joint's NAME is written, never an interned `Name` handle: a handle is a number
/// this process assigned and means nothing in the next one.
[[nodiscard]] Status write_cooked_skeleton(const ImportedSkeleton& skeleton,
                                           Array<u8>& out) noexcept;

/// Read one back, checking the invariants `skeleton.h` would otherwise refuse the record for: a
/// parent that is not already present, a bone level outside the range, and a joint count over the
/// mask's cap. For the tests, for a tool that inspects a package, and for the cook step.
[[nodiscard]] Status read_cooked_skeleton(Span<const u8> payload, ImportedSkeleton& out) noexcept;

/// The standard humanoid joint a source joint name plays, or `kUnmappedHumanoidJoint`.
///
/// MATCHED ON THE SUFFIX AFTER THE LAST ':', which is what makes it work on the files it is for.
/// Mixamo namespaces every bone, and the namespace is `mixamorig:` in one export and `mixamorig1:`
/// in the next — the digit appears when a rig has been re-exported, and all four sample files carry
/// it. A table keyed on the literal `mixamorig:` would match nothing at all.
[[nodiscard]] u16 humanoid_joint_of(std::string_view joint_name) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_FBX_SKELETON_H
