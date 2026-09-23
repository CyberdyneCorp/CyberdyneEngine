// SPDX-License-Identifier: MIT
#include <cy/physics/buoyancy/driver.h>

namespace cy::physics::buoyancy {

Expected<water::BuoyancyResult, Error> Driver::apply(BodyHandle body,
                                                     Span<const water::BuoyancySample> hull,
                                                     const water::BuoyancyParams& params,
                                                     world::WorldVec3d origin) noexcept {
    const auto state = physics_.body_state(body);
    if (!state) {
        return make_unexpected(state.error());
    }
    if (state->motion != MotionType::Dynamic) {
        return fail(ErrorCode::InvalidArgument, "buoyancy: body must be dynamic");
    }
    if (Status sized = samples_.resize(hull.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (usize index = 0; index < hull.size(); ++index) {
        samples_[index] = hull[index];
        samples_[index].offset = state->transform.rotation * hull[index].offset;
    }
    water::BuoyancyState floating;
    floating.position =
        world::WorldVec3d{origin.x + static_cast<f64>(state->transform.translation.x),
                          origin.y + static_cast<f64>(state->transform.translation.y),
                          origin.z + static_cast<f64>(state->transform.translation.z)};
    floating.velocity = state->linear_velocity;
    floating.angular_velocity = state->angular_velocity;
    const auto result = water_.buoyancy(floating, samples_.span(), params);
    if (!result) {
        return make_unexpected(result.error());
    }
    if (Status force = physics_.add_force(body, result->force); !force) {
        return make_unexpected(force.error());
    }
    if (Status torque = physics_.add_torque(body, result->torque); !torque) {
        return make_unexpected(torque.error());
    }
    return *result;
}

}  // namespace cy::physics::buoyancy
