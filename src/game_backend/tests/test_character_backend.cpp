// SPDX-License-Identifier: MIT
// integration.game_backend_character — `CharacterAdapter` over the reference physics server,
// through the ABI table. `add-swift-m12-gaps`.
//
// What the adapter promises: a zeroed description is a valid character, one per entity; a move in
// a fixed step walks it along the ground and the state reads back at once, grounded and naming the
// ground's entity; its body carries the entity, so a ray cast names the character; the same moves
// on two worlds give the same bytes; and a move during the step is UNAVAILABLE.

#include <cy/game_backend/character_backend.h>

#include "physics_fixture.h"

namespace {

constexpr CyEntity kGround = 1;
constexpr CyEntity kHero = 2;

/// A floor 40 m square with its top face at y = 0, owned by `kGround`.
void floor(Scene& scene) {
    (void)scene.box(kGround, Vec3{0.0F, -20.0F, 0.0F}, 20.0F);
}

CyCharacterDesc standing_at(f32 x) noexcept {
    CyCharacterDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.start.position[0] = x;
    desc.start.position[1] = 0.95F;  // the default 1.8 m capsule's centre, just above the floor
    return desc;
}

CyCharacterState state_of(Scene& scene, CyEntity entity) {
    CyCharacterState state{};
    state.struct_size = sizeof(state);
    CY_REQUIRE_EQ(table().character_state(scene.engine(), entity, &state), CY_RESULT_OK);
    return state;
}

/// Walk `ticks` fixed steps at `speed` m/s along +X, returning the final state.
CyCharacterState walk(Scene& scene, u32 ticks, f32 speed) {
    const abi::game::PhaseScope fixed(scene.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CyCharacterInput input{};
    input.struct_size = sizeof(input);
    input.desired_velocity[0] = speed;
    for (u32 tick = 0; tick < ticks; ++tick) {
        CY_REQUIRE_EQ(table().character_move(scene.engine(), kHero, &input), CY_RESULT_OK);
    }
    return state_of(scene, kHero);
}

}  // namespace

CY_TEST_CASE("a character created from a zeroed description walks and stands on the ground") {
    Scene scene;
    floor(scene);
    game_backend::CharacterAdapter adapter(allocator(), scene.server, scene.world);
    game_backend::bind_characters(scene.host, &adapter);

    const CyCharacterDesc desc = standing_at(0.0F);
    CY_REQUIRE_EQ(table().character_create(scene.engine(), kHero, &desc), CY_RESULT_OK);
    CY_CHECK_EQ(table().character_create(scene.engine(), kHero, &desc), CY_RESULT_ALREADY_EXISTS);
    CY_CHECK_EQ(adapter.count(), 1U);

    // One second at 3 m/s: about three metres along X, on the ground, the floor named.
    const CyCharacterState state = walk(scene, 60, 3.0F);
    CY_CHECK_GT(state.position[0], 2.5F);
    CY_CHECK_LT(state.position[0], 3.5F);
    CY_CHECK_EQ(state.ground, CY_GROUND_GROUNDED);
    CY_CHECK_EQ(state.ground_entity, kGround);

    CY_CHECK_EQ(table().character_destroy(scene.engine(), kHero), CY_RESULT_OK);
    CY_CHECK_EQ(table().character_destroy(scene.engine(), kHero), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(adapter.count(), 0U);
    game_backend::bind_characters(scene.host, nullptr);
    abi::clear_last_error();
}

CY_TEST_CASE("a character's body carries its entity, so a ray cast names the character") {
    Scene scene;
    floor(scene);
    game_backend::CharacterAdapter adapter(allocator(), scene.server, scene.world);
    game_backend::bind_characters(scene.host, &adapter);
    game_backend::PhysicsQueryAdapter queries(allocator(), scene.server, scene.world, adapter);
    game_backend::bind(scene.host, &queries);
    const CyCharacterDesc desc = standing_at(5.0F);
    CY_REQUIRE_EQ(table().character_create(scene.engine(), kHero, &desc), CY_RESULT_OK);

    CyRay ray{};
    ray.origin[0] = -5.0F;
    ray.origin[1] = 1.0F;
    ray.direction[0] = 1.0F;
    ray.max_distance = 50.0F;
    CyPhysicsHit hit{};
    bool has_hit = false;
    CY_REQUIRE_EQ(table().physics_raycast(scene.engine(), &ray, nullptr, &hit, &has_hit),
                  CY_RESULT_OK);
    CY_REQUIRE(has_hit);
    CY_CHECK_EQ(hit.entity, kHero);

    // And the adapter is the ignore list's map: skipping the hero, the ray hits nothing.
    CyQueryFilter filter{};
    filter.struct_size = sizeof(filter);
    filter.mask = 0xFFFFFFFFU;
    const CyEntity hero = kHero;
    filter.ignore = &hero;
    filter.ignore_count = 1;
    CY_REQUIRE_EQ(table().physics_raycast(scene.engine(), &ray, &filter, &hit, &has_hit),
                  CY_RESULT_OK);
    CY_CHECK_FALSE(has_hit);
    game_backend::bind(scene.host, nullptr);
    game_backend::bind_characters(scene.host, nullptr);
}

CY_TEST_CASE("the same moves give the same character, bit for bit") {
    CyCharacterState results[2] = {};
    for (CyCharacterState& result : results) {
        Scene scene;
        floor(scene);
        game_backend::CharacterAdapter adapter(allocator(), scene.server, scene.world);
        game_backend::bind_characters(scene.host, &adapter);
        const CyCharacterDesc desc = standing_at(0.0F);
        CY_REQUIRE_EQ(table().character_create(scene.engine(), kHero, &desc), CY_RESULT_OK);
        result = walk(scene, 90, 4.0F);
        game_backend::bind_characters(scene.host, nullptr);
    }
    CY_CHECK(same_bytes(&results[0], &results[1], sizeof(CyCharacterState)));
}

CY_TEST_CASE("a character move during the physics step is UNAVAILABLE, in every build") {
    Scene scene;
    floor(scene);
    game_backend::CharacterAdapter adapter(allocator(), scene.server, scene.world);
    game_backend::bind_characters(scene.host, &adapter);
    const CyCharacterDesc desc = standing_at(0.0F);
    CY_REQUIRE_EQ(table().character_create(scene.engine(), kHero, &desc), CY_RESULT_OK);
    const abi::game::PhaseScope fixed(scene.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CyCharacterInput input{};
    input.struct_size = sizeof(input);
    scene.server.forced_stepping = true;
    CY_CHECK_EQ(table().character_move(scene.engine(), kHero, &input), CY_RESULT_UNAVAILABLE);
    scene.server.forced_stepping = false;
    CY_CHECK_EQ(table().character_move(scene.engine(), kHero, &input), CY_RESULT_OK);
    game_backend::bind_characters(scene.host, nullptr);
    abi::clear_last_error();
}
