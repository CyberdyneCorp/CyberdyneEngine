// The 2D API: genuinely two-dimensional, over the 3D solver constrained to a plane. Task 4.2.1.

#include "fixture.h"

#include <cy/servers/physics/physics2d.h>

using namespace cy;
using namespace cy::physics;
using cy::physics::test::Fixture;
namespace two_d = cy::physics::two_d;

CY_TEST_CASE("a 2D raycast takes a Vec2 and returns a Vec2 normal") {
    // `physics` — "2D API is 2D": "WHEN gameplay code queries a 2D raycast THEN it SHALL pass a
    // Vec2 origin and direction and receive a Vec2 normal, with no awareness of the third axis".
    // Nothing in this case names a third axis, which is the point: it is unspellable at this API
    // rather than defaulted.
    const Fixture fixture;
    (void)fixture.body(fixture.box(Vec3{10.0f, 0.5f, 1.0f}), MotionType::Static,
                       Vec3{0.0f, -0.5f, 0.0f});

    two_d::RayCastInput2D ray;
    ray.origin = Vec2{0.0f, 5.0f};
    ray.direction = Vec2{0.0f, -1.0f};
    ray.max_distance = 20.0f;
    const Expected<two_d::RayCastHit2D, Error> hit =
        two_d::raycast(*fixture.server, fixture.world, ray, QueryFilter{});
    CY_REQUIRE(hit.has_value());
    CY_CHECK_FALSE(hit->body.is_null());
    CY_CHECK_NEAR(hit->distance, 5.0f, 1e-3f);
    CY_CHECK_NEAR(hit->normal.y, 1.0f, 1e-3f);
    CY_CHECK_NEAR(hit->position.y, 0.0f, 1e-3f);
}

CY_TEST_CASE("a 2D body cannot leave the plane, however it is pushed") {
    // `physics` — "Constraint is enforced". The lock is applied by `two_d::create_body` rather than
    // by the caller, so this is also the case that says a caller cannot forget it.
    const Fixture fixture;
    ColliderDescription collider;
    collider.shape = fixture.sphere(0.5f);
    two_d::BodyDescription2D description;
    description.transform.position = Vec2{0.0f, 5.0f};
    description.colliders = &collider;
    description.collider_count = 1;
    const Expected<BodyHandle, Error> body =
        two_d::create_body(*fixture.server, fixture.world, description);
    CY_REQUIRE(body.has_value());

    CY_REQUIRE(fixture.server->add_impulse(*body, Vec3{0.0f, 0.0f, 500.0f}).has_value());
    CY_REQUIRE(fixture.server->add_angular_impulse(*body, Vec3{500.0f, 500.0f, 5.0f}).has_value());
    for (u64 tick = 0; tick < 120; ++tick) {
        CY_REQUIRE(fixture.step(tick).has_value());
    }

    const Expected<two_d::BodyState2D, Error> state = two_d::body_state(*fixture.server, *body);
    CY_REQUIRE(state.has_value());
    // It fell, so the simulation ran; and it did not drift out of the plane, which the 3D state
    // confirms in the axes the 2D view cannot even name.
    CY_CHECK_LT(state->transform.position.y, 5.0f);
    const Expected<BodyState, Error> raw = fixture.server->body_state(*body);
    CY_REQUIRE(raw.has_value());
    CY_CHECK_EQ(raw->transform.translation.z, 0.0f);
    CY_CHECK_EQ(raw->linear_velocity.z, 0.0f);
    CY_CHECK_EQ(raw->angular_velocity.x, 0.0f);
    CY_CHECK_EQ(raw->angular_velocity.y, 0.0f);
    // The only rotation a 2D body has is about +Z, and the round trip through the angle recovers
    // it.
    CY_CHECK_NE(state->transform.rotation, 0.0f);
}

CY_TEST_CASE("an angle survives the round trip through a quaternion") {
    for (i32 degrees = -170; degrees <= 170; degrees += 17) {
        const f32 radians = static_cast<f32>(degrees) * cy::math::kDegToRad;
        CY_CHECK_NEAR(two_d::quat_to_angle(two_d::angle_to_quat(radians)), radians, 1e-4f);
    }
}

CY_TEST_CASE("a 2D circle extrudes to a cylinder, not to a sphere") {
    // A sphere's silhouette in the plane is a circle only at z = 0, so a body resting on one would
    // touch at a point that moves with any residual Z drift.
    two_d::ShapeDescription2D circle;
    circle.type = two_d::ShapeType2D::Circle;
    circle.radius = 0.75f;
    const Expected<ShapeDescription, Error> extruded = two_d::extrude(circle, Span<Vec3>());
    CY_REQUIRE(extruded.has_value());
    CY_CHECK_EQ(extruded->type, ShapeType::Cylinder);
    CY_CHECK_EQ(extruded->radius, 0.75f);
    CY_CHECK_EQ(extruded->half_height, two_d::kExtrusionHalfDepth);
}

CY_TEST_CASE("a 2D polygon needs a scratch span of two 3D points per 2D point") {
    const Vec2 points[4] = {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}};
    two_d::ShapeDescription2D polygon;
    polygon.type = two_d::ShapeType2D::Polygon;
    polygon.points = points;
    polygon.point_count = 4;

    Vec3 too_small[4];
    CY_CHECK_FALSE(two_d::extrude(polygon, Span<Vec3>(too_small, 4)).has_value());

    Vec3 scratch[8];
    const Expected<ShapeDescription, Error> extruded =
        two_d::extrude(polygon, Span<Vec3>(scratch, 8));
    CY_REQUIRE(extruded.has_value());
    CY_CHECK_EQ(extruded->type, ShapeType::ConvexHull);
    CY_CHECK_EQ(extruded->point_count, 8U);
    CY_CHECK_EQ(extruded->points[0].z, -two_d::kExtrusionHalfDepth);
    CY_CHECK_EQ(extruded->points[4].z, two_d::kExtrusionHalfDepth);
}
