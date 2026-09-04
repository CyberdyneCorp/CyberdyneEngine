// Visibility and enablement: two inherited flags, and the tags that make them queryable.
// Task 3.1.5.

#include <cy/test/test.h>

#include <cy/ecs/query.h>
#include <cy/scene/coherence.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

using cy::scene::test::Fixture;
using cy::scene::test::make_child;

namespace {

/// How many entities a gameplay-shaped query sees: nodes that are not disabled.
[[nodiscard]] cy::u64 simulated(Fixture& fixture) noexcept {
    cy::ecs::QueryDesc desc(cy::scene::test::allocator());
    if (!desc.with(fixture.tree.components().node_name) ||
        !desc.without(fixture.tree.components().disabled)) {
        return 0;
    }
    cy::ecs::Query query(fixture.world, std::move(desc));
    cy::u64 seen = 0;
    (void)query.for_each_chunk(
        [&seen](cy::ecs::QueryChunk& chunk) noexcept { seen += chunk.count(); });
    return seen;
}

}  // namespace

CY_TEST_CASE("a hidden parent hides its subtree and leaves it simulating") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Parent");
    const cy::scene::Node child = make_child(fixture.tree, parent, "Child");
    const cy::scene::Node grandchild = make_child(fixture.tree, child, "Grandchild");
    CY_REQUIRE(grandchild.valid());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(grandchild.effective_visible());

    CY_REQUIRE(parent.set_visible(false).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());

    // The whole subtree stops rendering...
    CY_CHECK_FALSE(parent.effective_visible());
    CY_CHECK_FALSE(child.effective_visible());
    CY_CHECK_FALSE(grandchild.effective_visible());
    // ...while its own authored flag is untouched, so re-showing the parent restores it.
    CY_CHECK(child.visible());
    // ...and it goes on simulating, because the two flags are orthogonal.
    CY_CHECK(grandchild.effective_enabled());
    CY_CHECK_EQ(simulated(fixture), 4U);

    CY_REQUIRE(parent.set_visible(true).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(grandchild.effective_visible());
    CY_CHECK(cy::scene::coherent(fixture.tree));
}

CY_TEST_CASE("a disabled subtree is excluded from queries and its data survives") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    CY_REQUIRE(health.has_value());

    cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Parent");
    cy::scene::Node child = make_child(fixture.tree, parent, "Child");
    const cy::scene::test::Health initial{55};
    CY_REQUIRE(child.add(*health, &initial).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK_EQ(simulated(fixture), 3U);

    CY_REQUIRE(parent.set_enabled(false).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());

    // `scene-graph-and-nodes`: "queries used by gameplay systems SHALL exclude it, while its data
    // remains intact for re-enabling". The exclusion is archetype-level, so the chunk is never
    // visited — which a bool field could not have delivered.
    CY_CHECK_FALSE(child.effective_enabled());
    CY_CHECK_EQ(simulated(fixture), 1U);
    CY_CHECK_EQ(child.get_as<cy::scene::test::Health>(*health)->value, 55);
    CY_CHECK(child.effective_visible());

    CY_REQUIRE(parent.set_enabled(true).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK_EQ(simulated(fixture), 3U);
    CY_CHECK_EQ(child.get_as<cy::scene::test::Health>(*health)->value, 55);
}

CY_TEST_CASE("a child disabled on its own stays disabled when its parent is re-enabled") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Parent");
    cy::scene::Node child = make_child(fixture.tree, parent, "Child");
    const cy::scene::Node grandchild = make_child(fixture.tree, child, "Grandchild");

    CY_REQUIRE(child.set_enabled(false).has_value());
    CY_REQUIRE(parent.set_enabled(false).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK_FALSE(grandchild.effective_enabled());

    CY_REQUIRE(parent.set_enabled(true).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    // The child's own flag is still false, so the effective answer for it and below is still false.
    CY_CHECK(parent.effective_enabled());
    CY_CHECK_FALSE(child.effective_enabled());
    CY_CHECK_FALSE(grandchild.effective_enabled());
    CY_CHECK(cy::scene::coherent(fixture.tree));
}

CY_TEST_CASE("reparenting under a hidden node inherits its effective visibility") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node hidden = make_child(fixture.tree, fixture.tree.root(), "Hidden");
    cy::scene::Node visible = make_child(fixture.tree, fixture.tree.root(), "Visible");
    cy::scene::Node moving = make_child(fixture.tree, visible, "Moving");
    CY_REQUIRE(hidden.set_visible(false).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(moving.effective_visible());

    CY_REQUIRE(moving.set_parent(hidden).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK_FALSE(moving.effective_visible());
    CY_CHECK(moving.visible());
    CY_CHECK(cy::scene::coherent(fixture.tree));
}
