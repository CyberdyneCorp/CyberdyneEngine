#pragma once
// Translation of engine constraint descriptions into Jolt joint settings.

#include "jolt_common.h"

#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>

#include <cy/core/base/expected.h>
#include <cy/servers/physics/constraints.h>

namespace cy::physics::jolt {

[[nodiscard]] Expected<JPH::Ref<JPH::TwoBodyConstraint>, Error> make_constraint(
    const ConstraintDescription& description, JPH::Body& body_a, JPH::Body& body_b,
    const Transform& world_frame_a, const Transform& world_frame_b) noexcept;

[[nodiscard]] Status update_constraint_motor(ConstraintType type,
                                             JPH::TwoBodyConstraint& constraint,
                                             const MotorSettings& motor) noexcept;
[[nodiscard]] Status update_constraint_orientation_motor(
    ConstraintType type, JPH::TwoBodyConstraint& constraint,
    const OrientationMotorSettings& motor) noexcept;

/// Force/torque magnitudes from Jolt's last-step solver impulses, divided by the fixed dt.
struct ConstraintLoad {
    f32 force = 0.0F;
    f32 torque = 0.0F;
};
[[nodiscard]] ConstraintLoad constraint_load(ConstraintType type,
                                             const JPH::TwoBodyConstraint& constraint,
                                             f32 delta_seconds) noexcept;

}  // namespace cy::physics::jolt
