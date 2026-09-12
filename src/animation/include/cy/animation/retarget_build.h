#pragma once
// Authoring the joint correspondence a retarget profile is built from — the half `retarget.h`
// deliberately leaves to its caller — and measuring whether two rigs need retargeting at all.
// M8.d.
//
// ================================================================================================
// WHAT WAS MISSING
// ================================================================================================
//
// `retarget.h` holds a mapping and nothing in the tree authored one. `RetargetProfile::map_chain`
// takes `JointPair`s — two raw joint indices — and until now every caller was a test with the pairs
// written out by hand (`src/animation/tests/retarget_chains.h`). That is right for the data
// structure, which must stay free of strings so that "WHEN two skeletons use entirely different
// naming conventions THEN the retarget profile SHALL map them by chain semantics with no name
// matching" is a property of the type rather than of a convention a caller is asked to follow. It
// left open the question nobody had answered: where do the pairs come from for two skeletons an
// importer has just produced?
//
// THE CASE THIS WAS WRITTEN FOR, stated because it decides every choice below. Four Mixamo exports
// of one character: one file carries the mesh, the skin and a rig; the others each carry ANOTHER
// COPY of that same rig and one animation. A clip is authored against the joint INDICES of the
// skeleton that shipped beside it, and `AnimationRig::bind` checks only that the skeleton has at
// least as many joints as the program addresses — so handing a clip from one export to the skeleton
// of another is not refused. It drives whichever joints happen to sit at those indices, and nothing
// reports it. The correspondence between the two rigs has to be derived before a clip crosses.
//
// ================================================================================================
// TWO CORRESPONDENCES, AND WHY BOTH EXIST
// ================================================================================================
//
// HUMANOID. The general answer: the two skeletons' `SkeletonProfile`s already say which joint is
// the left lower arm, and `chain_of()` says which chain that is, so the pairs fall out of the two
// side tables with no string compared here. It maps at most the twenty-two standard joints, and
// every other joint of the target keeps its rest placement — a finger the source curls stays
// straight. That is the price of a mapping that works between rigs that share nothing but a body
// plan.
//
// CONGRUENT. The answer for the case above: when the two skeletons are the SAME HIERARCHY — equal
// joint count and, joint for joint, the same parent — the correspondence is joint for joint,
// fingers and twists and terminators included, and nothing is left at rest. Each pair is still
// filed under a chain, because that is what decides where translation crosses and what the height
// scale is derived from; a joint takes the chain of the nearest humanoid-mapped joint at or above
// it.
//
// NOTHING HERE COMPARES A NAME, including the congruence test, and that is not purism. The same rig
// comes back as `mixamorig:Hips` from one export and `mixamorig1:Hips` from the next — the
// namespace is the half an exporter rewrites — so name equality is the one signal that FAILS on two
// exports of one rig. Shape and rest pose do not.
//
// ================================================================================================
// AND THE QUESTION TO ASK FIRST: IS A RETARGET NEEDED?
// ================================================================================================
//
// A clip's tracks hold absolute local transforms, not offsets from a bind pose. So two skeletons
// that are congruent AND share a bind pose within tolerance need no retarget at all: the clip binds
// to the other rig unchanged, at no cost. `compare_rigs()` measures that rather than assuming it,
// and it is worth measuring — it is the difference between a free rebind and a bake per clip. What
// a retarget then adds, when the rests differ, is exactly what `RetargetProfile::build` computes:
// the rest-pose reconciliation and the hip-height scaling.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <cy/animation/retarget.h>
#include <cy/animation/skeleton.h>

namespace cy::animation {

/// The chain a standard humanoid joint belongs to. Shoulder through hand is the arm; hip through
/// toes is the leg; the root and the hips are the root chain, which is the only one translation
/// crosses on.
[[nodiscard]] Chain chain_of(HumanoidJoint standard) noexcept;

/// How much two skeletons have in common, measured rather than assumed.
///
/// `congruent` is structure alone: the same joint count and the same parent for every joint.
/// `same_rest_pose` adds that every joint's bind placement agrees within the tolerance. Both true
/// means a clip authored against one plays on the other as it is.
struct RigMatch {
    bool congruent = false;
    bool same_rest_pose = false;
    /// The first joint at which the hierarchies differ, or `kInvalidJoint` when they do not.
    u16 first_difference = kInvalidJoint;
    /// The worst disagreement between the two bind poses, over the joints both skeletons have.
    /// Metres, degrees and a ratio-free scale difference.
    f32 worst_rest_translation = 0.0F;
    f32 worst_rest_rotation_degrees = 0.0F;
    f32 worst_rest_scale = 0.0F;
};

/// What `RigMatch::same_rest_pose` is measured against. The defaults are a tenth of a millimetre
/// and a tenth of a degree — the same numbers `CompressionSettings` calls a clip's error budget, so
/// two rests that agree this closely disagree by less than the codec that carries the animation.
struct RigMatchTolerance {
    f32 translation = 1e-4F;
    f32 rotation_degrees = 0.1F;
    f32 scale = 1e-3F;
};

[[nodiscard]] RigMatch compare_rigs(const Skeleton& source, const Skeleton& target,
                                    const RigMatchTolerance& tolerance = {}) noexcept;

/// Which correspondence a profile was authored from.
enum class Correspondence : u8 {
    /// The standard humanoid joints of both profiles, by chain. Every other target joint keeps its
    /// rest placement.
    Humanoid = 0,
    /// Joint for joint, over two skeletons that are the same hierarchy. Nothing is left at rest.
    Congruent,
};

struct RetargetBuildReport {
    Correspondence correspondence = Correspondence::Humanoid;
    /// Joint pairs written into the profile.
    u32 pairs = 0;
    /// Standard joints one profile maps and the other does not, so the pair could not be made.
    u32 unpaired_standard = 0;
    /// Target joints no entry writes. They hold the target's reference pose, which is what makes an
    /// unmapped joint look like the character standing rather than like a joint at the origin.
    u32 target_joints_at_rest = 0;
    /// What `RetargetProfile::build` reported, including the height scale it derived.
    RetargetReport retarget;
};

/// Author the twenty-two standard joints into `profile`, grouped by chain, from the two side
/// tables. A standard joint only one of them maps is counted and skipped.
///
/// THE HIPS ARE PAIRED FIRST AND THE ROOT LAST, and a joint already in a pair takes no second one.
/// That is the whole of how the two shapes a rig's top can have are reconciled: a Mixamo rig has no
/// root node, so `Root` and `Hips` resolve to one joint, and most other rigs carry a joint above
/// the hips that deforms nothing, so they resolve to two. Pairing in enum order would let `Root`
/// claim the target's hips — which then take the transform of a joint that never moves, and the
/// character animates everywhere except where it stands.
[[nodiscard]] Status map_humanoid_chains(RetargetProfile& profile, const SkeletonProfile& source,
                                         const SkeletonProfile& target,
                                         RetargetBuildReport& report) noexcept;

/// Author every joint of two congruent skeletons into `profile` as its own counterpart, grouped by
/// the chain of the nearest humanoid-mapped joint at or above it.
///
/// Refuses two skeletons that are not congruent rather than pairing indices that mean different
/// things on each side — the one mistake this correspondence can make. `humanoid` is the SOURCE's
/// side table, and congruence is what makes it the target's too.
[[nodiscard]] Status map_congruent_joints(RetargetProfile& profile, const Skeleton& source,
                                          const Skeleton& target, const SkeletonProfile& humanoid,
                                          RetargetBuildReport& report) noexcept;

/// The one call: pick the correspondence these two skeletons support, author it, and build the
/// profile over them.
///
/// `profile` must be freshly constructed. Congruent wins when the hierarchies match, at least one
/// standard joint is mapped, AND the two humanoid profiles read the hierarchy the same way — two
/// rigs of the same shape whose profiles disagree about which joint is the left hand are not the
/// same rig, whatever their shape says, and two rigs with no profile at all say nothing about where
/// the root chain is, which is the chain that decides what moves the character rather than posing
/// it.
///
/// REFUSES A PROFILE THAT WOULD MAP NOTHING, rather than returning one that quietly reproduces the
/// target's bind pose for every frame of every clip. That silent pose is the failure this whole
/// file exists to make impossible to ship.
[[nodiscard]] Status build_retarget_profile(const Skeleton& source,
                                            const SkeletonProfile& source_humanoid,
                                            const Skeleton& target,
                                            const SkeletonProfile& target_humanoid,
                                            RetargetProfile& profile,
                                            RetargetBuildReport& report) noexcept;

}  // namespace cy::animation
