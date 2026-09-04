// Groups as tag components, and broadcast. Task 3.1.8.

#include <cy/test/test.h>

#include <cy/ecs/query.h>
#include <cy/scene/group.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

#include <algorithm>

using cy::scene::test::Fixture;
using cy::scene::test::make_child;

namespace {

struct Visited {
    cy::Array<cy::u32> order;
    explicit Visited(cy::Allocator& allocator) noexcept : order(allocator) {}
};

void collect(cy::scene::Node node, void* user) noexcept {
    auto* visited = static_cast<Visited*>(user);
    (void)visited->order.push_back(node.entity().index());
}

}  // namespace

CY_TEST_CASE("group membership is a tag component and is queryable from a system") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::Name enemies = cy::Name::intern("enemies");

    cy::scene::Node first = make_child(fixture.tree, fixture.tree.root(), "Grunt");
    cy::scene::Node second = make_child(fixture.tree, fixture.tree.root(), "Grunt");
    const cy::scene::Node bystander = make_child(fixture.tree, fixture.tree.root(), "Crate");
    CY_REQUIRE(bystander.valid());

    // A membership test on a group nobody has joined must not register a component: it would be a
    // world slot spent on a question.
    CY_CHECK_FALSE(first.in_group(enemies));
    CY_CHECK_EQ(fixture.tree.groups().find(enemies), cy::ecs::kInvalidComponent);

    CY_REQUIRE(first.add_to_group(enemies).has_value());
    CY_REQUIRE(second.add_to_group(enemies).has_value());
    const cy::ecs::ComponentTypeId tag = fixture.tree.groups().find(enemies);
    CY_REQUIRE(tag != cy::ecs::kInvalidComponent);

    // A system sees the group without knowing a scene tree exists.
    cy::ecs::QueryDesc desc(cy::scene::test::allocator());
    CY_REQUIRE(desc.with(tag).has_value());
    cy::ecs::Query query(fixture.world, std::move(desc));
    cy::u64 seen = 0;
    CY_REQUIRE(
        query
            .for_each_chunk([&seen](cy::ecs::QueryChunk& chunk) noexcept { seen += chunk.count(); })
            .has_value());
    CY_CHECK_EQ(seen, 2U);

    CY_REQUIRE(second.remove_from_group(enemies).has_value());
    CY_CHECK_FALSE(second.in_group(enemies));
    CY_CHECK(first.in_group(enemies));
}

CY_TEST_CASE("broadcast reaches every member in a deterministic order") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::Name enemies = cy::Name::intern("enemies");

    cy::Array<cy::u32> expected(cy::scene::test::allocator());
    for (cy::u32 index = 0; index < 8; ++index) {
        cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Grunt");
        CY_REQUIRE(node.valid());
        CY_REQUIRE(node.add_to_group(enemies).has_value());
        CY_REQUIRE(expected.push_back(node.entity().index()).has_value());
    }
    // The entity index is a dense counter, so the expected order is creation order here — and, more
    // to the point, it does not depend on which chunk anything landed in.
    std::ranges::sort(expected);

    Visited visited(cy::scene::test::allocator());
    CY_REQUIRE(
        fixture.tree.groups().broadcast(fixture.tree, enemies, &collect, &visited).has_value());
    CY_REQUIRE_EQ(visited.order.size(), expected.size());
    for (cy::usize index = 0; index < expected.size(); ++index) {
        CY_CHECK_EQ(visited.order[index], expected[index]);
    }

    // Adding a component to one member moves its chunk; the broadcast order does not change.
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    CY_REQUIRE(health.has_value());
    CY_REQUIRE(fixture.tree.node(cy::ecs::Entity::make(expected[3], 1)).valid());
    cy::scene::Node moved = fixture.tree.node(cy::ecs::Entity::make(expected[3], 1));
    CY_REQUIRE(moved.add(*health).has_value());

    Visited again(cy::scene::test::allocator());
    CY_REQUIRE(
        fixture.tree.groups().broadcast(fixture.tree, enemies, &collect, &again).has_value());
    CY_REQUIRE_EQ(again.order.size(), expected.size());
    for (cy::usize index = 0; index < expected.size(); ++index) {
        CY_CHECK_EQ(again.order[index], expected[index]);
    }
}

CY_TEST_CASE("a broadcast may destroy the node it was handed") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::Name doomed = cy::Name::intern("doomed");
    for (cy::u32 index = 0; index < 4; ++index) {
        cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Victim");
        CY_REQUIRE(node.add_to_group(doomed).has_value());
    }

    // The members are collected before the visitor runs, so a structural change from the body is
    // not a mutation of a chunk the query is walking.
    CY_REQUIRE(fixture.tree.groups()
                   .broadcast(
                       fixture.tree, doomed,
                       [](cy::scene::Node node, void* user) noexcept {
                           (void)user;
                           (void)node.destroy();
                       },
                       nullptr)
                   .has_value());
    CY_CHECK_EQ(fixture.tree.stats().nodes, 1U);

    cy::Array<cy::scene::Node> members(cy::scene::test::allocator());
    CY_REQUIRE(fixture.tree.groups().members(fixture.tree, doomed, members).has_value());
    CY_CHECK(members.empty());
}
