// Component registration and the five storage kinds. Task 2.2.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/demo/types.h>
#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

}  // namespace

CY_TEST_CASE("a component is registered through M1's reflection and keeps its layout") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    // A real generated descriptor, not a fixture: `cy::demo::Health` is a committed reflected type
    // with a manifest identifier, and this is the path a game component takes.
    const auto health = world.register_component<cy::demo::Health>();
    CY_REQUIRE(health.has_value());
    const cy::ecs::ComponentInfo& info = world.components().info(*health);
    CY_CHECK_EQ(info.size, static_cast<cy::u32>(sizeof(cy::demo::Health)));
    CY_CHECK_EQ(info.alignment, static_cast<cy::u32>(alignof(cy::demo::Health)));
    CY_CHECK_EQ(info.type_id, cy::reflect::type_id_of<cy::demo::Health>());
    CY_CHECK_EQ(info.kind, cy::ecs::ComponentKind::Data);

    // Registering the same type twice is a module registered by two consumers, not an error.
    const auto again = world.register_component<cy::demo::Health>();
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(*again, *health);

    CY_CHECK_EQ(cy::ecs::component_id_of<cy::demo::Health>(world.components()), *health);
    CY_CHECK_EQ(cy::ecs::component_id_of<cy::demo::Placement>(world.components()),
                cy::ecs::kInvalidComponent);
}

CY_TEST_CASE("a component that is not trivially relocatable is refused") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    cy::reflect::TypeInfo hostile;
    hostile.name = "cy::ecs::test::NotRelocatable";
    hostile.id = cy::reflect::TypeId(9100);
    hostile.size = 16;
    hostile.alignment = 8;
    hostile.trivially_relocatable = false;

    const auto registered = world.components().register_reflected(hostile);
    CY_REQUIRE_FALSE(registered.has_value());
    // Chunk compaction moves a row's bytes with memcpy and nothing else; a type whose move is not a
    // copy of its bytes would be corrupted by the first archetype transition, silently.
    CY_CHECK_EQ(registered.error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("a tag occupies no column but is part of the archetype key") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    CY_CHECK_EQ(world.components().info(ids->frozen).size, 0u);

    auto entity = world.create();
    CY_REQUIRE(entity.has_value());
    CY_REQUIRE(world.add(*entity, ids->position).has_value());
    const cy::u32 before = world.archetypes().size();

    CY_REQUIRE(world.add(*entity, ids->frozen).has_value());
    CY_CHECK(world.has(*entity, ids->frozen));
    // Presence only: it changed the archetype, and it has no storage to read.
    CY_CHECK_GT(world.archetypes().size(), before);
    CY_CHECK(world.get(*entity, ids->frozen) == nullptr);
}

CY_TEST_CASE("a sparse component toggles without an archetype change") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    auto entity = world.create();
    CY_REQUIRE(entity.has_value());
    CY_REQUIRE(world.add(*entity, ids->position).has_value());
    const cy::u32 archetypes = world.archetypes().size();
    const cy::u64 transitions = world.archetype_transitions();

    const cy::ecs::test::Selected selected{42};
    for (int round = 0; round < 16; ++round) {
        CY_REQUIRE(world.set_sparse(*entity, ids->selected, &selected).has_value());
        CY_CHECK(world.has(*entity, ids->selected));
        CY_REQUIRE(world.remove_sparse(*entity, ids->selected).has_value());
        CY_CHECK_FALSE(world.has(*entity, ids->selected));
    }
    // Sixteen toggles, no archetype churn at all. That is the whole reason the kind exists.
    CY_CHECK_EQ(world.archetypes().size(), archetypes);
    CY_CHECK_EQ(world.archetype_transitions(), transitions);

    CY_REQUIRE(world.set_sparse(*entity, ids->selected, &selected).has_value());
    const auto* value =
        static_cast<const cy::ecs::test::Selected*>(world.get_sparse(*entity, ids->selected));
    CY_REQUIRE(value != nullptr);
    CY_CHECK_EQ(value->tick, 42u);
}

CY_TEST_CASE("a sparse entry does not survive its entity's index being recycled") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    auto first = world.create();
    CY_REQUIRE(first.has_value());
    const cy::ecs::test::Selected selected{7};
    CY_REQUIRE(world.set_sparse(*first, ids->selected, &selected).has_value());
    const cy::ecs::Entity stale = *first;
    CY_REQUIRE(world.destroy(stale).has_value());

    auto second = world.create();
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(second->index(), stale.index());
    CY_CHECK(world.get_sparse(*second, ids->selected) == nullptr);
    CY_CHECK(world.get_sparse(stale, ids->selected) == nullptr);
}

CY_TEST_CASE("entities sharing a shared component group into the same chunks") {
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
    CY_CHECK_NE(*stone_value, *glass_value);
    // Interning is by value: the same material twice is the same index, so archetype identity is an
    // integer comparison rather than a memcmp of the payload.
    const auto again = world.intern_shared(ids->material, &stone);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(*again, *stone_value);

    cy::ecs::Entity stones[4];
    cy::ecs::Entity glasses[4];
    for (int index = 0; index < 4; ++index) {
        auto first = world.create();
        auto second = world.create();
        CY_REQUIRE(first.has_value());
        CY_REQUIRE(second.has_value());
        stones[index] = *first;
        glasses[index] = *second;
        CY_REQUIRE(world.add(stones[index], ids->position).has_value());
        CY_REQUIRE(world.add(glasses[index], ids->position).has_value());
        CY_REQUIRE(world.set_shared(stones[index], ids->material, *stone_value).has_value());
        CY_REQUIRE(world.set_shared(glasses[index], ids->material, *glass_value).has_value());
    }

    // Two archetypes, one per value: the whole point of the kind is that a renderer can submit a
    // chunk without a per-entity material lookup.
    const cy::u32 stone_archetype = cy::ecs::test::archetype_of(world, stones[0]);
    const cy::u32 glass_archetype = cy::ecs::test::archetype_of(world, glasses[0]);
    CY_CHECK_NE(stone_archetype, glass_archetype);
    for (int index = 1; index < 4; ++index) {
        CY_CHECK_EQ(cy::ecs::test::archetype_of(world, stones[index]), stone_archetype);
        CY_CHECK_EQ(cy::ecs::test::archetype_of(world, glasses[index]), glass_archetype);
    }
    const auto value = world.shared_of(stones[2], ids->material);
    CY_REQUIRE(value.has_value());
    CY_CHECK_EQ(*value, *stone_value);
    const auto* material =
        static_cast<const cy::ecs::test::Material*>(world.shared_value(ids->material, *value));
    CY_REQUIRE(material != nullptr);
    CY_CHECK_EQ(material->id, 1u);
}

CY_TEST_CASE("a buffer component is inline until it is not, and spills to the heap after") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    auto entity = world.create();
    CY_REQUIRE(entity.has_value());
    CY_REQUIRE(world.add(*entity, ids->waypoints).has_value());

    auto buffer = world.buffer<cy::ecs::test::Waypoint>(*entity, ids->waypoints);
    CY_REQUIRE(buffer.has_value());
    CY_CHECK_EQ(buffer->size(), 0u);
    CY_CHECK_EQ(buffer->capacity(), 4u);  // the inline capacity registered above

    for (cy::u32 index = 0; index < 4; ++index) {
        CY_REQUIRE(buffer->push_back(cy::ecs::test::Waypoint{static_cast<cy::f32>(index), 0.0F})
                       .has_value());
    }
    CY_CHECK_EQ(buffer->capacity(), 4u);

    // The fifth element spills. Everything already written survives the spill.
    CY_REQUIRE(buffer->push_back(cy::ecs::test::Waypoint{4.0F, 0.0F}).has_value());
    CY_CHECK_GT(buffer->capacity(), 4u);
    CY_CHECK_EQ(buffer->size(), 5u);
    for (cy::u32 index = 0; index < 5; ++index) {
        CY_CHECK_EQ((*buffer)[index].x, static_cast<cy::f32>(index));
    }

    // A spilled buffer survives an archetype transition: the header moves with the row, so the
    // heap block changes owner rather than being copied.
    CY_REQUIRE(world.add(*entity, ids->position).has_value());
    auto moved = world.buffer<cy::ecs::test::Waypoint>(*entity, ids->waypoints);
    CY_REQUIRE(moved.has_value());
    CY_CHECK_EQ(moved->size(), 5u);
    CY_CHECK_EQ((*moved)[4].x, 4.0F);
}
