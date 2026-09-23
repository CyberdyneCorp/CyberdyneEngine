// SPDX-License-Identifier: MIT
#include <cy/physics/ragdoll/ragdoll.h>

#include <algorithm>
#include <cmath>

namespace cy::physics::ragdoll {

Expected<Profile, Error> Profile::generate(const animation::Skeleton& skeleton,
                                           Allocator& allocator) noexcept {
    if (!skeleton.finalized()) {
        return fail(ErrorCode::InvalidArgument, "ragdoll: skeleton must be finalized");
    }
    Profile profile(allocator);
    if (Status reserved = profile.bones_.reserve(skeleton.joint_count()); !reserved) {
        return make_unexpected(reserved.error());
    }
    const Span<const Transform> bind = skeleton.bind_model();
    for (u16 index = 0; index < skeleton.joint_count(); ++index) {
        const animation::Joint& joint = skeleton.joints()[index];
        BoneProfile bone;
        bone.parent = joint.parent;
        const Vec3 extents =
            joint.bounds.is_empty() ? Vec3{0.08f, 0.08f, 0.08f} : joint.bounds.half_extents();
        const f32 extent = std::max({extents.x, extents.y, extents.z, 0.05f});
        if (joint.parent == animation::kInvalidJoint) {
            bone.shape.type = ShapeType::Sphere;
            bone.shape.radius = std::max(0.10f, extent);
            bone.mass = 8.0f;
        } else {
            const Vec3 to_parent =
                inverse(bind[index]).transform_point(bind[joint.parent].translation);
            const f32 distance = length(to_parent);
            const f32 radius = std::max(0.035f, std::min(extent, distance * 0.25f));
            bone.shape.type = ShapeType::Capsule;
            bone.shape.radius = radius;
            bone.shape.half_height = std::max(0.01f, distance * 0.5f - radius);
            bone.collider_local.translation = to_parent * 0.5f;
            if (distance > math::kSmallLength) {
                bone.collider_local.rotation = Quat::from_to(kAxisUp, to_parent / distance);
            }
            bone.mass = std::max(0.3f, distance * 6.0f);
        }
        if (Status added = profile.bones_.push_back(bone); !added) {
            return make_unexpected(added.error());
        }
    }
    return profile;
}

}  // namespace cy::physics::ragdoll
