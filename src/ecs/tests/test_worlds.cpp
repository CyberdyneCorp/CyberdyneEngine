// Multiple worlds, and bulk instantiation from prepared archetype blocks. Task 2.11.

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

CY_TEST_CASE("two worlds share no entities, archetypes or component numbering state") {
    cy::ecs::WorldConfig edited_config;
    edited_config.name = "edited";
    cy::ecs::WorldConfig played_config;
    played_config.name = "play-mode";

    cy::ecs::World edited(allocator(), edited_config);
    cy::ecs::World played(allocator(), played_config);
    CY_REQUIRE(edited.initialize().has_value());
    CY_REQUIRE(played.initialize().has_value());

    const auto edited_ids = cy::ecs::test::register_all(edited);
    CY_REQUIRE(edited_ids.has_value());

    auto entity = edited.create();
    CY_REQUIRE(entity.has_value());
    CY_REQUIRE(edited.add(*entity, edited_ids->position).has_value());

    // The edited world's entity is not the played world's, even though both tables would issue the
    // same index: an entity id means something only in the world that issued it.
    CY_CHECK_EQ(edited.entity_count(), 1u);
    CY_CHECK_EQ(played.entity_count(), 0u);
    CY_CHECK_FALSE(played.is_alive(*entity));
    CY_CHECK_EQ(played.archetypes().size(), 0u);
    CY_CHECK(cy::ecs::test::same_text(edited.name(), "edited"));
    CY_CHECK(cy::ecs::test::same_text(played.name(), "play-mode"));

    // Component ids are positions in a world's own registry. The played world has registered
    // nothing, so the type that means Position in one world means nothing in the other.
    CY_CHECK(played.components().find(cy::ecs::test::position_type().id) == nullptr);
    CY_CHECK(edited.components().find(cy::ecs::test::position_type().id) != nullptr);

    const auto played_ids = cy::ecs::test::register_all(played);
    CY_REQUIRE(played_ids.has_value());
    // Registering in the same order gives the same numbers, which is what makes a snapshot from
    // one restorable into the other.
    CY_CHECK_EQ(played_ids->position, edited_ids->position);

    auto own = played.create();
    CY_REQUIRE(own.has_value());
    CY_REQUIRE(played.add(*own, played_ids->position).has_value());
    CY_CHECK_EQ(edited.entity_count(), 1u);
    CY_CHECK_EQ(played.entity_count(), 1u);
}

CY_TEST_CASE("a block whose columns do not match its component list is refused") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId components[] = {ids->position, ids->velocity};
    const void* columns[] = {nullptr};
    cy::ecs::World::ArchetypeBlock block;
    block.components = cy::Span<const cy::ecs::ComponentTypeId>(components, 2);
    block.columns = cy::Span<const void* const>(columns, 1);
    block.count = 4;

    cy::Array<cy::ecs::Entity> created(allocator());
    const auto refused = world.instantiate(block, created);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::InvalidArgument);
    CY_CHECK_EQ(world.entity_count(), 0u);
}
