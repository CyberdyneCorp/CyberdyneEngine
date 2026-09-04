// Hierarchy, naming, ordering and paths. Task 3.1.2.

#include <cy/test/test.h>

#include <cy/scene/node.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

using cy::scene::test::Fixture;
using cy::scene::test::make_child;

CY_TEST_CASE("the tree is the ECS relation, visible to systems") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Level");
    const cy::scene::Node child = make_child(fixture.tree, parent, "Player");
    CY_REQUIRE(child.valid());

    // `scene-graph-and-nodes`: "the tree SHALL be backed by the ECS `Parent`/`Children` relation,
    // so hierarchy is visible to systems". A system asks the world and gets the same answer.
    CY_CHECK(fixture.world.parent_of(child.entity()) == parent.entity());
    CY_CHECK_EQ(fixture.world.children_of(parent.entity()).size(), 1U);
    CY_CHECK(fixture.world.children_of(parent.entity())[0] == child.entity());
    CY_CHECK(child.parent() == parent);
    CY_CHECK(parent.is_ancestor_of(child));
    CY_CHECK_EQ(child.depth(), 2U);
}

CY_TEST_CASE("a sibling name collision is resolved by suffixing") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Rig");

    const cy::scene::Node first = make_child(fixture.tree, parent, "Light");
    const cy::scene::Node second = make_child(fixture.tree, parent, "Light");
    const cy::scene::Node third = make_child(fixture.tree, parent, "Light");
    CY_REQUIRE(third.valid());

    CY_CHECK(first.name() == cy::Name::intern("Light"));
    CY_CHECK(second.name() == cy::Name::intern("Light_2"));
    CY_CHECK(third.name() == cy::Name::intern("Light_3"));

    // A node under a different parent may hold the name again: uniqueness is among siblings.
    const cy::scene::Node elsewhere = make_child(fixture.tree, fixture.tree.root(), "Light");
    CY_CHECK(elsewhere.name() == cy::Name::intern("Light"));
}

CY_TEST_CASE("children are ordered, and the order survives a removal") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Row");
    cy::scene::Node a = make_child(fixture.tree, parent, "A");
    const cy::scene::Node b = make_child(fixture.tree, parent, "B");
    cy::scene::Node c = make_child(fixture.tree, parent, "C");
    const cy::scene::Node d = make_child(fixture.tree, parent, "D");
    CY_REQUIRE(d.valid());

    CY_CHECK(parent.child(0) == a);
    CY_CHECK(parent.child(3) == d);

    // `ecs-core` leaves the `Children` buffer's order unspecified and removes by swapping the last
    // element into the gap. The authored order is a component, so it survives that.
    CY_REQUIRE(b.destroy().has_value());
    CY_CHECK_EQ(parent.child_count(), 3U);
    CY_CHECK(parent.child(0) == a);
    CY_CHECK(parent.child(1) == c);
    CY_CHECK(parent.child(2) == d);

    CY_REQUIRE(c.set_sibling_index(0).has_value());
    CY_CHECK(parent.child(0) == c);
    CY_CHECK(parent.child(1) == a);
    CY_CHECK(parent.child(2) == d);
}

CY_TEST_CASE("paths resolve absolutely, relatively and through the parent") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::scene::Node level = make_child(fixture.tree, fixture.tree.root(), "Level");
    const cy::scene::Node player = make_child(fixture.tree, level, "Player");
    const cy::scene::Node camera = make_child(fixture.tree, level, "Camera");
    const cy::scene::Node gun = make_child(fixture.tree, player, "Gun");
    CY_REQUIRE(gun.valid());

    CY_CHECK(fixture.tree.find("/Level/Player/Gun") == gun);
    CY_CHECK(fixture.tree.find("/Level") == level);
    CY_CHECK(fixture.tree.find("/") == fixture.tree.root());
    // The specification's own scenario: `"../Camera"` from the player is the sibling.
    CY_CHECK(player.find("../Camera") == camera);
    CY_CHECK(player.find("Gun") == gun);
    CY_CHECK(player.find(".") == player);
    CY_CHECK(gun.find("/Level/Camera") == camera);

    // A path that names nothing is a null handle, not a crash and not a wrong node.
    CY_CHECK_FALSE(fixture.tree.find("/Level/Nobody").valid());
    CY_CHECK_FALSE(player.find("../Nobody").valid());
    CY_CHECK_FALSE(fixture.tree.find("Level").valid());  // absolute paths start with '/'

    cy::Array<char> text(cy::scene::test::allocator());
    CY_REQUIRE(gun.path(text).has_value());
    CY_CHECK(std::string_view(text.data()) == "/Level/Player/Gun");
    CY_REQUIRE(fixture.tree.root().path(text).has_value());
    CY_CHECK(std::string_view(text.data()) == "/");
}

CY_TEST_CASE("an alias is project-unique and resolves through the component that holds it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node player = make_child(fixture.tree, fixture.tree.root(), "Player");
    cy::scene::Node other = make_child(fixture.tree, fixture.tree.root(), "Other");
    CY_REQUIRE(other.valid());

    const cy::Name alias = cy::Name::intern("the-player");
    CY_REQUIRE(player.set_alias(alias).has_value());
    CY_CHECK(fixture.tree.find_alias(alias) == player);
    CY_CHECK(player.alias() == alias);

    // Unique: a second claim is refused rather than silently moving the alias.
    CY_CHECK_FALSE(other.set_alias(alias).has_value());

    // The component is the truth and the map is an index over it, so destroying the holder makes
    // the alias resolve to nothing rather than to a recycled entity.
    CY_REQUIRE(player.destroy().has_value());
    CY_CHECK_FALSE(fixture.tree.find_alias(alias).valid());
    CY_REQUIRE(other.set_alias(alias).has_value());
    CY_CHECK(fixture.tree.find_alias(alias) == other);
}

CY_TEST_CASE("a node created with no parent is live but not in the tree") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node detached =
        *fixture.tree.create_node(cy::Name::intern("Floating"), cy::scene::Node());
    CY_REQUIRE(detached.valid());
    CY_CHECK_FALSE(detached.in_tree());
    CY_CHECK_FALSE(detached.parent().valid());

    CY_REQUIRE(detached.set_parent(fixture.tree.root()).has_value());
    CY_CHECK(detached.in_tree());

    CY_REQUIRE(detached.set_parent(cy::scene::Node()).has_value());
    CY_CHECK_FALSE(detached.in_tree());
    CY_CHECK(detached.valid());
}
