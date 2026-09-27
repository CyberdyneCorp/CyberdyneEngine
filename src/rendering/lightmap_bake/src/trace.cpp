// SPDX-License-Identifier: MIT
// Path tracing one texel, and the surface cards the path tracer resolves materials through.

#include "internal.h"

#include <algorithm>
#include <cmath>

namespace cy::rendering::lightmap_bake::detail {
namespace {

/// The first rays of a texel that decide whether it is buried. Few, because a buried texel sees
/// back faces in almost every direction and an exposed one in almost none.
constexpr u32 kBuriedRays = 12;
/// Ray origins leave the surface by this much along the normal, in metres: the path tracer's own
/// bounce offset.
constexpr f32 kSurfaceOffset = 1.0e-3F;

/// The counter hash `gi::PathTracer` draws with, so a texel's sequence advances the same way
/// whichever of the two calls consumes it.
[[nodiscard]] f32 hashed_unit(u32& sequence) noexcept {
    sequence = (sequence * 747796405U) + 2891336453U;
    u32 value = ((sequence >> ((sequence >> 28U) + 4U)) ^ sequence) * 277803737U;
    value = (value >> 22U) ^ value;
    return static_cast<f32>(value & 0xFFFFFFU) / static_cast<f32>(0x1000000U);
}

[[nodiscard]] Vec3 cosine_direction(Vec3 normal, u32& sequence) noexcept {
    const f32 u1 = hashed_unit(sequence);
    const f32 u2 = hashed_unit(sequence);
    const f32 radius = std::sqrt(u1);
    const f32 angle = 6.2831853F * u2;
    const f32 x = radius * std::cos(angle);
    const f32 y = radius * std::sin(angle);
    const f32 z = std::sqrt(std::max(0.0F, 1.0F - u1));
    Vec3 tangent = std::abs(normal.y) < 0.99F ? cross(Vec3{0.0F, 1.0F, 0.0F}, normal)
                                              : cross(Vec3{1.0F, 0.0F, 0.0F}, normal);
    tangent = normalized_or(tangent, Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 bitangent = cross(normal, tangent);
    return normalized_or((tangent * x) + (bitangent * y) + (normal * z), normal);
}

[[nodiscard]] Vec3 direction_to(const gi::GiLight& light, Vec3 position) noexcept {
    if (light.directional) {
        return -normalized_or(light.direction, Vec3{0.0F, 1.0F, 0.0F});
    }
    return normalized_or(light.position - position, Vec3{0.0F, 1.0F, 0.0F});
}

void accumulate(TexelMoments& moments, Vec3 radiance, Vec3 direction, f32 weight) noexcept {
    const f32 lum = luminance(radiance);
    moments.mean = moments.mean + (radiance * weight);
    moments.luminance = moments.luminance + (lum * weight);
    moments.luminance_direction = moments.luminance_direction + (direction * (lum * weight));
    moments.channel_direction[0] = moments.channel_direction[0] + (direction * (radiance.x * weight));
    moments.channel_direction[1] = moments.channel_direction[1] + (direction * (radiance.y * weight));
    moments.channel_direction[2] = moments.channel_direction[2] + (direction * (radiance.z * weight));
}

/// Cards over one triangle: the centroids of an n-by-n subdivision, n chosen from the spacing.
[[nodiscard]] Status add_triangle_cards(const WorldTriangle& triangle, const BakeMaterial& material,
                                        f32 spacing, Array<gi::Surfel>& out) noexcept {
    const f32 longest = std::max({length(triangle.v1 - triangle.v0),
                                  length(triangle.v2 - triangle.v1),
                                  length(triangle.v0 - triangle.v2)});
    const u32 steps = std::max(1U, static_cast<u32>(std::ceil(longest / spacing)));
    const f32 area =
        0.5F * length(cross(triangle.v1 - triangle.v0, triangle.v2 - triangle.v0)) /
        static_cast<f32>(steps * steps);
    const auto card = [&](f32 a, f32 b, Vec3 normal, Vec3 emission) {
        gi::Surfel surfel;
        const f32 fa = a / static_cast<f32>(steps);
        const f32 fb = b / static_cast<f32>(steps);
        surfel.position =
            triangle.v0 + ((triangle.v1 - triangle.v0) * fa) + ((triangle.v2 - triangle.v0) * fb);
        surfel.normal = normal;
        surfel.albedo = material.albedo;
        surfel.emission = emission;
        surfel.roughness = 1.0F;
        surfel.area = area;
        surfel.instance_id = triangle.instance;
        surfel.material_id = triangle.material;
        return out.push_back(surfel);
    };
    for (u32 a = 0; a < steps; ++a) {
        for (u32 b = 0; a + b < steps; ++b) {
            // Both faces: a lightmap scene's surfaces reflect light arriving at either, and only
            // the front one emits.
            const f32 centres[2][2] = {{static_cast<f32>(a) + (1.0F / 3.0F),
                                        static_cast<f32>(b) + (1.0F / 3.0F)},
                                       {static_cast<f32>(a) + (2.0F / 3.0F),
                                        static_cast<f32>(b) + (2.0F / 3.0F)}};
            const u32 count = a + b + 1U < steps ? 2U : 1U;
            for (u32 which = 0; which < count; ++which) {
                if (Status added =
                        card(centres[which][0], centres[which][1], triangle.normal,
                             material.emission);
                    !added) {
                    return added;
                }
                if (Status added = card(centres[which][0], centres[which][1], -triangle.normal,
                                        Vec3{0.0F, 0.0F, 0.0F});
                    !added) {
                    return added;
                }
            }
        }
    }
    return ok();
}

}  // namespace

TexelMoments trace_moments(const TraceContext& context, Vec3 position, Vec3 normal, u32 sequence,
                           u32 samples) noexcept {
    const LightmapBakeSettings& settings = *context.settings;
    TexelMoments moments;
    const u32 count = std::max(1U, samples);
    const f32 weight = 1.0F / static_cast<f32>(count);
    const Vec3 origin = position + (normal * kSurfaceOffset);
    for (u32 index = 0; index < count; ++index) {
        const Vec3 direction = cosine_direction(normal, sequence);
        const Vec3 radiance =
            context.path->radiance(origin, direction, settings.trace.bounces,
                                   settings.trace.max_distance_metres, sequence);
        accumulate(moments, radiance, direction, weight);
    }
    if (settings.content == LightmapContent::DirectAndIndirect) {
        // The placed lights at the receiver, one at a time so each carries its own direction. The
        // lights are already divided by pi, so this is E / pi like the rest of the texel.
        for (const gi::GiLight& light : context.lights) {
            const Vec3 direct =
                gi::shaded_direct(Span<const gi::GiLight>(&light, 1), position, normal,
                                  context.tracer);
            accumulate(moments, direct, direction_to(light, position), 1.0F);
        }
    }
    return moments;
}

bool buried(const TraceContext& context, Vec3 position, Vec3 normal, u32 sequence) noexcept {
    const Vec3 origin = position + (normal * kSurfaceOffset);
    u32 back_faces = 0;
    for (u32 index = 0; index < kBuriedRays; ++index) {
        const Vec3 direction = cosine_direction(normal, sequence);
        f32 t = 0.0F;
        u32 triangle = 0;
        bool back_face = false;
        if (context.tracer->trace_triangle(origin, direction,
                                           context.settings->trace.max_distance_metres, t,
                                           triangle, back_face) &&
            back_face) {
            back_faces += 1U;
        }
    }
    return static_cast<f32>(back_faces) >
           context.settings->buried_threshold * static_cast<f32>(kBuriedRays);
}

Status build_surfels(const LightmapScene& scene, const MeshSceneTracer& tracer, f32 spacing,
                     gi::GiScene& out) noexcept {
    Array<gi::Surfel> surfels;
    Aabb bounds = Aabb::empty();
    bool first = true;
    for (const WorldTriangle& triangle : tracer.triangles()) {
        if (Status added = add_triangle_cards(triangle, scene.materials[triangle.material],
                                              std::max(spacing, 0.01F), surfels);
            !added) {
            return added;
        }
        const Aabb box = Aabb::from_min_max(
            cwise_min(triangle.v0, cwise_min(triangle.v1, triangle.v2)),
            cwise_max(triangle.v0, cwise_max(triangle.v1, triangle.v2)));
        bounds = first ? box : Aabb::from_min_max(cwise_min(bounds.min, box.min),
                                                  cwise_max(bounds.max, box.max));
        first = false;
    }
    return out.ingest_cell(0, bounds, surfels.span(), 0);
}

}  // namespace cy::rendering::lightmap_bake::detail
