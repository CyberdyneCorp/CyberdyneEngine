#include <cy/servers/physics/debug.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::physics;

namespace {

struct Capture final : DebugDrawSink {
    u32 shapes = 0;
    u32 results = 0;

    void line(Vec3, Vec3, DebugColor color) noexcept override {
        if (color == DebugColor::QueryShape) {
            ++shapes;
        } else if (color == DebugColor::QueryResult) {
            ++results;
        }
    }
    void sphere(Vec3, f32, DebugColor color) noexcept override {
        if (color == DebugColor::QueryShape) {
            ++shapes;
        } else if (color == DebugColor::QueryResult) {
            ++results;
        }
    }
};

}  // namespace

CY_TEST_CASE("query debug draws inputs and results without changing the physics server") {
    Capture capture;
    RayCastInput ray;
    RayCastHit ray_hit;
    ray_hit.position = Vec3{0.0f, -2.0f, 0.0f};
    debug_draw_query(ray, &ray_hit, capture);
    CY_CHECK_EQ(capture.shapes, 1U);
    CY_CHECK_EQ(capture.results, 2U);

    ShapeCastInput sweep;
    ShapeDescription sphere;
    sphere.type = ShapeType::Sphere;
    ShapeCastHit sweep_hit;
    debug_draw_query(sweep, sphere, &sweep_hit, capture);
    CY_CHECK_EQ(capture.shapes, 4U);
    CY_CHECK_EQ(capture.results, 4U);

    OverlapInput overlap;
    debug_draw_query(overlap, sphere, capture);
    CY_CHECK_EQ(capture.shapes, 5U);

    ClosestPointInput closest;
    ClosestPoint point;
    debug_draw_query(closest, &point, capture);
    CY_CHECK_EQ(capture.shapes, 6U);
    CY_CHECK_EQ(capture.results, 7U);
}
