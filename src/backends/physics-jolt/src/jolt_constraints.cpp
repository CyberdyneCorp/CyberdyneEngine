// SPDX-License-Identifier: MIT
// Engine joint frames and limits translated to Jolt's concrete constraint settings.

#include "jolt_constraints.h"

// clang-format off
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/GearConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/RackAndPinionConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
// clang-format on

#include <algorithm>
#include <cmath>

namespace cy::physics::jolt {
namespace {

template <typename Settings>
void common(Settings& settings, const ConstraintDescription& description) noexcept {
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    settings.mUserData = description.user_data;
}

[[nodiscard]] JPH::Vec3 axis_x(const Transform& frame) noexcept {
    return to_jolt(frame.right());
}

[[nodiscard]] JPH::Vec3 axis_y(const Transform& frame) noexcept {
    return to_jolt(frame.up());
}

[[nodiscard]] JPH::RVec3 position(const Transform& frame) noexcept {
    const Vec3 value = frame.translation;
    return JPH::RVec3(value.x, value.y, value.z);
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> fixed(const ConstraintDescription& description,
                                                     JPH::Body& body_a, JPH::Body& body_b,
                                                     const Transform& a,
                                                     const Transform& b) noexcept {
    JPH::FixedConstraintSettings settings;
    common(settings, description);
    settings.mPoint1 = position(a);
    settings.mPoint2 = position(b);
    settings.mAxisX1 = axis_x(a);
    settings.mAxisX2 = axis_x(b);
    settings.mAxisY1 = axis_y(a);
    settings.mAxisY2 = axis_y(b);
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> point(const ConstraintDescription& description,
                                                     JPH::Body& body_a, JPH::Body& body_b,
                                                     const Transform& a,
                                                     const Transform& b) noexcept {
    JPH::PointConstraintSettings settings;
    common(settings, description);
    settings.mPoint1 = position(a);
    settings.mPoint2 = position(b);
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> hinge(const ConstraintDescription& description,
                                                     JPH::Body& body_a, JPH::Body& body_b,
                                                     const Transform& a,
                                                     const Transform& b) noexcept {
    JPH::HingeConstraintSettings settings;
    common(settings, description);
    settings.mPoint1 = position(a);
    settings.mPoint2 = position(b);
    settings.mHingeAxis1 = axis_x(a);
    settings.mHingeAxis2 = axis_x(b);
    settings.mNormalAxis1 = axis_y(a);
    settings.mNormalAxis2 = axis_y(b);
    if (description.limit.limited()) {
        settings.mLimitsMin = description.limit.min;
        settings.mLimitsMax = description.limit.max;
    }
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> slider(const ConstraintDescription& description,
                                                      JPH::Body& body_a, JPH::Body& body_b,
                                                      const Transform& a,
                                                      const Transform& b) noexcept {
    JPH::SliderConstraintSettings settings;
    common(settings, description);
    settings.mPoint1 = position(a);
    settings.mPoint2 = position(b);
    settings.mSliderAxis1 = axis_x(a);
    settings.mSliderAxis2 = axis_x(b);
    settings.mNormalAxis1 = axis_y(a);
    settings.mNormalAxis2 = axis_y(b);
    if (description.limit.limited()) {
        settings.mLimitsMin = description.limit.min;
        settings.mLimitsMax = description.limit.max;
    }
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> distance(const ConstraintDescription& description,
                                                        JPH::Body& body_a, JPH::Body& body_b,
                                                        const Transform& a,
                                                        const Transform& b) noexcept {
    JPH::DistanceConstraintSettings settings;
    common(settings, description);
    settings.mPoint1 = position(a);
    settings.mPoint2 = position(b);
    settings.mMinDistance = description.min_distance;
    settings.mMaxDistance = description.max_distance;
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> cone(const ConstraintDescription& description,
                                                    JPH::Body& body_a, JPH::Body& body_b,
                                                    const Transform& a,
                                                    const Transform& b) noexcept {
    JPH::ConeConstraintSettings settings;
    common(settings, description);
    settings.mPoint1 = position(a);
    settings.mPoint2 = position(b);
    settings.mTwistAxis1 = axis_x(a);
    settings.mTwistAxis2 = axis_x(b);
    settings.mHalfConeAngle = std::max(description.swing_limit_y, description.swing_limit_z);
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> swing_twist(const ConstraintDescription& description,
                                                           JPH::Body& body_a, JPH::Body& body_b,
                                                           const Transform& a,
                                                           const Transform& b) noexcept {
    JPH::SwingTwistConstraintSettings settings;
    common(settings, description);
    settings.mPosition1 = position(a);
    settings.mPosition2 = position(b);
    settings.mTwistAxis1 = axis_x(a);
    settings.mTwistAxis2 = axis_x(b);
    settings.mPlaneAxis1 = axis_y(a);
    settings.mPlaneAxis2 = axis_y(b);
    settings.mNormalHalfConeAngle = description.swing_limit_y;
    settings.mPlaneHalfConeAngle = description.swing_limit_z;
    if (description.twist_limit.limited()) {
        settings.mTwistMinAngle = description.twist_limit.min;
        settings.mTwistMaxAngle = description.twist_limit.max;
    } else {
        settings.mTwistMinAngle = -JPH::JPH_PI;
        settings.mTwistMaxAngle = JPH::JPH_PI;
    }
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> six_dof(const ConstraintDescription& description,
                                                       JPH::Body& body_a, JPH::Body& body_b,
                                                       const Transform& a,
                                                       const Transform& b) noexcept {
    JPH::SixDOFConstraintSettings settings;
    common(settings, description);
    settings.mPosition1 = position(a);
    settings.mPosition2 = position(b);
    settings.mAxisX1 = axis_x(a);
    settings.mAxisX2 = axis_x(b);
    settings.mAxisY1 = axis_y(a);
    settings.mAxisY2 = axis_y(b);
    for (u32 index = 0; index < 6; ++index) {
        const auto axis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(index);
        if (description.dof_limits[index].limited()) {
            settings.SetLimitedAxis(axis, description.dof_limits[index].min,
                                    description.dof_limits[index].max);
        } else {
            settings.MakeFreeAxis(axis);
        }
    }
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> gear(const ConstraintDescription& description,
                                                    JPH::Body& body_a, JPH::Body& body_b,
                                                    const Transform& a,
                                                    const Transform& b) noexcept {
    JPH::GearConstraintSettings settings;
    common(settings, description);
    settings.mHingeAxis1 = axis_x(a);
    settings.mHingeAxis2 = axis_x(b);
    settings.mRatio = description.ratio;
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraint> rack_and_pinion(
    const ConstraintDescription& description, JPH::Body& body_a, JPH::Body& body_b,
    const Transform& a, const Transform& b) noexcept {
    JPH::RackAndPinionConstraintSettings settings;
    common(settings, description);
    settings.mHingeAxis = axis_x(a);
    settings.mSliderAxis = axis_x(b);
    settings.mRatio = description.ratio;
    return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(body_a, body_b));
}

[[nodiscard]] JPH::EMotorState motor_state(const MotorSettings& motor) noexcept {
    if (motor.max_force <= 0.0F) {
        return JPH::EMotorState::Off;
    }
    return motor.position_driven ? JPH::EMotorState::Position : JPH::EMotorState::Velocity;
}

void set_motor_settings(JPH::MotorSettings& target, const MotorSettings& source,
                        bool angular) noexcept {
    // Jolt disables a position motor at zero frequency. Use a stiff finite spring for the
    // engine's rigid-drive spelling until the backend has an exact hard-target path.
    target.mSpringSettings.mFrequency =
        source.spring_frequency > 0.0F ? source.spring_frequency : 30.0F;
    target.mSpringSettings.mDamping = source.spring_damping;
    if (angular) {
        target.SetTorqueLimit(source.max_force);
    } else {
        target.SetForceLimit(source.max_force);
    }
}

void apply_six_dof_motors(JPH::SixDOFConstraint& joint, const MotorSettings (&motors)[6]) noexcept {
    f32 speed[6] = {};
    f32 position[6] = {};
    for (u32 index = 0; index < 6; ++index) {
        const auto axis = static_cast<JPH::SixDOFConstraint::EAxis>(index);
        set_motor_settings(joint.GetMotorSettings(axis), motors[index], index >= 3);
        joint.SetMotorState(axis, motor_state(motors[index]));
        speed[index] = motors[index].target_velocity;
        position[index] = motors[index].target_position;
    }
    joint.SetTargetVelocityCS(JPH::Vec3(speed[0], speed[1], speed[2]));
    joint.SetTargetAngularVelocityCS(JPH::Vec3(speed[3], speed[4], speed[5]));
    joint.SetTargetPositionCS(JPH::Vec3(position[0], position[1], position[2]));
    joint.SetTargetOrientationCS(
        JPH::Quat::sEulerAngles(JPH::Vec3(position[3], position[4], position[5])));
}

[[nodiscard]] bool valid_motor(const MotorSettings& motor) noexcept {
    return std::isfinite(motor.max_force) && motor.max_force >= 0.0F &&
           std::isfinite(motor.target_velocity) && std::isfinite(motor.target_position) &&
           std::isfinite(motor.spring_frequency) && motor.spring_frequency >= 0.0F &&
           std::isfinite(motor.spring_damping) && motor.spring_damping >= 0.0F;
}

}  // namespace

Expected<JPH::Ref<JPH::TwoBodyConstraint>, Error> make_constraint(
    const ConstraintDescription& description, JPH::Body& body_a, JPH::Body& body_b,
    const Transform& world_frame_a, const Transform& world_frame_b) noexcept {
    if (!valid_motor(description.motor)) {
        return fail(ErrorCode::InvalidArgument, "jolt: invalid constraint motor settings");
    }
    for (const MotorSettings& motor : description.dof_motors) {
        if (!valid_motor(motor)) {
            return fail(ErrorCode::InvalidArgument, "jolt: invalid six-degree motor settings");
        }
    }
    JPH::Ref<JPH::TwoBodyConstraint> joint;
    switch (description.type) {
        case ConstraintType::Fixed:
            joint = fixed(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::Point:
            joint = point(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::Hinge:
            joint = hinge(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::Slider:
            joint = slider(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::Distance:
            joint = distance(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::Cone:
            joint = cone(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::SwingTwist:
            joint = swing_twist(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::SixDof:
            joint = six_dof(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::RackAndPinion:
            joint = rack_and_pinion(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
        case ConstraintType::Gear:
            joint = gear(description, body_a, body_b, world_frame_a, world_frame_b);
            break;
    }
    if (joint == nullptr) {
        return fail(ErrorCode::Unsupported, "jolt: constraint settings were rejected");
    }
    if (description.type == ConstraintType::SixDof) {
        apply_six_dof_motors(static_cast<JPH::SixDOFConstraint&>(*joint), description.dof_motors);
    }
    if (description.motor.max_force > 0.0F) {
        if (Status driven = update_constraint_motor(description.type, *joint, description.motor);
            !driven) {
            return make_unexpected(driven.error());
        }
    }
    return joint;
}

Status update_constraint_motor(ConstraintType type, JPH::TwoBodyConstraint& constraint,
                               const MotorSettings& motor) noexcept {
    if (!valid_motor(motor)) {
        return fail(ErrorCode::InvalidArgument, "jolt: invalid constraint motor settings");
    }
    if (type == ConstraintType::Hinge) {
        auto& hinge = static_cast<JPH::HingeConstraint&>(constraint);
        set_motor_settings(hinge.GetMotorSettings(), motor, true);
        hinge.SetMotorState(motor_state(motor));
        hinge.SetTargetAngularVelocity(motor.target_velocity);
        hinge.SetTargetAngle(motor.target_position);
        return ok();
    }
    if (type == ConstraintType::Slider) {
        auto& slider = static_cast<JPH::SliderConstraint&>(constraint);
        set_motor_settings(slider.GetMotorSettings(), motor, false);
        slider.SetMotorState(motor_state(motor));
        slider.SetTargetVelocity(motor.target_velocity);
        slider.SetTargetPosition(motor.target_position);
        return ok();
    }
    return fail(ErrorCode::Unsupported,
                "jolt: this constraint kind does not have one runtime motor axis");
}

Status update_constraint_orientation_motor(ConstraintType type, JPH::TwoBodyConstraint& constraint,
                                           const OrientationMotorSettings& motor) noexcept {
    const Quat& target = motor.target_orientation;
    const f32 length = length_squared(target);
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z) ||
        !std::isfinite(target.w) || !std::isfinite(length) || length < 1.0e-6F ||
        !std::isfinite(motor.max_torque) || motor.max_torque < 0.0F ||
        !std::isfinite(motor.spring_frequency) || motor.spring_frequency < 0.0F ||
        !std::isfinite(motor.spring_damping) || motor.spring_damping < 0.0F) {
        return fail(ErrorCode::InvalidArgument, "jolt: invalid orientation motor settings");
    }
    if (type != ConstraintType::SwingTwist) {
        return fail(ErrorCode::Unsupported,
                    "jolt: orientation motor requires a swing-twist constraint");
    }
    auto& joint = static_cast<JPH::SwingTwistConstraint&>(constraint);
    MotorSettings axis;
    axis.max_force = motor.max_torque;
    axis.spring_frequency = motor.spring_frequency;
    axis.spring_damping = motor.spring_damping;
    set_motor_settings(joint.GetSwingMotorSettings(), axis, true);
    set_motor_settings(joint.GetTwistMotorSettings(), axis, true);
    const JPH::EMotorState state =
        motor.max_torque > 0.0F ? JPH::EMotorState::Position : JPH::EMotorState::Off;
    joint.SetSwingMotorState(state);
    joint.SetTwistMotorState(state);
    joint.SetTargetOrientationBS(to_jolt(normalize(target)));
    return ok();
}

ConstraintLoad constraint_load(ConstraintType type, const JPH::TwoBodyConstraint& constraint,
                               f32 delta_seconds) noexcept {
    ConstraintLoad load;
    if (delta_seconds <= 0.0F) {
        return load;
    }
    if (type == ConstraintType::Fixed) {
        const auto& fixed = static_cast<const JPH::FixedConstraint&>(constraint);
        load.force = fixed.GetTotalLambdaPosition().Length() / delta_seconds;
        load.torque = fixed.GetTotalLambdaRotation().Length() / delta_seconds;
    } else if (type == ConstraintType::Point) {
        const auto& point = static_cast<const JPH::PointConstraint&>(constraint);
        load.force = point.GetTotalLambdaPosition().Length() / delta_seconds;
    } else if (type == ConstraintType::Hinge) {
        const auto& hinge = static_cast<const JPH::HingeConstraint&>(constraint);
        load.force = hinge.GetTotalLambdaPosition().Length() / delta_seconds;
        load.torque = (hinge.GetTotalLambdaRotation().Length() +
                       std::fabs(hinge.GetTotalLambdaRotationLimits()) +
                       std::fabs(hinge.GetTotalLambdaMotor())) /
                      delta_seconds;
    } else if (type == ConstraintType::Slider) {
        const auto& slider = static_cast<const JPH::SliderConstraint&>(constraint);
        load.force = (slider.GetTotalLambdaPosition().Length() +
                      std::fabs(slider.GetTotalLambdaPositionLimits()) +
                      std::fabs(slider.GetTotalLambdaMotor())) /
                     delta_seconds;
        load.torque = slider.GetTotalLambdaRotation().Length() / delta_seconds;
    } else if (type == ConstraintType::Distance) {
        const auto& distance = static_cast<const JPH::DistanceConstraint&>(constraint);
        load.force = std::fabs(distance.GetTotalLambdaPosition()) / delta_seconds;
    } else if (type == ConstraintType::Cone) {
        const auto& cone = static_cast<const JPH::ConeConstraint&>(constraint);
        load.force = cone.GetTotalLambdaPosition().Length() / delta_seconds;
        load.torque = std::fabs(cone.GetTotalLambdaRotation()) / delta_seconds;
    } else if (type == ConstraintType::SwingTwist) {
        const auto& swing = static_cast<const JPH::SwingTwistConstraint&>(constraint);
        load.force = swing.GetTotalLambdaPosition().Length() / delta_seconds;
        load.torque =
            (std::fabs(swing.GetTotalLambdaTwist()) + std::fabs(swing.GetTotalLambdaSwingY()) +
             std::fabs(swing.GetTotalLambdaSwingZ()) + swing.GetTotalLambdaMotor().Length()) /
            delta_seconds;
    } else if (type == ConstraintType::SixDof) {
        const auto& six = static_cast<const JPH::SixDOFConstraint&>(constraint);
        load.force = six.GetTotalLambdaPosition().Length() / delta_seconds;
        load.torque = six.GetTotalLambdaRotation().Length() / delta_seconds;
    } else if (type == ConstraintType::Gear) {
        const auto& gear = static_cast<const JPH::GearConstraint&>(constraint);
        load.torque = std::fabs(gear.GetTotalLambda()) / delta_seconds;
    } else if (type == ConstraintType::RackAndPinion) {
        const auto& rack = static_cast<const JPH::RackAndPinionConstraint&>(constraint);
        load.force = std::fabs(rack.GetTotalLambda()) / delta_seconds;
    }
    return load;
}

}  // namespace cy::physics::jolt
