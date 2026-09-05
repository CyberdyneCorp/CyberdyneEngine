// The component-to-body mapping. Task 4.2.3.
//
// Three functions rather than one with a `MotionType` argument, because the three components
// legitimately carry different fields — a `StaticBody` has no damping and no sleep settings — and a
// single function taking all of them would have to be handed defaults it then could not tell from
// authored values.

#include <cy/servers/physics/components.h>

namespace cy::physics {
namespace {

[[nodiscard]] BodyDescription common(const Transform& transform,
                                     Span<const ColliderDescription> colliders,
                                     UserData user_data) noexcept {
    BodyDescription description;
    description.transform = transform;
    description.colliders = colliders.data();
    description.collider_count = static_cast<u32>(colliders.size());
    description.user_data = user_data;
    return description;
}

}  // namespace

BodyDescription body_from(const RigidBody& rigid, const Transform& transform,
                          Span<const ColliderDescription> colliders, UserData user_data) noexcept {
    BodyDescription description = common(transform, colliders, user_data);
    description.motion = MotionType::Dynamic;
    description.mass = rigid.mass;
    description.center_of_mass = rigid.center_of_mass;
    description.override_center_of_mass = rigid.override_center_of_mass;
    description.linear_damping = rigid.linear_damping;
    description.angular_damping = rigid.angular_damping;
    description.gravity_scale = rigid.gravity_scale;
    description.allow_sleeping = rigid.allow_sleeping;
    description.start_asleep = rigid.start_asleep;
    description.continuous = rigid.continuous;
    description.locked_axes = rigid.locked_axes;
    return description;
}

BodyDescription static_body_from(const Transform& transform,
                                 Span<const ColliderDescription> colliders,
                                 UserData user_data) noexcept {
    BodyDescription description = common(transform, colliders, user_data);
    description.motion = MotionType::Static;
    // A static body is never integrated, so damping and gravity are meaningless on it. Zeroed
    // rather than left at the dynamic defaults, so that a body promoted to dynamic at run time
    // starts from something a reader can predict.
    description.linear_damping = 0.0f;
    description.angular_damping = 0.0f;
    description.gravity_scale = 0.0f;
    return description;
}

BodyDescription kinematic_body_from(const Transform& transform,
                                    Span<const ColliderDescription> colliders,
                                    UserData user_data) noexcept {
    BodyDescription description = common(transform, colliders, user_data);
    description.motion = MotionType::Kinematic;
    description.gravity_scale = 0.0f;
    description.allow_sleeping = false;  // it is moved by code, and code does not wake it first
    return description;
}

}  // namespace cy::physics
