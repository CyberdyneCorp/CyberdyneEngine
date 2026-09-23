#include <cy/servers/physics/vehicle.h>

#include <cy/core/math/scalar.h>

namespace cy::physics {
namespace {

[[nodiscard]] bool nonnegative_finite(f32 value) noexcept {
    return math::is_finite(value) && value >= 0.0f;
}

}  // namespace

Status validate(const VehicleDescription& description) noexcept {
    if (description.chassis.is_null() || description.wheels == nullptr ||
        description.wheel_count < 2 || description.wheel_count > 16 ||
        description.differentials == nullptr || description.differential_count == 0 ||
        description.differential_count > description.wheel_count / 2 ||
        !nonnegative_finite(description.max_engine_torque)) {
        return fail(ErrorCode::InvalidArgument, "physics vehicle: invalid chassis or drivetrain");
    }
    for (u32 index = 0; index < description.wheel_count; ++index) {
        const VehicleWheelDescription& wheel = description.wheels[index];
        if (!math::is_finite(wheel.position.x) || !math::is_finite(wheel.position.y) ||
            !math::is_finite(wheel.position.z) || !nonnegative_finite(wheel.radius) ||
            wheel.radius == 0.0f || !nonnegative_finite(wheel.width) || wheel.width == 0.0f ||
            !nonnegative_finite(wheel.suspension_min) ||
            !nonnegative_finite(wheel.suspension_max) ||
            wheel.suspension_min > wheel.suspension_max ||
            !nonnegative_finite(wheel.suspension_frequency) ||
            !nonnegative_finite(wheel.suspension_damping) ||
            !nonnegative_finite(wheel.max_steer_angle) ||
            !nonnegative_finite(wheel.max_brake_torque)) {
            return fail(ErrorCode::InvalidArgument, "physics vehicle: invalid wheel");
        }
    }
    f32 torque_sum = 0.0f;
    for (u32 index = 0; index < description.differential_count; ++index) {
        const VehicleDifferentialDescription& differential = description.differentials[index];
        if (differential.left_wheel >= description.wheel_count ||
            differential.right_wheel >= description.wheel_count ||
            differential.left_wheel == differential.right_wheel ||
            !nonnegative_finite(differential.torque_fraction) ||
            !math::is_finite(differential.ratio) || differential.ratio <= 0.0f) {
            return fail(ErrorCode::InvalidArgument, "physics vehicle: invalid differential");
        }
        torque_sum += differential.torque_fraction;
    }
    if (!math::is_finite(torque_sum) || torque_sum < 0.999f || torque_sum > 1.001f) {
        return fail(ErrorCode::InvalidArgument,
                    "physics vehicle: torque fractions must sum to one");
    }
    return ok();
}

Status validate(const VehicleInput& input) noexcept {
    if (!math::is_finite(input.throttle) || input.throttle < -1.0f || input.throttle > 1.0f ||
        !math::is_finite(input.steering) || input.steering < -1.0f || input.steering > 1.0f ||
        !math::is_finite(input.brake) || input.brake < 0.0f || input.brake > 1.0f ||
        !math::is_finite(input.hand_brake) || input.hand_brake < 0.0f || input.hand_brake > 1.0f) {
        return fail(ErrorCode::InvalidArgument, "physics vehicle: driver input outside its range");
    }
    return ok();
}

}  // namespace cy::physics
