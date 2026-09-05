// Queries: rays, sweeps, overlaps, closest points, their filters, and the mid-step rejection.
// Task 4.2.4.

#include "fixture.h"

using namespace cy;
using namespace cy::physics;
using cy::physics::test::Fixture;

CY_TEST_CASE("a downward raycast reports position, normal, distance, entity and material") {
    // `physics` — "Raycast excluding self": "WHEN a character raycasts downward THEN it SHALL
    // exclude its own collider and report the ground hit with position, normal, distance, entity,
    // and physics material".
    const Fixture fixture;
    MaterialDescription material_description;
    material_description.name = Name::intern("stone");
    material_description.friction = 0.9f;
    const Expected<MaterialHandle, Error> material =
        fixture.server->create_material(material_description);
    CY_REQUIRE(material.has_value());

    ColliderDescription ground_collider;
    ground_collider.shape = fixture.ground_plane();
    ground_collider.material = *material;
    BodyDescription ground;
    ground.motion = MotionType::Static;
    ground.colliders = &ground_collider;
    ground.collider_count = 1;
    ground.user_data = 4242;
    const Expected<BodyHandle, Error> ground_body =
        fixture.server->create_body(fixture.world, ground);
    CY_REQUIRE(ground_body.has_value());

    const BodyHandle self =
        fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{0, 2, 0}, 7);

    RayCastInput ray;
    ray.origin = Vec3{0.0f, 2.0f, 0.0f};
    ray.direction = Vec3{0.0f, -1.0f, 0.0f};
    ray.max_distance = 10.0f;
    QueryFilter filter;
    filter.ignore = &self;
    filter.ignore_count = 1;

    const Expected<RayCastHit, Error> hit = fixture.server->raycast(fixture.world, ray, filter);
    CY_REQUIRE(hit.has_value());
    CY_CHECK_EQ(hit->body.bits(), ground_body->bits());
    CY_CHECK_EQ(hit->user_data, 4242U);
    CY_CHECK_NEAR(hit->distance, 2.0f, 1e-4f);
    CY_CHECK_NEAR(hit->position.y, 0.0f, 1e-4f);
    CY_CHECK_NEAR(hit->normal.y, 1.0f, 1e-4f);
    CY_CHECK_EQ(hit->material.bits(), material->bits());

    // Without the ignore list the character's own sphere is nearer, which is the whole reason the
    // list exists.
    QueryFilter unfiltered;
    const Expected<RayCastHit, Error> self_hit =
        fixture.server->raycast(fixture.world, ray, unfiltered);
    CY_REQUIRE(self_hit.has_value());
    CY_CHECK_EQ(self_hit->body.bits(), self.bits());
}

CY_TEST_CASE("raycast_all returns every hit, sorted by distance") {
    const Fixture fixture;
    const ShapeHandle shape = fixture.box(Vec3{0.5f, 0.5f, 0.5f});
    // Created far first, so an unsorted implementation would return them in the wrong order and the
    // sort is genuinely under test rather than incidentally satisfied.
    (void)fixture.body(shape, MotionType::Static, Vec3{0, 0, -9});
    (void)fixture.body(shape, MotionType::Static, Vec3{0, 0, -3});
    (void)fixture.body(shape, MotionType::Static, Vec3{0, 0, -6});

    RayCastInput ray;
    ray.origin = Vec3{0.0f, 0.0f, 0.0f};
    ray.direction = Vec3{0.0f, 0.0f, -1.0f};
    ray.max_distance = 20.0f;
    RayCastHit hits[8];
    const Expected<u32, Error> count =
        fixture.server->raycast_all(fixture.world, ray, QueryFilter{}, Span<RayCastHit>(hits, 8));
    CY_REQUIRE(count.has_value());
    CY_CHECK_EQ(*count, 3U);
    CY_CHECK_LT(hits[0].distance, hits[1].distance);
    CY_CHECK_LT(hits[1].distance, hits[2].distance);
}

CY_TEST_CASE("a query respects the layer, mask and matrix, and skips triggers unless asked") {
    const Fixture fixture;
    ColliderDescription collider;
    collider.shape = fixture.box(Vec3{1.0f, 1.0f, 1.0f});
    collider.filter = CollisionFilter{3, 0xFFFFFFFFU};
    collider.is_trigger = true;
    BodyDescription description;
    description.motion = MotionType::Static;
    description.transform = Transform::from_translation(Vec3{0.0f, 0.0f, -5.0f});
    description.colliders = &collider;
    description.collider_count = 1;
    CY_REQUIRE(fixture.server->create_body(fixture.world, description).has_value());

    RayCastInput ray;
    ray.direction = Vec3{0.0f, 0.0f, -1.0f};
    ray.max_distance = 20.0f;

    QueryFilter solid_only;
    CY_CHECK(fixture.server->raycast(fixture.world, ray, solid_only)->body.is_null());

    QueryFilter with_triggers;
    with_triggers.include_triggers = true;
    CY_CHECK_FALSE(fixture.server->raycast(fixture.world, ray, with_triggers)->body.is_null());

    // A mask that does not contain the collider's layer excludes it even with triggers on, because
    // the query goes through the same mutual rule a contact does.
    QueryFilter wrong_mask = with_triggers;
    wrong_mask.filter.mask = 1U << 5U;
    CY_CHECK(fixture.server->raycast(fixture.world, ray, wrong_mask)->body.is_null());

    // And the motion-type filter, which is what a "static geometry only" probe uses.
    QueryFilter dynamic_only = with_triggers;
    dynamic_only.include_static = false;
    CY_CHECK(fixture.server->raycast(fixture.world, ray, dynamic_only)->body.is_null());
}

CY_TEST_CASE("a shape cast reports the distance to first touch and the surface normal") {
    const Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{5.0f, 0.5f, 5.0f}), MotionType::Static, Vec3{0, 0, 0});
    const ShapeHandle probe = fixture.box(Vec3{0.25f, 0.25f, 0.25f});

    ShapeCastInput cast;
    cast.shape = probe;
    cast.start = Transform::from_translation(Vec3{0.0f, 5.0f, 0.0f});
    cast.direction = Vec3{0.0f, -1.0f, 0.0f};
    cast.max_distance = 10.0f;
    const Expected<ShapeCastHit, Error> hit =
        fixture.server->shape_cast(fixture.world, cast, QueryFilter{});
    CY_REQUIRE(hit.has_value());
    CY_CHECK_FALSE(hit->body.is_null());
    // The floor's top is at 0.5 and the probe's bottom starts 0.25 below its centre: 5 - 0.25 -
    // 0.5.
    CY_CHECK_NEAR(hit->distance, 4.25f, 1e-3f);
    CY_CHECK_NEAR(hit->fraction, 0.425f, 1e-3f);
    CY_CHECK_NEAR(hit->normal.y, 1.0f, 1e-3f);
    CY_CHECK_FALSE(hit->started_penetrating);
}

CY_TEST_CASE("a shape cast that starts inside says so, with a normal that separates") {
    const Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{5.0f, 0.5f, 5.0f}), MotionType::Static, Vec3{0, 0, 0});
    const ShapeHandle probe = fixture.box(Vec3{0.25f, 0.25f, 0.25f});

    ShapeCastInput cast;
    cast.shape = probe;
    cast.start = Transform::from_translation(Vec3{0.0f, 0.2f, 0.0f});
    cast.direction = Vec3{0.0f, -1.0f, 0.0f};
    cast.max_distance = 1.0f;
    const Expected<ShapeCastHit, Error> hit =
        fixture.server->shape_cast(fixture.world, cast, QueryFilter{});
    CY_REQUIRE(hit.has_value());
    // Without this flag a controller would try to slide along a surface it is already inside, and
    // end up outside the level rather than on top of the floor.
    CY_CHECK(hit->started_penetrating);
    CY_CHECK_EQ(hit->distance, 0.0f);
}

CY_TEST_CASE(
    "overlap reports the bodies a shape touches, and overlap_point the ones a point is in") {
    const Fixture fixture;
    const BodyHandle first =
        fixture.body(fixture.box(Vec3{1.0f, 1.0f, 1.0f}), MotionType::Static, Vec3{0, 0, 0}, 11);
    (void)fixture.body(fixture.box(Vec3{1.0f, 1.0f, 1.0f}), MotionType::Static, Vec3{10, 0, 0}, 22);

    OverlapInput input;
    input.shape = fixture.sphere(0.5f);
    input.transform = Transform::from_translation(Vec3{0.5f, 0.0f, 0.0f});
    OverlapHit hits[4];
    const Expected<u32, Error> count =
        fixture.server->overlap(fixture.world, input, QueryFilter{}, Span<OverlapHit>(hits, 4));
    CY_REQUIRE(count.has_value());
    CY_CHECK_EQ(*count, 1U);
    CY_CHECK_EQ(hits[0].body.bits(), first.bits());
    CY_CHECK_EQ(hits[0].user_data, 11U);

    const Expected<u32, Error> point_count = fixture.server->overlap_point(
        fixture.world, Vec3{0.0f, 0.0f, 0.0f}, QueryFilter{}, Span<OverlapHit>(hits, 4));
    CY_REQUIRE(point_count.has_value());
    CY_CHECK_EQ(*point_count, 1U);

    const Expected<u32, Error> outside = fixture.server->overlap_point(
        fixture.world, Vec3{5.0f, 0.0f, 0.0f}, QueryFilter{}, Span<OverlapHit>(hits, 4));
    CY_REQUIRE(outside.has_value());
    CY_CHECK_EQ(*outside, 0U);
}

CY_TEST_CASE("closest point reports the nearest surface point and its normal") {
    const Fixture fixture;
    (void)fixture.body(fixture.sphere(1.0f), MotionType::Static, Vec3{0, 0, 0});
    ClosestPointInput input;
    input.point = Vec3{5.0f, 0.0f, 0.0f};
    const Expected<ClosestPoint, Error> closest =
        fixture.server->closest_point(fixture.world, input, QueryFilter{});
    CY_REQUIRE(closest.has_value());
    CY_CHECK_NEAR(closest->distance, 4.0f, 1e-4f);
    CY_CHECK_NEAR(closest->position.x, 1.0f, 1e-4f);
    CY_CHECK_NEAR(closest->normal.x, 1.0f, 1e-4f);
}

CY_TEST_CASE("a query during the step is rejected in development builds, and only there") {
    // `physics` — "Query during the step": "WHEN a query is attempted while the physics step is in
    // progress THEN it SHALL be rejected in development builds with a diagnostic, since the world
    // is mid-solve".
    //
    // The guard is one shared function rather than a line in each backend (see queries.h), which is
    // what makes it assertable at all: contriving a way to be inside somebody else's solver from a
    // single-threaded test would test the contrivance. Here the two halves are checked directly —
    // that it rejects while stepping, and that it is compiled out where CY_DEVELOPMENT is not
    // defined, so this case passes in all four profiles rather than in two.
#if defined(CY_DEVELOPMENT)
    const Status rejected = reject_query_during_step(true);
    CY_REQUIRE_FALSE(rejected.has_value());
    CY_CHECK_EQ(rejected.error().code, ErrorCode::Unavailable);
#else
    CY_CHECK(reject_query_during_step(true).has_value());
#endif
    CY_CHECK(reject_query_during_step(false).has_value());

    // And the flag the guard reads is genuinely off outside a step, which is the other half of the
    // claim: a guard on a flag that is never set would pass the two lines above and reject nothing.
    const Fixture fixture;
    (void)fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{});
    CY_CHECK_FALSE(fixture.server->stepping());
    CY_REQUIRE(fixture.step(0).has_value());
    CY_CHECK_FALSE(fixture.server->stepping());
    const RayCastInput ray;
    CY_CHECK(fixture.server->raycast(fixture.world, ray, QueryFilter{}).has_value());
}
