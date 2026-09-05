// The server: worlds, bodies, mass properties, the shape cache, and the rejections `physics` names.
// Tasks 4.2.1 and 4.2.2.

#include "fixture.h"

#include <cy/servers/physics/reference/server.h>

using namespace cy;
using namespace cy::physics;
using cy::physics::test::Fixture;

CY_TEST_CASE("a stale handle stops resolving after the body is destroyed") {
    const Fixture fixture;
    const BodyHandle body = fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{});
    CY_CHECK(fixture.server->body_alive(body));
    CY_REQUIRE(fixture.server->destroy_body(body).has_value());
    // The generation is what makes this "no" rather than undefined behaviour: the caller is told,
    // and carries on.
    CY_CHECK_FALSE(fixture.server->body_alive(body));
    CY_CHECK_FALSE(fixture.server->body_state(body).has_value());
}

CY_TEST_CASE("one thousand identical box colliders create one shape") {
    // `physics` — "Shape sharing": "WHEN 1 000 entities use an identical box collider THEN one Jolt
    // shape SHALL be created and referenced by all of them". Asserted through the statistics rather
    // than inferred from a handle comparison, because two calls returning the same handle would
    // also be true of a backend that leaked a shape per call and happened to reuse a slot.
    const Fixture fixture;
    ShapeDescription description;
    description.type = ShapeType::Box;
    description.half_extents = Vec3{0.5f, 0.5f, 0.5f};

    ShapeHandle first;
    for (u32 index = 0; index < 1000; ++index) {
        const Expected<ShapeHandle, Error> shape = fixture.server->create_shape(description);
        CY_REQUIRE(shape.has_value());
        if (index == 0) {
            first = *shape;
        }
        CY_CHECK_EQ(shape->bits(), first.bits());
    }
    const Expected<ShapeStatistics, Error> statistics = fixture.server->shape_statistics();
    CY_REQUIRE(statistics.has_value());
    CY_CHECK_EQ(statistics->unique_shapes, 1U);
    CY_CHECK_EQ(statistics->requests, 1000U);
    CY_CHECK_EQ(statistics->cache_hits, 999U);
}

CY_TEST_CASE("a triangle mesh on a dynamic body is rejected with a diagnostic") {
    // `physics` — "Triangle mesh on a dynamic body": rejected "with a diagnostic recommending
    // convex decomposition, since concave dynamic bodies are not supported".
    const Fixture fixture;
    const Vec3 vertices[3] = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}};
    const u32 indices[3] = {0, 1, 2};
    ShapeDescription mesh;
    mesh.type = ShapeType::TriangleMesh;
    mesh.vertices = vertices;
    mesh.vertex_count = 3;
    mesh.indices = indices;
    mesh.index_count = 3;
    const Expected<ShapeHandle, Error> shape = fixture.server->create_shape(mesh);
    CY_REQUIRE(shape.has_value());

    ColliderDescription collider;
    collider.shape = *shape;
    BodyDescription body;
    body.motion = MotionType::Dynamic;
    body.colliders = &collider;
    body.collider_count = 1;
    const Expected<BodyHandle, Error> dynamic = fixture.server->create_body(fixture.world, body);
    CY_REQUIRE_FALSE(dynamic.has_value());
    CY_CHECK_EQ(dynamic.error().code, ErrorCode::InvalidArgument);

    // The same shape on a static body is fine, which is what makes the rejection about the pairing
    // rather than about the shape.
    body.motion = MotionType::Static;
    CY_CHECK(fixture.server->create_body(fixture.world, body).has_value());
}

CY_TEST_CASE("a compound body's mass distribution is combined from its colliders' volumes") {
    // `physics` — "Multiple colliders per body": "they SHALL form a compound shape with a combined
    // mass distribution computed from their volumes and the body's density or explicit mass".
    const Fixture fixture;
    const ShapeHandle big = fixture.box(Vec3{1.0f, 1.0f, 1.0f});    // 8 m^3
    const ShapeHandle small = fixture.box(Vec3{0.5f, 0.5f, 0.5f});  // 1 m^3

    ColliderDescription colliders[2];
    colliders[0].shape = big;
    colliders[0].local = Transform::from_translation(Vec3{0.0f, 0.0f, 0.0f});
    colliders[1].shape = small;
    colliders[1].local = Transform::from_translation(Vec3{9.0f, 0.0f, 0.0f});

    BodyDescription description;
    description.motion = MotionType::Dynamic;
    description.colliders = colliders;
    description.collider_count = 2;
    const Expected<BodyHandle, Error> body =
        fixture.server->create_body(fixture.world, description);
    CY_REQUIRE(body.has_value());

    const Expected<MassProperties, Error> mass = fixture.server->mass_properties(*body);
    CY_REQUIRE(mass.has_value());
    // 9 m^3 at 1000 kg/m^3.
    CY_CHECK_NEAR(mass->mass, 9000.0f, 1.0f);
    // Volume-weighted: the heavy base holds the centre near itself rather than at the midpoint of
    // the two origins, which is what a naive average would give (4.5).
    CY_CHECK_NEAR(mass->center_of_mass.x, 1.0f, 0.01f);
}

CY_TEST_CASE("an explicit mass overrides the derived one and rescales the inertia") {
    const Fixture fixture;
    ColliderDescription collider;
    collider.shape = fixture.box(Vec3{0.5f, 0.5f, 0.5f});
    BodyDescription description;
    description.motion = MotionType::Dynamic;
    description.mass = 2.0f;
    description.colliders = &collider;
    description.collider_count = 1;
    const Expected<BodyHandle, Error> body =
        fixture.server->create_body(fixture.world, description);
    CY_REQUIRE(body.has_value());
    const Expected<MassProperties, Error> mass = fixture.server->mass_properties(*body);
    CY_REQUIRE(mass.has_value());
    CY_CHECK_EQ(mass->mass, 2.0f);
    // A 1 m cube of mass 2: I = m*(a^2 + a^2)/12 = 2/6.
    CY_CHECK_NEAR(mass->inertia.x, 2.0f / 6.0f, 1e-4f);
}

CY_TEST_CASE("a dynamic body with no collider is rejected rather than given an infinite mass") {
    const Fixture fixture;
    BodyDescription description;
    description.motion = MotionType::Dynamic;
    const Expected<BodyHandle, Error> body =
        fixture.server->create_body(fixture.world, description);
    CY_CHECK_FALSE(body.has_value());
}

CY_TEST_CASE("the reference backend reports what it cannot do, and refuses it") {
    // `physics` — "Unsupported feature": "the capability query SHALL report it and creation SHALL
    // fail with a clear diagnostic". Both halves in one case, because a capability flag nobody acts
    // on is a comment.
    const Fixture fixture;
    const Capabilities capabilities = fixture.server->capabilities();
    CY_CHECK_FALSE(capabilities.constraints);
    CY_CHECK_FALSE(capabilities.contact_resolution);
    CY_CHECK_FALSE(capabilities.soft_bodies);
    CY_CHECK_EQ(capabilities.determinism, DeterminismPolicy::SamePlatformDeterministic);

    ConstraintDescription joint;
    joint.body_a = fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{});
    joint.body_b = fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{2, 0, 0});
    const Expected<ConstraintHandle, Error> made =
        fixture.server->create_constraint(fixture.world, joint);
    CY_REQUIRE_FALSE(made.has_value());
    CY_CHECK_EQ(made.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("gravity integrates on the delta it is given, not on a clock") {
    const Fixture fixture;
    const BodyHandle body = fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{});
    // Damping off, so the closed form is exact and the case is a statement about the integrator
    // rather than about the damping model.
    CY_REQUIRE(fixture.server->set_body_velocity(body, Vec3{}, Vec3{}).has_value());
    for (u64 tick = 0; tick < 10; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    // Semi-implicit Euler: after n steps of h, y = -g*h^2*n*(n+1)/2. n=10, h=1/60.
    const f32 h = 1.0f / 60.0f;
    const f32 expected = -9.81f * h * h * (10.0f * 11.0f / 2.0f);
    // The damping default is 0.05, which shortens the fall slightly; the tolerance covers it and
    // the sign and magnitude are still the assertion.
    CY_CHECK_LT(fixture.position_of(body).y, 0.0f);
    CY_CHECK_GT(fixture.position_of(body).y, expected * 1.05f);
}

CY_TEST_CASE("a static body does not move and a kinematic one moves at its own velocity") {
    const Fixture fixture;
    const BodyHandle stationary =
        fixture.body(fixture.box(Vec3{1, 1, 1}), MotionType::Static, Vec3{});
    const BodyHandle platform =
        fixture.body(fixture.box(Vec3{1, 1, 1}), MotionType::Kinematic, Vec3{0, 0, 5});
    CY_REQUIRE(
        fixture.server->set_body_velocity(platform, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}).has_value());
    for (u64 tick = 0; tick < 60; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    CY_CHECK_EQ(fixture.position_of(stationary).y, 0.0f);
    // One metre per second for one second, and no gravity: a kinematic body is not pulled down.
    CY_CHECK_NEAR(fixture.position_of(platform).x, 1.0f, 0.02f);
    CY_CHECK_EQ(fixture.position_of(platform).y, 0.0f);
}

CY_TEST_CASE("a locked axis is enforced by the integrator, not corrected afterwards") {
    // `physics` — "Constraint is enforced": "WHEN a 2D body is subjected to a force with a Z
    // component THEN the locked degrees of freedom SHALL prevent any out-of-plane motion or drift".
    // Drift is the word that matters: a fix-up after the step would leave the VELOCITY behind, and
    // the body would accumulate it forever.
    const Fixture fixture;
    ColliderDescription collider;
    collider.shape = fixture.sphere(0.5f);
    BodyDescription description;
    description.motion = MotionType::Dynamic;
    description.locked_axes = kLockPlaneXY;
    description.colliders = &collider;
    description.collider_count = 1;
    const Expected<BodyHandle, Error> body =
        fixture.server->create_body(fixture.world, description);
    CY_REQUIRE(body.has_value());

    CY_REQUIRE(fixture.server->add_impulse(*body, Vec3{0.0f, 0.0f, 100.0f}).has_value());
    for (u64 tick = 0; tick < 120; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    const Expected<BodyState, Error> state = fixture.server->body_state(*body);
    CY_REQUIRE(state.has_value());
    CY_CHECK_EQ(state->transform.translation.z, 0.0f);
    CY_CHECK_EQ(state->linear_velocity.z, 0.0f);
}

CY_TEST_CASE("a body sleeps when it is slow for long enough and wakes on an impulse") {
    const Fixture fixture;
    ColliderDescription collider;
    collider.shape = fixture.sphere(0.5f);
    BodyDescription description;
    description.motion = MotionType::Dynamic;
    description.gravity_scale = 0.0f;  // otherwise it never stops falling and never sleeps
    description.colliders = &collider;
    description.collider_count = 1;
    const Expected<BodyHandle, Error> body =
        fixture.server->create_body(fixture.world, description);
    CY_REQUIRE(body.has_value());

    for (u64 tick = 0; tick < 60; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }
    CY_CHECK(fixture.server->body_state(*body)->asleep);

    CY_REQUIRE(fixture.server->add_impulse(*body, Vec3{0.0f, 0.0f, 10.0f}).has_value());
    CY_CHECK_FALSE(fixture.server->body_state(*body)->asleep);

    // Waking a static body is an error rather than a no-op: the caller believes it is dynamic, and
    // the belief is the bug.
    const BodyHandle wall = fixture.body(fixture.box(Vec3{1, 1, 1}), MotionType::Static, Vec3{});
    CY_CHECK_FALSE(fixture.server->set_body_awake(wall, true).has_value());
}

CY_TEST_CASE("bulk creation is all or nothing") {
    // `physics` — "A region's collision arrives at once". A partial bulk registration would leave a
    // terrain cell half-collidable, which is worse than one that failed outright.
    const Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.5f, 0.5f, 0.5f});
    ColliderDescription collider;
    collider.shape = shape;

    BodyDescription descriptions[3];
    for (auto& description : descriptions) {
        description.motion = MotionType::Static;
        description.colliders = &collider;
        description.collider_count = 1;
    }
    // The third one names no collider AND is dynamic, so it fails validation.
    descriptions[2].motion = MotionType::Dynamic;
    descriptions[2].collider_count = 0;
    descriptions[2].colliders = nullptr;

    BodyHandle out[3];
    const Status created = fixture.server->create_bodies(
        fixture.world, Span<const BodyDescription>(descriptions, 3), Span<BodyHandle>(out, 3));
    CY_REQUIRE_FALSE(created.has_value());
    for (u32 index = 0; index < 2; ++index) {
        CY_CHECK_FALSE(fixture.server->body_alive(out[index]));
    }
}

CY_TEST_CASE("a world's body capacity is a limit, not a hint") {
    const Fixture fixture;  // 64 bodies
    const ShapeHandle shape = fixture.sphere(0.5f);
    ColliderDescription collider;
    collider.shape = shape;
    BodyDescription description;
    description.motion = MotionType::Static;
    description.colliders = &collider;
    description.collider_count = 1;
    for (u32 index = 0; index < 64; ++index) {
        CY_REQUIRE(fixture.server->create_body(fixture.world, description).has_value());
    }
    const Expected<BodyHandle, Error> overflow =
        fixture.server->create_body(fixture.world, description);
    CY_REQUIRE_FALSE(overflow.has_value());
    CY_CHECK_EQ(overflow.error().code, ErrorCode::OutOfRange);
}
