// The bridge against a solver that actually resolves contacts. M8.a task 4.1, and the physics half
// of the closing artefact's claim.
//
// ================================================================================================
// WHY THIS IS A SEPARATE FILE FROM test_bridge.cpp
// ================================================================================================
//
// The reference backend **integrates motion and does not resolve contacts** —
// `Capabilities::contact_resolution` is false and its own header says so in its first five lines.
// That is the right shape for an interface conformance backend, and it is why `test_bridge.cpp` can
// run in every configuration: it asserts that gravity reached the body and that the result came
// back through the sink, which is what the bridge is responsible for.
//
// **A sphere coming to REST on a box is a different claim**, it is the one M8.a's artefact makes
// ("create a sphere, drop it on a box, press play, watch it fall"), and only a real solver can
// answer it. So it is here, behind `CY_PHYSICS` — which is ON by default — and it is an integration
// test because settling a contact takes a second of simulated time.
//
// With `-D CY_PHYSICS=OFF` this file compiles to nothing and says so, rather than quietly passing.

#include "fixture.h"

#include <cy/test/test.h>

#if defined(CY_PHYSICS)
#    include <cy/backends/physics/jolt/server.h>
#endif

using cy::f32;
using cy::u32;
using namespace cy::physics;
using namespace cy::physics::test;

#if defined(CY_PHYSICS)

namespace {

/// The same shape as `test::Fixture`, over Jolt instead of the reference backend.
///
/// Deliberately not a template parameter on the shared fixture: `fixture.h` is included by suites
/// that build in every configuration, and a template that mentioned `jolt::create_server` would put
/// the backend's header on their include path.
struct JoltFixture {
    JoltFixture() noexcept : world(allocator()), tree(world) {
        const cy::Expected<PhysicsServer*, cy::Error> made =
            jolt::create_server(allocator(), nullptr);
        if (!made) {
            return;
        }
        server = *made;
        if (!server->initialize()) {
            return;
        }
        WorldDescription description;
        description.name = cy::Name::intern("physics-bridge-jolt");
        description.body_capacity = 64;
        description.body_pair_capacity = 512;
        description.contact_constraint_capacity = 512;
        const cy::Expected<WorldHandle, cy::Error> created = server->create_world(description);
        if (!created) {
            return;
        }
        physics_world = *created;

        if (!world.initialize() || !tree.initialize()) {
            return;
        }
        const auto registered = PhysicsComponents::register_all(world);
        if (!registered) {
            return;
        }
        components = *registered;
        cy::determinism::ClockConfig config;
        config.mode = cy::determinism::TickMode::FixedStep;
        if (!clock.configure(config)) {
            return;
        }
        auto built = cy::make_unique<PhysicsBridge>(allocator(), allocator(), tree, components,
                                                    *server, physics_world);
        if (!built) {
            return;
        }
        bridge = std::move(*built);
        started = true;
    }

    ~JoltFixture() {
        // The bridge lets its bodies go before the world does, and the world before the server.
        // Getting this order wrong is task 4.4's whole subject; here it is simply obeyed.
        bridge.reset();
        if (server != nullptr) {
            if (!physics_world.is_null()) {
                (void)server->destroy_world(physics_world);
            }
            server->shutdown();
            jolt::destroy_server(server, allocator());
        }
    }

    JoltFixture(const JoltFixture&) = delete;
    JoltFixture& operator=(const JoltFixture&) = delete;

    [[nodiscard]] cy::ecs::Entity node(const char* name, cy::Vec3 position) noexcept {
        auto made = tree.create_node(cy::Name::intern(name), tree.root(), cy::Name());
        if (!made) {
            return cy::ecs::Entity{};
        }
        cy::scene::LocalTransform local;
        local.value = cy::Transform::from_translation(position);
        if (!world.set(made->entity(), tree.components().local_transform, local)) {
            return cy::ecs::Entity{};
        }
        return made->entity();
    }

    [[nodiscard]] cy::Vec3 position_of(cy::ecs::Entity entity) const noexcept {
        const auto* local =
            world.get<cy::scene::LocalTransform>(entity, tree.components().local_transform);
        return local == nullptr ? cy::Vec3{} : local->value.translation;
    }

    [[nodiscard]] cy::Status run(u32 ticks) noexcept {
        for (u32 tick = 0; tick < ticks; ++tick) {
            clock.advance();
            if (cy::Status advanced = bridge->advance(clock); !advanced) {
                return advanced;
            }
        }
        return cy::ok();
    }

    cy::ecs::World world;
    cy::scene::SceneTree tree;
    PhysicsServer* server = nullptr;
    WorldHandle physics_world;
    PhysicsComponents components;
    cy::UniquePtr<PhysicsBridge> bridge;
    cy::determinism::SimulationClock clock;
    bool started = false;
};

}  // namespace

CY_TEST_CASE("a sphere authored above a box falls and comes to rest on it") {
    JoltFixture fixture;
    CY_REQUIRE(fixture.started);
    CY_REQUIRE(fixture.server->capabilities().contact_resolution);

    // THE ARTEFACT'S OWN ARRANGEMENT, in components rather than in backend calls: two nodes with a
    // placement, a collider each, one static and one dynamic. Nothing below names a body, a shape
    // or a step — the bridge does all three from what the components say, which is the difference
    // between this and `samples/04-character`'s host.
    const cy::ecs::Entity ground = fixture.node("Ground", cy::Vec3{0.0F, 0.0F, 0.0F});
    CY_REQUIRE(ground.valid());
    Collider slab;
    slab.shape.type = ShapeType::Box;
    slab.shape.half_extents = cy::Vec3{5.0F, 0.5F, 5.0F};
    CY_REQUIRE(fixture.world.add(ground, fixture.components.collider, &slab).has_value());
    StaticBody immovable;
    CY_REQUIRE(fixture.world.add(ground, fixture.components.static_body, &immovable).has_value());

    const cy::ecs::Entity ball = fixture.node("Ball", cy::Vec3{0.0F, 4.0F, 0.0F});
    CY_REQUIRE(ball.valid());
    Collider sphere;
    sphere.shape.type = ShapeType::Sphere;
    sphere.shape.radius = 0.5F;
    CY_REQUIRE(fixture.world.add(ball, fixture.components.collider, &sphere).has_value());
    RigidBody dynamic;
    CY_REQUIRE(fixture.world.add(ball, fixture.components.rigid_body, &dynamic).has_value());

    CY_REQUIRE(fixture.run(180).has_value());
    CY_CHECK_EQ(fixture.bridge->tracked_bodies(), 2U);
    CY_CHECK_EQ(fixture.bridge->statistics().steps, 180U);

    // The slab's top is at y = 0.5 and the sphere's radius is 0.5, so it rests at y = 1.0. The
    // tolerance is the solver's penetration allowance, not a fudge for a number nobody predicted.
    const cy::Vec3 resting = fixture.position_of(ball);
    CY_CHECK_NEAR(resting.y, 1.0F, 0.05);
    CY_CHECK_NEAR(resting.x, 0.0F, 0.02);
    CY_CHECK_NEAR(resting.z, 0.0F, 0.02);

    // The static body did not move, which is what static means and what a bridge publishing every
    // body's transform indiscriminately would break.
    CY_CHECK_NEAR(fixture.position_of(ground).y, 0.0F, 0.0001);
}

CY_TEST_CASE("the same world stepped twice produces the same resting place") {
    // Not a determinism claim about the backend — `physics`' determinism.h is careful about what is
    // and is not promised — but about the BRIDGE: nothing in it depends on iteration order over a
    // hash map, on an allocation address, or on anything else that differs between two identical
    // runs in one process. That is the property `rendering::SnapshotExtractor`'s last case checks
    // for the same reason, and it is cheap to keep and expensive to recover.
    cy::Vec3 first;
    cy::Vec3 second;
    for (u32 round = 0; round < 2; ++round) {
        JoltFixture fixture;
        CY_REQUIRE(fixture.started);

        const cy::ecs::Entity ground = fixture.node("Ground", cy::Vec3{0.0F, 0.0F, 0.0F});
        CY_REQUIRE(ground.valid());
        Collider slab;
        slab.shape.type = ShapeType::Box;
        slab.shape.half_extents = cy::Vec3{5.0F, 0.5F, 5.0F};
        CY_REQUIRE(fixture.world.add(ground, fixture.components.collider, &slab).has_value());
        StaticBody immovable;
        CY_REQUIRE(
            fixture.world.add(ground, fixture.components.static_body, &immovable).has_value());

        for (u32 index = 0; index < 6; ++index) {
            const cy::ecs::Entity ball = fixture.node(
                "Ball",
                cy::Vec3{static_cast<f32>(index) * 0.3F, 2.0F + static_cast<f32>(index), 0.0F});
            CY_REQUIRE(ball.valid());
            Collider sphere;
            sphere.shape.type = ShapeType::Sphere;
            sphere.shape.radius = 0.4F;
            CY_REQUIRE(fixture.world.add(ball, fixture.components.collider, &sphere).has_value());
            RigidBody dynamic;
            CY_REQUIRE(
                fixture.world.add(ball, fixture.components.rigid_body, &dynamic).has_value());
            if (index == 5) {
                CY_REQUIRE(fixture.run(150).has_value());
                (round == 0 ? first : second) = fixture.position_of(ball);
            }
        }
    }
    CY_CHECK_EQ(first.x, second.x);
    CY_CHECK_EQ(first.y, second.y);
    CY_CHECK_EQ(first.z, second.z);
}

#else

CY_TEST_CASE("physics: this build has no solver that resolves contacts") {
    // `-D CY_PHYSICS=OFF`. Stated rather than skipped silently: a suite that reported success with
    // its only case compiled out is the failure mode a whole milestone was once graded on.
    CY_TEST_MESSAGE(
        "CY_PHYSICS is off; the resting-contact cases need a backend that resolves "
        "contacts and the reference backend declares that it does not");
    CY_CHECK(true);
}

#endif  // CY_PHYSICS
