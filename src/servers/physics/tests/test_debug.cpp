// SPDX-License-Identifier: MIT
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

namespace {

/// Every constraint primitive, with where it was drawn.
struct ConstraintCapture final : DebugDrawSink {
    u32 anchors = 0;
    u32 connections = 0;
    u32 limits = 0;
    Vec3 first_anchor{};
    Vec3 limit_from{};
    Vec3 limit_to{};

    void line(Vec3 from, Vec3 to, DebugColor color) noexcept override {
        if (color == DebugColor::Constraint) {
            ++connections;
        } else if (color == DebugColor::ConstraintLimit) {
            if (limits == 0) {
                limit_from = from;
                limit_to = to;
            }
            ++limits;
        }
    }
    void sphere(Vec3 center, f32, DebugColor color) noexcept override {
        if (color == DebugColor::Constraint) {
            if (anchors == 0) {
                first_anchor = center;
            }
            ++anchors;
        } else if (color == DebugColor::ConstraintLimit) {
            ++limits;
        }
    }
};

}  // namespace

CY_TEST_CASE("a constraint draws both anchors, the line between them and its limits") {
    ConstraintDescription hinge;
    hinge.type = ConstraintType::Hinge;
    hinge.limit = AxisLimit{-0.5f, 0.5f};
    const Transform a = Transform::from_translation(Vec3{1.0f, 2.0f, 3.0f});
    const Transform b = Transform::from_translation(Vec3{1.0f, 2.0f, 4.0f});
    ConstraintCapture capture;
    debug_draw_constraint(hinge, a, b, capture);
    CY_CHECK_EQ(capture.anchors, 2U);
    CY_CHECK_EQ(capture.connections, 1U);
    CY_CHECK_EQ(capture.limits, 2U);
    CY_CHECK_NEAR(capture.first_anchor.z, 3.0f, 1e-6);

    // Free is `min > max`, and a free hinge has nothing to show but its anchors.
    hinge.limit = AxisLimit{};
    ConstraintCapture free;
    debug_draw_constraint(hinge, a, b, free);
    CY_CHECK_EQ(free.limits, 0U);
}

CY_TEST_CASE("a slider's travel is drawn along its frame's X axis about anchor A") {
    ConstraintDescription slider;
    slider.type = ConstraintType::Slider;
    slider.limit = AxisLimit{-1.0f, 2.0f};
    // The frame is turned a quarter about Y, so its local X points down world -Z.
    Transform a = Transform::from_translation(Vec3{0.0f, 1.0f, 0.0f});
    a.rotation = Quat::from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 1.5707963f);
    ConstraintCapture capture;
    debug_draw_constraint(slider, a, a, capture);
    CY_REQUIRE(capture.limits == 1U);
    CY_CHECK_NEAR(capture.limit_from.z, 1.0f, 1e-5);
    CY_CHECK_NEAR(capture.limit_to.z, -2.0f, 1e-5);
    CY_CHECK_NEAR(capture.limit_from.y, 1.0f, 1e-5);
}
