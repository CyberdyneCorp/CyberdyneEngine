// Bulk creation, bulk instantiation and chunk accounting at scale. Tasks 2.1, 2.3 and 2.11.
//
// Integration rather than unit, and measured rather than assumed: 100 000 entities are the number
// `ecs-core`'s scenarios name, and even the thousand-entity cases at the end of this file take
// 1.0-2.1 ms in the Debug and Profile configurations — over the unit suite's one-millisecond
// budget, which is the taxonomy telling them where they belong.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/query.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

}  // namespace

CY_TEST_CASE("100 000 entities of one component set are allocated into chunks in bulk") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    constexpr cy::u32 kCount = 100'000;
    const cy::ecs::ComponentTypeId set[] = {ids->position, ids->velocity};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(kCount, cy::Span<const cy::ecs::ComponentTypeId>(set, 2), created)
                   .has_value());

    CY_CHECK_EQ(created.size(), kCount);
    CY_CHECK_EQ(world.entity_count(), kCount);

    const cy::ecs::WorldStats stats = world.stats();
    // One archetype: the set was resolved once for the whole batch, not once per entity.
    CY_CHECK_EQ(stats.archetypes, 1u);
    CY_CHECK_EQ(stats.entities, kCount);
    // Chunks are filled rather than sprinkled: every chunk but the last is full.
    const cy::ecs::Archetype& archetype = world.archetypes().at(0);
    CY_CHECK_EQ(stats.chunks, (kCount + archetype.capacity() - 1) / archetype.capacity());
    CY_CHECK_GT(stats.fill_ratio, 0.99);
    // `create_many` is one structural change, not a hundred thousand of them.
    CY_CHECK_EQ(world.structural_changes(), 1u);

    // Every entity is addressable and lands in block order.
    CY_CHECK_EQ(cy::ecs::test::row_of(world, created[0]), 0u);
    CY_CHECK(world.is_alive(created[kCount - 1]));

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
                       for (cy::u32 row = 0; row < chunk.count(); ++row) {
                           positions[row].x += velocities[row].x + 1.0F;
                       }
                       seen += chunk.count();
                   })
                   .has_value());
    CY_CHECK_EQ(seen, kCount);
    CY_CHECK_EQ(query.stats().entities_visited, kCount);
    CY_CHECK_EQ(query.stats().matched_archetypes, 1u);

    CY_REQUIRE(world.destroy_many(created.span()).has_value());
    CY_CHECK_EQ(world.entity_count(), 0u);
    CY_CHECK_GT(world.trim(), 0u);
}

CY_TEST_CASE("a cooked block of 100 000 rows activates as one memcpy per column per chunk") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    constexpr cy::u32 kRows = 100'000;
    cy::Array<cy::ecs::test::Position> positions(allocator());
    CY_REQUIRE(positions.resize(kRows).has_value());
    for (cy::u32 row = 0; row < kRows; ++row) {
        positions[row].x = static_cast<cy::f32>(row % 1024);
    }

    const cy::ecs::ComponentTypeId components[] = {ids->position};
    const void* columns[] = {static_cast<const void*>(positions.data())};
    cy::ecs::World::ArchetypeBlock block;
    block.components = cy::Span<const cy::ecs::ComponentTypeId>(components, 1);
    block.columns = cy::Span<const void* const>(columns, 1);
    block.count = kRows;

    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.instantiate(block, created).has_value());
    CY_CHECK_EQ(created.size(), kRows);

    // Spot-check the copy at the chunk boundaries, where an off-by-one in the run arithmetic would
    // land.
    const cy::u32 capacity = world.archetypes().at(0).capacity();
    for (cy::u32 row = 0; row < kRows; row += capacity) {
        const auto* value = world.get<cy::ecs::test::Position>(created[row], ids->position);
        CY_REQUIRE(value != nullptr);
        CY_CHECK_EQ(value->x, static_cast<cy::f32>(row % 1024));
    }
    const auto* last = world.get<cy::ecs::test::Position>(created[kRows - 1], ids->position);
    CY_REQUIRE(last != nullptr);
    CY_CHECK_EQ(last->x, static_cast<cy::f32>((kRows - 1) % 1024));
}

CY_TEST_CASE("a prepared archetype block is instantiated as a bulk column copy") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    // What a cooked cell or an entity template hands over: whole columns, already laid out.
    constexpr cy::u32 kRows =
        1200;  // more than one chunk holds, so the run arithmetic is exercised
    cy::Array<cy::ecs::test::Position> positions(allocator());
    cy::Array<cy::ecs::test::Velocity> velocities(allocator());
    CY_REQUIRE(positions.resize(kRows).has_value());
    CY_REQUIRE(velocities.resize(kRows).has_value());
    for (cy::u32 row = 0; row < kRows; ++row) {
        positions[row] = cy::ecs::test::Position{static_cast<cy::f32>(row), 0.0F, 0.0F};
        velocities[row] = cy::ecs::test::Velocity{0.0F, static_cast<cy::f32>(row), 0.0F};
    }

    const cy::ecs::ComponentTypeId components[] = {ids->position, ids->velocity};
    const void* columns[] = {static_cast<const void*>(positions.data()),
                             static_cast<const void*>(velocities.data())};

    cy::ecs::World::ArchetypeBlock block;
    block.components = cy::Span<const cy::ecs::ComponentTypeId>(components, 2);
    block.columns = cy::Span<const void* const>(columns, 2);
    block.count = kRows;

    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.instantiate(block, created).has_value());
    CY_CHECK_EQ(created.size(), kRows);
    CY_CHECK_EQ(world.entity_count(), kRows);

    // The rows arrived in block order, and the data is what the block held — no per-entity
    // construction happened on the way in.
    for (cy::u32 row = 0; row < kRows; ++row) {
        const auto* position = world.get<cy::ecs::test::Position>(created[row], ids->position);
        const auto* velocity = world.get<cy::ecs::test::Velocity>(created[row], ids->velocity);
        CY_REQUIRE(position != nullptr);
        CY_REQUIRE(velocity != nullptr);
        CY_CHECK_EQ(position->x, static_cast<cy::f32>(row));
        CY_CHECK_EQ(velocity->y, static_cast<cy::f32>(row));
    }
    // Every entity landed in one archetype, and it spans more than one chunk.
    const cy::u32 archetype = cy::ecs::test::archetype_of(world, created[0]);
    CY_CHECK_EQ(cy::ecs::test::archetype_of(world, created[kRows - 1]), archetype);
    CY_CHECK_GT(world.archetypes().at(archetype).chunk_count(), 1u);
}

CY_TEST_CASE("the ECS allocates its chunks through M1's budget tree and gives them back") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId set[] = {ids->position};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(2000, cy::Span<const cy::ecs::ComponentTypeId>(set, 1), created)
                   .has_value());
    const cy::ecs::WorldStats filled = world.stats();
    CY_CHECK_GT(filled.chunks, 1u);
    CY_CHECK_GT(filled.committed_bytes, 0u);
    CY_CHECK_GT(filled.fill_ratio, 0.5);

    CY_REQUIRE(world.destroy_many(created.span()).has_value());
    // trim() is the pressure response: the chunks go back to the allocator they came from, which is
    // the one M6's residency policy will hold allocations from. The ECS has none of its own.
    CY_CHECK_GT(world.trim(), 0u);
    CY_CHECK_EQ(world.stats().entities, 0u);
}
