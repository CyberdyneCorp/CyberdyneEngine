// The physics ECS bridge. M8.a tasks 4.1 and 4.2.
//
// What `physics` says, and where each sentence is checked here:
//
//   "Physics components ... expressed as ECS components"
//        -> the components register, keep their ids, and a second bridge binds to the same numbers
//   "physics SHALL step exactly once per simulation tick"
//        -> `steps == clock.tick()` after a run, and the stage runs the step and nothing else does
//   "within the `Physics` stage"
//        -> `install()` puts the system in `Stage::Physics` and running any other stage steps
//           nothing
//   "Transforms written by physics SHALL be published to `LocalTransform`/`WorldTransform`"
//        -> a sphere dropped on a box lands ON the box, in `LocalTransform`, and propagation
//           derives `WorldTransform` from it in the same tick
//
// AND THE ONE THAT IS NOT A SENTENCE IN THE SPECIFICATION: a body whose component is removed loses
// its body, and a body whose entity is destroyed loses its body. Play mode does both constantly and
// nothing else in the tree does either.

#include "fixture.h"

#include <cy/scene/propagation.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::physics;
using namespace cy::physics::test;

CY_TEST_CASE("physics' components register once and keep their ids") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    CY_CHECK(fixture.components.registered());

    const auto again = PhysicsComponents::register_all(fixture.world);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(again->rigid_body, fixture.components.rigid_body);
    CY_CHECK_EQ(again->static_body, fixture.components.static_body);
    CY_CHECK_EQ(again->collider, fixture.components.collider);
    CY_CHECK_EQ(again->character_body, fixture.components.character_body);

    // The ids are distinct, which is the property a schema over them depends on: eight subjects
    // that were secretly the same subject would be one declaration and seven refusals.
    CY_CHECK_NE(again->rigid_body, again->static_body);
    CY_CHECK_NE(again->collider, again->trigger);
}

CY_TEST_CASE("a rigid body and a collider become a body in the solver") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    const cy::ecs::Entity crate = fixture.node("Crate", cy::Vec3{0.0F, 4.0F, 0.0F});
    CY_REQUIRE(crate.valid());
    CY_REQUIRE(fixture.box(crate, cy::Vec3{0.5F, 0.5F, 0.5F}));
    CY_REQUIRE(fixture.dynamic(crate));

    CY_REQUIRE(fixture.bridge->sync().has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 1U);
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_created, 1U);
    CY_CHECK_EQ(fixture.bridge->statistics().shapes_created, 1U);
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_refused, 0U);

    const BodyHandle body = fixture.bridge->body_of(crate);
    CY_CHECK_FALSE(body.is_null());
    CY_CHECK(fixture.backend.server().body_alive(body));

    // The handle went back into the component, which is what makes `BodyRef`'s "null until the
    // bridge creates it" true rather than aspirational.
    const auto* component = fixture.world.get<RigidBody>(crate, fixture.components.rigid_body);
    CY_REQUIRE(component != nullptr);
    CY_CHECK_EQ(component->body.bits(), body.bits());

    // Idempotent: syncing again creates nothing.
    CY_REQUIRE(fixture.bridge->sync().has_value());
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_created, 1U);
}

CY_TEST_CASE("a rigid body with no collider is refused by name and the world keeps simulating") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    // THIS IS THE AUTHORING CASE, not a contrived one: an inspector that adds a body before a
    // collider has produced exactly this for one edit. A dynamic body with no collider has no
    // volume, therefore no derived mass — `validate()` refuses it, and rightly.
    const cy::ecs::Entity empty = fixture.node("Empty", cy::Vec3{0.0F, 1.0F, 0.0F});
    CY_REQUIRE(empty.valid());
    CY_REQUIRE(fixture.dynamic(empty));

    const cy::ecs::Entity crate = fixture.node("Crate", cy::Vec3{3.0F, 4.0F, 0.0F});
    CY_REQUIRE(crate.valid());
    CY_REQUIRE(fixture.box(crate, cy::Vec3{0.5F, 0.5F, 0.5F}));
    CY_REQUIRE(fixture.dynamic(crate));

    CY_REQUIRE(fixture.bridge->sync().has_value());
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_refused, 1U);
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_created, 1U);
    CY_CHECK_FALSE(fixture.bridge->last_error().has_value());
    // The crate still exists and still simulates. One entity's refusal is not the world's.
    CY_CHECK_FALSE(fixture.bridge->body_of(crate).is_null());
    CY_CHECK(fixture.bridge->body_of(empty).is_null());
}

CY_TEST_CASE("a body falls under gravity and the placement lands in LocalTransform") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    // THE REFERENCE BACKEND INTEGRATES MOTION AND DOES NOT RESOLVE CONTACTS — it says so itself
    // (`Capabilities::contact_resolution == false`), and that is what keeps it honest about being
    // an interface conformance backend rather than a second solver. So what this case checks is the
    // half it can answer: gravity reached the body, and the result came back through the sink into
    // `LocalTransform`. The other half — a sphere coming to REST on a box — is
    // `test_simulation.cpp`, against Jolt.
    const cy::ecs::Entity ball = fixture.node("Ball", cy::Vec3{0.0F, 6.0F, 0.0F});
    CY_REQUIRE(ball.valid());
    Collider sphere;
    sphere.shape.type = ShapeType::Sphere;
    sphere.shape.radius = 0.5F;
    CY_REQUIRE(fixture.world.add(ball, fixture.components.collider, &sphere).has_value());
    CY_REQUIRE(fixture.dynamic(ball));

    CY_REQUIRE(fixture.run(12).has_value());

    // Exactly once per tick, and the stepper agrees with the clock.
    CY_CHECK_EQ(fixture.bridge->statistics().steps, 12U);
    CY_CHECK_EQ(fixture.clock.tick(), 12U);

    // Twelve steps at 1/60 s is 0.2 s, and −9.81 m/s² over 0.2 s is about 0.2 m of fall. The
    // tolerance is wide because a semi-implicit integrator's first step differs from the closed
    // form by half a step; the claim is that gravity reached the body, not that the integrator was
    // reimplemented here.
    const cy::Vec3 fallen = fixture.position_of(ball);
    CY_CHECK_LT(fallen.y, 6.0F);
    CY_CHECK_NEAR(fallen.y, 6.0F - 0.2F, 0.35);
    CY_CHECK_EQ(fixture.bridge->statistics().transforms_written, 12U);
    CY_CHECK_EQ(fixture.bridge->statistics().transforms_marked, 12U);
    CY_CHECK_EQ(fixture.bridge->statistics().orphan_publications, 0U);

    // And propagation derives `WorldTransform` from what physics wrote, because the bridge marked
    // the node's transform dirty. A bridge that wrote the component and not the bit leaves this at
    // the authored y = 6.
    CY_REQUIRE(cy::scene::propagate(fixture.tree, cy::scene::PropagationPhase::Simulation, nullptr,
                                    nullptr)
                   .has_value());
    const auto* derived = fixture.world.get<cy::scene::WorldTransform>(
        ball, fixture.tree.components().world_transform);
    CY_REQUIRE(derived != nullptr);
    CY_CHECK_NEAR(derived->value.translation.y, fallen.y, 0.0001);
}

CY_TEST_CASE("a static body is created and never moved") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    const cy::ecs::Entity ground = fixture.node("Ground", cy::Vec3{0.0F, 0.0F, 0.0F});
    CY_REQUIRE(ground.valid());
    CY_REQUIRE(fixture.box(ground, cy::Vec3{5.0F, 0.5F, 5.0F}));
    CY_REQUIRE(fixture.immovable(ground));

    CY_REQUIRE(fixture.run(8).has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 1U);
    CY_CHECK_NEAR(fixture.position_of(ground).y, 0.0F, 0.0001);
    const auto* component = fixture.world.get<StaticBody>(ground, fixture.components.static_body);
    CY_REQUIRE(component != nullptr);
    CY_CHECK_FALSE(component->body.is_null());
}

CY_TEST_CASE("the authored scale survives a step") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    const cy::ecs::Entity crate = fixture.node("Crate", cy::Vec3{0.0F, 4.0F, 0.0F});
    CY_REQUIRE(crate.valid());
    auto* local = fixture.world.get_mut<cy::scene::LocalTransform>(
        crate, fixture.tree.components().local_transform);
    CY_REQUIRE(local != nullptr);
    local->value.scale = cy::Vec3{2.0F, 3.0F, 4.0F};
    CY_REQUIRE(fixture.box(crate, cy::Vec3{0.5F, 0.5F, 0.5F}));
    CY_REQUIRE(fixture.dynamic(crate));

    CY_REQUIRE(fixture.run(10).has_value());

    // A solver has no opinion about scale, and a bridge that wrote the body's transform whole would
    // reset every scaled object in the world to 1 on the first tick of play — which is exactly the
    // kind of residue task 5.2 is about.
    const auto* after = fixture.world.get<cy::scene::LocalTransform>(
        crate, fixture.tree.components().local_transform);
    CY_REQUIRE(after != nullptr);
    CY_CHECK_NEAR(after->value.scale.x, 2.0F, 0.0001);
    CY_CHECK_NEAR(after->value.scale.y, 3.0F, 0.0001);
    CY_CHECK_NEAR(after->value.scale.z, 4.0F, 0.0001);
}

CY_TEST_CASE("removing the component destroys the body, and so does destroying the entity") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    const cy::ecs::Entity first = fixture.node("First", cy::Vec3{0.0F, 4.0F, 0.0F});
    const cy::ecs::Entity second = fixture.node("Second", cy::Vec3{2.0F, 4.0F, 0.0F});
    CY_REQUIRE(first.valid());
    CY_REQUIRE(second.valid());
    for (const cy::ecs::Entity entity : {first, second}) {
        CY_REQUIRE(fixture.box(entity, cy::Vec3{0.5F, 0.5F, 0.5F}));
        CY_REQUIRE(fixture.dynamic(entity));
    }
    CY_REQUIRE(fixture.bridge->sync().has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 2U);
    const BodyHandle doomed = fixture.bridge->body_of(first);

    // The inspector's "remove the rigid body" — task 4.3's undo, on the engine's side of the wire.
    CY_REQUIRE(fixture.world.remove(first, fixture.components.rigid_body).has_value());
    CY_REQUIRE(fixture.bridge->sync().has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 1U);
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_destroyed, 1U);
    CY_CHECK_FALSE(fixture.backend.server().body_alive(doomed));
    // The survivor's index was repaired by the swap-and-pop rather than left pointing at the hole.
    CY_CHECK_FALSE(fixture.bridge->body_of(second).is_null());

    CY_REQUIRE(fixture.world.destroy(second).has_value());
    CY_REQUIRE(fixture.bridge->sync().has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 0U);
    CY_CHECK_EQ(fixture.bridge->statistics().bodies_destroyed, 2U);

    // And a step over an empty world publishes nothing at all rather than naming a dead entity.
    const u64 orphans_before = fixture.bridge->statistics().orphan_publications;
    CY_REQUIRE(fixture.run(4).has_value());
    CY_CHECK_EQ(fixture.bridge->statistics().orphan_publications, orphans_before);
}

CY_TEST_CASE("the bridge steps in the Physics stage and in no other") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    const cy::ecs::Entity crate = fixture.node("Crate", cy::Vec3{0.0F, 4.0F, 0.0F});
    CY_REQUIRE(crate.valid());
    CY_REQUIRE(fixture.box(crate, cy::Vec3{0.5F, 0.5F, 0.5F}));
    CY_REQUIRE(fixture.dynamic(crate));

    cy::ecs::Schedule schedule(fixture.world);
    const auto installed = fixture.bridge->install(schedule, fixture.clock);
    CY_REQUIRE(installed.has_value());
    CY_REQUIRE(schedule.build().has_value());
    CY_CHECK_EQ(schedule.system_count(cy::ecs::Stage::Physics), 1U);
    CY_CHECK_EQ(schedule.system_count(cy::ecs::Stage::Simulation), 0U);
    CY_CHECK_EQ(schedule.system_count(cy::ecs::Stage::Frame), 0U);

    // Running a stage that is not Physics steps nothing. This is the requirement's "SHALL never
    // step during a variable-rate frame" as a property of where the system is registered, rather
    // than as a discipline the caller has to keep.
    fixture.clock.advance();
    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Frame).has_value());
    CY_CHECK_EQ(fixture.bridge->statistics().steps, 0U);

    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Physics).has_value());
    CY_CHECK_EQ(fixture.bridge->statistics().steps, 1U);
    CY_CHECK(fixture.bridge->last_error().has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 1U);

    for (u32 tick = 0; tick < 30; ++tick) {
        fixture.clock.advance();
        CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Physics).has_value());
    }
    CY_CHECK_EQ(fixture.bridge->statistics().steps, 31U);
    CY_CHECK_EQ(fixture.clock.tick(), 31U);
    CY_CHECK_LT(fixture.position_of(crate).y, 4.0F);
}

CY_TEST_CASE("a joint and a character body are counted rather than silently ignored") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);

    const cy::ecs::Entity hinge = fixture.node("Hinge", cy::Vec3{});
    CY_REQUIRE(hinge.valid());
    Joint joint;
    joint.description.type = ConstraintType::Hinge;
    CY_REQUIRE(fixture.world.add(hinge, fixture.components.joint, &joint).has_value());

    const cy::ecs::Entity walker = fixture.node("Walker", cy::Vec3{});
    CY_REQUIRE(walker.valid());
    CharacterBody character;
    CY_REQUIRE(
        fixture.world.add(walker, fixture.components.character_body, &character).has_value());

    CY_REQUIRE(fixture.bridge->sync().has_value());
    CY_CHECK_EQ(fixture.bridge->statistics().joints_deferred, 1U);
    CY_CHECK_EQ(fixture.bridge->statistics().characters_deferred, 1U);
    // Neither produced a body, which is the honest answer while no backend maps constraints:
    // `Capabilities::constraints` is false in both, and a bridge that tried would fail every world.
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 0U);
}
