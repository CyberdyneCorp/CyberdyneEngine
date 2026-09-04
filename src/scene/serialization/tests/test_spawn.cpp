// Entity templates: spawning as a copy, batching, reference fixup, and live prefab update.

#include <cy/scene/serialization/spawn.h>
#include <cy/test/test.h>

#include <cstring>

#include "fixtures.h"

using namespace cy;
using namespace cy::scene::serialization;
using namespace cy::scene::serialization::test;

namespace {

[[nodiscard]] TransformBinding placement_binding() noexcept {
    return TransformBinding{reflect::TypeId(kPlacementType), reflect::FieldId(kPlacementLocal)};
}

[[nodiscard]] Status make_world(ecs::World& world) noexcept {
    if (Status started = world.initialize(); !started) {
        return started;
    }
    return register_fixture_components(world);
}

[[nodiscard]] Expected<ResolvedEntity*, Error> add(ResolvedGraph& graph, u32 id, u32 parent,
                                                   MotionKind motion) noexcept {
    Expected<ResolvedEntity*, Error> entity = graph.add(LocalId(id));
    if (!entity) {
        return entity;
    }
    (*entity)->parent = LocalId(parent);
    (*entity)->motion = motion;
    (*entity)->origin = asset(1);
    (*entity)->origin_local = LocalId(id);
    return write_transform_of(**entity, placement_binding(), cy::Transform::identity(), asset(1),
                              ValueSource::Base, graph.allocator())
               ? Expected<ResolvedEntity*, Error>(*entity)
               : fail(ErrorCode::Internal, "could not place the entity");
}

[[nodiscard]] Status set_maximum(ResolvedEntity& entity, f32 maximum,
                                 Allocator& allocator) noexcept {
    const Expected<ResolvedComponent*, Error> health =
        ensure_resolved_component(entity, reflect::TypeId(kHealthType), allocator);
    if (!health) {
        return make_unexpected(health.error());
    }
    return (*health)->record.set_scalar(reflect::FieldId(kHealthMaximum), serialize::WireType::F32,
                                        &maximum, sizeof(maximum));
}

}  // namespace

CY_TEST_CASE("a hundred-entity template spawns as a copy, with its relationships attached") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"spawn", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Dynamic).has_value());
    for (u32 id = 2; id <= 100; ++id) {
        // Dynamic, so the relationships survive the cook and the spawn has to attach them.
        CY_REQUIRE(add(graph, id, 1, MotionKind::Dynamic).has_value());
    }

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());
    CY_CHECK_EQ(report.relationships_retained, 99U);

    EntityTemplate entity_template(test_allocator());
    CY_REQUIRE(entity_template.bind(world, layouts, cooked).has_value());
    CY_CHECK_EQ(entity_template.entity_count(), 100U);

    Array<ecs::Entity> spawned(test_allocator());
    CY_REQUIRE(entity_template.spawn(world, spawned).has_value());
    CY_REQUIRE_EQ(spawned.size(), 100U);
    for (const ecs::Entity entity : spawned) {
        CY_REQUIRE(world.is_alive(entity));
    }
    CY_CHECK_EQ(world.children_of(spawned[0]).size(), 99U);
    CY_CHECK_EQ(world.parent_of(spawned[42]), spawned[0]);
}

CY_TEST_CASE("a thousand instances are created in one batch, not a thousand spawns") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"spawn", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    for (u32 id = 1; id <= 3; ++id) {
        CY_REQUIRE(add(graph, id, 0, MotionKind::Static).has_value());
    }

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());

    EntityTemplate entity_template(test_allocator());
    CY_REQUIRE(entity_template.bind(world, layouts, cooked).has_value());

    Array<ecs::Entity> spawned(test_allocator());
    CY_REQUIRE(entity_template.spawn_many(world, 1000, spawned).has_value());
    CY_REQUIRE_EQ(spawned.size(), 3000U);
    CY_CHECK_EQ(world.entity_count(), 3000U);
    // One instantiation per archetype for the whole batch, not one per instance.
    CY_CHECK_EQ(entity_template.block_count(), 1U);
}

CY_TEST_CASE("an intra-template reference becomes a live entity of the same instance") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"spawn", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static).has_value());
    const Expected<ResolvedEntity*, Error> pointer = add(graph, 2, 0, MotionKind::Static);
    CY_REQUIRE(pointer.has_value());
    const Expected<ResolvedComponent*, Error> target =
        ensure_resolved_component(**pointer, reflect::TypeId(kTargetType), graph.allocator());
    CY_REQUIRE(target.has_value());
    CY_REQUIRE(
        (*target)->record.set_local_reference(reflect::FieldId(kTargetEntity), 1).has_value());

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());

    EntityTemplate entity_template(test_allocator());
    CY_REQUIRE(entity_template.bind(world, layouts, cooked).has_value());

    Array<ecs::Entity> spawned(test_allocator());
    CY_REQUIRE(entity_template.spawn_many(world, 2, spawned).has_value());
    CY_REQUIRE_EQ(spawned.size(), 4U);

    const ecs::ComponentInfo* info = world.components().find(reflect::TypeId(kTargetType));
    CY_REQUIRE(info != nullptr);
    for (u32 instance = 0; instance < 2; ++instance) {
        const ecs::Entity pointing = spawned[(static_cast<usize>(instance) * 2) + 1];
        const ecs::Entity pointed = spawned[static_cast<usize>(instance) * 2];
        const void* element = world.get(pointing, info->id);
        CY_REQUIRE(element != nullptr);
        ecs::Entity stored;
        std::memcpy(&stored, element, sizeof(stored));
        // Each instance's reference points inside *its own* copy, not at the first instance's.
        CY_CHECK_EQ(stored, pointed);
        CY_CHECK(world.is_alive(stored));
    }
}

CY_TEST_CASE("a null reference stays null through the cook and the spawn") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"spawn", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    const Expected<ResolvedEntity*, Error> entity = add(graph, 1, 0, MotionKind::Static);
    CY_REQUIRE(entity.has_value());
    const Expected<ResolvedComponent*, Error> target =
        ensure_resolved_component(**entity, reflect::TypeId(kTargetType), graph.allocator());
    CY_REQUIRE(target.has_value());
    CY_REQUIRE(
        (*target)->record.set_local_reference(reflect::FieldId(kTargetEntity), 0).has_value());

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());

    EntityTemplate entity_template(test_allocator());
    CY_REQUIRE(entity_template.bind(world, layouts, cooked).has_value());
    Array<ecs::Entity> spawned(test_allocator());
    CY_REQUIRE(entity_template.spawn(world, spawned).has_value());

    const ecs::ComponentInfo* info = world.components().find(reflect::TypeId(kTargetType));
    CY_REQUIRE(info != nullptr);
    const void* element = world.get(spawned[0], info->id);
    CY_REQUIRE(element != nullptr);
    ecs::Entity stored;
    std::memcpy(&stored, element, sizeof(stored));
    CY_CHECK_EQ(stored, ecs::kNoEntity);
}

CY_TEST_CASE("a batch is placed by its per-instance transforms") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"spawn", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static).has_value());

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());

    EntityTemplate entity_template(test_allocator());
    CY_REQUIRE(entity_template.bind(world, layouts, cooked).has_value());

    const cy::Transform placements[] = {
        cy::Transform::from_translation(cy::Vec3{1, 0, 0}),
        cy::Transform::from_translation(cy::Vec3{2, 0, 0}),
        cy::Transform::from_translation(cy::Vec3{3, 0, 0}),
    };
    Array<ecs::Entity> spawned(test_allocator());
    CY_REQUIRE(entity_template
                   .spawn_many(world, Span<const cy::Transform>(placements, 3), placement_binding(),
                               spawned)
                   .has_value());
    CY_REQUIRE_EQ(spawned.size(), 3U);

    const ecs::ComponentInfo* info = world.components().find(reflect::TypeId(kPlacementType));
    CY_REQUIRE(info != nullptr);
    for (u32 index = 0; index < 3; ++index) {
        const void* element = world.get(spawned[index], info->id);
        CY_REQUIRE(element != nullptr);
        cy::Transform placed;
        std::memcpy(&placed, element, sizeof(placed));
        CY_CHECK_EQ(placed.translation.x, static_cast<f32>(index + 1));
    }
}

CY_TEST_CASE("a template bound to a world with a different schema is refused at the header") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"spawn", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static).has_value());
    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());

    // A layout table missing one component is a different schema, which is exactly the situation
    // "cooked data mismatch is fatal" is about.
    ComponentLayoutTable narrower(test_allocator());
    CY_REQUIRE(narrower.add(placement_type(), {}).has_value());
    EntityTemplate entity_template(test_allocator());
    const Status bound = entity_template.bind(world, narrower, cooked);
    CY_REQUIRE_FALSE(bound.has_value());
    CY_CHECK_EQ(bound.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("a live edit updates the authoring field and keeps the running state") {
    // "A designer changes a robot prefab's maximum health while the game runs; existing robots take
    // the new maximum and keep their current health."
    ecs::World world(test_allocator(), ecs::WorldConfig{"live", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph before(test_allocator());
    const Expected<ResolvedEntity*, Error> robot = add(before, 1, 0, MotionKind::Static);
    CY_REQUIRE(robot.has_value());
    CY_REQUIRE(set_maximum(**robot, 100.0F, before.allocator()).has_value());

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(before, options, cooked, report).has_value());

    EntityTemplate entity_template(test_allocator());
    CY_REQUIRE(entity_template.bind(world, layouts, cooked).has_value());
    Array<ecs::Entity> spawned(test_allocator());
    CY_REQUIRE(entity_template.spawn(world, spawned).has_value());

    // The simulation runs and changes the current health.
    const ecs::ComponentInfo* info = world.components().find(reflect::TypeId(kHealthType));
    CY_REQUIRE(info != nullptr);
    {
        void* element = world.get_mut(spawned[0], info->id);
        CY_REQUIRE(element != nullptr);
        static_cast<Health*>(element)->current = 17.0F;
    }

    // The designer edits the prefab's maximum.
    ResolvedGraph after(test_allocator());
    const Expected<ResolvedEntity*, Error> edited = add(after, 1, 0, MotionKind::Static);
    CY_REQUIRE(edited.has_value());
    CY_REQUIRE(set_maximum(**edited, 250.0F, after.allocator()).has_value());

    LiveUpdatePlan plan(test_allocator());
    CY_REQUIRE(plan_live_update(before, after, world, layouts, plan).has_value());
    CY_CHECK_FALSE(plan.requires_recreation());
    CY_REQUIRE_EQ(plan.updates().size(), 1U);
    CY_REQUIRE(apply_live_update(world, plan, spawned.span()).has_value());

    const void* element = world.get(spawned[0], info->id);
    CY_REQUIRE(element != nullptr);
    const auto* health = static_cast<const Health*>(element);
    CY_CHECK_EQ(health->maximum, 250.0F);
    CY_CHECK_EQ(health->current, 17.0F);
}

CY_TEST_CASE("a runtime-state field edited in the prefab is preserved, not written over") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"live", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph before(test_allocator());
    const Expected<ResolvedEntity*, Error> robot = add(before, 1, 0, MotionKind::Static);
    CY_REQUIRE(robot.has_value());
    CY_REQUIRE(set_maximum(**robot, 100.0F, before.allocator()).has_value());

    ResolvedGraph after(test_allocator());
    const Expected<ResolvedEntity*, Error> edited = add(after, 1, 0, MotionKind::Static);
    CY_REQUIRE(edited.has_value());
    CY_REQUIRE(set_maximum(**edited, 100.0F, after.allocator()).has_value());
    const Expected<ResolvedComponent*, Error> health =
        ensure_resolved_component(**edited, reflect::TypeId(kHealthType), after.allocator());
    CY_REQUIRE(health.has_value());
    const f32 current = 5.0F;
    CY_REQUIRE(
        (*health)
            ->record
            .set_scalar(reflect::FieldId(kHealthCurrent), serialize::WireType::F32, &current, 4)
            .has_value());

    LiveUpdatePlan plan(test_allocator());
    CY_REQUIRE(plan_live_update(before, after, world, layouts, plan).has_value());
    CY_CHECK(plan.updates().empty());
    CY_CHECK_EQ(plan.preserved_runtime_fields, 1U);
}

CY_TEST_CASE("a structural edit announces its policy instead of resetting instances") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"live", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph before(test_allocator());
    CY_REQUIRE(add(before, 1, 0, MotionKind::Static).has_value());

    ResolvedGraph after(test_allocator());
    CY_REQUIRE(add(after, 1, 0, MotionKind::Static).has_value());
    CY_REQUIRE(add(after, 2, 0, MotionKind::Static).has_value());

    LiveUpdatePlan plan(test_allocator());
    CY_REQUIRE(plan_live_update(before, after, world, layouts, plan).has_value());
    CY_CHECK(plan.requires_recreation());

    Array<ecs::Entity> nothing(test_allocator());
    const Status refused = apply_live_update(world, plan, nothing.span());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unavailable);
}
