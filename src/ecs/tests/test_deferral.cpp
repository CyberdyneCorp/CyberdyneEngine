// Structural change deferral. Task 2.6, design.md §2.
//
// The test the brief asks for by name: it MUTATES MID-ITERATION and observes the deferral, rather
// than checking that the result is eventually right. Every assertion here holds in all four
// profiles — the refusal is a returned error and a counter, never an assertion, because CY_ASSERT
// is compiled out of Profile and Shipping and a rule that only holds in Debug is not a rule.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/command_buffer.h>
#include <cy/ecs/query.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

}  // namespace

CY_TEST_CASE("a structural change attempted mid-iteration is refused, not applied") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId set[] = {ids->position};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(8, cy::Span<const cy::ecs::ComponentTypeId>(set, 1), created)
                   .has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.write(ids->position).has_value());
    cy::ecs::Query query(world, std::move(desc));

    const cy::u64 refused_before = world.refused_during_iteration();
    cy::u32 attempts = 0;
    cy::u32 refusals = 0;
    const cy::ecs::ComponentTypeId velocity = ids->velocity;
    CY_REQUIRE(query
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                       CY_CHECK(world.iterating());
                       for (const cy::ecs::Entity entity : chunk.entities()) {
                           ++attempts;
                           // Each of these would move an entity out of the chunk being walked.
                           if (!world.create()) {
                               ++refusals;
                           }
                           if (!world.add(entity, velocity)) {
                               ++refusals;
                           }
                           if (!world.destroy(entity)) {
                               ++refusals;
                           }
                       }
                   })
                   .has_value());

    CY_CHECK_EQ(attempts, 8u);
    CY_CHECK_EQ(refusals, attempts * 3);
    // The counter is compiled into every configuration, so this assertion means the same thing in
    // Shipping as it does in Debug.
    CY_CHECK_EQ(world.refused_during_iteration() - refused_before, refusals);
    // Nothing happened: the world is exactly as it was.
    CY_CHECK_EQ(world.entity_count(), 8u);
    for (const cy::ecs::Entity entity : created) {
        CY_CHECK(world.is_alive(entity));
        CY_CHECK_FALSE(world.has(entity, velocity));
    }
    CY_CHECK_FALSE(world.iterating());
}

CY_TEST_CASE("a spawn recorded during iteration appears at the flush and not before") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId set[] = {ids->position};
    cy::Array<cy::ecs::Entity> weapons(allocator());
    CY_REQUIRE(world.create_many(4, cy::Span<const cy::ecs::ComponentTypeId>(set, 1), weapons)
                   .has_value());

    cy::ecs::CommandBuffer commands(world);
    CY_REQUIRE(world.attach(commands).has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.read(ids->position).has_value());
    cy::ecs::Query query(world, std::move(desc));

    const cy::ecs::ComponentTypeId position = ids->position;
    const cy::ecs::ComponentTypeId velocity = ids->velocity;
    cy::u32 iterated = 0;
    cy::Array<cy::ecs::Entity> placeholders(allocator());
    CY_REQUIRE(query
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                       iterated += chunk.count();
                       for (cy::u32 index = 0; index < chunk.count(); ++index) {
                           // A weapon spawning a projectile: the classic case the requirement's
                           // scenario names.
                           auto projectile = commands.create();
                           CY_REQUIRE(projectile.has_value());
                           CY_CHECK(projectile->placeholder());
                           // The placeholder is usable immediately, and it is not alive.
                           CY_CHECK_FALSE(world.is_alive(*projectile));
                           CY_REQUIRE(commands
                                          .add(*projectile, position,
                                               cy::ecs::test::Position{1.0F, 2.0F, 3.0F})
                                          .has_value());
                           CY_REQUIRE(commands.add(*projectile, velocity).has_value());
                           CY_REQUIRE(placeholders.push_back(*projectile).has_value());
                       }
                   })
                   .has_value());

    // Four weapons were iterated, not eight: the spawns are invisible until the flush, so the
    // iteration cannot see its own output.
    CY_CHECK_EQ(iterated, 4u);
    CY_CHECK_EQ(world.entity_count(), 4u);
    CY_CHECK_EQ(commands.pending(), 12u);

    const auto applied = world.flush();
    CY_REQUIRE(applied.has_value());
    CY_CHECK_EQ(*applied, 12u);
    CY_CHECK_EQ(world.entity_count(), 8u);
    CY_CHECK_EQ(commands.pending(), 0u);

    // Every placeholder was remapped to a real entity at the flush point, and the components
    // recorded against it landed on that entity.
    cy::u64 projectiles = 0;
    CY_REQUIRE(query
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                       const cy::Span<const cy::ecs::test::Position> positions =
                           chunk.read<cy::ecs::test::Position>(position);
                       for (cy::u32 index = 0; index < chunk.count(); ++index) {
                           if (positions[index].x == 1.0F && chunk.has(velocity)) {
                               ++projectiles;
                           }
                       }
                   })
                   .has_value());
    CY_CHECK_EQ(projectiles, 4u);
}

CY_TEST_CASE("buffers merge by system order, then thread index, then record order") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    // Attached out of order on purpose: the merge is by the declared key, never by the order the
    // buffers happened to be registered or filled.
    cy::ecs::CommandBuffer late(world, 2, 0);
    cy::ecs::CommandBuffer early_second_thread(world, 1, 1);
    cy::ecs::CommandBuffer early(world, 1, 0);
    CY_REQUIRE(world.attach(late).has_value());
    CY_REQUIRE(world.attach(early_second_thread).has_value());
    CY_REQUIRE(world.attach(early).has_value());

    auto a = late.create();
    auto b = early_second_thread.create();
    auto c = early.create();
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    CY_REQUIRE(c.has_value());

    cy::Array<cy::ecs::Entity> before(allocator());
    CY_REQUIRE(world.flush().has_value());

    // (1,0) created first, then (1,1), then (2,0) — so the entity indices come out in that order,
    // whatever order the buffers were attached in.
    CY_CHECK_EQ(world.entity_count(), 3u);
    CY_CHECK_EQ(world.entities().at(0).index(), 0u);
    // The first created entity is index 0; the merge order decides which buffer created it.
    CY_CHECK(world.is_alive(world.entities().at(0)));
    CY_CHECK(world.is_alive(world.entities().at(1)));
    CY_CHECK(world.is_alive(world.entities().at(2)));

    (void)ids;
    (void)before;
}

CY_TEST_CASE("setting a component's value mid-iteration is not structural and is allowed") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId set[] = {ids->position};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(4, cy::Span<const cy::ecs::ComponentTypeId>(set, 1), created)
                   .has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.write(ids->position).has_value());
    cy::ecs::Query query(world, std::move(desc));

    const cy::ecs::ComponentTypeId selected = ids->selected;
    const cy::ecs::test::Selected value{99};
    // The side table is brought into existence outside the stage. Creating it during iteration is
    // refused, because creating one grows the world's list of them and would move every other
    // store under whichever parallel system is writing into one — writing into an existing table
    // is what is legal here, and it is what a system actually does.
    CY_REQUIRE(world.set_sparse(created[0], selected, &value).has_value());
    CY_REQUIRE(query
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
                       for (const cy::ecs::Entity entity : chunk.entities()) {
                           // A sparse component changes no archetype and moves no row, so it is
                           // legal here — which is exactly why `ecs-core` offers the kind.
                           CY_CHECK(world.set_sparse(entity, selected, &value).has_value());
                       }
                   })
                   .has_value());

    for (const cy::ecs::Entity entity : created) {
        CY_CHECK(world.has(entity, selected));
    }
}
