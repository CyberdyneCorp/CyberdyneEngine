// Scenes: additive load, independent unload, and budgeted instantiation. Task 3.1.9.

#include <cy/test/test.h>

#include <cy/scene/coherence.h>
#include <cy/scene/scene.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

using cy::scene::test::Fixture;

namespace {

/// A four-node scene: a body with two attachments and one grandchild.
[[nodiscard]] cy::Span<const cy::scene::NodeDesc> level_nodes() noexcept {
    static const cy::scene::NodeDesc nodes[] = {
        {cy::Name::intern("Body"), cy::scene::NodeDesc::kNoParent, cy::Name(),
         cy::Transform::from_translation(cy::Vec3{1.0F, 0.0F, 0.0F}), true, true, cy::Name(),
         cy::Name()},
        {cy::Name::intern("Turret"), 0, cy::Name(),
         cy::Transform::from_translation(cy::Vec3{0.0F, 2.0F, 0.0F}), true, true, cy::Name(),
         cy::Name()},
        {cy::Name::intern("Wheel"), 0, cy::Name(), cy::Transform::identity(), true, true,
         cy::Name(), cy::Name()},
        {cy::Name::intern("Muzzle"), 1, cy::Name(),
         cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, -1.0F}), true, true, cy::Name(),
         cy::Name()},
    };
    return {nodes, 4};
}

/// The same, with the turret claiming a project-unique alias. Separate from `level_nodes()` because
/// an alias is unique across the project: loading a description that claims one twice is refused,
/// which is the point of the uniqueness and is asserted below.
[[nodiscard]] cy::Span<const cy::scene::NodeDesc> aliased_level_nodes() noexcept {
    static const cy::scene::NodeDesc nodes[] = {
        {cy::Name::intern("Body"), cy::scene::NodeDesc::kNoParent, cy::Name(),
         cy::Transform::identity(), true, true, cy::Name(), cy::Name()},
        {cy::Name::intern("Turret"), 0, cy::Name(), cy::Transform::identity(), true, true,
         cy::Name::intern("the-turret"), cy::Name()},
    };
    return {nodes, 2};
}

}  // namespace

CY_TEST_CASE("an additive load adds a scene root, and unloading destroys exactly its entities") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::scene::Node standing =
        cy::scene::test::make_child(fixture.tree, fixture.tree.root(), "AlreadyHere");
    CY_REQUIRE(standing.valid());

    cy::scene::SceneDescription first;
    first.name = cy::Name::intern("LevelA");
    first.nodes = level_nodes();
    const auto scene_a = fixture.tree.load(first);
    CY_REQUIRE(scene_a.has_value());

    cy::scene::SceneDescription second;
    second.name = cy::Name::intern("LevelB");
    second.nodes = level_nodes();
    const auto scene_b = fixture.tree.load(second);
    CY_REQUIRE(scene_b.has_value());
    CY_CHECK_NE(*scene_a, *scene_b);

    // Root, the standing node, and two scenes of five (a scene root plus four described nodes).
    CY_CHECK_EQ(fixture.tree.stats().nodes, 12U);
    CY_CHECK_EQ(fixture.tree.stats().scenes_active, 2U);
    CY_CHECK(fixture.tree.find("/LevelA/Body/Turret/Muzzle").valid());
    CY_CHECK(fixture.tree.find("/LevelB/Body/Wheel").valid());
    // The two scenes' identical names do not collide: they are under different roots.
    CY_CHECK(fixture.tree.find("/LevelA/Body") != fixture.tree.find("/LevelB/Body"));

    CY_REQUIRE(fixture.tree.unload(*scene_a).has_value());
    CY_CHECK_EQ(fixture.tree.stats().nodes, 7U);
    CY_CHECK_FALSE(fixture.tree.find("/LevelA").valid());
    CY_CHECK(fixture.tree.find("/LevelB/Body/Turret/Muzzle").valid());
    CY_CHECK(standing.valid());
    CY_CHECK_EQ(fixture.tree.stats().scenes_active, 1U);

    // The transforms the description carried are the authored ones, and propagation composes them.
    CY_REQUIRE(fixture.tree.propagate().has_value());
    const cy::scene::Node muzzle = fixture.tree.find("/LevelB/Body/Turret/Muzzle");
    CY_CHECK(cy::nearly_equal(muzzle.world_transform().translation, cy::Vec3{1.0F, 2.0F, -1.0F},
                              1.0e-4F));
    CY_CHECK(cy::scene::coherent(fixture.tree));
}

CY_TEST_CASE("a replacing load unloads what was there") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::SceneDescription description;
    description.name = cy::Name::intern("Level");
    description.nodes = level_nodes();
    CY_REQUIRE(fixture.tree.load(description).has_value());
    CY_CHECK_EQ(fixture.tree.stats().nodes, 6U);

    CY_REQUIRE(fixture.tree.load(description, cy::scene::LoadMode::Replace).has_value());
    CY_CHECK_EQ(fixture.tree.stats().nodes, 6U);
    CY_CHECK_EQ(fixture.tree.stats().scenes_loaded, 1U);
}

CY_TEST_CASE("an asynchronous load is spread across pumps and is not active until it is finished") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::SceneDescription description;
    description.name = cy::Name::intern("Streamed");
    description.nodes = level_nodes();

    const auto scene = fixture.tree.begin_load(description);
    CY_REQUIRE(scene.has_value());
    CY_CHECK(fixture.tree.scene(*scene)->status == cy::scene::SceneStatus::Loading);
    CY_CHECK_EQ(fixture.tree.scene(*scene)->instantiated, 0U);
    CY_CHECK_EQ(fixture.tree.stats().scenes_active, 0U);
    // The scene root exists so the nodes have somewhere to land, and nothing else does.
    CY_CHECK_EQ(fixture.tree.stats().nodes, 2U);

    const auto first = fixture.tree.pump_loads(2);
    CY_REQUIRE(first.has_value());
    CY_CHECK_EQ(*first, 2U);
    CY_CHECK(fixture.tree.scene(*scene)->status == cy::scene::SceneStatus::Loading);
    CY_CHECK_EQ(fixture.tree.stats().nodes, 4U);

    const auto second = fixture.tree.pump_loads(64);
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(*second, 2U);
    // "The scene SHALL become active only once fully instantiated."
    CY_CHECK(fixture.tree.scene(*scene)->status == cy::scene::SceneStatus::Active);
    CY_CHECK_EQ(fixture.tree.scene(*scene)->instantiated, 4U);
    CY_CHECK_EQ(fixture.tree.stats().scenes_active, 1U);

    // A further pump has nothing to do.
    const auto third = fixture.tree.pump_loads(64);
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(*third, 0U);
}

CY_TEST_CASE("a node reparented out of its scene is still destroyed with it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::SceneDescription description;
    description.name = cy::Name::intern("Level");
    description.nodes = level_nodes();
    const auto scene = fixture.tree.load(description);
    CY_REQUIRE(scene.has_value());

    cy::scene::Node wheel = fixture.tree.find("/Level/Body/Wheel");
    CY_REQUIRE(wheel.valid());
    CY_REQUIRE(wheel.set_parent(fixture.tree.root()).has_value());
    CY_CHECK(fixture.tree.find("/Wheel").valid());

    // Membership is the `SceneRef` component, not a subtree, so "taking its entities with it" holds
    // even for one a game moved out of the scene's root.
    CY_REQUIRE(fixture.tree.unload(*scene).has_value());
    CY_CHECK_FALSE(wheel.valid());
    CY_CHECK_EQ(fixture.tree.stats().nodes, 1U);
}

CY_TEST_CASE("a scene's alias is claimed on load and released on unload") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::SceneDescription description;
    description.name = cy::Name::intern("Level");
    description.nodes = aliased_level_nodes();
    const auto scene = fixture.tree.load(description);
    CY_REQUIRE(scene.has_value());

    const cy::Name alias = cy::Name::intern("the-turret");
    CY_CHECK(fixture.tree.find_alias(alias) == fixture.tree.find("/Level/Body/Turret"));

    // An alias is project-unique, so loading a second copy of a description that claims one is
    // refused rather than silently moving it. That is a property of the alias and not of the load;
    // a scene meant to be instantiated twice addresses its nodes by path.
    cy::scene::SceneDescription second;
    second.name = cy::Name::intern("Level2");
    second.nodes = aliased_level_nodes();
    CY_CHECK_FALSE(fixture.tree.load(second).has_value());

    CY_REQUIRE(fixture.tree.unload(*scene).has_value());
    CY_CHECK_FALSE(fixture.tree.find_alias(alias).valid());
    CY_CHECK_EQ(fixture.tree.alias_count(), 0U);
}
