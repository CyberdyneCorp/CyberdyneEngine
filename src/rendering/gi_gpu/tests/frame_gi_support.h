// SPDX-License-Identifier: MIT
#pragma once
// The pipeline suites' scene as a GI scene: what `render.gi_gpu_frame` and `render.gi_gpu_live`
// light the one frame scene with, rather than each building a second.

#include <cy/core/math/shapes.h>
#include <cy/rendering/gi/system.h>

#include "screen_support.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace gi_support {

/// The pipeline scene's boxes as the GI scene: a field of boxes, a card every half metre over every
/// face, and the scene's own sun and two point lights. With `emissive_face` below the quad count,
/// that face's cards glow — a lamp an emitter rule can put out by its material, the face's
/// `Surfel::material_id`.
struct GiOfScene {
    std::vector<std::unique_ptr<BoxField>> fields;
    std::vector<cy::rendering::gi::Surfel> surfels;
    std::vector<cy::rendering::gi::GiLight> lights;
    cy::rendering::gi::IlluminationSystem system;
    /// The emissive face's material, or zero.
    cy::u32 emissive_material = 0;

    [[nodiscard]] bool build(cy::Span<const cy::Aabb> boxes, cy::u32 emissive_face = ~0U,
                             cy::Vec3 emission = cy::Vec3{0.0F, 0.0F, 0.0F}) {
        namespace gi = cy::rendering::gi;
        using cy::f32;
        using cy::u32;
        using cy::Vec3;
        gi::IlluminationSettings settings;
        settings.field.levels = 2;
        settings.field.resolution = 64;
        settings.field.base_extent_metres = 16.0F;
        settings.probes.levels = 2;
        settings.probes.base_spacing_metres = 1.0F;
        settings.probes.half_extent_probes = 5;
        settings.rays_per_probe = 16;
        settings.max_ray_distance_metres = 24.0F;
        settings.surface_bounce = gi::SurfaceBounce::Cards;
        settings.card_gather.max_distance_metres = 24.0F;
        // The sky the frame's ambient term comes from, in the same physical units: an overcast
        // dome of a few thousand nits under a 22 000 lux sun.
        settings.sky.intensity = 2500.0F;
        if (!system.configure(settings).has_value()) {
            return false;
        }
        std::vector<AxisQuad> quads;
        cy::u64 id = 1;
        for (const cy::Aabb& box : boxes) {
            const Vec3 centre = box.center();
            const Vec3 half = box.half_extents();
            const u32 grid = std::max(
                9U,
                static_cast<u32>(std::ceil((std::max({half.x, half.y, half.z}) + 0.5F) * 6.0F)));
            fields.push_back(std::make_unique<BoxField>(half, 0.5F, grid | 1U));
            if (!system.field()
                     .place(id++, fields.back()->asset(), cy::Mat4::from_translation(centre))
                     .has_value()) {
                return false;
            }
            const Vec3 lo = box.min;
            const Vec3 hi = box.max;
            const Vec3 grey{0.62F, 0.6F, 0.58F};
            quads.push_back({0, lo.x, {lo.y, lo.z}, {hi.y, hi.z}, grey, -1.0F});
            quads.push_back({0, hi.x, {lo.y, lo.z}, {hi.y, hi.z}, grey, 1.0F});
            quads.push_back({1, lo.y, {lo.x, lo.z}, {hi.x, hi.z}, grey, -1.0F});
            quads.push_back({1, hi.y, {lo.x, lo.z}, {hi.x, hi.z}, grey, 1.0F});
            quads.push_back({2, lo.z, {lo.x, lo.y}, {hi.x, hi.y}, grey, -1.0F});
            quads.push_back({2, hi.z, {lo.x, lo.y}, {hi.x, hi.y}, grey, 1.0F});
        }
        surfels = surfels_from_quads(quads, 0.5F);
        if (emissive_face < quads.size()) {
            // `surfels_from_quads` numbers a quad's material from one, in quad order.
            emissive_material = emissive_face + 1U;
            for (gi::Surfel& surfel : surfels) {
                if (surfel.material_id == emissive_material) {
                    surfel.emission = emission;
                }
            }
        }
        const cy::Aabb bounds =
            cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, -8.0F}, Vec3{12.0F, 6.0F, 12.0F});
        if (!system.ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value()) {
            return false;
        }
        system.surfaces().set_lookup_radius(0.4F);

        gi::GiLight sun;
        sun.directional = true;
        sun.direction = cy::normalize(Vec3{-0.28F, -0.82F, -0.50F});
        sun.colour = Vec3{1.0F, 0.96F, 0.88F};
        sun.intensity = 22000.0F;
        sun.id = 1;
        lights.push_back(sun);
        for (u32 which = 1; which < 3U; ++which) {
            gi::GiLight point;
            point.position = Vec3{which == 1U ? -3.2F : 3.2F, 1.4F, -5.4F};
            point.colour = which == 1U ? Vec3{0.4F, 0.7F, 1.0F} : Vec3{1.0F, 0.5F, 0.3F};
            point.intensity = 9000.0F;
            point.range = 16.0F;
            point.id = 1 + which;
            lights.push_back(point);
        }
        return true;
    }
};

}  // namespace gi_support
