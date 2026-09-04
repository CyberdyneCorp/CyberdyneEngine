// A node is a view onto an entity. Task 3.1.1.
//
// `scene-graph-and-nodes` — "Node is a view onto an entity". Every case here is written so that it
// would fail if someone added a cache: the assertions compare what the node reports against what a
// direct ECS read reports, in both directions, with no propagation or sync step between.

#include <cy/test/test.h>

#include <cy/ecs/command_buffer.h>
#include <cy/ecs/query.h>
#include <cy/scene/node.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

using cy::scene::test::Fixture;

CY_TEST_CASE("a Node is a tree pointer and an entity id, and nothing else") {
    // The static_asserts in node.h are the real gate; this restates the size so that the number a
    // reviewer expects is in the suite as well as in the header.
    CY_CHECK_EQ(sizeof(cy::scene::Node), sizeof(void*) + sizeof(cy::ecs::Entity));
    CY_CHECK(std::is_trivially_copyable_v<cy::scene::Node>);
    CY_CHECK_FALSE(cy::scene::Node{}.valid());
}

CY_TEST_CASE("reading a node property reads the entity's component") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    const cy::scene::Node node =
        cy::scene::test::make_child(fixture.tree, fixture.tree.root(), "Player");
    CY_REQUIRE(node.valid());

    const cy::Transform placed = cy::Transform::from_translation(cy::Vec3{1.0F, 2.0F, 3.0F});
    CY_REQUIRE(node.set_local_transform(placed).has_value());

    // The direct ECS read, with no node involved. If a node ever cached a transform, this is the
    // line that would still be the old value.
    const auto* stored = fixture.world.get<cy::scene::LocalTransform>(
        node.entity(), fixture.tree.components().local_transform);
    CY_REQUIRE(stored != nullptr);
    CY_CHECK(stored->value == placed);
    CY_CHECK(node.local_transform() == placed);
}

CY_TEST_CASE("writing the component through the ECS is visible through the node immediately") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node node = cy::scene::test::make_child(fixture.tree, fixture.tree.root(), "Player");
    CY_REQUIRE(node.valid());

    // The other direction, and the one a cache breaks: a system writes the column and the node
    // reports the new value with no sync step, because there is nothing to sync.
    auto* stored = fixture.world.get_mut<cy::scene::LocalTransform>(
        node.entity(), fixture.tree.components().local_transform);
    CY_REQUIRE(stored != nullptr);
    stored->value.translation = cy::Vec3{7.0F, 0.0F, 0.0F};

    CY_CHECK(node.local_transform().translation == cy::Vec3{7.0F, 0.0F, 0.0F});

    // Two handles onto one entity are the same view, not two objects.
    const cy::scene::Node other = fixture.tree.node(node.entity());
    CY_CHECK(other == node);
    CY_CHECK(other.local_transform() == node.local_transform());
}

CY_TEST_CASE("entities without nodes are fully first class and are not in the tree") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    CY_REQUIRE(health.has_value());

    // `scene-graph-and-nodes`: "a system spawns bulk entities with no nodes ... they SHALL be fully
    // simulated and rendered, and SHALL NOT appear in the scene tree".
    cy::Array<cy::ecs::Entity> spawned(cy::scene::test::allocator());
    const cy::ecs::ComponentTypeId set[] = {*health};
    CY_REQUIRE(
        fixture.world.create_many(64, cy::Span<const cy::ecs::ComponentTypeId>(set, 1), spawned)
            .has_value());
    CY_CHECK_EQ(spawned.size(), 64U);

    for (const cy::ecs::Entity entity : spawned) {
        CY_CHECK(fixture.world.is_alive(entity));
        CY_CHECK(fixture.world.get(entity, *health) != nullptr);
        // Alive, queryable, and not a node.
        CY_CHECK_FALSE(fixture.tree.node(entity).valid());
    }
    CY_CHECK_EQ(fixture.tree.stats().nodes, 1U);  // only the tree root
}

CY_TEST_CASE("destroying the entity invalidates every node handle onto it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::scene::Node node =
        cy::scene::test::make_child(fixture.tree, fixture.tree.root(), "Doomed");
    const cy::scene::Node copy = node;
    CY_REQUIRE(node.valid());

    CY_REQUIRE(fixture.world.destroy(node.entity()).has_value());

    // No invalidation pass ran. Both handles report the entity table's answer, which is what makes
    // "the node SHALL be detached and invalidated" a property rather than a step.
    CY_CHECK_FALSE(node.valid());
    CY_CHECK_FALSE(copy.valid());
    CY_CHECK(node.local_transform() == cy::Transform::identity());
    CY_CHECK_FALSE(node.name().index() != 0);
}

CY_TEST_CASE("a node destroyed by a system through a command buffer is invalid at the flush") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::scene::Node node =
        cy::scene::test::make_child(fixture.tree, fixture.tree.root(), "Doomed");
    CY_REQUIRE(node.valid());

    cy::ecs::CommandBuffer commands(fixture.world);
    CY_REQUIRE(fixture.world.attach(commands).has_value());
    CY_REQUIRE(commands.destroy(node.entity()).has_value());
    // Deferred: still there until the flush point, which is the whole of `ecs-core`'s deferral.
    CY_CHECK(node.valid());

    CY_REQUIRE(fixture.world.flush().has_value());
    CY_CHECK_FALSE(node.valid());

    // And the tree agrees once it has pumped, without having been told which entity died.
    CY_REQUIRE(fixture.tree.pump().has_value());
    CY_CHECK_EQ(fixture.tree.stats().nodes, 1U);
    fixture.world.detach(commands);
}

CY_TEST_CASE("component access through a node is the world's, unchanged") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    CY_REQUIRE(health.has_value());

    cy::scene::Node node = cy::scene::test::make_child(fixture.tree, fixture.tree.root(), "Unit");
    CY_REQUIRE(node.valid());
    CY_CHECK_FALSE(node.has(*health));

    const cy::scene::test::Health initial{42};
    CY_REQUIRE(node.add(*health, &initial).has_value());
    CY_CHECK(node.has(*health));
    CY_CHECK_EQ(node.get_as<cy::scene::test::Health>(*health)->value, 42);
    CY_CHECK_EQ(fixture.world.get<cy::scene::test::Health>(node.entity(), *health)->value, 42);

    CY_REQUIRE(node.set(*health, cy::scene::test::Health{7}).has_value());
    CY_CHECK_EQ(fixture.world.get<cy::scene::test::Health>(node.entity(), *health)->value, 7);

    CY_REQUIRE(node.remove(*health).has_value());
    CY_CHECK_FALSE(node.has(*health));
}
