// SPDX-License-Identifier: MIT
// Authored joints. See cy/gameplay/play/joints.h.

#include <cy/gameplay/play/joints.h>

#include <cy/core/math/scalar.h>

#include <cmath>

#include "authored_fields.h"

namespace cy::gameplay {
namespace {

namespace ser = scene::serialization;
namespace fields = joint_fields;

/// Every kind, in `ConstraintType`'s order, so a word is looked up rather than spelled twice.
constexpr physics::ConstraintType kKinds[] = {
    physics::ConstraintType::Fixed,         physics::ConstraintType::Point,
    physics::ConstraintType::Hinge,         physics::ConstraintType::Slider,
    physics::ConstraintType::Distance,      physics::ConstraintType::Cone,
    physics::ConstraintType::SwingTwist,    physics::ConstraintType::SixDof,
    physics::ConstraintType::RackAndPinion, physics::ConstraintType::Gear,
};

/// The fields of one authored joint, bound to the file's declarations once.
struct Reader {
    const ser::World& world;
    const ser::WorldTypeDecl& declared;
    const ser::WorldComponent& component;

    [[nodiscard]] const ser::WorldValue* get(std::string_view name) const noexcept {
        return authored::field_named(world, declared, component, name);
    }
    [[nodiscard]] f32 real(std::string_view name, f32 fallback) const noexcept {
        return authored::float_of(get(name), fallback);
    }
    [[nodiscard]] Vec3 vec3(std::string_view name, Vec3 fallback) const noexcept {
        return authored::vec3_of(get(name), fallback);
    }
};

/// The limits a kind reads from the shared `limit_min`/`limit_max` pair. One pair rather than one
/// per kind, because a joint has one kind and a hinge's range, a slider's travel, a swing-twist's
/// twist and a distance joint's span are never wanted at once.
void read_range(const Reader& reader, physics::ConstraintDescription& out) noexcept {
    const physics::AxisLimit range{reader.real(fields::kLimitMin, 1.0F),
                                   reader.real(fields::kLimitMax, -1.0F)};
    switch (out.type) {
        case physics::ConstraintType::Hinge:
        case physics::ConstraintType::Slider:
            out.limit = range;
            break;
        case physics::ConstraintType::SwingTwist:
            out.twist_limit = range;
            break;
        case physics::ConstraintType::Distance:
            out.min_distance = range.min;
            out.max_distance = range.max;
            break;
        default:
            break;
    }
}

/// The six-degrees-of-freedom limits: linear XYZ then angular XYZ, as `dof_limits` orders them.
/// Translation is locked and rotation free by default, which is a ball joint an author can open up.
void read_six_axes(const Reader& reader, physics::ConstraintDescription& out) noexcept {
    const Vec3 linear_min = reader.vec3(fields::kLinearMin, Vec3{0.0F, 0.0F, 0.0F});
    const Vec3 linear_max = reader.vec3(fields::kLinearMax, Vec3{0.0F, 0.0F, 0.0F});
    const Vec3 angular_min = reader.vec3(fields::kAngularMin, Vec3{1.0F, 1.0F, 1.0F});
    const Vec3 angular_max = reader.vec3(fields::kAngularMax, Vec3{-1.0F, -1.0F, -1.0F});
    for (u32 axis = 0; axis < 3; ++axis) {
        out.dof_limits[axis] = physics::AxisLimit{linear_min[axis], linear_max[axis]};
        out.dof_limits[axis + 3] = physics::AxisLimit{angular_min[axis], angular_max[axis]};
    }
}

}  // namespace

Expected<physics::ConstraintType, Error> joint_kind_of(std::string_view word) noexcept {
    for (const physics::ConstraintType kind : kKinds) {
        if (word == physics::constraint_type_name(kind)) {
            return kind;
        }
    }
    return fail(ErrorCode::InvalidArgument,
                "joint: the kind is not one of fixed, point, hinge, slider, distance, cone, "
                "swing-twist, six-dof, rack-and-pinion or gear");
}

Quat joint_axis_rotation(Vec3 axis) noexcept {
    const f32 squared = length_squared(axis);
    if (!(squared > math::kSmallLength)) {
        return Quat::identity();
    }
    return Quat::from_to(kAxisRight, axis * (1.0F / std::sqrt(squared)));
}

Expected<AuthoredJoint, Error> authored_joint(const ser::World& world,
                                              const ser::WorldNode& node) noexcept {
    const ser::WorldTypeDecl* declared = authored::type_named(world, fields::kComponent);
    const ser::WorldComponent* component =
        declared == nullptr ? nullptr : node.find(declared->file_type);
    if (component == nullptr) {
        return fail(ErrorCode::NotFound, "joint: the node carries no Joint");
    }
    const Reader reader{world, *declared, *component};
    const Expected<physics::ConstraintType, Error> kind =
        joint_kind_of(authored::text_of(world, reader.get(fields::kKind), "fixed"));
    if (!kind) {
        return make_unexpected(kind.error());
    }

    AuthoredJoint joint;
    physics::ConstraintDescription& out = joint.description;
    out.type = *kind;
    out.frame_a.translation = reader.vec3(fields::kAnchor, Vec3{0.0F, 0.0F, 0.0F});
    out.frame_a.rotation = joint_axis_rotation(reader.vec3(fields::kAxis, kAxisRight));
    read_range(reader, out);
    out.swing_limit_y = reader.real(fields::kSwingY, 0.0F);
    out.swing_limit_z = reader.real(fields::kSwingZ, 0.0F);
    if (out.type == physics::ConstraintType::SixDof) {
        read_six_axes(reader, out);
    }
    out.motor.target_velocity = reader.real(fields::kMotorVelocity, 0.0F);
    out.motor.max_force = reader.real(fields::kMotorMaxForce, 0.0F);
    out.ratio = reader.real(fields::kRatio, 1.0F);
    out.break_force = reader.real(fields::kBreakForce, 0.0F);
    out.break_torque = reader.real(fields::kBreakTorque, 0.0F);
    out.collide_connected = authored::bool_of(reader.get(fields::kCollideConnected), false);
    joint.target = authored::entity_of(reader.get(fields::kTarget));
    return joint;
}

Transform frame_on_target(const Transform& body_a, const Transform& frame_a,
                          const Transform* body_b) noexcept {
    const Transform anchor = body_a * frame_a;
    if (body_b == nullptr) {
        return anchor;
    }
    return inverse(*body_b) * anchor;
}

}  // namespace cy::gameplay
