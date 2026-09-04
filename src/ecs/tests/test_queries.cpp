// Query matching, caching and filtering. Tasks 2.4 and 2.8.

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

CY_TEST_CASE("With, Without and Optional select the archetypes they say they do") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId moving[] = {ids->position, ids->velocity};
    const cy::ecs::ComponentTypeId still[] = {ids->position};
    const cy::ecs::ComponentTypeId frozen[] = {ids->position, ids->velocity, ids->frozen};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(3, cy::Span<const cy::ecs::ComponentTypeId>(moving, 2), created)
                   .has_value());
    CY_REQUIRE(world.create_many(5, cy::Span<const cy::ecs::ComponentTypeId>(still, 1), created)
                   .has_value());
    CY_REQUIRE(world.create_many(7, cy::Span<const cy::ecs::ComponentTypeId>(frozen, 3), created)
                   .has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.read(ids->position).has_value());
    CY_REQUIRE(desc.read(ids->velocity).has_value());
    CY_REQUIRE(desc.without(ids->frozen).has_value());
    cy::ecs::Query query(world, std::move(desc));
    CY_REQUIRE(query.refresh().has_value());
    CY_CHECK_EQ(query.entity_count(), 3u);

    cy::ecs::QueryDesc any_position(allocator());
    CY_REQUIRE(any_position.read(ids->position).has_value());
    CY_REQUIRE(any_position.optional(ids->velocity).has_value());
    cy::ecs::Query all(world, std::move(any_position));
    CY_CHECK_EQ(all.entity_count(), 15u);

    // An Optional term does not constrain matching; the body tests for it per chunk.
    const cy::ecs::ComponentTypeId velocity = ids->velocity;
    cy::u64 with_velocity = 0;
    CY_REQUIRE(all.for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                      if (chunk.has(velocity)) {
                          with_velocity += chunk.count();
                      } else {
                          CY_CHECK(chunk.read<cy::ecs::test::Velocity>(velocity).empty());
                      }
                  })
                   .has_value());
    CY_CHECK_EQ(with_velocity, 10u);
}

CY_TEST_CASE("a query's archetype list is cached and updated incrementally") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.read(ids->position).has_value());
    cy::ecs::Query query(world, std::move(desc));
    CY_REQUIRE(query.refresh().has_value());
    CY_CHECK_EQ(query.matched_archetypes(), 0u);

    auto first = world.create();
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(world.add(*first, ids->position).has_value());
    CY_REQUIRE(query.refresh().has_value());
    CY_CHECK_EQ(query.matched_archetypes(), 1u);

    // A world with a stable archetype set costs nothing to re-match: the cached list is what
    // iteration walks, and refresh only tests archetypes it has never seen.
    for (int round = 0; round < 32; ++round) {
        CY_REQUIRE(query.refresh().has_value());
    }
    CY_CHECK_EQ(query.matched_archetypes(), 1u);

    auto second = world.create();
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(world.add(*second, ids->position).has_value());
    CY_REQUIRE(world.add(*second, ids->velocity).has_value());
    CY_REQUIRE(query.refresh().has_value());
    CY_CHECK_EQ(query.matched_archetypes(), 2u);
}

CY_TEST_CASE("a read does not dirty and a change filter skips untouched chunks") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId set[] = {ids->position, ids->velocity};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(4, cy::Span<const cy::ecs::ComponentTypeId>(set, 2), created)
                   .has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.read(ids->position).has_value());
    desc.filter_changed(ids->position);
    cy::ecs::Query changed(world, std::move(desc));

    // First run sees the chunk: the rows were added at the current version.
    CY_REQUIRE(changed.for_each_chunk([](cy::ecs::QueryChunk&) {}).has_value());
    CY_CHECK_EQ(changed.stats().chunks_visited, 1u);

    world.advance_version();

    // A pure read leaves the version alone, so the next filtered run skips the chunk entirely.
    cy::ecs::QueryDesc reader(allocator());
    CY_REQUIRE(reader.read(ids->position).has_value());
    cy::ecs::Query reads(world, std::move(reader));
    const cy::ecs::ComponentTypeId position = ids->position;
    cy::f32 total = 0.0F;
    CY_REQUIRE(reads
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                       for (const auto& value : chunk.read<cy::ecs::test::Position>(position)) {
                           total += value.x;
                       }
                   })
                   .has_value());
    CY_CHECK_EQ(total, 0.0F);

    CY_REQUIRE(changed.for_each_chunk([](cy::ecs::QueryChunk&) {}).has_value());
    CY_CHECK_EQ(changed.stats().chunks_visited, 0u);
    CY_CHECK_EQ(changed.stats().chunks_skipped, 1u);

    // A write does bump it, and the whole chunk is considered changed — chunk-granular, as the
    // specification documents it, not entity-granular.
    world.advance_version();
    cy::ecs::QueryDesc writer(allocator());
    CY_REQUIRE(writer.write(ids->position).has_value());
    cy::ecs::Query writes(world, std::move(writer));
    CY_REQUIRE(writes
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                       const cy::Span<cy::ecs::test::Position> positions =
                           chunk.write<cy::ecs::test::Position>(position);
                       CY_REQUIRE_FALSE(positions.empty());
                       positions[0].x = 1.0F;
                   })
                   .has_value());

    CY_REQUIRE(changed.for_each_chunk([](cy::ecs::QueryChunk&) {}).has_value());
    CY_CHECK_EQ(changed.stats().chunks_visited, 1u);
    CY_CHECK_EQ(changed.stats().chunks_skipped, 0u);
}

CY_TEST_CASE("a shared-component filter selects only the chunks carrying that value") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::test::Material stone{1};
    const cy::ecs::test::Material glass{2};
    const auto stone_value = world.intern_shared(ids->material, &stone);
    const auto glass_value = world.intern_shared(ids->material, &glass);
    CY_REQUIRE(stone_value.has_value());
    CY_REQUIRE(glass_value.has_value());

    for (cy::u32 index = 0; index < 6; ++index) {
        auto entity = world.create();
        CY_REQUIRE(entity.has_value());
        CY_REQUIRE(world.add(*entity, ids->position).has_value());
        CY_REQUIRE(
            world.set_shared(*entity, ids->material, (index % 3 == 0) ? *glass_value : *stone_value)
                .has_value());
    }

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.read(ids->position).has_value());
    CY_REQUIRE(desc.with(ids->material).has_value());
    desc.filter_shared(ids->material, *stone_value);
    cy::ecs::Query query(world, std::move(desc));
    CY_CHECK_EQ(query.entity_count(), 4u);

    const cy::ecs::ComponentTypeId material = ids->material;
    CY_REQUIRE(query
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                       const auto* value =
                           static_cast<const cy::ecs::test::Material*>(chunk.shared(material));
                       CY_REQUIRE(value != nullptr);
                       CY_CHECK_EQ(value->id, 1u);
                   })
                   .has_value());
}

CY_TEST_CASE("a query's terms are its access declaration") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.write(ids->position).has_value());
    CY_REQUIRE(desc.read(ids->velocity).has_value());
    CY_REQUIRE(desc.without(ids->frozen).has_value());

    // A system declares what its query reads because it *is* the query's declaration; there is no
    // second list to drift from the first.
    cy::jobs::Access access = cy::jobs::Access::Read;
    CY_REQUIRE(desc.access().find(cy::jobs::AccessDomain::Component, ids->position, access));
    CY_CHECK_EQ(access, cy::jobs::Access::Write);
    CY_REQUIRE(desc.access().find(cy::jobs::AccessDomain::Component, ids->velocity, access));
    CY_CHECK_EQ(access, cy::jobs::Access::Read);
    CY_REQUIRE(desc.access().find(cy::jobs::AccessDomain::Component, ids->frozen, access));
    CY_CHECK_EQ(access, cy::jobs::Access::Exclude);

    // Naming a component this world has not registered is caught where the term is written.
    cy::ecs::QueryDesc broken(allocator());
    const auto refused = broken.read(cy::ecs::kInvalidComponent);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::NotFound);
}

CY_TEST_CASE("random access finds a component outside iteration and is documented as slower") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    auto entity = world.create();
    CY_REQUIRE(entity.has_value());
    CY_REQUIRE(world.add(*entity, ids->position).has_value());
    CY_REQUIRE(
        world.set(*entity, ids->position, cy::ecs::test::Position{4.0F, 5.0F, 6.0F}).has_value());

    // A table lookup and a binary search over the archetype's columns. Inappropriate for bulk work,
    // which is why the iteration path above never uses it.
    const auto* position = world.get<cy::ecs::test::Position>(*entity, ids->position);
    CY_REQUIRE(position != nullptr);
    CY_CHECK_EQ(position->y, 5.0F);
    CY_CHECK(world.get(*entity, ids->velocity) == nullptr);
}
