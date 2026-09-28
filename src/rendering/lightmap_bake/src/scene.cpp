// SPDX-License-Identifier: MIT
// The bake's triangle tracer. See scene.h.

#include <cy/rendering/lightmap_bake/scene.h>

#include <cy/core/math/geometry.h>
#include <cy/core/math/shapes.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::rendering::lightmap_bake {
namespace {

/// Offsets a ray's start past the surface it leaves, in metres. The path tracer offsets its own
/// bounces by a millimetre; this is the floor under a ray a caller started exactly on a triangle.
constexpr f32 kSelfHitEpsilon = 1.0e-4F;

[[nodiscard]] u32 float_bits(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// A deterministic draw in [0, 1) from the triangle and the ray, for transparency.
[[nodiscard]] f32 transmission_draw(u32 triangle, Vec3 origin, Vec3 direction) noexcept {
    u32 hash = triangle * 0x9E3779B1U;
    const f32 words[6] = {origin.x, origin.y, origin.z, direction.x, direction.y, direction.z};
    for (const f32 word : words) {
        hash ^= float_bits(word) + 0x7F4A7C15U + (hash << 6U) + (hash >> 2U);
        hash *= 0x85EBCA6BU;
        hash ^= hash >> 13U;
    }
    return static_cast<f32>(hash & 0xFFFFFFU) / static_cast<f32>(0x1000000U);
}

[[nodiscard]] Aabb triangle_bounds(const WorldTriangle& triangle) noexcept {
    return Aabb::from_min_max(cwise_min(triangle.v0, cwise_min(triangle.v1, triangle.v2)),
                              cwise_max(triangle.v0, cwise_max(triangle.v1, triangle.v2)));
}

}  // namespace

void measure_uv2(Span<const Vec2> uv2, Span<const u32> indices, f32& coverage,
                 f32& aspect) noexcept {
    f32 area = 0.0F;
    f32 du = 0.0F;
    f32 dv = 0.0F;
    for (usize at = 0; at + 2U < indices.size(); at += 3U) {
        if (indices[at] >= uv2.size() || indices[at + 1U] >= uv2.size() ||
            indices[at + 2U] >= uv2.size()) {
            continue;
        }
        const Vec2 corners[3] = {uv2[indices[at]], uv2[indices[at + 1U]], uv2[indices[at + 2U]]};
        area += 0.5F * std::fabs(cross(corners[1] - corners[0], corners[2] - corners[0]));
        for (u32 edge = 0; edge < 3U; ++edge) {
            const Vec2 step = corners[(edge + 1U) % 3U] - corners[edge];
            du += step.x * step.x;
            dv += step.y * step.y;
        }
    }
    coverage = std::clamp(area, 0.05F, 1.0F);
    aspect = du > 0.0F && dv > 0.0F ? std::clamp(std::sqrt(dv / du), 1.0F / 16.0F, 16.0F) : 1.0F;
}

f32 AlphaMask::sample(Vec2 uv) const noexcept {
    if (width == 0 || height == 0 || alpha.size() < usize{width} * height) {
        return 1.0F;
    }
    const f32 u = uv.x - std::floor(uv.x);
    const f32 v = uv.y - std::floor(uv.y);
    const u32 x = std::min(width - 1U, static_cast<u32>(u * static_cast<f32>(width)));
    const u32 y = std::min(height - 1U, static_cast<u32>(v * static_cast<f32>(height)));
    return alpha[(usize{y} * width) + x];
}

Status MeshSceneTracer::build(const LightmapScene& scene) noexcept {
    triangles_.clear();
    tree_.clear();
    materials_ = scene.materials;
    for (u32 index = 0; index < scene.instances.size(); ++index) {
        const BakeInstance& instance = scene.instances[index];
        if (instance.mesh >= scene.meshes.size() || instance.material >= scene.materials.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a lightmap instance names a mesh or a material the scene does not have");
        }
        const BakeMesh& mesh = scene.meshes[instance.mesh];
        for (usize at = 0; at + 2U < mesh.indices.size(); at += 3U) {
            const u32 corners[3] = {mesh.indices[at], mesh.indices[at + 1U], mesh.indices[at + 2U]};
            if (corners[0] >= mesh.positions.size() || corners[1] >= mesh.positions.size() ||
                corners[2] >= mesh.positions.size()) {
                return fail(ErrorCode::InvalidArgument,
                            "a lightmap mesh index is outside its vertex array");
            }
            WorldTriangle triangle;
            triangle.v0 = transform_point(instance.transform, mesh.positions[corners[0]]);
            triangle.v1 = transform_point(instance.transform, mesh.positions[corners[1]]);
            triangle.v2 = transform_point(instance.transform, mesh.positions[corners[2]]);
            const Vec3 face = cross(triangle.v1 - triangle.v0, triangle.v2 - triangle.v0);
            if (length_squared(face) <= 1.0e-16F) {
                continue;  // degenerate: nothing a ray can stop at
            }
            triangle.normal = normalize(face);
            for (u32 corner = 0; corner < 3U; ++corner) {
                triangle.uv0[corner] =
                    corners[corner] < mesh.uv0.size() ? mesh.uv0[corners[corner]] : Vec2{};
            }
            triangle.instance = index;
            triangle.material = instance.material;
            if (Status pushed = triangles_.push_back(triangle); !pushed) {
                return pushed;
            }
        }
    }
    if (triangles_.empty()) {
        return fail(ErrorCode::InvalidArgument, "a lightmap scene with no triangles");
    }

    Array<Aabb> bounds;
    Array<u32> payloads;
    if (Status sized = bounds.resize(triangles_.size()); !sized) {
        return sized;
    }
    if (Status sized = payloads.resize(triangles_.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < triangles_.size(); ++index) {
        bounds[index] = triangle_bounds(triangles_[index]);
        payloads[index] = static_cast<u32>(index);
    }
    return tree_.build(bounds.data(), payloads.data(), triangles_.size());
}

bool MeshSceneTracer::stops(u32 triangle, f32 u, f32 v, Vec3 origin,
                            Vec3 direction) const noexcept {
    const WorldTriangle& hit = triangles_[triangle];
    const BakeMaterial& material = materials_[hit.material];
    if (material.mask != nullptr) {
        const f32 w = 1.0F - u - v;
        const Vec2 uv = (hit.uv0[0] * w) + (hit.uv0[1] * u) + (hit.uv0[2] * v);
        if (material.mask->sample(uv) < material.alpha_cutoff) {
            return false;
        }
    }
    if (material.opacity < 1.0F) {
        return transmission_draw(triangle, origin, direction) < material.opacity;
    }
    return true;
}

bool MeshSceneTracer::trace_triangle(Vec3 origin, Vec3 direction, f32 max_distance, f32& t,
                                     u32& triangle, bool& back_face) const noexcept {
    rays_ += 1;
    const Ray ray{origin, normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F})};
    f32 nearest = max_distance;
    bool found = false;
    tree_.query_ray(ray, max_distance, [&](const u32& candidate, const Aabb& /*bounds*/) {
        const WorldTriangle& shape = triangles_[candidate];
        geom::TriangleHit hit;
        if (!geom::ray_triangle(ray, shape.v0, shape.v1, shape.v2, nearest, false, hit)) {
            return;
        }
        if (hit.t <= kSelfHitEpsilon || hit.t >= nearest) {
            return;
        }
        if (!stops(candidate, hit.u, hit.v, origin, ray.direction)) {
            return;
        }
        nearest = hit.t;
        triangle = candidate;
        back_face = dot(shape.normal, ray.direction) > 0.0F;
        found = true;
    });
    t = nearest;
    return found;
}

bool MeshSceneTracer::trace(Vec3 origin, Vec3 direction, f32 max_distance,
                            gi::SceneHit& hit) const noexcept {
    f32 t = 0.0F;
    u32 triangle = 0;
    bool back_face = false;
    if (!trace_triangle(origin, direction, max_distance, t, triangle, back_face)) {
        hit.hit = false;
        return false;
    }
    const Vec3 heading = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});
    hit.hit = true;
    hit.t = t;
    hit.position = origin + (heading * t);
    const Vec3 normal = triangles_[triangle].normal;
    hit.normal = back_face ? -normal : normal;
    hit.confidence = 1.0F;
    hit.declared_error_metres = 0.0F;
    return true;
}

bool MeshSceneTracer::occluded(Vec3 from, Vec3 to) const noexcept {
    const Vec3 offset = to - from;
    const f32 distance = length(offset);
    if (distance <= kSelfHitEpsilon) {
        return false;
    }
    f32 t = 0.0F;
    u32 triangle = 0;
    bool back_face = false;
    return trace_triangle(from, offset / distance, distance - kSelfHitEpsilon, t, triangle,
                          back_face);
}

}  // namespace cy::rendering::lightmap_bake
