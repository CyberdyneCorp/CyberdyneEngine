#include <cy/animation/skeleton.h>

namespace cy::animation {
namespace {

constexpr const char* kHumanoidNames[] = {
    "Root",          "Hips",          "Spine",         "Chest",         "Neck",
    "Head",          "LeftShoulder",  "LeftUpperArm",  "LeftLowerArm",  "LeftHand",
    "RightShoulder", "RightUpperArm", "RightLowerArm", "RightHand",     "LeftUpperLeg",
    "LeftLowerLeg",  "LeftFoot",      "LeftToes",      "RightUpperLeg", "RightLowerLeg",
    "RightFoot",     "RightToes",
};

static_assert(sizeof(kHumanoidNames) / sizeof(kHumanoidNames[0]) ==
                  static_cast<usize>(HumanoidJoint::Count),
              "every humanoid joint has a spelling");

}  // namespace

const char* humanoid_joint_name(HumanoidJoint joint) noexcept {
    const auto index = static_cast<usize>(joint);
    if (index >= static_cast<usize>(HumanoidJoint::Count)) {
        return "?";
    }
    return kHumanoidNames[index];
}

// --- SkeletonProfile ----------------------------------------------------------------------------

void SkeletonProfile::initialise() noexcept {
    if (initialised_) {
        return;
    }
    for (u16& slot : joints_) {
        slot = kInvalidJoint;
    }
    initialised_ = true;
}

void SkeletonProfile::map(HumanoidJoint standard, u16 joint) noexcept {
    initialise();
    const auto index = static_cast<usize>(standard);
    if (index < static_cast<usize>(HumanoidJoint::Count)) {
        joints_[index] = joint;
    }
}

u16 SkeletonProfile::resolve(HumanoidJoint standard) const noexcept {
    if (!initialised_) {
        return kInvalidJoint;
    }
    const auto index = static_cast<usize>(standard);
    if (index >= static_cast<usize>(HumanoidJoint::Count)) {
        return kInvalidJoint;
    }
    return joints_[index];
}

u32 SkeletonProfile::mapped_count() const noexcept {
    if (!initialised_) {
        return 0;
    }
    u32 count = 0;
    for (const u16 slot : joints_) {
        if (slot != kInvalidJoint) {
            ++count;
        }
    }
    return count;
}

// --- Skeleton -----------------------------------------------------------------------------------

Skeleton::Skeleton(Allocator& allocator) noexcept
    : joints_(allocator), bind_model_(allocator), inverse_bind_(allocator) {}

Expected<u16, Error> Skeleton::add_joint(Name name, u16 parent, const Transform& bind_local,
                                         u8 dropped_at) noexcept {
    const auto index = static_cast<u16>(joints_.size());
    if (joints_.size() >= kMaxJoints) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "a skeleton may not carry more joints than a pose mask "
                                     "addresses; see cy/graph/lower_pose.h",
                                     0});
    }
    if (parent != kInvalidJoint && parent >= index) {
        // The whole of "one linear pass": a parent that is not already in the array would make
        // model-space resolution a graph walk.
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a joint's parent must already be in the skeleton, so the "
                                     "array is ordered parent before child",
                                     0});
    }
    Joint joint;
    joint.name = name;
    joint.parent = parent;
    joint.bind_local = bind_local;
    joint.dropped_at = dropped_at;
    if (Status pushed = joints_.push_back(joint); !pushed) {
        return make_unexpected(pushed.error());
    }
    finalized_ = false;
    return index;
}

Status Skeleton::set_bounds(u16 joint, const Aabb& bounds) noexcept {
    if (joint >= joints_.size()) {
        return fail(ErrorCode::OutOfRange, "no such joint");
    }
    joints_[joint].bounds = bounds;
    return ok();
}

Status Skeleton::set_user_data(u16 joint, u32 value) noexcept {
    if (joint >= joints_.size()) {
        return fail(ErrorCode::OutOfRange, "no such joint");
    }
    joints_[joint].user_data = value;
    return ok();
}

Status Skeleton::finalize() noexcept {
    if (joints_.empty()) {
        return fail(ErrorCode::InvalidArgument, "a skeleton with no joints deforms nothing");
    }
    if (Status sized = bind_model_.resize(joints_.size()); !sized) {
        return sized;
    }
    if (Status sized = inverse_bind_.resize(joints_.size()); !sized) {
        return sized;
    }

    for (usize index = 0; index < joints_.size(); ++index) {
        const Joint& joint = joints_[index];
        if (joint.parent != kInvalidJoint && joints_[joint.parent].dropped_at < joint.dropped_at) {
            // "nested subsets of joints": a level's retained set must be closed under parenthood,
            // or a retained joint would be parented to one that was not evaluated.
            return fail(ErrorCode::InvalidArgument,
                        "a joint may not survive a bone level of detail its parent is dropped at; "
                        "bone levels are nested subsets");
        }
        bind_model_[index] = joint.parent == kInvalidJoint
                                 ? joint.bind_local
                                 : bind_model_[joint.parent] * joint.bind_local;
        const Expected<Mat4, Error> inverted = inverse(bind_model_[index].to_matrix());
        if (!inverted) {
            return fail(ErrorCode::InvalidArgument,
                        "a joint's bind pose is singular and cannot be inverted for skinning");
        }
        inverse_bind_[index] = *inverted;
    }

    for (u8 level = 0; level < kBoneLodLevels; ++level) {
        retained_[level] = JointMask{};
        retained_count_[level] = 0;
    }
    for (usize index = 0; index < joints_.size(); ++index) {
        for (u8 level = 0; level < kBoneLodLevels; ++level) {
            if (joints_[index].dropped_at > level) {
                retained_[level].set(static_cast<u32>(index));
                ++retained_count_[level];
            }
        }
    }
    finalized_ = true;
    return ok();
}

u16 Skeleton::find(Name name) const noexcept {
    for (usize index = 0; index < joints_.size(); ++index) {
        if (joints_[index].name == name) {
            return static_cast<u16>(index);
        }
    }
    return kInvalidJoint;
}

const JointMask& Skeleton::retained(u8 level) const noexcept {
    return retained_[level < kBoneLodLevels ? level : kBoneLodLevels - 1];
}

u32 Skeleton::retained_count(u8 level) const noexcept {
    return retained_count_[level < kBoneLodLevels ? level : kBoneLodLevels - 1];
}

void Skeleton::to_model(Span<const Transform> local, const JointMask& mask,
                        Span<Transform> model) const noexcept {
    const usize count = joints_.size();
    if (local.size() < count || model.size() < count) {
        return;
    }
    for (usize index = 0; index < count; ++index) {
        if (!mask.test(static_cast<u32>(index))) {
            continue;
        }
        const u16 parent = joints_[index].parent;
        model[index] = parent == kInvalidJoint ? local[index] : model[parent] * local[index];
    }
}

void Skeleton::to_skinning(Span<const Transform> model, const JointMask& mask,
                           Span<Mat4> out) const noexcept {
    const usize count = joints_.size();
    if (model.size() < count || out.size() < count || inverse_bind_.size() < count) {
        return;
    }
    for (usize index = 0; index < count; ++index) {
        if (!mask.test(static_cast<u32>(index))) {
            out[index] = Mat4::identity();
            continue;
        }
        out[index] = model[index].to_matrix() * inverse_bind_[index];
    }
}

Aabb Skeleton::skinned_bounds(Span<const Transform> model) const noexcept {
    Aabb total = Aabb::empty();
    const usize count = joints_.size() < model.size() ? joints_.size() : model.size();
    for (usize index = 0; index < count; ++index) {
        if (joints_[index].bounds.is_empty()) {
            continue;
        }
        total.grow(transformed(joints_[index].bounds, model[index].to_matrix()));
    }
    return total;
}

void Skeleton::reference_pose(Span<Transform> out) const noexcept {
    const usize count = joints_.size() < out.size() ? joints_.size() : out.size();
    for (usize index = 0; index < count; ++index) {
        out[index] = joints_[index].bind_local;
    }
}

}  // namespace cy::animation
