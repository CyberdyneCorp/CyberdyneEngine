// SPDX-License-Identifier: MIT
// The contact query and the response on the CPU path. See collision.h for what each mode costs and
// cannot see, and `emit_slang.cpp`'s collision prelude for the GPU's transcription of every
// function below — written in the same order, because a reordering that is equal in exact
// arithmetic is a disagreement in float.

#include <cy/vfx/collision.h>

#include <algorithm>
#include <cmath>

namespace cy::vfx {
namespace {

constexpr Vec3 kUp{0.0F, 1.0F, 0.0F};

[[nodiscard]] Vec4 no_contact() noexcept {
    return Vec4{0.0F, 1.0F, 0.0F, kNoContact};
}

[[nodiscard]] Vec4 with_normal(Vec3 normal, f32 separation) noexcept {
    return Vec4{normal.x, normal.y, normal.z, separation};
}

[[nodiscard]] f32 dot3(Vec3 a, Vec3 b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

[[nodiscard]] Vec3 xyz(Vec4 v) noexcept {
    return Vec3{v.x, v.y, v.z};
}

/// `v` over its length, or `fallback` when it has none.
[[nodiscard]] Vec3 normalised_or(Vec3 v, Vec3 fallback) noexcept {
    const f32 squared = dot3(v, v);
    if (!(squared > 1.0e-12F)) {
        return fallback;
    }
    const f32 inverse = 1.0F / std::sqrt(squared);
    return Vec3{v.x * inverse, v.y * inverse, v.z * inverse};
}

/// Away from a point, which is a sphere's and a capsule's normal.
[[nodiscard]] Vec4 round_contact(Vec3 position, Vec3 centre, f32 shape_radius,
                                 f32 radius) noexcept {
    const Vec3 offset = position - centre;
    const f32 distance = std::sqrt(dot3(offset, offset));
    const Vec3 normal = normalised_or(offset, kUp);
    return with_normal(normal, distance - shape_radius - radius);
}

[[nodiscard]] Vec4 plane_contact(Vec3 position, f32 radius, Vec4 a) noexcept {
    const Vec3 normal = normalised_or(xyz(a), kUp);
    return with_normal(normal, dot3(normal, position) - a.w - radius);
}

[[nodiscard]] Vec4 capsule_contact(Vec3 position, f32 radius, Vec4 a, Vec4 b) noexcept {
    const Vec3 start = xyz(a);
    const Vec3 axis = xyz(b) - start;
    const f32 axis_squared = dot3(axis, axis);
    const f32 along = axis_squared > 1.0e-12F ? dot3(position - start, axis) / axis_squared : 0.0F;
    const f32 t = std::clamp(along, 0.0F, 1.0F);
    const Vec3 closest = start + (axis * t);
    return round_contact(position, closest, a.w, radius);
}

/// The signed distance to an axis-aligned box, and the normal of the face or edge that answers it.
[[nodiscard]] Vec4 box_contact(Vec3 position, f32 radius, Vec4 a, Vec4 b) noexcept {
    const Vec3 offset = position - xyz(a);
    const Vec3 q{std::fabs(offset.x) - b.x, std::fabs(offset.y) - b.y, std::fabs(offset.z) - b.z};
    const Vec3 outside{std::max(q.x, 0.0F), std::max(q.y, 0.0F), std::max(q.z, 0.0F)};
    const f32 outside_length = std::sqrt(dot3(outside, outside));
    const f32 deepest = std::max({q.x, q.y, q.z});
    const f32 sx = offset.x < 0.0F ? -1.0F : 1.0F;
    const f32 sy = offset.y < 0.0F ? -1.0F : 1.0F;
    const f32 sz = offset.z < 0.0F ? -1.0F : 1.0F;
    if (outside_length > 0.0F) {
        const f32 inverse = 1.0F / outside_length;
        const Vec3 normal{sx * outside.x * inverse, sy * outside.y * inverse,
                          sz * outside.z * inverse};
        return with_normal(normal, outside_length - radius);
    }
    // Inside: the nearest face, preferring x then y then z on a tie.
    Vec3 normal{0.0F, 0.0F, sz};
    if (q.x >= q.y && q.x >= q.z) {
        normal = Vec3{sx, 0.0F, 0.0F};
    } else if (q.y >= q.z) {
        normal = Vec3{0.0F, sy, 0.0F};
    }
    return with_normal(normal, deepest - radius);
}

/// The field's gradient by finite differences: six samples at quality 3, the four of a tetrahedron
/// below it.
[[nodiscard]] Vec3 field_gradient(const CollisionField& field, Vec3 p, f32 step,
                                  u8 quality) noexcept {
    if (quality >= 3) {
        const f32 gx =
            field.distance(Vec3{p.x + step, p.y, p.z}) - field.distance(Vec3{p.x - step, p.y, p.z});
        const f32 gy =
            field.distance(Vec3{p.x, p.y + step, p.z}) - field.distance(Vec3{p.x, p.y - step, p.z});
        const f32 gz =
            field.distance(Vec3{p.x, p.y, p.z + step}) - field.distance(Vec3{p.x, p.y, p.z - step});
        return Vec3{gx, gy, gz};
    }
    const f32 a = field.distance(Vec3{p.x + step, p.y - step, p.z - step});
    const f32 b = field.distance(Vec3{p.x - step, p.y - step, p.z + step});
    const f32 c = field.distance(Vec3{p.x - step, p.y + step, p.z - step});
    const f32 d = field.distance(Vec3{p.x + step, p.y + step, p.z + step});
    return Vec3{(a - b - c) + d, ((-a - b) + c) + d, ((-a + b) - c) + d};
}

[[nodiscard]] Vec4 sdf_contact(Vec3 position, f32 radius, Vec4 a,
                               const CollisionInputs& inputs) noexcept {
    if (inputs.field == nullptr || inputs.quality < 2) {
        return no_contact();
    }
    const Vec3 world = position + inputs.origin;
    const f32 separation = inputs.field->distance(world) - radius;
    if (separation >= kContactSkin) {
        // CLEAR: one sample, and the normal is never read — every response gates on touching.
        return with_normal(kUp, separation);
    }
    const f32 step = a.x > 0.0F ? a.x : kDefaultSdfStep;
    const Vec3 normal =
        normalised_or(field_gradient(*inputs.field, world, step, inputs.quality), kUp);
    return with_normal(normal, separation);
}

/// `m * (v, 1)`, column by column — the order the Slang writes it in.
[[nodiscard]] Vec4 clip_of(const Mat4& m, Vec3 v) noexcept {
    const Vec4& c0 = m.columns[0];
    const Vec4& c1 = m.columns[1];
    const Vec4& c2 = m.columns[2];
    const Vec4& c3 = m.columns[3];
    return Vec4{(((c0.x * v.x) + (c1.x * v.y)) + (c2.x * v.z)) + c3.x,
                (((c0.y * v.x) + (c1.y * v.y)) + (c2.y * v.z)) + c3.y,
                (((c0.z * v.x) + (c1.z * v.y)) + (c2.z * v.z)) + c3.z,
                (((c0.w * v.x) + (c1.w * v.y)) + (c2.w * v.z)) + c3.w};
}

[[nodiscard]] f32 depth_at(const DepthCollisionView& view, i32 x, i32 y) noexcept {
    const i32 cx = std::clamp(x, 0, static_cast<i32>(view.width) - 1);
    const i32 cy = std::clamp(y, 0, static_cast<i32>(view.height) - 1);
    return view.depths[(static_cast<usize>(cy) * view.width) + static_cast<usize>(cx)];
}

/// The world point a texel's depth is the surface at: its centre unprojected.
[[nodiscard]] Vec3 unproject(const DepthCollisionView& view, i32 x, i32 y) noexcept {
    const i32 cx = std::clamp(x, 0, static_cast<i32>(view.width) - 1);
    const i32 cy = std::clamp(y, 0, static_cast<i32>(view.height) - 1);
    const f32 u = (static_cast<f32>(cx) + 0.5F) / static_cast<f32>(view.width);
    const f32 v = (static_cast<f32>(cy) + 0.5F) / static_cast<f32>(view.height);
    const Vec3 ndc{(u * 2.0F) - 1.0F, 1.0F - (v * 2.0F), depth_at(view, cx, cy)};
    const Vec4 world = clip_of(view.inverse_view_projection, ndc);
    const f32 inverse = 1.0F / world.w;
    return Vec3{world.x * inverse, world.y * inverse, world.z * inverse};
}

/// Of the two neighbours along one axis, the one on the same surface: the smaller depth step. At
/// quality 2 only the forward one is read.
[[nodiscard]] Vec3 depth_tangent(const DepthCollisionView& view, i32 x, i32 y, i32 dx, i32 dy,
                                 Vec3 centre, f32 centre_depth, u8 quality) noexcept {
    const Vec3 forward = unproject(view, x + dx, y + dy) - centre;
    if (quality < 3) {
        return forward;
    }
    const Vec3 backward = centre - unproject(view, x - dx, y - dy);
    const f32 forward_step = std::fabs(depth_at(view, x + dx, y + dy) - centre_depth);
    const f32 backward_step = std::fabs(centre_depth - depth_at(view, x - dx, y - dy));
    return backward_step < forward_step ? backward : forward;
}

[[nodiscard]] Vec4 depth_contact(Vec3 position, f32 radius, Vec4 a,
                                 const CollisionInputs& inputs) noexcept {
    const DepthCollisionView* view = inputs.depth;
    if (view == nullptr || inputs.quality < 2 || view->width == 0 || view->height == 0 ||
        view->depths.size() < static_cast<usize>(view->width) * view->height) {
        return no_contact();
    }
    const Vec3 world = position + inputs.origin;
    const Vec4 clip = clip_of(view->view_projection, world);
    if (!(clip.w > 1.0e-6F)) {
        return no_contact();  // behind the camera: the depth buffer has never seen it
    }
    const f32 ndc_x = clip.x / clip.w;
    const f32 ndc_y = clip.y / clip.w;
    if (!(std::fabs(ndc_x) < 1.0F) || !(std::fabs(ndc_y) < 1.0F)) {
        return no_contact();  // off screen: the documented blind spot
    }
    const f32 sx = ((ndc_x * 0.5F) + 0.5F) * static_cast<f32>(view->width);
    const f32 sy = (0.5F - (ndc_y * 0.5F)) * static_cast<f32>(view->height);
    const i32 x = std::min(static_cast<i32>(sx), static_cast<i32>(view->width) - 1);
    const i32 y = std::min(static_cast<i32>(sy), static_cast<i32>(view->height) - 1);
    const f32 centre_depth = depth_at(*view, x, y);
    if (!(centre_depth > 0.0F)) {
        return no_contact();  // sky
    }
    const Vec3 surface = unproject(*view, x, y);
    const Vec3 along_x = depth_tangent(*view, x, y, 1, 0, surface, centre_depth, inputs.quality);
    const Vec3 along_y = depth_tangent(*view, x, y, 0, 1, surface, centre_depth, inputs.quality);
    Vec3 normal = normalised_or(cross(along_y, along_x), kUp);
    if (dot3(normal, view->camera - surface) < 0.0F) {
        normal = -normal;
    }
    const f32 separation = dot3(world - surface, normal) - radius;
    const f32 thickness = a.x > 0.0F ? a.x : kDefaultDepthThickness;
    if (separation < -thickness) {
        return no_contact();  // behind the surface by more than it is thick: occluded, not inside
    }
    return with_normal(normal, separation);
}

}  // namespace

CollisionField::~CollisionField() = default;

Vec4 collision_contact(ColliderShape shape, Vec3 position, f32 radius, Vec4 a, Vec4 b,
                       const CollisionInputs& inputs) noexcept {
    if (inputs.quality == 0) {
        return no_contact();
    }
    switch (shape) {
        case ColliderShape::Plane:
            return plane_contact(position, radius, a);
        case ColliderShape::Sphere:
            return round_contact(position, xyz(a), a.w, radius);
        case ColliderShape::Box:
            return box_contact(position, radius, a, b);
        case ColliderShape::Capsule:
            return capsule_contact(position, radius, a, b);
        case ColliderShape::Sdf:
            return sdf_contact(position, radius, a, inputs);
        case ColliderShape::Depth:
            return depth_contact(position, radius, a, inputs);
        case ColliderShape::Count:
            break;
    }
    return no_contact();
}

Vec3 collision_bounce(Vec3 velocity, Vec4 contact, Vec4 response) noexcept {
    if (!(contact.w < kContactSkin)) {
        return velocity;
    }
    if (response.z > 0.5F) {
        return Vec3{0.0F, 0.0F, 0.0F};  // stick
    }
    const Vec3 normal = xyz(contact);
    const f32 closing = dot3(velocity, normal);
    if (!(closing < 0.0F)) {
        return velocity;  // already leaving: nothing to push against
    }
    const f32 restitution = -closing < response.w ? 0.0F : response.x;
    // The normal impulse, per unit mass, and the most friction may take out of the tangent.
    const f32 impulse = -(1.0F + restitution) * closing;
    const Vec3 tangent = velocity - (normal * closing);
    const f32 tangent_speed = std::sqrt(dot3(tangent, tangent));
    f32 keep = 1.0F;
    if (tangent_speed > 1.0e-6F) {
        keep = std::max(1.0F - ((response.y * impulse) / tangent_speed), 0.0F);
    }
    return (tangent * keep) + (normal * (-restitution * closing));
}

Vec3 collision_resolve(Vec3 position, Vec4 contact) noexcept {
    if (!(contact.w < 0.0F)) {
        return position;
    }
    return position + (xyz(contact) * -contact.w);
}

f32 collision_impact(Vec3 velocity, Vec4 contact) noexcept {
    if (!(contact.w < kContactSkin)) {
        return 0.0F;
    }
    return std::max(-dot3(velocity, xyz(contact)), 0.0F);
}

}  // namespace cy::vfx
