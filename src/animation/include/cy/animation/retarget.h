#pragma once
// Retargeting by semantic chains: mapping animation between skeletons that share a body plan and
// share nothing else. M8.b task 5.3.
//
// ================================================================================================
// CHAINS, NOT NAMES
// ================================================================================================
//
// `animation-and-skinning` — "Retargeting by semantic chains": "Retargeting SHALL map animation
// between skeletons through a retarget profile describing semantic chains — root, spine, neck,
// head, and left and right arms and legs — rather than by matching bone names."
//
// So a profile is a list of joint PAIRS grouped by chain, and there is no string comparison in this
// file. "WHEN two skeletons use entirely different naming conventions THEN the retarget profile
// SHALL map them by chain semantics with no name matching" is a property of the data structure
// here, not of a convention a caller is asked to follow.
//
// WHAT PRESERVES A FOOT CONTACT. Rotations are transferred; bone LENGTHS are not. The target keeps
// its own bind translations, so its limbs stay its own length, and only the root chain's
// translation crosses — scaled by the ratio of the two skeletons' hip heights. A character half the
// height of the one a clip was authored for plants its feet on the same ground because it moves
// its hips half as far, not because anything was measured at runtime.
//
// OFFLINE AND AT RUNTIME, WITH THE COST DIFFERENCE STATED. `retarget_pose()` is one quaternion
// multiply and one composition per mapped joint, per evaluation — tens of joints, so tens of
// nanoseconds, and it runs after the graph. `bake_clip()` does the same work once per sampled frame
// at cook time and produces a clip the target plays directly, which costs nothing at runtime and
// costs a clip's worth of memory per source-target pair. Prefer the bake for the combinations a
// game ships and the runtime path for content it did not.

#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>

#include <cy/animation/clip.h>
#include <cy/animation/skeleton.h>

namespace cy::animation {

/// The semantic chains a profile maps. The list `animation-and-skinning` states, and no more: a
/// chain that is not one of these is not a chain both skeletons are guaranteed to have.
enum class Chain : u8 {
    Root = 0,
    Spine,
    Neck,
    Head,
    LeftArm,
    RightArm,
    LeftLeg,
    RightLeg,
    Count,
};

[[nodiscard]] const char* chain_name(Chain chain) noexcept;

/// One joint of a chain, in both skeletons.
struct JointPair {
    u16 source = kInvalidJoint;
    u16 target = kInvalidJoint;
};

struct RetargetReport {
    u32 chains_mapped = 0;
    u32 joints_mapped = 0;
    u32 joints_unmapped_source = 0;
    u32 joints_unmapped_target = 0;
    /// The ratio of target hip height to source hip height, from the two bind poses.
    f32 height_scale = 1.0F;
};

/// The mapping between two skeletons.
class RetargetProfile {
public:
    explicit RetargetProfile(Allocator& allocator) noexcept;

    [[nodiscard]] Status map_chain(Chain chain, Span<const JointPair> pairs) noexcept;

    /// Reconcile the rest poses and derive the scaling rules. Must be called after every chain is
    /// mapped and before any pose is retargeted.
    [[nodiscard]] Status build(const Skeleton& source, const Skeleton& target,
                               RetargetReport& report) noexcept;

    [[nodiscard]] bool built() const noexcept { return built_; }
    [[nodiscard]] f32 height_scale() const noexcept { return height_scale_; }
    [[nodiscard]] const RetargetReport& report() const noexcept { return report_; }

    /// Map one pose. `target_local` is seeded with the target's reference pose, so a joint the
    /// profile does not map keeps its rest placement rather than an arbitrary one.
    [[nodiscard]] Status retarget_pose(const Skeleton& source, const Skeleton& target,
                                       Span<const Transform> source_local,
                                       Span<Transform> target_local) const noexcept;

    /// Whether a target joint is mapped, and whether it is the one translation crosses on.
    [[nodiscard]] bool maps_target(u16 target_joint, bool& carries_translation) const noexcept;
    /// The source joint a target joint takes its rotation from, or `kInvalidJoint`.
    [[nodiscard]] u16 source_of(u16 target_joint) const noexcept;

private:
    struct Entry {
        JointPair pair;
        /// The rest-pose reconciliation: what takes the source's model-space rest rotation to the
        /// target's. Computed once in `build()`, which is what makes the runtime path a multiply.
        Quat offset = Quat::identity();
        Chain chain = Chain::Root;
        bool root_translation = false;
    };

    Array<Entry> entries_;
    RetargetReport report_;
    f32 height_scale_ = 1.0F;
    bool built_ = false;
};

/// Bake a retargeted clip at cook time. `out` is authored and compressed by this call, and
/// `allocator` holds the frames while they are being written.
[[nodiscard]] Status bake_clip(Allocator& allocator, const RetargetProfile& profile,
                               const Skeleton& source, const Skeleton& target, const Clip& clip,
                               f32 sample_rate, const CompressionSettings& settings,
                               Clip& out) noexcept;

}  // namespace cy::animation
