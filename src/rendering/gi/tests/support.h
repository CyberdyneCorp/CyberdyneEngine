#pragma once
// A room, its distance field and its illumination, shared by the GI suites. Tasks 9.1, 9.2 and 9.5.
//
// The fixture is a closed box with a divider down the middle and a doorway through it. That shape
// is chosen rather than a Cornell box because three of the properties under test are only visible
// in it: the divider is what a probe can leak through, the doorway is the corner the adaptive
// placement is supposed to find, and the two halves let a light in one of them be measured in the
// other.

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/raytracing/acceleration.h>

#include <cmath>
#include <vector>

namespace gi_support {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;

/// Half extents of the room's interior, in metres.
inline constexpr f32 kRoomX = 4.0F;
inline constexpr f32 kRoomY = 2.0F;
inline constexpr f32 kRoomZ = 4.0F;

/// A box distance field sampled onto a dense grid, which is what a cook produces per asset.
struct BoxField {
    u32 dimension = 9;
    cy::Aabb bounds{};
    std::vector<f32> distances;

    explicit BoxField(Vec3 half_extents, f32 margin = 1.0F, u32 grid = 9) : dimension(grid) {
        const Vec3 extent = half_extents + Vec3{margin, margin, margin};
        bounds = cy::Aabb::from_min_max(-extent, extent);
        distances.resize(static_cast<size_t>(grid) * grid * grid);
        const Vec3 size = bounds.size();
        for (u32 z = 0; z < grid; ++z) {
            for (u32 y = 0; y < grid; ++y) {
                for (u32 x = 0; x < grid; ++x) {
                    const auto axis = [&](u32 index, f32 span, f32 origin) {
                        return origin +
                               (span * static_cast<f32>(index) / static_cast<f32>(grid - 1));
                    };
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

/// The room itself, as a field: positive INSIDE the room and negative in the walls, which is the
/// inverse of a solid box. A hollow room is the complement of a solid, and the complement is what a
/// cook produces for an interior — the distance a ray inside it has left to travel.
struct RoomField {
    u32 dimension = 25;
    cy::Aabb bounds{};
    std::vector<f32> distances;

    explicit RoomField(Vec3 half_extents, u32 grid = 25) : dimension(grid) {
        const Vec3 extent = half_extents + Vec3{1.0F, 1.0F, 1.0F};
        bounds = cy::Aabb::from_min_max(-extent, extent);
        distances.resize(static_cast<size_t>(grid) * grid * grid);
        const Vec3 size = bounds.size();
        for (u32 z = 0; z < grid; ++z) {
            for (u32 y = 0; y < grid; ++y) {
                for (u32 x = 0; x < grid; ++x) {
                    const auto axis = [&](u32 index, f32 span, f32 origin) {
                        return origin +
                               (span * static_cast<f32>(index) / static_cast<f32>(grid - 1));
                    };
                    const Vec3 point{axis(x, size.x, bounds.min.x), axis(y, size.y, bounds.min.y),
                                     axis(z, size.z, bounds.min.z)};
                    distances[(((static_cast<size_t>(z) * grid) + y) * grid) + x] =
                        -cy::rendering::gi::box_distance(point, half_extents);
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

/// The room's surface cards: floor, ceiling, four walls and a divider with a doorway.
[[nodiscard]] inline std::vector<cy::rendering::gi::Surfel> room_surfels(f32 spacing = 1.0F,
                                                                         bool with_divider = true) {
    std::vector<cy::rendering::gi::Surfel> surfels;
    const auto add = [&](Vec3 position, Vec3 normal, Vec3 albedo, Vec3 emission, u32 instance) {
        cy::rendering::gi::Surfel surfel;
        surfel.position = position;
        surfel.normal = normal;
        surfel.albedo = albedo;
        surfel.emission = emission;
        surfel.roughness = 0.8F;
        surfel.area = spacing * spacing;
        surfel.instance_id = instance;
        surfel.material_id = instance;
        surfels.push_back(surfel);
    };

    // Integer loops with the coordinate derived from the step, rather than a float accumulated in
    // the induction variable. The card at +4 exists or it does not; adding a float eight times and
    // hoping the comparison still holds is how a fixture silently loses a wall.
    const auto steps = [spacing](f32 half) {
        return static_cast<u32>(std::lround(2.0F * half / spacing)) + 1U;
    };
    const auto coordinate = [spacing](u32 index, f32 half) {
        return -half + (static_cast<f32>(index) * spacing);
    };

    for (u32 ix = 0; ix < steps(kRoomX); ++ix) {
        const f32 x = coordinate(ix, kRoomX);
        for (u32 iz = 0; iz < steps(kRoomZ); ++iz) {
            const f32 z = coordinate(iz, kRoomZ);
            add(Vec3{x, -kRoomY, z}, Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.6F, 0.6F, 0.6F}, Vec3{}, 1);
            add(Vec3{x, kRoomY, z}, Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.7F, 0.7F, 0.7F}, Vec3{}, 2);
        }
    }
    for (u32 iy = 0; iy < steps(kRoomY); ++iy) {
        const f32 y = coordinate(iy, kRoomY);
        for (u32 iz = 0; iz < steps(kRoomZ); ++iz) {
            const f32 z = coordinate(iz, kRoomZ);
            // The red wall: the one whose colour bleeds.
            add(Vec3{-kRoomX, y, z}, Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.85F, 0.08F, 0.06F}, Vec3{}, 3);
            add(Vec3{kRoomX, y, z}, Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.08F, 0.12F, 0.85F}, Vec3{}, 4);
        }
        for (u32 ix = 0; ix < steps(kRoomX); ++ix) {
            const f32 x = coordinate(ix, kRoomX);
            add(Vec3{x, y, -kRoomZ}, Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.6F, 0.6F, 0.6F}, Vec3{}, 5);
            add(Vec3{x, y, kRoomZ}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.6F, 0.6F, 0.6F}, Vec3{}, 6);
            // The divider at z = 0, with a doorway between x = -1 and x = 1.
            if (with_divider && (x < -1.0F || x > 1.0F)) {
                add(Vec3{x, y, 0.0F}, Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.55F, 0.55F, 0.5F}, Vec3{}, 7);
                add(Vec3{x, y, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.55F, 0.55F, 0.5F}, Vec3{}, 7);
            }
        }
    }
    return surfels;
}

/// Triangles for the same room, for the hardware tier's acceleration structures.
struct RoomTriangles {
    std::vector<Vec3> positions;
    std::vector<u32> indices;

    explicit RoomTriangles(bool with_divider = true) {
        const auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
            const auto base = static_cast<u32>(positions.size());
            positions.push_back(a);
            positions.push_back(b);
            positions.push_back(c);
            positions.push_back(d);
            indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        };
        // Floor, ceiling and the four walls, inward facing. Winding is not culled by the GI
        // queries, so the order only decides which way the reported normal points before it is
        // flipped toward the ray.
        quad({-kRoomX, -kRoomY, -kRoomZ}, {kRoomX, -kRoomY, -kRoomZ}, {kRoomX, -kRoomY, kRoomZ},
             {-kRoomX, -kRoomY, kRoomZ});
        quad({-kRoomX, kRoomY, -kRoomZ}, {-kRoomX, kRoomY, kRoomZ}, {kRoomX, kRoomY, kRoomZ},
             {kRoomX, kRoomY, -kRoomZ});
        quad({-kRoomX, -kRoomY, -kRoomZ}, {-kRoomX, -kRoomY, kRoomZ}, {-kRoomX, kRoomY, kRoomZ},
             {-kRoomX, kRoomY, -kRoomZ});
        quad({kRoomX, -kRoomY, -kRoomZ}, {kRoomX, kRoomY, -kRoomZ}, {kRoomX, kRoomY, kRoomZ},
             {kRoomX, -kRoomY, kRoomZ});
        quad({-kRoomX, -kRoomY, -kRoomZ}, {-kRoomX, kRoomY, -kRoomZ}, {kRoomX, kRoomY, -kRoomZ},
             {kRoomX, -kRoomY, -kRoomZ});
        quad({-kRoomX, -kRoomY, kRoomZ}, {kRoomX, -kRoomY, kRoomZ}, {kRoomX, kRoomY, kRoomZ},
             {-kRoomX, kRoomY, kRoomZ});
        if (with_divider) {
            // The divider, in two pieces either side of the doorway.
            quad({-kRoomX, -kRoomY, 0.0F}, {-1.0F, -kRoomY, 0.0F}, {-1.0F, kRoomY, 0.0F},
                 {-kRoomX, kRoomY, 0.0F});
            quad({1.0F, -kRoomY, 0.0F}, {kRoomX, -kRoomY, 0.0F}, {kRoomX, kRoomY, 0.0F},
                 {1.0F, kRoomY, 0.0F});
        }
    }

    [[nodiscard]] cy::rendering::rt::TriangleGeometry geometry() const noexcept {
        cy::rendering::rt::TriangleGeometry triangles;
        triangles.positions = {positions.data(), positions.size()};
        triangles.indices = {indices.data(), indices.size()};
        return triangles;
    }
};

/// One bright light in the negative-z half of the room.
[[nodiscard]] inline std::vector<cy::rendering::gi::GiLight> room_lights() {
    cy::rendering::gi::GiLight light;
    light.position = Vec3{-2.0F, 1.0F, -2.0F};
    light.colour = Vec3{1.0F, 0.96F, 0.9F};
    light.intensity = 30.0F;
    light.range = 30.0F;
    light.id = 1;
    return {light};
}

/// Settings small enough for an integration budget and large enough to be the real thing: three
/// clipmap levels, a real probe window, and a field whose finest voxel is 25 cm.
[[nodiscard]] inline cy::rendering::gi::IlluminationSettings room_settings() {
    cy::rendering::gi::IlluminationSettings settings;
    // One clipmap level at a quarter-metre voxel, covering the room and its walls. One level rather
    // than two because the room fits in it: a second level would only re-solve the same emptiness
    // more coarsely, and the finest voxel is what the software tier's agreement with the hardware
    // tier is limited by.
    settings.field.levels = 1;
    settings.field.resolution = 64;
    settings.field.base_extent_metres = 16.0F;
    settings.probes.levels = 1;
    settings.probes.base_spacing_metres = 2.0F;
    settings.probes.half_extent_probes = 2;
    settings.max_ray_distance_metres = 24.0F;
    settings.rays_per_probe = 16;
    settings.convergence_region_metres = 4.0F;
    return settings;
}

}  // namespace gi_support
