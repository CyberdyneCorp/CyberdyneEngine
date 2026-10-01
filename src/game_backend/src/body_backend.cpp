// SPDX-License-Identifier: MIT
// The rigid-body adapter: `cy::abi::game::PhysicsBodyBackend` over `cy::physics::PhysicsServer`.
// `add-swift-m12-gaps`. See include/cy/game_backend/physics_backend.h for what it adds to the
// server's own calls, and tests/test_bodies_backend.cpp for the proof.

#include <cy/abi/errors.h>
#include <cy/game_backend/physics_backend.h>
#include <cy/servers/physics/server.h>

namespace cy::game_backend {
namespace {

Vec3 vec3(const f32* value) noexcept {
    return Vec3{value[0], value[1], value[2]};
}

void write3(f32* out, Vec3 value) noexcept {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

CyResult answer(const Status& status) noexcept {
    if (!status) {
        return abi::report(status.error());
    }
    return CY_RESULT_OK;
}

}  // namespace

CyResult PhysicsBodyAdapter::resolve(CyEntity entity, bool dynamic_only,
                                     physics::BodyHandle& out) const noexcept {
    if (server_->stepping()) {
        return abi::report(CY_RESULT_UNAVAILABLE,
                           "the physics step is running; bodies are written before or after it");
    }
    const physics::BodyHandle body = bodies_->body_of(entity);
    if (body.is_null() || !server_->body_alive(body)) {
        return abi::report(CY_RESULT_NOT_FOUND, "the entity owns no physics body");
    }
    const Expected<physics::BodyState, Error> state = server_->body_state(body);
    if (!state) {
        return abi::report(state.error());
    }
    if (state->motion == physics::MotionType::Static ||
        (dynamic_only && state->motion != physics::MotionType::Dynamic)) {
        return abi::report(CY_RESULT_INVALID_ARGUMENT,
                           dynamic_only ? "only a dynamic body is moved by a force or an impulse"
                                        : "a static body has no velocity to set");
    }
    out = body;
    return CY_RESULT_OK;
}

CyResult PhysicsBodyAdapter::apply_force(CyEntity entity, const f32* force) noexcept {
    physics::BodyHandle body;
    if (const CyResult found = resolve(entity, true, body); found != CY_RESULT_OK) {
        return found;
    }
    if (const CyResult added = answer(server_->add_force(body, vec3(force)));
        added != CY_RESULT_OK) {
        return added;
    }
    return answer(server_->set_body_awake(body, true));
}

CyResult PhysicsBodyAdapter::apply_impulse(CyEntity entity, const f32* impulse,
                                           const f32* point) noexcept {
    physics::BodyHandle body;
    if (const CyResult found = resolve(entity, true, body); found != CY_RESULT_OK) {
        return found;
    }
    return answer(point == nullptr ? server_->add_impulse(body, vec3(impulse))
                                   : server_->add_impulse_at(body, vec3(impulse), vec3(point)));
}

CyResult PhysicsBodyAdapter::apply_torque(CyEntity entity, const f32* torque) noexcept {
    physics::BodyHandle body;
    if (const CyResult found = resolve(entity, true, body); found != CY_RESULT_OK) {
        return found;
    }
    if (const CyResult added = answer(server_->add_torque(body, vec3(torque)));
        added != CY_RESULT_OK) {
        return added;
    }
    return answer(server_->set_body_awake(body, true));
}

CyResult PhysicsBodyAdapter::set_velocity(CyEntity entity, const f32* linear,
                                          const f32* angular) noexcept {
    physics::BodyHandle body;
    if (const CyResult found = resolve(entity, false, body); found != CY_RESULT_OK) {
        return found;
    }
    // A null half keeps the body's own: the server sets both at once, so read the one not given.
    const Expected<physics::BodyState, Error> state = server_->body_state(body);
    if (!state) {
        return abi::report(state.error());
    }
    const Vec3 new_linear = linear != nullptr ? vec3(linear) : state->linear_velocity;
    const Vec3 new_angular = angular != nullptr ? vec3(angular) : state->angular_velocity;
    return answer(server_->set_body_velocity(body, new_linear, new_angular));
}

CyResult PhysicsBodyAdapter::velocity(CyEntity entity, f32* out_linear,
                                      f32* out_angular) const noexcept {
    const physics::BodyHandle body = bodies_->body_of(entity);
    if (body.is_null() || !server_->body_alive(body)) {
        return abi::report(CY_RESULT_NOT_FOUND, "the entity owns no physics body");
    }
    const Expected<physics::BodyState, Error> state = server_->body_state(body);
    if (!state) {
        return abi::report(state.error());
    }
    write3(out_linear, state->linear_velocity);
    write3(out_angular, state->angular_velocity);
    return CY_RESULT_OK;
}

void bind_bodies(cy::abi::Host& host, PhysicsBodyAdapter* adapter) noexcept {
    host.game.bodies = adapter;
}

}  // namespace cy::game_backend
