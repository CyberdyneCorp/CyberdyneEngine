// The 2D API over the constrained 3D solver. Task 4.2.1.
//
// Every conversion in the engine between the plane and the world goes through this file. See
// physics2d.h for the convention and for why it is stated exactly once.

#include <cy/servers/physics/physics2d.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::physics::two_d {

Quat angle_to_quat(f32 radians) noexcept {
    return Quat::from_axis_angle(kAxisZ, radians);
}

f32 quat_to_angle(const Quat& rotation) noexcept {
    // atan2 of the +Z rotation's sine and cosine halves. Valid because a 2D body's X and Y
    // rotations are locked, so the quaternion is always of the form (0, 0, sin(t/2), cos(t/2)).
    return 2.0f * std::atan2(rotation.z, rotation.w);
}

Transform to_3d(const Transform2D& t) noexcept {
    return Transform{angle_to_quat(t.rotation), to_3d(t.position),
                     Vec3{t.scale.x, t.scale.y, 1.0f}};
}

Transform2D to_2d(const Transform& t) noexcept {
    return Transform2D{to_2d(t.translation), quat_to_angle(t.rotation), to_2d(t.scale)};
}

Expected<ShapeDescription, Error> extrude(const ShapeDescription2D& shape,
                                          Span<Vec3> scratch) noexcept {
    ShapeDescription out;
    out.density = shape.density;
    switch (shape.type) {
        case ShapeType2D::Circle:
            // A circle extrudes to a CYLINDER along Z, not to a sphere: a sphere's silhouette in
            // the plane is a circle only at z = 0, so a body resting on one would touch at a point
            // that moves with any residual Z drift.
            out.type = ShapeType::Cylinder;
            out.radius = shape.radius;
            out.half_height = kExtrusionHalfDepth;
            return out;
        case ShapeType2D::Box:
            out.type = ShapeType::Box;
            out.half_extents =
                Vec3{shape.half_extents.x, shape.half_extents.y, kExtrusionHalfDepth};
            return out;
        case ShapeType2D::Capsule:
            out.type = ShapeType::Capsule;
            out.radius = shape.radius;
            out.half_height = shape.half_length;
            return out;
        case ShapeType2D::Polygon: {
            if (shape.points == nullptr || shape.point_count < 3) {
                return fail(ErrorCode::InvalidArgument,
                            "2D polygon: at least three points are required");
            }
            const usize needed = static_cast<usize>(shape.point_count) * 2U;
            if (scratch.size() < needed) {
                return fail(ErrorCode::BufferTooSmall,
                            "2D polygon: the scratch span must hold two 3D points per 2D point");
            }
            for (u32 index = 0; index < shape.point_count; ++index) {
                scratch[index] =
                    Vec3{shape.points[index].x, shape.points[index].y, -kExtrusionHalfDepth};
                scratch[shape.point_count + index] =
                    Vec3{shape.points[index].x, shape.points[index].y, kExtrusionHalfDepth};
            }
            out.type = ShapeType::ConvexHull;
            out.points = scratch.data();
            out.point_count = static_cast<u32>(needed);
            return out;
        }
    }
    return fail(ErrorCode::InvalidArgument, "2D shape: unknown type");
}

Expected<BodyHandle, Error> create_body(PhysicsServer& server, WorldHandle world,
                                        const BodyDescription2D& description) noexcept {
    BodyDescription body;
    body.name = description.name;
    body.motion = description.motion;
    body.transform = to_3d(description.transform);
    body.linear_velocity = to_3d(description.linear_velocity);
    body.angular_velocity = Vec3{0.0f, 0.0f, description.angular_velocity};
    body.mass = description.mass;
    body.linear_damping = description.linear_damping;
    body.angular_damping = description.angular_damping;
    body.gravity_scale = description.gravity_scale;
    body.allow_sleeping = description.allow_sleeping;
    // THE WHOLE STRATEGY, IN ONE ASSIGNMENT. Applied here rather than left to the caller, so that
    // `physics`' "Constraint is enforced" scenario cannot be defeated by forgetting it.
    body.locked_axes = kLockPlaneXY;
    body.colliders = description.colliders;
    body.collider_count = description.collider_count;
    body.user_data = description.user_data;
    return server.create_body(world, body);
}

Expected<BodyState2D, Error> body_state(const PhysicsServer& server, BodyHandle body) noexcept {
    const Expected<BodyState, Error> state = server.body_state(body);
    if (!state) {
        return make_unexpected(state.error());
    }
    BodyState2D out;
    out.transform = to_2d(state->transform);
    out.linear_velocity = to_2d(state->linear_velocity);
    out.angular_velocity = state->angular_velocity.z;
    out.asleep = state->asleep;
    return out;
}

Expected<RayCastHit2D, Error> raycast(const PhysicsServer& server, WorldHandle world,
                                      const RayCastInput2D& input,
                                      const QueryFilter& filter) noexcept {
    RayCastInput ray;
    ray.origin = to_3d(input.origin);
    ray.direction = to_3d(input.direction);
    ray.max_distance = input.max_distance;
    const Expected<RayCastHit, Error> hit = server.raycast(world, ray, filter);
    if (!hit) {
        return make_unexpected(hit.error());
    }
    RayCastHit2D out;
    out.body = hit->body;
    out.user_data = hit->user_data;
    out.position = to_2d(hit->position);
    out.normal = to_2d(hit->normal);
    out.distance = hit->distance;
    out.material = hit->material;
    out.trigger = hit->trigger;
    return out;
}

}  // namespace cy::physics::two_d
