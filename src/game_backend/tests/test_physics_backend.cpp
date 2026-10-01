// SPDX-License-Identifier: MIT
// integration.game_backend_physics — `PhysicsQueryAdapter` against the reference physics server,
// through the ABI table. `add-swift-game-api`, task 2.2.
//
// The unit suite (src/abi/tests/test_game_physics.cpp) proves the thunks; this proves what the
// adapter promises on top of the server: hits carry entities, every list is in the total order
// (equal distances by entity, whatever order the bodies were created in), the same query twice is
// byte-identical, the ignore list and the layer mask are honoured, identical shapes are created
// once, and a query during the step is UNAVAILABLE in every build.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/physics_backend.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/servers/physics/server.h>
#include <cy/test/test.h>

#include <cstring>

#include "physics_fixture.h"

namespace {

CyRay ray_along_x(f32 y = 0.0F) noexcept {
    CyRay ray{};
    ray.origin[0] = -10.0F;
    ray.origin[1] = y;
    ray.direction[0] = 1.0F;
    ray.max_distance = 100.0F;
    return ray;
}

}  // namespace

CY_TEST_CASE("a ray through the table hits the nearest body and names the entity that owns it") {
    Scene scene;
    (void)scene.box(501, Vec3{5.0F, 0.0F, 0.0F});
    (void)scene.box(502, Vec3{0.0F, 0.0F, 0.0F});
    game_backend::PhysicsQueryAdapter adapter(allocator(), scene.server, scene.world, scene.bodies);
    game_backend::bind(scene.host, &adapter);

    const CyRay ray = ray_along_x();
    CyPhysicsHit hit{};
    bool has_hit = false;
    CY_REQUIRE(table().physics_raycast(scene.engine(), &ray, nullptr, &hit, &has_hit) ==
               CY_RESULT_OK);
    CY_CHECK(has_hit);
    CY_CHECK_EQ(hit.entity, 502U);
    CY_CHECK_NEAR(hit.distance, 9.5F, 1e-4F);
    CY_CHECK_NEAR(hit.fraction, 0.095F, 1e-5F);
    CY_CHECK_NEAR(hit.normal[0], -1.0F, 1e-4F);
    CY_CHECK_NEAR(hit.point[0], -0.5F, 1e-4F);

    // A ray that misses is OK with no hit.
    const CyRay above = ray_along_x(5.0F);
    CY_REQUIRE(table().physics_raycast(scene.engine(), &above, nullptr, &hit, &has_hit) ==
               CY_RESULT_OK);
    CY_CHECK_FALSE(has_hit);
    game_backend::bind(scene.host, nullptr);
}

CY_TEST_CASE("equal distances come back in entity order, whatever order the bodies were made in") {
    Scene scene;
    // Created highest entity first, so neither creation order nor handle order is entity order.
    (void)scene.box(930, Vec3{0.0F, 0.0F, 0.0F});
    (void)scene.box(710, Vec3{0.0F, 0.0F, 0.0F});
    (void)scene.box(820, Vec3{0.0F, 0.0F, 0.0F});
    (void)scene.box(600, Vec3{4.0F, 0.0F, 0.0F});
    game_backend::PhysicsQueryAdapter adapter(allocator(), scene.server, scene.world, scene.bodies);
    game_backend::bind(scene.host, &adapter);
    const abi::game::PhaseScope fixed(scene.host.game.clock, CY_PHASE_FIXED_UPDATE);

    const CyRay ray = ray_along_x();
    CyPhysicsHit first[8] = {};
    u32 count = 0;
    CY_REQUIRE(table().physics_raycast_all(scene.engine(), &ray, nullptr, first, 8, &count) ==
               CY_RESULT_OK);
    CY_REQUIRE(count == 4U);
    CY_CHECK_EQ(first[0].entity, 710U);
    CY_CHECK_EQ(first[1].entity, 820U);
    CY_CHECK_EQ(first[2].entity, 930U);
    CY_CHECK_EQ(first[3].entity, 600U);

    // The single-hit query agrees with the head of the list rather than picking among equals.
    CyPhysicsHit nearest{};
    bool has_hit = false;
    CY_REQUIRE(table().physics_raycast(scene.engine(), &ray, nullptr, &nearest, &has_hit) ==
               CY_RESULT_OK);
    CY_CHECK_EQ(nearest.entity, 710U);

    // The same query in the same world is the same bytes.
    CyPhysicsHit second[8] = {};
    CY_REQUIRE(table().physics_raycast_all(scene.engine(), &ray, nullptr, second, 8, &count) ==
               CY_RESULT_OK);
    CY_CHECK(same_bytes(first, second, sizeof(first)));

    // A short buffer holds the nearest two, in the same order, and reports the total.
    CyPhysicsHit two[2] = {};
    CY_CHECK_EQ(table().physics_raycast_all(scene.engine(), &ray, nullptr, two, 2, &count),
                CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(count, 4U);
    CY_CHECK(same_bytes(two, first, sizeof(two)));
    game_backend::bind(scene.host, nullptr);
}

CY_TEST_CASE("the ignore list skips an entity's body and the next one is hit") {
    Scene scene;
    (void)scene.box(11, Vec3{0.0F, 0.0F, 0.0F});
    (void)scene.box(12, Vec3{5.0F, 0.0F, 0.0F});
    game_backend::PhysicsQueryAdapter adapter(allocator(), scene.server, scene.world, scene.bodies);
    game_backend::bind(scene.host, &adapter);

    const CyEntity self = 11;
    CyQueryFilter filter{};
    filter.mask = 0xFFFFFFFFU;
    filter.ignore = &self;
    filter.ignore_count = 1;
    const CyRay ray = ray_along_x();
    CyPhysicsHit hit{};
    bool has_hit = false;
    CY_REQUIRE(table().physics_raycast(scene.engine(), &ray, &filter, &hit, &has_hit) ==
               CY_RESULT_OK);
    CY_CHECK(has_hit);
    CY_CHECK_EQ(hit.entity, 12U);

    // A sweep honours it too — the case a post-filter could not handle.
    CyShape sphere{};
    sphere.kind = CY_SHAPE_SPHERE;
    sphere.radius = 0.25F;
    CyPose start{};
    start.position[0] = -10.0F;
    const float direction[3] = {1.0F, 0.0F, 0.0F};
    CY_REQUIRE(table().physics_shape_cast(scene.engine(), &sphere, &start, direction, 100.0F,
                                          &filter, &hit, &has_hit) == CY_RESULT_OK);
    CY_CHECK(has_hit);
    CY_CHECK_EQ(hit.entity, 12U);
    CY_CHECK_NEAR(hit.distance, 14.25F, 1e-3F);
    game_backend::bind(scene.host, nullptr);
}

CY_TEST_CASE("the layer mask decides what a query can hit") {
    Scene scene;
    (void)scene.box(21, Vec3{0.0F, 0.0F, 0.0F}, 0.5F, 2);
    (void)scene.box(22, Vec3{5.0F, 0.0F, 0.0F}, 0.5F, 3);
    game_backend::PhysicsQueryAdapter adapter(allocator(), scene.server, scene.world, scene.bodies);
    game_backend::bind(scene.host, &adapter);

    const CyRay ray = ray_along_x();
    CyQueryFilter filter{};
    filter.mask = 1U << 3U;  // units on layer 3 only
    CyPhysicsHit hits[4] = {};
    u32 count = 0;
    CY_REQUIRE(table().physics_raycast_all(scene.engine(), &ray, &filter, hits, 4, &count) ==
               CY_RESULT_OK);
    CY_REQUIRE(count == 1U);
    CY_CHECK_EQ(hits[0].entity, 22U);

    filter.mask = 0;
    CY_REQUIRE(table().physics_raycast_all(scene.engine(), &ray, &filter, hits, 4, &count) ==
               CY_RESULT_OK);
    CY_CHECK_EQ(count, 0U);
    game_backend::bind(scene.host, nullptr);
}

CY_TEST_CASE("an overlap lists each entity once, in entity order, and reuses its shape") {
    Scene scene;
    (void)scene.box(44, Vec3{1.0F, 0.0F, 0.0F});
    (void)scene.box(33, Vec3{-1.0F, 0.0F, 0.0F});
    (void)scene.box(55, Vec3{0.0F, 0.0F, 1.0F});
    (void)scene.box(66, Vec3{20.0F, 0.0F, 0.0F});
    game_backend::PhysicsQueryAdapter adapter(allocator(), scene.server, scene.world, scene.bodies);
    game_backend::bind(scene.host, &adapter);

    CyShape box{};
    box.kind = CY_SHAPE_BOX;
    box.half_extents[0] = 2.0F;
    box.half_extents[1] = 2.0F;
    box.half_extents[2] = 2.0F;
    const CyPose pose{};
    CyEntity entities[8] = {};
    u32 count = 0;
    CY_REQUIRE(table().physics_overlap(scene.engine(), &box, &pose, nullptr, entities, 8, &count) ==
               CY_RESULT_OK);
    CY_REQUIRE(count == 3U);
    CY_CHECK_EQ(entities[0], 33U);
    CY_CHECK_EQ(entities[1], 44U);
    CY_CHECK_EQ(entities[2], 55U);
    CY_CHECK_EQ(adapter.cached_shapes(), 1U);

    // The same shape again is a cache hit, not a second server shape.
    CY_REQUIRE(table().physics_overlap(scene.engine(), &box, &pose, nullptr, nullptr, 0, &count) ==
               CY_RESULT_OK);
    CY_CHECK_EQ(count, 3U);
    CY_CHECK_EQ(adapter.cached_shapes(), 1U);
    game_backend::bind(scene.host, nullptr);
}

CY_TEST_CASE("a query during the physics step is UNAVAILABLE, in every build") {
    Scene scene;
    (void)scene.box(1, Vec3{0.0F, 0.0F, 0.0F});
    game_backend::PhysicsQueryAdapter adapter(allocator(), scene.server, scene.world, scene.bodies);
    game_backend::bind(scene.host, &adapter);
    scene.server.forced_stepping = true;

    const CyRay ray = ray_along_x();
    CyPhysicsHit hit{};
    bool has_hit = false;
    CY_CHECK_EQ(table().physics_raycast(scene.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_UNAVAILABLE);
    u32 count = 0;
    CY_CHECK_EQ(table().physics_raycast_all(scene.engine(), &ray, nullptr, nullptr, 0, &count),
                CY_RESULT_UNAVAILABLE);
    CyShape sphere{};
    sphere.kind = CY_SHAPE_SPHERE;
    sphere.radius = 1.0F;
    const CyPose pose{};
    CY_CHECK_EQ(
        table().physics_overlap(scene.engine(), &sphere, &pose, nullptr, nullptr, 0, &count),
        CY_RESULT_UNAVAILABLE);
    const float direction[3] = {1.0F, 0.0F, 0.0F};
    CY_CHECK_EQ(table().physics_shape_cast(scene.engine(), &sphere, &pose, direction, 1.0F, nullptr,
                                           &hit, &has_hit),
                CY_RESULT_UNAVAILABLE);

    scene.server.forced_stepping = false;
    CY_CHECK_EQ(table().physics_raycast(scene.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_OK);
    CY_CHECK(has_hit);
    game_backend::bind(scene.host, nullptr);
    abi::clear_last_error();
}
