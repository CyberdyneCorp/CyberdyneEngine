// SPDX-License-Identifier: MIT
// A scene of axis-aligned proxy boxes. See proxy_scene.h.

#include <cy/rendering/gi/proxy_scene.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cy::rendering::gi {
namespace {

/// How close to a face a point must be to lie on it, in metres.
constexpr f32 kSurfaceTolerance = 2.0e-3F;

/// A ray's entry and exit parameters through a box, and the axis each happened on.
struct Slab {
    f32 near = -INFINITY;
    f32 far = INFINITY;
    u32 near_axis = 0;
    u32 far_axis = 0;
};

bool intersect(const Aabb& box, Vec3 origin, Vec3 direction, Slab& slab) noexcept {
    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 d[3] = {direction.x, direction.y, direction.z};
    const f32 low[3] = {box.min.x, box.min.y, box.min.z};
    const f32 high[3] = {box.max.x, box.max.y, box.max.z};
    for (u32 axis = 0; axis < 3U; ++axis) {
        if (std::fabs(d[axis]) < 1.0e-12F) {
            if (o[axis] < low[axis] || o[axis] > high[axis]) {
                return false;
            }
            continue;
        }
        const f32 t0 = (low[axis] - o[axis]) / d[axis];
        const f32 t1 = (high[axis] - o[axis]) / d[axis];
        const f32 entry = std::min(t0, t1);
        const f32 exit = std::max(t0, t1);
        if (entry > slab.near) {
            slab.near = entry;
            slab.near_axis = axis;
        }
        if (exit < slab.far) {
            slab.far = exit;
            slab.far_axis = axis;
        }
    }
    return slab.near <= slab.far && slab.far >= 0.0F;
}

/// The outward normal of the face a ray crosses on `axis`, entering (`sign` -1) or leaving (+1).
Vec3 face_normal(u32 axis, f32 component, f32 sign) noexcept {
    const f32 value = component > 0.0F ? sign : -sign;
    return Vec3{axis == 0U ? value : 0.0F, axis == 1U ? value : 0.0F, axis == 2U ? value : 0.0F};
}

f32 component(Vec3 v, u32 axis) noexcept {
    if (axis == 0U) {
        return v.x;
    }
    return axis == 1U ? v.y : v.z;
}

/// How far inside the box a point is, from its nearest face; negative outside.
f32 depth_inside(const Aabb& box, Vec3 p) noexcept {
    return std::min({p.x - box.min.x, box.max.x - p.x, p.y - box.min.y, box.max.y - p.y,
                     p.z - box.min.z, box.max.z - p.z});
}

}  // namespace

bool BoxProxyScene::trace(Vec3 origin, Vec3 direction, f32 max_distance,
                          SceneHit& hit) const noexcept {
    hit = SceneHit{};
    f32 best = max_distance;
    for (const ProxyBox& box : boxes_) {
        Slab slab;
        if (!intersect(box.bounds, origin, direction, slab)) {
            continue;
        }
        const bool inside = slab.near < 0.0F;
        const f32 t = inside ? slab.far : slab.near;
        if (t > best) {
            continue;
        }
        best = t;
        hit.hit = true;
        hit.t = t;
        hit.position = origin + (direction * t);
        const u32 axis = inside ? slab.far_axis : slab.near_axis;
        hit.normal = face_normal(axis, component(direction, axis), inside ? 1.0F : -1.0F);
    }
    return hit.hit;
}

bool BoxProxyScene::occluded(Vec3 from, Vec3 to) const noexcept {
    const Vec3 offset = to - from;
    const f32 span = length(offset);
    if (span <= kSurfaceTolerance) {
        return false;
    }
    SceneHit hit;
    return trace(from, offset / span, span - kSurfaceTolerance, hit);
}

const ProxyBox* BoxProxyScene::surface_at(Vec3 position) const noexcept {
    const ProxyBox* nearest = nullptr;
    f32 closest = kSurfaceTolerance;
    for (const ProxyBox& box : boxes_) {
        const f32 depth = std::fabs(depth_inside(box.bounds, position));
        if (depth <= closest) {
            closest = depth;
            nearest = &box;
        }
    }
    return nearest;
}

bool BoxProxyScene::radiance_at(Vec3 position, Vec3 normal, Vec3& radiance,
                                u32& age_frames) const noexcept {
    const ProxyBox* box = surface_at(position);
    if (box == nullptr) {
        return false;
    }
    const Vec3 irradiance = shaded_direct(lights_, position, normal, this);
    Vec3 incoming = irradiance / std::numbers::pi_v<f32>;
    if (indirect_ != nullptr) {
        incoming = incoming + indirect_->gather(position, normal);
    }
    radiance = cwise_mul(box->albedo, incoming) + box->emission;
    age_frames = 0;
    return true;
}

}  // namespace cy::rendering::gi
