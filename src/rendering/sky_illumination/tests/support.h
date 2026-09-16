#pragma once
// An OUTDOOR fixture, because a sky term read in a closed room is a sky term nothing can see.
//
// The GI suites' own fixture is a closed box with a divider, chosen so that a probe can leak
// through it — the right shape for the questions those suites ask and the wrong one here. What this
// module's cases need is a surface the sky reaches: a ground slab under an open hemisphere, so that
// `DistanceField::sky_visibility` is non-zero and the sky's contribution to `indirect_diffuse` is
// something a change in the term can move.

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/system.h>

#include <cmath>
#include <vector>

namespace sky_illumination_support {

using cy::f32;
using cy::u32;
using cy::Vec3;

/// Half extents of the ground slab, in metres. Wide enough that a probe over its middle sees sky
/// above and ground below rather than the edge of the world.
inline constexpr f32 kGroundHalfX = 6.0F;
inline constexpr f32 kGroundHalfY = 0.5F;
inline constexpr f32 kGroundHalfZ = 6.0F;
/// The slab's top surface, which is where the surfels and the query points are.
inline constexpr f32 kGroundTop = 0.0F;

/// A solid box sampled onto a dense grid, which is what a cook produces per asset.
struct SlabField {
    u32 dimension = 17;
    cy::Aabb bounds{};
    std::vector<f32> distances;

    explicit SlabField(Vec3 half_extents, u32 grid = 17) : dimension(grid) {
        const Vec3 extent = half_extents + Vec3{2.0F, 2.0F, 2.0F};
        bounds = cy::Aabb::from_min_max(-extent, extent);
        distances.resize(static_cast<size_t>(grid) * grid * grid);
        const Vec3 size = bounds.size();
        const auto axis = [grid](u32 index, f32 span, f32 origin) {
            return origin + (span * static_cast<f32>(index) / static_cast<f32>(grid - 1));
        };
        for (u32 z = 0; z < grid; ++z) {
            for (u32 y = 0; y < grid; ++y) {
                for (u32 x = 0; x < grid; ++x) {
                    const Vec3 point{axis(x, size.x, bounds.min.x), axis(y, size.y, bounds.min.y),
                                     axis(z, size.z, bounds.min.z)};
                    distances[(((static_cast<size_t>(z) * grid) + y) * grid) + x] =
                        cy::rendering::gi::box_distance(point, half_extents);
                }
            }
        }
    }

    [[nodiscard]] cy::rendering::gi::AssetDistanceField asset() const noexcept {
        cy::rendering::gi::AssetDistanceField field;
        field.dimension = dimension;
        field.bounds = bounds;
        field.distances = {distances.data(), distances.size()};
        return field;
    }
};

/// Upward-facing cards over the slab's top.
[[nodiscard]] inline std::vector<cy::rendering::gi::Surfel> ground_surfels(f32 spacing = 1.5F) {
    std::vector<cy::rendering::gi::Surfel> surfels;
    const auto steps = [spacing](f32 half) {
        return static_cast<u32>(std::lround(2.0F * half / spacing)) + 1U;
    };
    for (u32 ix = 0; ix < steps(kGroundHalfX); ++ix) {
        for (u32 iz = 0; iz < steps(kGroundHalfZ); ++iz) {
            cy::rendering::gi::Surfel surfel;
            surfel.position = Vec3{-kGroundHalfX + (static_cast<f32>(ix) * spacing), kGroundTop,
                                   -kGroundHalfZ + (static_cast<f32>(iz) * spacing)};
            surfel.normal = Vec3{0.0F, 1.0F, 0.0F};
            surfel.albedo = Vec3{0.32F, 0.30F, 0.27F};
            surfel.roughness = 0.9F;
            surfel.area = spacing * spacing;
            surfel.instance_id = 1;
            surfel.material_id = 1;
            surfels.push_back(surfel);
        }
    }
    return surfels;
}

/// SMALL ON PURPOSE. Every case here runs hundreds of frames of a day cycle inside a one-second
/// integration budget, so the clipmap and the probe window are sized for the question — does the
/// sky term move illumination and what did moving it cost — rather than for image quality.
[[nodiscard]] inline cy::rendering::gi::IlluminationSettings ground_settings() {
    cy::rendering::gi::IlluminationSettings settings;
    settings.field.levels = 1;
    settings.field.resolution = 16;
    settings.field.base_extent_metres = 16.0F;
    settings.probes.levels = 1;
    settings.probes.base_spacing_metres = 4.0F;
    settings.probes.half_extent_probes = 1;
    settings.max_ray_distance_metres = 20.0F;
    settings.rays_per_probe = 8;
    settings.convergence_region_metres = 8.0F;
    return settings;
}

/// The slab, its cards and an illumination system wired to them.
struct Ground {
    SlabField field{Vec3{kGroundHalfX, kGroundHalfY, kGroundHalfZ}};
    std::vector<cy::rendering::gi::Surfel> surfels = ground_surfels();
    std::vector<cy::rendering::gi::GiLight> lights;
    cy::rendering::gi::IlluminationSystem system;

    Ground() {
        CY_REQUIRE(system.configure(ground_settings()).has_value());
        // The slab sits with its top at y = 0, so the field is placed half a slab below.
        CY_REQUIRE(system.field()
                       .place(1, field.asset(),
                              cy::Mat4::from_translation(Vec3{0.0F, -kGroundHalfY, 0.0F}))
                       .has_value());
        CY_REQUIRE(system.scene()
                       .ingest_cell(1, bounds(), {surfels.data(), surfels.size()}, 0)
                       .has_value());
        CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds()).has_value());
        system.surfaces().set_lookup_radius(1.8F);
    }

    [[nodiscard]] static cy::Aabb bounds() noexcept {
        return cy::Aabb::from_center_extents(Vec3{0.0F, 1.0F, 0.0F},
                                             Vec3{kGroundHalfX + 1.0F, 3.0F, kGroundHalfZ + 1.0F});
    }

    [[nodiscard]] cy::rendering::gi::FrameContext context(cy::u64 frame) const noexcept {
        cy::rendering::gi::FrameContext ctx;
        ctx.camera = Vec3{0.0F, 1.5F, 0.0F};
        ctx.lights = {lights.data(), lights.size()};
        ctx.frame = frame;
        ctx.measured_gi_ms = 1.0F;
        return ctx;
    }

    void run(u32 frames) {
        for (u32 index = 0; index < frames; ++index) {
            (void)system.update(context(index));
        }
    }
};

}  // namespace sky_illumination_support
