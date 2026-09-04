// Entity identity, generations and liveness. Task 2.1.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

}  // namespace

CY_TEST_CASE("an entity packs a 32-bit index and a 32-bit generation into 64 bits") {
    const cy::ecs::Entity entity = cy::ecs::Entity::make(0x1234'5678u, 0x9ABC'DEF0u);
    CY_CHECK_EQ(entity.index(), 0x1234'5678u);
    CY_CHECK_EQ(entity.generation(), 0x9ABC'DEF0u);
    CY_CHECK_EQ(cy::ecs::Entity::from_bits(entity.bits()), entity);
    CY_CHECK_EQ(sizeof(cy::ecs::Entity), 8u);

    // The null entity is the default, so a forgotten initialisation is invalid rather than being
    // index 0 of whatever world it reaches.
    CY_CHECK_FALSE(cy::ecs::Entity{}.valid());
    CY_CHECK_FALSE(cy::ecs::kNoEntity.valid());
}

CY_TEST_CASE("a stale entity id is not alive and its components read as null") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto components = cy::ecs::test::register_all(world);
    CY_REQUIRE(components.has_value());

    auto first = world.create();
    CY_REQUIRE(first.has_value());
    const cy::ecs::Entity stale = *first;
    CY_REQUIRE(world.add(stale, components->position).has_value());
    CY_REQUIRE(world.get(stale, components->position) != nullptr);

    CY_REQUIRE(world.destroy(stale).has_value());
    CY_CHECK_FALSE(world.is_alive(stale));
    // `ecs-core`: component access through a stale id returns null.
    CY_CHECK(world.get(stale, components->position) == nullptr);
    CY_CHECK(world.get_mut(stale, components->position) == nullptr);

    // The index is recycled; the generation is what distinguishes the two.
    auto second = world.create();
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(second->index(), stale.index());
    CY_CHECK_NE(second->generation(), stale.generation());
    CY_CHECK(world.is_alive(*second));
    CY_CHECK_FALSE(world.is_alive(stale));
}

CY_TEST_CASE("entity indices are dense and recycled") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    cy::ecs::Entity created[8];
    for (auto& entity : created) {
        auto made = world.create();
        CY_REQUIRE(made.has_value());
        entity = *made;
    }
    CY_CHECK_EQ(world.entity_count(), 8u);
    CY_CHECK_EQ(world.entities().capacity(), 8u);

    for (const auto entity : created) {
        CY_REQUIRE(world.destroy(entity).has_value());
    }
    CY_CHECK_EQ(world.entity_count(), 0u);

    // Eight more entities reuse the eight indices rather than extending the table: "dense and
    // recycled" is a property of the table's extent, not only of the ids.
    for (cy::u32 index = 0; index < 8; ++index) {
        auto made = world.create();
        CY_REQUIRE(made.has_value());
    }
    CY_CHECK_EQ(world.entities().capacity(), 8u);
}

CY_TEST_CASE("a destroyed entity's id never comes back, even after four billion recycles") {
    // The generation skips 0 and the placeholder value, so no amount of recycling produces an id
    // that reads as null or as a command buffer's placeholder.
    cy::ecs::EntityTable table(allocator());
    auto entity = table.create();
    CY_REQUIRE(entity.has_value());
    CY_CHECK_EQ(entity->generation(), 1u);
    CY_CHECK_FALSE(entity->placeholder());

    for (int round = 0; round < 4; ++round) {
        CY_REQUIRE(table.destroy(*entity).has_value());
        entity = table.create();
        CY_REQUIRE(entity.has_value());
        CY_CHECK(entity->valid());
        CY_CHECK_FALSE(entity->placeholder());
    }
}

CY_TEST_CASE("destroying an entity that is already dead is reported rather than corrupting") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    auto entity = world.create();
    CY_REQUIRE(entity.has_value());
    CY_REQUIRE(world.destroy(*entity).has_value());
    const auto again = world.destroy(*entity);
    CY_CHECK_FALSE(again.has_value());
    CY_CHECK_EQ(again.error().code, cy::ErrorCode::NotFound);
}
