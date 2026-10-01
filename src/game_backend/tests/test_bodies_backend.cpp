// SPDX-License-Identifier: MIT
// integration.game_backend_bodies — `PhysicsBodyAdapter` against the reference physics server,
// through the ABI table. `add-swift-m12-gaps`.
//
// The unit suite (src/abi/tests/test_game_bodies.cpp) proves the thunks; this proves what the
// adapter adds: an entity's body moves under a force, an impulse and a torque from the table; a
// static or kinematic body is refused rather than silently ignored; a velocity half can be kept;
// an entity with no body is NOT_FOUND; and every write is UNAVAILABLE during the step.

#include "physics_fixture.h"

namespace {

constexpr f32 kStep = 1.0F / 60.0F;

void step(Scene& scene) {
    StepInput input;
    input.delta_seconds = kStep;
    CY_REQUIRE(scene.server.step(scene.world, input).has_value());
}

Vec3 velocity_of(Scene& scene, CyEntity entity) {
    float linear[3] = {};
    CY_REQUIRE_EQ(table().physics_get_velocity(scene.engine(), entity, linear, nullptr),
                  CY_RESULT_OK);
    return Vec3{linear[0], linear[1], linear[2]};
}

}  // namespace

CY_TEST_CASE("a force, an impulse and a torque from the table move the entity's dynamic body") {
    Scene scene;
    (void)scene.dynamic_box(7, Vec3{0.0F, 5.0F, 0.0F});
    game_backend::PhysicsBodyAdapter adapter(scene.server, scene.bodies);
    game_backend::bind_bodies(scene.host, &adapter);
    const abi::game::PhaseScope fixed(scene.host.game.clock, CY_PHASE_FIXED_UPDATE);

    // An impulse changes the velocity at once, before any step.
    const float impulse[3] = {0.0F, 0.0F, 4.0F};
    CY_REQUIRE_EQ(table().physics_apply_impulse(scene.engine(), 7, impulse, nullptr), CY_RESULT_OK);
    CY_CHECK_GT(velocity_of(scene, 7).z, 0.0F);

    // A force accumulates and is applied by the next step.
    const float force[3] = {50.0F, 0.0F, 0.0F};
    CY_REQUIRE_EQ(table().physics_apply_force(scene.engine(), 7, force), CY_RESULT_OK);
    CY_CHECK_EQ(velocity_of(scene, 7).x, 0.0F);
    step(scene);
    CY_CHECK_GT(velocity_of(scene, 7).x, 0.0F);

    // A torque spins it.
    const float torque[3] = {0.0F, 20.0F, 0.0F};
    CY_REQUIRE_EQ(table().physics_apply_torque(scene.engine(), 7, torque), CY_RESULT_OK);
    step(scene);
    float angular[3] = {};
    CY_REQUIRE_EQ(table().physics_get_velocity(scene.engine(), 7, nullptr, angular), CY_RESULT_OK);
    CY_CHECK_GT(angular[1], 0.0F);
    game_backend::bind_bodies(scene.host, nullptr);
}

CY_TEST_CASE("set_velocity replaces one half and keeps the other") {
    Scene scene;
    (void)scene.dynamic_box(7, Vec3{0.0F, 5.0F, 0.0F});
    game_backend::PhysicsBodyAdapter adapter(scene.server, scene.bodies);
    game_backend::bind_bodies(scene.host, &adapter);

    const float linear[3] = {1.0F, 2.0F, 3.0F};
    const float angular[3] = {0.0F, 0.5F, 0.0F};
    CY_REQUIRE_EQ(table().physics_set_velocity(scene.engine(), 7, linear, angular), CY_RESULT_OK);
    const float faster[3] = {4.0F, 5.0F, 6.0F};
    CY_REQUIRE_EQ(table().physics_set_velocity(scene.engine(), 7, faster, nullptr), CY_RESULT_OK);
    float read_linear[3] = {};
    float read_angular[3] = {};
    CY_REQUIRE_EQ(table().physics_get_velocity(scene.engine(), 7, read_linear, read_angular),
                  CY_RESULT_OK);
    CY_CHECK_EQ(read_linear[0], 4.0F);
    CY_CHECK_EQ(read_linear[2], 6.0F);
    CY_CHECK_EQ(read_angular[1], 0.5F);
    game_backend::bind_bodies(scene.host, nullptr);
}

CY_TEST_CASE("a body that cannot move is refused, and an entity with none is not found") {
    Scene scene;
    (void)scene.box(3, Vec3{0.0F, 0.0F, 0.0F});  // static
    game_backend::PhysicsBodyAdapter adapter(scene.server, scene.bodies);
    game_backend::bind_bodies(scene.host, &adapter);

    const float push[3] = {1.0F, 0.0F, 0.0F};
    CY_CHECK_EQ(table().physics_apply_force(scene.engine(), 3, push), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().physics_apply_impulse(scene.engine(), 3, push, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().physics_set_velocity(scene.engine(), 3, push, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().physics_apply_force(scene.engine(), 99, push), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().physics_get_velocity(scene.engine(), 99, nullptr, nullptr),
                CY_RESULT_NOT_FOUND);
    game_backend::bind_bodies(scene.host, nullptr);
    abi::clear_last_error();
}

CY_TEST_CASE("a body write during the physics step is UNAVAILABLE, in every build") {
    Scene scene;
    (void)scene.dynamic_box(7, Vec3{0.0F, 5.0F, 0.0F});
    game_backend::PhysicsBodyAdapter adapter(scene.server, scene.bodies);
    game_backend::bind_bodies(scene.host, &adapter);
    scene.server.forced_stepping = true;
    const float push[3] = {1.0F, 0.0F, 0.0F};
    CY_CHECK_EQ(table().physics_apply_force(scene.engine(), 7, push), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().physics_apply_impulse(scene.engine(), 7, push, nullptr),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().physics_set_velocity(scene.engine(), 7, push, nullptr),
                CY_RESULT_UNAVAILABLE);
    scene.server.forced_stepping = false;
    CY_CHECK_EQ(table().physics_apply_force(scene.engine(), 7, push), CY_RESULT_OK);
    game_backend::bind_bodies(scene.host, nullptr);
    abi::clear_last_error();
}
