// SPDX-License-Identifier: MIT
// THE AUTHORITATIVE TRANSFORM AND ITS PRESENTATION SYNC. Task 6.1, design §7.2 and §9.2.
//
// The mover's state is published into `AuthoritativeTransform` (Fixed), and the scene's
// `LocalTransform` (f32) is derived from it, camera-relative, once per tick.

#include <cy/ecs/world.h>
#include <cy/movement/components.h>
#include <cy/scene/tree.h>

#include "movement_fixture.h"

namespace {

using cy::u32;
using cy::detmath::FixedVec3;
using cy::movement::KinematicMover;
using cy::movement::MoverParams;
using cy::movement::UnitDesc;
using namespace cy::movement_test;

struct Scene {
    Scene() noexcept : world(allocator()), tree(world) {}

    [[nodiscard]] bool start() noexcept {
        return world.initialize().has_value() && tree.initialize().has_value();
    }

    cy::ecs::World world;
    cy::scene::SceneTree tree;
};

}  // namespace

CY_TEST_CASE("movement components: units publish Fixed and the scene derives f32 from it") {
    Scene scene;
    CY_REQUIRE(scene.start());
    const auto component = cy::movement::register_authoritative_transform(scene.world);
    CY_REQUIRE(component.has_value());
    // Registration is idempotent by name.
    CY_CHECK_EQ(*cy::movement::register_authoritative_transform(scene.world), *component);

    auto first = scene.tree.create_node(cy::Name::intern("unit"), cy::scene::Node());
    auto second = scene.tree.create_node(cy::Name::intern("unit"), cy::scene::Node());
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(scene.world.add(first->entity(), *component).has_value());
    // `second` carries no authoritative transform: the sync leaves it alone.

    MoverParams params;
    params.grid_min = at(-16, -16);
    params.grid_max = at(16, 16);
    KinematicMover mover(allocator(), params);
    UnitDesc unit;
    unit.entity = first->entity().bits();
    unit.position = at(10, -6);
    CY_REQUIRE(mover.add(unit).has_value());
    unit.entity = second->entity().bits();
    unit.position = at(-10, 6);
    CY_REQUIRE(mover.add(unit).has_value());

    const auto published = cy::movement::publish_units(mover, scene.world, *component);
    CY_REQUIRE(published.has_value());
    CY_CHECK_EQ(published->written, 1U);
    CY_CHECK_EQ(published->skipped, 1U);
    const auto* stored =
        scene.world.get<cy::movement::AuthoritativeTransform>(first->entity(), *component);
    CY_REQUIRE(stored != nullptr);
    CY_CHECK(stored->value == mover.transform(0));

    // A camera at (8, 0, -8): the presented position is the difference, exact in f32.
    const auto synced = cy::movement::sync_presentation(
        scene.tree, *component, FixedVec3{metres(8), metres(0), metres(-8)});
    CY_REQUIRE(synced.has_value());
    CY_CHECK_EQ(synced->synced, 1U);
    CY_CHECK_EQ(synced->without_node, 0U);
    const cy::Transform presented = first->local_transform();
    CY_CHECK_EQ(presented.translation.x, 2.0F);
    CY_CHECK_EQ(presented.translation.y, 0.0F);
    CY_CHECK_EQ(presented.translation.z, 2.0F);
    CY_CHECK_EQ(presented.rotation.w, 1.0F);
    CY_CHECK_EQ(second->local_transform().translation.x, 0.0F);
}

CY_TEST_CASE("movement components: an authoritative entity with no scene node is counted") {
    Scene scene;
    CY_REQUIRE(scene.start());
    const auto component = cy::movement::register_authoritative_transform(scene.world);
    CY_REQUIRE(component.has_value());
    const cy::ecs::ComponentTypeId ids[] = {*component};
    auto bare = scene.world.create(cy::Span<const cy::ecs::ComponentTypeId>(ids));
    CY_REQUIRE(bare.has_value());
    const auto synced = cy::movement::sync_presentation(scene.tree, *component, FixedVec3{});
    CY_REQUIRE(synced.has_value());
    CY_CHECK_EQ(synced->synced, 0U);
    CY_CHECK_EQ(synced->without_node, 1U);
}
