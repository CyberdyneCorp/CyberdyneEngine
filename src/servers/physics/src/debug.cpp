// The debug sink's defaults: every primitive reduced to lines, so a sink that implements only
// `line()` draws everything. Task 4.2.4.
//
// The decompositions are deliberately coarse — eight segments per circle, twelve edges per box.
// This is a debug overlay whose job is "the collider is not where the mesh is", and a smoother
// circle costs vertices in the frame where somebody is already looking at thousands of them.

#include <cy/servers/physics/debug.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::physics {
namespace {

constexpr u32 kCircleSegments = 16;

void circle(DebugDrawSink& sink, Vec3 center, Vec3 axis_u, Vec3 axis_v, f32 radius,
            DebugColor color) noexcept {
    Vec3 previous = center + axis_u * radius;
    for (u32 step = 1; step <= kCircleSegments; ++step) {
        const f32 angle =
            (2.0f * math::kPi * static_cast<f32>(step)) / static_cast<f32>(kCircleSegments);
        const Vec3 point =
            center + axis_u * (radius * std::cos(angle)) + axis_v * (radius * std::sin(angle));
        sink.line(previous, point, color);
        previous = point;
    }
}

}  // namespace

void DebugDrawSink::box(const Aabb& box, const Transform& transform, DebugColor color) noexcept {
    // The twelve edges of the box, as pairs of corner indices in shapes.h's bit order: bit 0 is X,
    // bit 1 is Y, bit 2 is Z.
    static constexpr u32 kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                          {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : kEdges) {
        line(transform.transform_point(box.corner(edge[0])),
             transform.transform_point(box.corner(edge[1])), color);
    }
}

void DebugDrawSink::sphere(Vec3 center, f32 radius, DebugColor color) noexcept {
    circle(*this, center, kAxisX, kAxisY, radius, color);
    circle(*this, center, kAxisY, kAxisZ, radius, color);
    circle(*this, center, kAxisZ, kAxisX, radius, color);
}

void DebugDrawSink::capsule(const Transform& transform, f32 radius, f32 half_height,
                            DebugColor color) noexcept {
    const Vec3 up = transform.rotate_vector(kAxisY);
    const Vec3 top = transform.translation + up * half_height;
    const Vec3 bottom = transform.translation - up * half_height;
    sphere(top, radius, color);
    sphere(bottom, radius, color);
    const Vec3 right = transform.rotate_vector(kAxisX) * radius;
    const Vec3 forward = transform.rotate_vector(kAxisZ) * radius;
    line(top + right, bottom + right, color);
    line(top - right, bottom - right, color);
    line(top + forward, bottom + forward, color);
    line(top - forward, bottom - forward, color);
}

void DebugDrawSink::contact(Vec3 position, Vec3 normal, f32 penetration) noexcept {
    // The normal is drawn one decimetre long so contacts on a large body are still visible, and the
    // penetration is drawn back along it so a deep overlap reads as a longer stub than a resting
    // touch — which is the difference somebody looking at this is trying to see.
    line(position, position + normal * 0.1f, DebugColor::Normal);
    if (penetration > 0.0f) {
        line(position, position - normal * penetration, DebugColor::Contact);
    }
}

}  // namespace cy::physics
