#pragma once
// The skeleton: the lean runtime deformation hierarchy, its bone level of detail, and the humanoid
// profile that lets engine systems address a joint without knowing what an artist called it.
// M8.b task 5.1.
//
// ================================================================================================
// A SKELETON IS NOT A RIG, AND THIS FILE IS THE HALF THE RUNTIME EVALUATES
// ================================================================================================
//
// `animation-and-skinning` — "Animation asset model": "the skeleton is what the runtime evaluates
// and skins against; the rig is what artists manipulate and what compiles into a program run
// against a skeleton", and "A skeleton SHALL contain no authoring-only control data". So there are
// no controls here, no space switching and no pose drivers: a joint is a name, a parent, a bind
// pose, a level of detail and a box.
//
// PARENT BEFORE CHILD IS ENFORCED, NOT ASSUMED. "stored as flat arrays ordered parent-before-child
// so evaluation is a single linear pass" — `add_joint` refuses a parent index that is not smaller
// than the joint being added, which is what makes `to_model()` one forward loop with no recursion
// and no visited set.
//
// BONE LEVEL OF DETAIL IS AUTHORED. "nested subsets of joints, ordered so that detail joints —
// facial, finger, twist, and accessory chains — are dropped first." A joint carries the FIRST level
// at which it is dropped, and `finalize()` refuses a skeleton where a child outlives its parent:
// retaining a finger whose hand was dropped would leave the finger parented to nothing.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/graph/lower_pose.h>

namespace cy::animation {

/// The joint mask a pose value carries, shared with the compiled program rather than converted at
/// the boundary: `cy::graph::pose::PoseInstruction::required` is one of these, and pose dependency
/// analysis hands it straight to the sampler.
using JointMask = graph::pose::JointMask;

/// The cap the mask imposes. A rig with more joints than this needs a mask that allocates, and that
/// decision belongs to whoever brings one — see `cy/graph/lower_pose.h`.
inline constexpr u32 kMaxJoints = graph::pose::kMaxJoints;

inline constexpr u16 kInvalidJoint = 0xFFFFU;

/// How many bone levels of detail a skeleton declares. Four is the table in
/// `animation-and-skinning`'s "Animation level of detail" — Full, Simplified, Cached and Baked —
/// read from the skeleton's side, and level 0 is the whole skeleton.
inline constexpr u8 kBoneLodLevels = 4;

/// One joint of the deformation hierarchy.
struct Joint {
    Name name;
    /// The index of the parent, always smaller than this joint's own, or `kInvalidJoint` for a
    /// root.
    u16 parent = kInvalidJoint;
    /// The rest placement, in the parent's space.
    Transform bind_local;
    /// THE FIRST BONE LOD LEVEL AT WHICH THIS JOINT IS DROPPED. `kBoneLodLevels` — the default —
    /// means it survives every level, which is what a spine or a hip is. A finger set to 1 is
    /// present at level 0 and absent from level 1 upward.
    u8 dropped_at = kBoneLodLevels;
    /// The box of the vertices this joint influences, in the joint's own space. Empty for a joint
    /// that influences nothing, which is what lets a skeleton carry attachment points without
    /// inflating what the culler tests.
    Aabb bounds = Aabb::empty();
    /// Per-joint user data, opaque here. `animation-and-skinning` asks for it by name.
    u32 user_data = 0;
};

/// The engine-standard humanoid joints a `SkeletonProfile` maps a source rig onto.
///
/// "WHEN a skeleton is assigned a humanoid profile THEN engine systems SHALL address joints by
/// standard names regardless of the source rig's naming."
enum class HumanoidJoint : u8 {
    Root = 0,
    Hips,
    Spine,
    Chest,
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToes,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToes,
    Count,
};

[[nodiscard]] const char* humanoid_joint_name(HumanoidJoint joint) noexcept;

/// A mapping from the engine's standard humanoid joints onto one skeleton's own indices.
///
/// A side table and not part of the skeleton, because a skeleton that is not humanoid has none and
/// must not pay for one.
class SkeletonProfile {
public:
    void map(HumanoidJoint standard, u16 joint) noexcept;

    /// The skeleton's index for a standard joint, or `kInvalidJoint` when it is unmapped.
    [[nodiscard]] u16 resolve(HumanoidJoint standard) const noexcept;

    /// How many of the standard joints are mapped.
    [[nodiscard]] u32 mapped_count() const noexcept;

private:
    u16 joints_[static_cast<usize>(HumanoidJoint::Count)] = {};
    bool initialised_ = false;

    void initialise() noexcept;
};

/// The runtime deformation hierarchy.
///
/// Built by repeated `add_joint`, then `finalize()`, which computes what the runtime reads: the
/// model-space bind pose, the inverse bind matrices skinning multiplies by, and one joint mask per
/// bone level of detail. Nothing derived is computed lazily in a frame.
class Skeleton {
public:
    explicit Skeleton(Allocator& allocator) noexcept;

    Skeleton(const Skeleton&) = delete;
    Skeleton& operator=(const Skeleton&) = delete;
    Skeleton(Skeleton&&) noexcept = default;
    Skeleton& operator=(Skeleton&&) noexcept = default;
    ~Skeleton() = default;

    void set_name(Name name) noexcept { name_ = name; }
    [[nodiscard]] Name name() const noexcept { return name_; }

    /// Append a joint. `parent` must already have been added, or be `kInvalidJoint` for a root.
    [[nodiscard]] Expected<u16, Error> add_joint(Name name, u16 parent, const Transform& bind_local,
                                                 u8 dropped_at = kBoneLodLevels) noexcept;

    [[nodiscard]] Status set_bounds(u16 joint, const Aabb& bounds) noexcept;
    [[nodiscard]] Status set_user_data(u16 joint, u32 value) noexcept;

    /// Compute the derived tables and check the hierarchy. Refuses a child that survives a level
    /// its parent does not, and a skeleton with no joints.
    [[nodiscard]] Status finalize() noexcept;

    [[nodiscard]] bool finalized() const noexcept { return finalized_; }
    [[nodiscard]] u16 joint_count() const noexcept { return static_cast<u16>(joints_.size()); }
    [[nodiscard]] Span<const Joint> joints() const noexcept { return joints_.span(); }
    [[nodiscard]] u16 find(Name name) const noexcept;

    /// The rest pose in model space, and the matrices that take a vertex from model space into a
    /// joint's space. Both are empty until `finalize()`.
    [[nodiscard]] Span<const Transform> bind_model() const noexcept { return bind_model_.span(); }
    [[nodiscard]] Span<const Mat4> inverse_bind() const noexcept { return inverse_bind_.span(); }

    /// The joints retained at a bone level of detail. Level 0 is every joint.
    [[nodiscard]] const JointMask& retained(u8 level) const noexcept;
    [[nodiscard]] u32 retained_count(u8 level) const noexcept;

    /// Local to model space, in one forward pass. Only the joints in `mask` are written; a joint
    /// outside it keeps whatever `model` already held, which is what makes a reduced bone level of
    /// detail cost nothing for the joints it dropped.
    void to_model(Span<const Transform> local, const JointMask& mask,
                  Span<Transform> model) const noexcept;

    /// Model space to the matrices a skinning pass multiplies a vertex by: `model * inverse_bind`.
    void to_skinning(Span<const Transform> model, const JointMask& mask,
                     Span<Mat4> out) const noexcept;

    /// The animated bounds: the union of each joint's own box through its model transform.
    /// "computed from bone transforms and per-bone bounds, not from the bind pose".
    [[nodiscard]] Aabb skinned_bounds(Span<const Transform> model) const noexcept;

    /// The rest pose, in local space, into `out`.
    void reference_pose(Span<Transform> out) const noexcept;

private:
    Name name_;
    Array<Joint> joints_;
    Array<Transform> bind_model_;
    Array<Mat4> inverse_bind_;
    JointMask retained_[kBoneLodLevels];
    u32 retained_count_[kBoneLodLevels] = {};
    bool finalized_ = false;
};

}  // namespace cy::animation
