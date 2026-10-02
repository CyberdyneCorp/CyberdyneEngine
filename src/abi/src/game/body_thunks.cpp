// SPDX-License-Identifier: MIT
// The rigid-body thunks of ABI 1.5: `physics_apply_force`, `physics_apply_impulse`,
// `physics_apply_torque`, `physics_set_velocity` and `physics_get_velocity`. `add-swift-m12-gaps`.
//
// The writes are simulation: `[N F]`, refused in a frame update, where a force would land in one
// peer's world and not another's. The read is `[N F U]`. Every vector the caller passes is checked
// finite here, whatever backend answers, so the fake one `unit.abi` binds sees the refusal the real
// one would. None of them is structural.

#include <cy/abi/errors.h>
#include <cy/abi/game/physics.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kWritePhases = kPhaseNone | kPhaseFixed;

/// The engine, the phase and the backend; null having reported why.
[[nodiscard]] PhysicsBodyBackend* enter(CyEngine engine, u32 phases, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, phases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.bodies == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no physics body backend is bound to this engine");
    }
    return engine->game.bodies;
}

/// Null is allowed where the entry says so; a non-null vector must be finite.
[[nodiscard]] bool finite_or_null(const float* xyz) noexcept {
    return xyz == nullptr ||
           (std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]));
}

/// The common shape of the three accumulating writes: a required, finite vector.
[[nodiscard]] CyResult check_vector(CyEntity entity, const float* xyz, const char* entry) noexcept {
    if (entity == CY_ENTITY_NULL || xyz == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, entry);
    }
    if (!finite_or_null(xyz)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the vector must be finite");
    }
    return CY_RESULT_OK;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

}  // namespace

CyResult physics_apply_force(CyEngine engine, CyEntity entity, const float* force_xyz) {
    PhysicsBodyBackend* backend = enter(engine, kWritePhases, "physics_apply_force");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked =
            check_vector(entity, force_xyz, "physics_apply_force needs an entity and a force");
        checked != CY_RESULT_OK) {
        return checked;
    }
    return finish(backend->apply_force(entity, force_xyz));
}

CyResult physics_apply_impulse(CyEngine engine, CyEntity entity, const float* impulse_xyz,
                               const float* point_xyz) {
    PhysicsBodyBackend* backend = enter(engine, kWritePhases, "physics_apply_impulse");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked = check_vector(
            entity, impulse_xyz, "physics_apply_impulse needs an entity and an impulse");
        checked != CY_RESULT_OK) {
        return checked;
    }
    if (!finite_or_null(point_xyz)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the point of application must be finite");
    }
    return finish(backend->apply_impulse(entity, impulse_xyz, point_xyz));
}

CyResult physics_apply_torque(CyEngine engine, CyEntity entity, const float* torque_xyz) {
    PhysicsBodyBackend* backend = enter(engine, kWritePhases, "physics_apply_torque");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked =
            check_vector(entity, torque_xyz, "physics_apply_torque needs an entity and a torque");
        checked != CY_RESULT_OK) {
        return checked;
    }
    return finish(backend->apply_torque(entity, torque_xyz));
}

CyResult physics_set_velocity(CyEngine engine, CyEntity entity, const float* linear_xyz,
                              const float* angular_xyz) {
    PhysicsBodyBackend* backend = enter(engine, kWritePhases, "physics_set_velocity");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL) {
        return report(CY_RESULT_INVALID_ARGUMENT, "physics_set_velocity needs an entity");
    }
    if (!finite_or_null(linear_xyz) || !finite_or_null(angular_xyz)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "a velocity must be finite");
    }
    if (linear_xyz == nullptr && angular_xyz == nullptr) {
        clear_last_error();
        return CY_RESULT_OK;  // both halves kept: nothing to write, and nothing wrong
    }
    return finish(backend->set_velocity(entity, linear_xyz, angular_xyz));
}

CyResult physics_get_velocity(CyEngine engine, CyEntity entity, float* out_linear_xyz,
                              float* out_angular_xyz) {
    PhysicsBodyBackend* backend = enter(engine, kPhaseAny, "physics_get_velocity");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL) {
        return report(CY_RESULT_INVALID_ARGUMENT, "physics_get_velocity needs an entity");
    }
    // The backend always writes both; a caller that wanted only one passed null for the other.
    float linear[3] = {};
    float angular[3] = {};
    if (const CyResult result = backend->velocity(entity, linear, angular);
        result != CY_RESULT_OK) {
        return result;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (out_linear_xyz != nullptr) {
            out_linear_xyz[axis] = linear[axis];
        }
        if (out_angular_xyz != nullptr) {
            out_angular_xyz[axis] = angular[axis];
        }
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
