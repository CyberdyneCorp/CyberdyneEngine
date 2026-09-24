// SPDX-License-Identifier: MIT
#pragma once
// Engine-owned wheeled vehicle vocabulary. The chassis is an ordinary dynamic physics body.

#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/servers/physics/handles.h>

namespace cy::physics {

struct VehicleWheelDescription {
    Vec3 position;
    f32 radius = 0.3f;
    f32 width = 0.1f;
    f32 suspension_min = 0.3f;
    f32 suspension_max = 0.5f;
    f32 suspension_frequency = 1.5f;
    f32 suspension_damping = 0.5f;
    f32 max_steer_angle = 0.0f;
    f32 max_brake_torque = 1500.0f;
};

struct VehicleDifferentialDescription {
    u32 left_wheel = 0;
    u32 right_wheel = 1;
    f32 torque_fraction = 1.0f;
    f32 ratio = 3.42f;
};

struct VehicleDescription {
    BodyHandle chassis;
    /// Wheel and differential arrays are borrowed only during create_vehicle().
    const VehicleWheelDescription* wheels = nullptr;
    u32 wheel_count = 0;
    const VehicleDifferentialDescription* differentials = nullptr;
    u32 differential_count = 0;
    f32 max_engine_torque = 500.0f;
};

struct VehicleInput {
    f32 throttle = 0.0f;    ///< [-1, 1], negative for reverse.
    f32 steering = 0.0f;    ///< [-1, 1], positive to the right.
    f32 brake = 0.0f;       ///< [0, 1].
    f32 hand_brake = 0.0f;  ///< [0, 1].
};

struct VehicleWheelState {
    bool grounded = false;
    f32 suspension_length = 0.0f;
    f32 steer_angle = 0.0f;
    f32 angular_velocity = 0.0f;
    Vec3 contact_position;
    Vec3 contact_normal;
};

[[nodiscard]] Status validate(const VehicleDescription& description) noexcept;
[[nodiscard]] Status validate(const VehicleInput& input) noexcept;

}  // namespace cy::physics
