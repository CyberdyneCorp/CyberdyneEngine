// Archetypes over M1's chunked storage. Task 2.3.

#include <cy/test/test.h>

#include <cy/core/memory/chunk_storage.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/query.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

}  // namespace

CY_TEST_CASE("component addition order does not change the archetype") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    auto first = world.create();
    auto second = world.create();
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    CY_REQUIRE(world.add(*first, ids->position).has_value());
    CY_REQUIRE(world.add(*first, ids->velocity).has_value());
    CY_REQUIRE(world.add(*second, ids->velocity).has_value());
    CY_REQUIRE(world.add(*second, ids->position).has_value());

    // The mask is the identity and the sorted list is only its expansion, so the order they arrived
    // in cannot be observed afterwards.
    CY_CHECK_EQ(cy::ecs::test::archetype_of(world, *first),
                cy::ecs::test::archetype_of(world, *second));
}

CY_TEST_CASE("chunk capacity is derived from the archetype's per-entity size") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId one[] = {ids->position};
    const cy::ecs::ComponentTypeId two[] = {ids->position, ids->velocity};
    auto narrow = world.create(cy::Span<const cy::ecs::ComponentTypeId>(one, 1));
    auto wide = world.create(cy::Span<const cy::ecs::ComponentTypeId>(two, 2));
    CY_REQUIRE(narrow.has_value());
    CY_REQUIRE(wide.has_value());

    const cy::ecs::Archetype& narrow_archetype =
        world.archetypes().at(cy::ecs::test::archetype_of(world, *narrow));
    const cy::ecs::Archetype& wide_archetype =
        world.archetypes().at(cy::ecs::test::archetype_of(world, *wide));

    // A wider row means fewer of them per 16 KiB. The number itself is `ChunkLayout`'s; what this
    // asserts is that the ECS asked it rather than assuming one.
    CY_CHECK_GT(narrow_archetype.capacity(), wide_archetype.capacity());
    CY_CHECK_EQ(narrow_archetype.layout().chunk_bytes(),
                static_cast<cy::u32>(cy::kDefaultChunkBytes));
    CY_CHECK_EQ(narrow_archetype.layout().key_size(),
                static_cast<cy::u32>(sizeof(cy::ecs::Entity)));
}

CY_TEST_CASE("an archetype transition moves the data and repairs the vacated slot") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    cy::ecs::Entity entities[3];
    for (cy::u32 index = 0; index < 3; ++index) {
        auto made = world.create();
        CY_REQUIRE(made.has_value());
        entities[index] = *made;
        CY_REQUIRE(world.add(entities[index], ids->position).has_value());
        const cy::ecs::test::Position position{static_cast<cy::f32>(index), 0.0F, 0.0F};
        CY_REQUIRE(world.set(entities[index], ids->position, position).has_value());
    }

    // Moving the first entity out fills its slot with the chunk's last entity; that entity's
    // location record has to follow it, and `ChunkStore::remove_row` reports the move precisely so
    // that the layer above can do it.
    CY_REQUIRE(world.add(entities[0], ids->velocity).has_value());
    for (cy::u32 index = 0; index < 3; ++index) {
        const auto* position = world.get<cy::ecs::test::Position>(entities[index], ids->position);
        CY_REQUIRE(position != nullptr);
        CY_CHECK_EQ(position->x, static_cast<cy::f32>(index));
    }
    CY_CHECK(world.has(entities[0], ids->velocity));
    CY_CHECK_FALSE(world.has(entities[1], ids->velocity));

    // Removing it again lands the entity back in the archetype it started in.
    CY_REQUIRE(world.remove(entities[0], ids->velocity).has_value());
    CY_CHECK_EQ(cy::ecs::test::archetype_of(world, entities[0]),
                cy::ecs::test::archetype_of(world, entities[1]));
    CY_CHECK_EQ(
        cy::ecs::test::value_of<cy::ecs::test::Position>(world, entities[0], ids->position).x,
        0.0F);
}

CY_TEST_CASE("iteration walks contiguous spans with no per-row indirection") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId set[] = {ids->position, ids->velocity};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(64, cy::Span<const cy::ecs::ComponentTypeId>(set, 2), created)
                   .has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.write(ids->position).has_value());
    CY_REQUIRE(desc.read(ids->velocity).has_value());
    cy::ecs::Query query(world, std::move(desc));

    const cy::ecs::ComponentTypeId position = ids->position;
    const cy::ecs::ComponentTypeId velocity = ids->velocity;
    cy::u64 seen = 0;
    CY_REQUIRE(query
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                       const cy::Span<cy::ecs::test::Position> positions =
                           chunk.write<cy::ecs::test::Position>(position);
                       const cy::Span<const cy::ecs::test::Velocity> velocities =
                           chunk.read<cy::ecs::test::Velocity>(velocity);
                       CY_CHECK_EQ(positions.size(), chunk.count());
                       CY_CHECK_EQ(velocities.size(), chunk.count());
                       // The loop body a compiler can vectorise: two spans, walked forward.
                       for (cy::u32 row = 0; row < chunk.count(); ++row) {
                           positions[row].x += velocities[row].x + 1.0F;
                       }
                       seen += chunk.count();
                   })
                   .has_value());
    CY_CHECK_EQ(seen, 64u);
    for (const cy::ecs::Entity entity : created) {
        CY_CHECK_EQ(cy::ecs::test::value_of<cy::ecs::test::Position>(world, entity, position).x,
                    1.0F);
    }
}
