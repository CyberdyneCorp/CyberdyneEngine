// The debug sink's defaults: every primitive reduced to lines, so a sink that implements only
// `line()` draws everything. Task 4.2.4.
//
// The decompositions are deliberately coarse — eight segments per circle, twelve edges per box.
// This is a debug overlay whose job is "the collider is not where the mesh is", and a smoother
// circle costs vertices in the frame where somebody is already looking at thousands of them.

#include <cy/servers/physics/debug.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
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

void draw_query_shape(const ShapeDescription& shape, const Transform& transform,
                      DebugDrawSink& sink) noexcept {
    if (shape.type == ShapeType::Sphere) {
        sink.sphere(transform.translation, shape.radius, DebugColor::QueryShape);
    } else if (shape.type == ShapeType::Capsule) {
        sink.capsule(transform, shape.radius, shape.half_height, DebugColor::QueryShape);
    } else {
        const Aabb bounds = local_bounds(shape);
        if (!bounds.is_empty()) {
            sink.box(bounds, transform, DebugColor::QueryShape);
        }
    }
}

void draw_query_hit(Vec3 position, Vec3 normal, DebugDrawSink& sink) noexcept {
    sink.sphere(position, 0.035f, DebugColor::QueryResult);
    sink.line(position, position + normal * 0.2f, DebugColor::QueryResult);
}

void draw_angular_range(Vec3 origin, Vec3 axis, Vec3 reference, f32 min_angle, f32 max_angle,
                        DebugDrawSink& sink) noexcept {
    const Vec3 radius = reference * 0.25f;
    sink.line(origin, origin + Quat::from_axis_angle(axis, min_angle) * radius,
              DebugColor::ConstraintLimit);
    sink.line(origin, origin + Quat::from_axis_angle(axis, max_angle) * radius,
              DebugColor::ConstraintLimit);
}

void draw_constraint_limits(const ConstraintDescription& description, const Transform& anchor,
                            DebugDrawSink& sink) noexcept {
    const Vec3 origin = anchor.translation;
    const Vec3 axis = anchor.right();
    if (description.type == ConstraintType::Slider && description.limit.limited()) {
        sink.line(origin + axis * description.limit.min, origin + axis * description.limit.max,
                  DebugColor::ConstraintLimit);
    } else if (description.type == ConstraintType::Hinge && description.limit.limited()) {
        draw_angular_range(origin, axis, anchor.up(), description.limit.min, description.limit.max,
                           sink);
    } else if (description.type == ConstraintType::Distance) {
        sink.sphere(origin, description.min_distance, DebugColor::ConstraintLimit);
        sink.sphere(origin, description.max_distance, DebugColor::ConstraintLimit);
    } else if (description.type == ConstraintType::Cone ||
               description.type == ConstraintType::SwingTwist) {
        const f32 normal = description.type == ConstraintType::Cone
                               ? std::max(description.swing_limit_y, description.swing_limit_z)
                               : description.swing_limit_y;
        const f32 plane =
            description.type == ConstraintType::Cone ? normal : description.swing_limit_z;
        draw_angular_range(origin, anchor.up(), axis, -normal, normal, sink);
        draw_angular_range(origin, -anchor.forward(), axis, -plane, plane, sink);
        if (description.type == ConstraintType::SwingTwist && description.twist_limit.limited()) {
            draw_angular_range(origin, axis, anchor.up(), description.twist_limit.min,
                               description.twist_limit.max, sink);
        }
    } else if (description.type == ConstraintType::SixDof) {
        const Vec3 axes[] = {anchor.right(), anchor.up(), -anchor.forward()};
        for (u32 index = 0; index < 3; ++index) {
            const AxisLimit& limit = description.dof_limits[index];
            if (limit.limited()) {
                sink.line(origin + axes[index] * limit.min, origin + axes[index] * limit.max,
                          DebugColor::ConstraintLimit);
            }
        }
        const Vec3 references[] = {axes[1], axes[2], axes[0]};
        for (u32 index = 0; index < 3; ++index) {
            const AxisLimit& limit = description.dof_limits[index + 3];
            if (limit.limited()) {
                draw_angular_range(origin, axes[index], references[index], limit.min, limit.max,
                                   sink);
            }
        }
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

void debug_draw_constraint(const ConstraintDescription& description, const Transform& frame_a,
                           const Transform& frame_b, DebugDrawSink& sink) noexcept {
    sink.sphere(frame_a.translation, 0.04f, DebugColor::Constraint);
    sink.sphere(frame_b.translation, 0.04f, DebugColor::Constraint);
    sink.line(frame_a.translation, frame_b.translation, DebugColor::Constraint);
    draw_constraint_limits(description, frame_a, sink);
}

void debug_draw_query(const RayCastInput& input, const RayCastHit* hit,
                      DebugDrawSink& sink) noexcept {
    sink.line(input.origin, input.origin + input.direction * input.max_distance,
              DebugColor::QueryShape);
    if (hit != nullptr) {
        draw_query_hit(hit->position, hit->normal, sink);
    }
}

void debug_draw_query(const ShapeCastInput& input, const ShapeDescription& shape,
                      const ShapeCastHit* hit, DebugDrawSink& sink) noexcept {
    draw_query_shape(shape, input.start, sink);
    Transform end = input.start;
    end.translation += input.direction * input.max_distance;
    draw_query_shape(shape, end, sink);
    sink.line(input.start.translation, end.translation, DebugColor::QueryShape);
    if (hit != nullptr) {
        draw_query_hit(hit->position, hit->normal, sink);
    }
}

void debug_draw_query(const OverlapInput& input, const ShapeDescription& shape,
                      DebugDrawSink& sink) noexcept {
    draw_query_shape(shape, input.transform, sink);
}

void debug_draw_query(const ClosestPointInput& input, const ClosestPoint* hit,
                      DebugDrawSink& sink) noexcept {
    sink.sphere(input.point, 0.035f, DebugColor::QueryShape);
    if (hit != nullptr) {
        sink.line(input.point, hit->position, DebugColor::QueryResult);
        draw_query_hit(hit->position, hit->normal, sink);
    }
}

}  // namespace cy::physics
