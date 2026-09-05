// Shape validation, bounds, mass properties and the cache key. Task 4.2.1.

#include <cy/servers/physics/shapes.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::physics;

CY_TEST_CASE(
    "two identical descriptions produce the same cache key and two different ones do not") {
    ShapeDescription a;
    a.type = ShapeType::Box;
    a.half_extents = Vec3{1.0f, 2.0f, 3.0f};
    ShapeDescription b = a;
    CY_CHECK_EQ(shape_key(a), shape_key(b));

    // One ulp apart is a different shape, deliberately: a tolerance here would share a shape
    // between two colliders an author made different on purpose.
    b.half_extents.x = 1.0000001f;
    CY_CHECK_NE(shape_key(a), shape_key(b));

    // A sphere of radius 1 and a box of half extent 1 must not collide in the cache just because
    // their numbers overlap. The type is folded in first for exactly this.
    ShapeDescription sphere;
    sphere.type = ShapeType::Sphere;
    sphere.radius = 1.0f;
    ShapeDescription box;
    box.type = ShapeType::Box;
    box.half_extents = Vec3{1.0f, 1.0f, 1.0f};
    CY_CHECK_NE(shape_key(sphere), shape_key(box));
}

CY_TEST_CASE("a mesh hashes its contents, so the same mesh from two buffers is one shape") {
    const Vec3 vertices_a[3] = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    const Vec3 vertices_b[3] = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    const u32 indices[3] = {0, 1, 2};
    ShapeDescription a;
    a.type = ShapeType::TriangleMesh;
    a.vertices = vertices_a;
    a.vertex_count = 3;
    a.indices = indices;
    a.index_count = 3;
    ShapeDescription b = a;
    b.vertices = vertices_b;
    // Different pointers, identical contents. Hashing the pointers would make this the case the
    // shape cache silently fails on — and it is the one `physics`' sharing requirement is about.
    CY_CHECK_NE(static_cast<const void*>(a.vertices), static_cast<const void*>(b.vertices));
    CY_CHECK_EQ(shape_key(a), shape_key(b));
}

CY_TEST_CASE("a degenerate shape is rejected where it is written") {
    ShapeDescription sphere;
    sphere.type = ShapeType::Sphere;
    sphere.radius = 0.0f;
    CY_CHECK_FALSE(validate(sphere).has_value());

    ShapeDescription hull;
    hull.type = ShapeType::ConvexHull;
    const Vec3 points[3] = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    hull.points = points;
    hull.point_count = 3;
    // Three points are a triangle. A hull built from one has a singular inertia tensor, which the
    // solver discovers as a NaN several frames later rather than here.
    CY_CHECK_FALSE(validate(hull).has_value());

    ShapeDescription mesh;
    mesh.type = ShapeType::TriangleMesh;
    const u32 indices[4] = {0, 1, 2, 0};
    mesh.vertices = points;
    mesh.vertex_count = 3;
    mesh.indices = indices;
    mesh.index_count = 4;
    CY_CHECK_FALSE(validate(mesh).has_value());
}

CY_TEST_CASE("a capsule's height is its cylinder plus both caps") {
    ShapeDescription capsule;
    capsule.type = ShapeType::Capsule;
    capsule.radius = 0.5f;
    capsule.half_height = 1.0f;
    // The other convention — half_height as half the TOTAL height — differs by exactly one radius
    // and produces a character that stands inside the floor.
    const Aabb bounds = local_bounds(capsule);
    CY_CHECK_EQ(bounds.max.y, 1.5f);
    CY_CHECK_EQ(bounds.max.x, 0.5f);
}

CY_TEST_CASE("shapes with no finite volume report zero, which is why they are static only") {
    ShapeDescription plane;
    plane.type = ShapeType::Plane;
    CY_CHECK_EQ(volume(plane), 0.0f);
    CY_CHECK(is_static_only(ShapeType::Plane));
    CY_CHECK(is_static_only(ShapeType::TriangleMesh));
    CY_CHECK(is_static_only(ShapeType::HeightField));
    CY_CHECK_FALSE(is_static_only(ShapeType::Box));

    ShapeDescription box;
    box.type = ShapeType::Box;
    box.half_extents = Vec3{0.5f, 0.5f, 0.5f};
    CY_CHECK_EQ(volume(box), 1.0f);
}

CY_TEST_CASE("a height field's bounds skip its holes") {
    // `physics`: "Heightfield holes SHALL be representable, so a cave entrance is not blocked by an
    // invisible floor." A hole is the sentinel, and a bound that included it would be infinite.
    const f32 samples[4] = {0.0f, 1.0f, kHeightFieldHole, 2.0f};
    ShapeDescription field;
    field.type = ShapeType::HeightField;
    field.height_field.samples = samples;
    field.height_field.sample_count_x = 2;
    field.height_field.sample_count_z = 2;
    CY_CHECK(validate(field).has_value());
    const Aabb bounds = local_bounds(field);
    CY_CHECK_EQ(bounds.max.y, 2.0f);
}
