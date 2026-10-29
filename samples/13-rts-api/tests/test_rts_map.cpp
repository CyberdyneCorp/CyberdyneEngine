// SPDX-License-Identifier: MIT
// integration.rts_map — the RTS test map of samples/13-rts-api: its source cooks, its package loads
// through the asset system, and what loads is the map the source describes.
//
//   the cook is a pure function   two cooks of content/rts/frontier.cymap write byte-identical
//                                 packages, and the package loads back to the content it was
//                                 cooked from, digest for digest
//   the build's package           the package the build cooked (`cy_rts_map_cook`) mounts, every
//                                 asset is served by id by `AssetSystem`, and it holds a map: the
//                                 level square flat at zero, hills and a lake round it, painted
//                                 layers, roads, buildings standing on the ground, resources,
//                                 a forest kept off the level and the roads, the skinned worker
//   the fog covers the map        the fog of war's grid a run on the map builds covers the cooked
//                                 terrain and the level, and a unit of another side far from
//                                 blue's sight, or off the grid, is not drawn
//   the battle's ground           ABI 1.16's battle stands on the company's field where the map
//                                 places it, and its minimap covers the map's whole terrain
//   the topographic minimap       baked from the cooked heights, every contour line on a texel the
//                                 interval's level crosses and none elsewhere, the lake drawn as
//                                 water and its shore lined, roads laid; and through blue's fog of
//                                 war, the ground a worker sees drawn as baked, what it saw grey,
//                                 and the rest black
//                                 (docs/design/images/rts-topographic-minimap.png is the bake)
//   refusals                      a damaged payload, an asset loaded as the wrong kind, a newer
//                                 format, a truncated payload and a malformed source each fail and
//                                 say why
//
// The albedo of the cooks this test runs itself is 256 texels a side: the claims are about the
// cook's determinism and the package's round trip, which the size of one texture does not change.

#include <cy/core/assets/cooked.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <cy/rendering/fog_of_war/visibility_field.h>

#include "battle.h"
#include "fog_grid.h"
#include "golden.h"
#include "map_cook.h"
#include "map_load.h"
#include "map_minimap.h"
#include "map_source.h"

namespace {

using namespace sample::rts::map;

[[nodiscard]] std::vector<u8> file_bytes(const std::string& path) {
    std::vector<u8> out;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return out;
    }
    bool read = std::fseek(file, 0, SEEK_END) == 0;
    const long size = read ? std::ftell(file) : -1L;
    read = read && size >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    if (read) {
        out.resize(static_cast<std::size_t>(size));
        if (!out.empty() && std::fread(out.data(), 1, out.size(), file) != out.size()) {
            out.clear();
        }
    }
    (void)std::fclose(file);
    return out;
}

[[nodiscard]] std::string binary(const char* name) {
    return std::string(CY_TEST_BINARY_DIR) + "/" + name;
}

[[nodiscard]] CookRequest small_cook(const char* package) {
    CookRequest request;
    request.source = CY_RTS_MAP_SOURCE;
    request.content_root = CY_RTS_MAP_CONTENT_ROOT;
    request.package = package == nullptr ? std::string{} : binary(package);
    request.albedo_edge = 256;
    return request;
}

/// The cooked bytes of one asset, wrapped as the cook wraps them, from a payload.
[[nodiscard]] std::vector<u8> wrap(cy::assets::AssetKind kind, const std::vector<u8>& payload) {
    cy::Array<u8> cooked(cy::system_allocator(cy::MemoryDomain::Assets));
    const cy::Status wrapped = cy::assets::write_cooked_asset(
        kind, cy::assets::VariantKey{}, cy::Span<const u8>(payload.data(), payload.size()), cooked);
    CY_REQUIRE(wrapped.has_value());
    return {cooked.begin(), cooked.end()};
}

[[nodiscard]] std::string refusal(const cy::Status& status) {
    return status.has_value() ? std::string{} : std::string(status.error().message);
}

}  // namespace

CY_TEST_CASE("samples/13-rts-api map: two cooks of the source are byte-identical and load back") {
    MapContent first;
    MapContent second;
    CookReport report;
    std::string detail;
    const cy::Status cooked = cook_map(small_cook("rts-map-a.cypak"), first, report, detail);
    if (!cooked.has_value()) {
        std::fprintf(stderr, "%s (%s)\n", refusal(cooked).c_str(), detail.c_str());
    }
    CY_REQUIRE(cooked.has_value());
    CY_REQUIRE(cook_map(small_cook("rts-map-b.cypak"), second, report, detail).has_value());
    const std::vector<u8> a = file_bytes(binary("rts-map-a.cypak"));
    const std::vector<u8> b = file_bytes(binary("rts-map-b.cypak"));
    CY_REQUIRE(a.size() > static_cast<std::size_t>(100000U));
    CY_CHECK(a == b);
    CY_CHECK_EQ(content_digest(first), content_digest(second));

    MapContent loaded;
    LoadReport load;
    const cy::Status read = load_map(cy::system_allocator(cy::MemoryDomain::Assets),
                                     binary("rts-map-a.cypak"), loaded, load, detail);
    if (!read.has_value()) {
        std::fprintf(stderr, "%s (%s)\n", refusal(read).c_str(), detail.c_str());
    }
    CY_REQUIRE(read.has_value());
    CY_CHECK_EQ(load.assets, 7U + static_cast<u32>(first.layout.textures.size()));
    CY_CHECK_EQ(load.digest, content_digest(first));
    std::printf("rts map cook digest=%016llx bytes=%zu foliage=%u\n",
                static_cast<unsigned long long>(load.digest), a.size(), report.foliage);
}

CY_TEST_CASE("samples/13-rts-api map: the build's package is a map, read by id") {
    MapContent map;
    LoadReport load;
    std::string detail;
    const cy::Status read = load_map(cy::system_allocator(cy::MemoryDomain::Assets),
                                     CY_RTS_MAP_PACKAGE, map, load, detail);
    if (!read.has_value()) {
        std::fprintf(stderr, "%s (%s)\n", refusal(read).c_str(), detail.c_str());
    }
    CY_REQUIRE(read.has_value());
    const MapLayout& layout = map.layout;
    CY_CHECK_EQ(layout.name, std::string("The Frontier"));
    CY_CHECK_EQ(map.albedo.width, 2048U);
    CY_CHECK_EQ(map.albedo.mips, 12U);
    CY_CHECK_EQ(map.textures.size(), static_cast<std::size_t>(3U));

    // THE LEVEL IS FLAT AT ZERO: the gameplay of the sample runs on its square unchanged.
    const TerrainData& terrain = map.terrain;
    f32 level_low = 1e9F;
    f32 level_high = -1e9F;
    const auto steps = static_cast<u32>((layout.base_max_x - layout.base_min_x) * 2.0F);
    for (u32 j = 0; j <= steps; ++j) {
        for (u32 i = 0; i <= steps; ++i) {
            const f32 x = layout.base_min_x + (0.5F * static_cast<f32>(i));
            const f32 z = layout.base_min_z + (0.5F * static_cast<f32>(j));
            level_low = std::min(level_low, terrain.height_at(x, z));
            level_high = std::max(level_high, terrain.height_at(x, z));
        }
    }
    CY_CHECK_LT(std::fabs(level_low), 0.05F);
    CY_CHECK_LT(std::fabs(level_high), 0.05F);
    // Hills round it and a lake beside it.
    f32 highest = -1e9F;
    f32 lowest = 1e9F;
    for (u32 j = 0; j < terrain.edge; ++j) {
        for (u32 i = 0; i < terrain.edge; ++i) {
            highest = std::max(highest, terrain.sample(i, j));
            lowest = std::min(lowest, terrain.sample(i, j));
        }
    }
    CY_CHECK_GT(highest, 6.0F);
    CY_CHECK_LT(lowest, layout.water_level - 1.5F);
    CY_CHECK_LT(terrain.height_at(-32.0F, 14.0F), layout.water_level);

    // PAINTED: every declared layer the stack writes is somewhere in the texels, and the roads'.
    std::vector<u32> dominant(16, 0);
    for (std::size_t texel = 0; texel < terrain.texel_layers.size(); texel += 4) {
        dominant[std::min<u32>(terrain.texel_layers[texel], 15U)] += 1;
    }
    for (const u32 painted : {1U, 4U, 5U, 6U}) {
        CY_CHECK_GT(dominant[painted], 10U);
    }
    CY_CHECK_EQ(terrain.layer_at(44.0F, 12.0F), 1U);

    // BUILDINGS STAND ON THE GROUND, and the level's barracks is where the GI's box is.
    u32 buildings = 0;
    u32 resources = 0;
    bool barracks = false;
    for (const Placement& placement : layout.placements) {
        buildings += placement.kind == PlacementKind::Building ? 1U : 0U;
        resources += placement.kind == PlacementKind::Resource ? 1U : 0U;
        if (placement.kind != PlacementKind::Water) {
            CY_CHECK_LT(std::fabs(placement.position.y -
                                  terrain.height_at(placement.position.x, placement.position.z)),
                        0.01F);
        }
        if (placement.tag == "Barracks") {
            barracks = std::fabs(placement.position.x - 8.0F) < 1e-4F &&
                       std::fabs(placement.position.z - 27.0F) < 1e-4F;
        }
    }
    CY_CHECK_GE(buildings, 6U);
    CY_CHECK_GE(resources, 6U);
    CY_CHECK(barracks);

    // THE FOREST: thousands of plants, none on the level, none on a road, every species placed.
    const FoliageData& foliage = map.foliage;
    CY_CHECK_GT(foliage.plants.size(), static_cast<std::size_t>(500U));
    std::vector<u32> per_species(foliage.species.size(), 0);
    u32 on_level = 0;
    u32 on_road = 0;
    for (const FoliagePlant& plant : foliage.plants) {
        per_species[plant.species] += 1;
        on_level += plant.position.x > 0.0F && plant.position.x < 32.0F &&
                            plant.position.z > 0.0F && plant.position.z < 32.0F
                        ? 1U
                        : 0U;
        on_road += terrain.layer_at(plant.position.x, plant.position.z) == 1U ? 1U : 0U;
        CY_CHECK_LT(
            std::fabs(plant.position.y - terrain.height_at(plant.position.x, plant.position.z)),
            0.25F);
    }
    for (const u32 count : per_species) {
        CY_CHECK_GT(count, 10U);
    }
    CY_CHECK_EQ(on_level, 0U);
    CY_CHECK_EQ(on_road, 0U);

    // THE SKINNED WORKER: eight joints, every one of them carrying vertices.
    CY_CHECK_EQ(map.worker.joint_count, 8U);
    std::vector<u32> per_joint(8, 0);
    for (const u8 joint : map.worker.joints) {
        per_joint[joint] += 1;
    }
    for (u32 joint = 1; joint < 8U; ++joint) {
        CY_CHECK_GT(per_joint[joint], 0U);
    }
    std::printf(
        "rts map package assets=%u bytes=%llu load_ms=%.1f plants=%zu placements=%zu "
        "triangles=%zu digest=%016llx\n",
        load.assets, static_cast<unsigned long long>(load.bytes), load.load_ms,
        foliage.plants.size(), layout.placements.size(), map.ground.indices.size() / 3U,
        static_cast<unsigned long long>(load.digest));
}

CY_TEST_CASE("samples/13-rts-api map: the fog of war's grid covers the whole map") {
    // The regression: the grid was the level's 32 m square while the frame drew 128 m of terrain,
    // and everything off the square was drawn as seen.
    MapContent map;
    LoadReport load;
    std::string detail;
    CY_REQUIRE(load_map(cy::system_allocator(cy::MemoryDomain::Assets), CY_RTS_MAP_PACKAGE, map,
                        load, detail)
                   .has_value());
    const TerrainData& terrain = map.terrain;
    CY_REQUIRE(terrain.edge >= 2U);
    const f32 terrain_max_x =
        terrain.origin_x + (terrain.spacing * static_cast<f32>(terrain.edge - 1U));
    const f32 terrain_max_z =
        terrain.origin_z + (terrain.spacing * static_cast<f32>(terrain.edge - 1U));

    // The grid the host builds for a run on this map (main.cpp: HostOptions::fog_bounds).
    const cy::gameplay::fog::FogGridDescription grid = sample::rts::fog_grid_over(map_bounds(map));
    const cy::Aabb covered = sample::rts::fog_grid_bounds(grid);
    std::fprintf(stderr,
                 "fog grid %u x %u from (%.1f, %.1f); terrain (%.1f, %.1f) to (%.1f, %.1f)\n",
                 grid.width, grid.height, static_cast<double>(covered.min.x),
                 static_cast<double>(covered.min.z), static_cast<double>(terrain.origin_x),
                 static_cast<double>(terrain.origin_z), static_cast<double>(terrain_max_x),
                 static_cast<double>(terrain_max_z));
    CY_CHECK_LE(covered.min.x, terrain.origin_x);
    CY_CHECK_LE(covered.min.z, terrain.origin_z);
    CY_CHECK_GE(covered.max.x, terrain_max_x);
    CY_CHECK_GE(covered.max.z, terrain_max_z);
    // And the level's square, whose cells stay the metre cells they were.
    CY_CHECK_LE(covered.min.x, map.layout.base_min_x);
    CY_CHECK_LE(covered.min.z, map.layout.base_min_z);
    CY_CHECK_GE(covered.max.x, map.layout.base_max_x);
    CY_CHECK_GE(covered.max.z, map.layout.base_max_z);
    CY_CHECK_EQ(grid.cell_size_raw, cy::gameplay::fog::kOneMetreRaw);
    CY_CHECK_EQ(grid.origin_x_raw % cy::gameplay::fog::kOneMetreRaw, 0);
    CY_CHECK_EQ(grid.origin_z_raw % cy::gameplay::fog::kOneMetreRaw, 0);
    cy::gameplay::fog::FogOfWar fog(cy::system_allocator(cy::MemoryDomain::World));
    CY_CHECK(fog.create(grid).has_value());
}

CY_TEST_CASE("samples/13-rts-api map: a unit of another side far from blue's sight is not drawn") {
    MapContent map;
    LoadReport load;
    std::string detail;
    CY_REQUIRE(load_map(cy::system_allocator(cy::MemoryDomain::Assets), CY_RTS_MAP_PACKAGE, map,
                        load, detail)
                   .has_value());
    namespace gfog = cy::gameplay::fog;
    constexpr cy::gameplay::TeamId kBlue = 1;
    constexpr cy::gameplay::TeamId kRival = 3;
    gfog::FogOfWar fog(cy::system_allocator(cy::MemoryDomain::World));
    CY_REQUIRE(fog.create(sample::rts::fog_grid_over(map_bounds(map))).has_value());
    // Blue's worker in the level, seeing 7 m, as game/Fog.swift gives it.
    gfog::FogUnit worker;
    worker.id = 1;
    worker.team = kBlue;
    worker.x_raw = sample::rts::fog_raw(16.0F);
    worker.z_raw = sample::rts::fog_raw(16.0F);
    worker.reveal_radius_raw = sample::rts::fog_raw(7.0F);
    CY_REQUIRE(fog.update({&worker, 1}, 1).has_value());

    // The company's field, north of the level: far from any blue sight, and never drawn.
    const Vec3 company{map.layout.company_x + 24.0F, 0.0F, map.layout.company_z + 24.0F};
    CY_CHECK_FALSE(sample::rts::shown_by_fog(fog, kBlue, kRival, company));
    // The map's far corners, on the grid but never seen, and a point off the grid altogether.
    CY_CHECK_FALSE(sample::rts::shown_by_fog(fog, kBlue, kRival, Vec3{-40.0F, 0.0F, 60.0F}));
    CY_CHECK_FALSE(sample::rts::shown_by_fog(fog, kBlue, kRival, Vec3{70.0F, 0.0F, -60.0F}));
    CY_CHECK_FALSE(sample::rts::shown_by_fog(fog, kBlue, kRival, Vec3{-500.0F, 0.0F, 0.0F}));
    // Beside the worker, it is; and blue's own units always are.
    CY_CHECK(sample::rts::shown_by_fog(fog, kBlue, kRival, Vec3{18.0F, 0.0F, 17.0F}));
    CY_CHECK(sample::rts::shown_by_fog(fog, kBlue, kBlue, company));
}

CY_TEST_CASE("samples/13-rts-api map: the battle stands on the company's field, its map whole") {
    MapContent map;
    LoadReport load;
    std::string detail;
    CY_REQUIRE(load_map(cy::system_allocator(cy::MemoryDomain::Assets), CY_RTS_MAP_PACKAGE, map,
                        load, detail)
                   .has_value());
    // host/battle.h places the field and sizes a headless battle's fog by these numbers, and
    // game/Battle.swift and game/Contract.swift state them again: they are the map's.
    CY_CHECK_EQ(map.layout.company_x, sample::rts::battle::kFieldX);
    CY_CHECK_EQ(map.layout.company_z, sample::rts::battle::kFieldZ);
    const cy::Aabb whole = map_bounds(map);
    CY_CHECK_EQ(whole.min.x, sample::rts::battle::kMapMinX);
    CY_CHECK_EQ(whole.min.z, sample::rts::battle::kMapMinZ);
    CY_CHECK_EQ(whole.max.x - whole.min.x, sample::rts::battle::kMapSize);
    CY_CHECK_EQ(whole.max.z - whole.min.z, sample::rts::battle::kMapSize);
    // The battle is fought on the field's flattened ground.
    for (const sample::rts::battle::Squad& squad : sample::rts::battle::kSquads) {
        const Vec3 at{sample::rts::battle::kFieldX + (static_cast<f32>(squad.x_quarters) * 0.25F),
                      0.0F,
                      sample::rts::battle::kFieldZ + (static_cast<f32>(squad.z_quarters) * 0.25F)};
        CY_CHECK_LT(std::fabs(map.terrain.height_at(at.x, at.z)), 0.05F);
    }
}

namespace {

namespace mm = cy::rendering::minimap;
namespace fow = cy::rendering::fog_of_war;

constexpr cy::u32 kMinimapTexels = 256;

[[nodiscard]] mm::MinimapRegion whole_map(const MapContent& map) noexcept {
    const cy::Aabb whole = map_bounds(map);
    return mm::MinimapRegion{whole.min.x, whole.max.x, whole.min.z, whole.max.z};
}

[[nodiscard]] cy::Status save_rgba(const std::string& path, const std::vector<cy::u32>& texels,
                                   cy::u32 extent) {
    cy::render_test::Image image(cy::system_allocator(cy::MemoryDomain::Renderer));
    if (cy::Status adopted = cy::render_test::adopt(
            image, cy::Span<const cy::u32>(texels.data(), texels.size()), extent, extent);
        !adopted) {
        return adopted;
    }
    return cy::render_test::write_png(path.c_str(), image);
}

}  // namespace

CY_TEST_CASE("samples/13-rts-api map: the minimap's contours lie where the map's heights cross") {
    // THE CONTOUR PLACEMENT, AGAINST THE MAP. The bake's heights under each texel are the cooked
    // terrain's, read here through the map's own `height_at`; for every pair of neighbouring
    // texels a level of the interval lies between, the higher one carries a line (an index line
    // when the level is a fifth), and no texel carries one that no level crosses.
    MapContent map;
    LoadReport load;
    std::string detail;
    CY_REQUIRE(load_map(cy::system_allocator(cy::MemoryDomain::Assets), CY_RTS_MAP_PACKAGE, map,
                        load, detail)
                   .has_value());
    const mm::MinimapRegion region = whole_map(map);
    const MinimapBake bake = minimap_bake(map, region, kMinimapTexels);
    const std::size_t count = static_cast<std::size_t>(kMinimapTexels) * kMinimapTexels;
    std::vector<f32> heights(count);
    for (cy::u32 row = 0; row < kMinimapTexels; ++row) {
        for (cy::u32 column = 0; column < kMinimapTexels; ++column) {
            const cy::Vec2 at =
                mm::texel_point(region, kMinimapTexels, kMinimapTexels, column, row);
            heights[(static_cast<std::size_t>(row) * kMinimapTexels) + column] =
                map.terrain.height_at(at.x, at.y);
        }
    }
    std::vector<mm::Contour> contours(count);
    CY_REQUIRE(mm::classify_contours({heights.data(), heights.size()}, kMinimapTexels,
                                     kMinimapTexels, bake.style, {contours.data(), contours.size()})
                   .has_value());
    const f32 interval = bake.style.contour_interval;
    const auto level = [interval](f32 h) {
        return static_cast<cy::i64>(
            std::floor(static_cast<double>(h) / static_cast<double>(interval)));
    };
    const f32 water = map.layout.water_level;
    cy::u32 lines = 0;
    cy::u32 shore = 0;
    cy::u32 misplaced = 0;
    for (cy::u32 row = 0; row < kMinimapTexels; ++row) {
        for (cy::u32 column = 0; column < kMinimapTexels; ++column) {
            const f32 here = heights[(static_cast<std::size_t>(row) * kMinimapTexels) + column];
            bool crossed = false;
            bool wet_beside = false;
            const int steps[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto& step : steps) {
                const int c = static_cast<int>(column) + step[0];
                const int r = static_cast<int>(row) + step[1];
                if (c < 0 || r < 0 || !std::cmp_less(c, kMinimapTexels) ||
                    !std::cmp_less(r, kMinimapTexels)) {
                    continue;
                }
                const f32 there = heights[(static_cast<std::size_t>(r) * kMinimapTexels) +
                                          static_cast<std::size_t>(c)];
                wet_beside = wet_beside || there < water;
                crossed = crossed || level(there) < level(here);
            }
            const mm::Contour got =
                contours[(static_cast<std::size_t>(row) * kMinimapTexels) + column];
            if (here < water) {
                misplaced += got != mm::Contour::None ? 1U : 0U;
                continue;
            }
            if (wet_beside) {
                misplaced += got != mm::Contour::Shore ? 1U : 0U;
                ++shore;
                continue;
            }
            misplaced += (got != mm::Contour::None) != crossed ? 1U : 0U;
            lines += got != mm::Contour::None ? 1U : 0U;
        }
    }
    std::fprintf(stderr, "minimap contours: %u line texels, %u shore texels, %u misplaced\n", lines,
                 shore, misplaced);
    CY_CHECK_EQ(misplaced, 0U);
    // The map's hills give it contours, and its lake a shore.
    CY_CHECK_GT(lines, 1000U);
    CY_CHECK_GT(shore, 50U);

    // THE BAKE draws them: a line is darker than the ground beside it, the lake is water.
    std::vector<cy::u32> baked(count);
    CY_REQUIRE(
        mm::bake_topographic(bake.input, bake.style, {baked.data(), baked.size()}).has_value());
    CY_CHECK(save_rgba(binary("rts-topographic-minimap.png"), baked, kMinimapTexels).has_value());
    const cy::Vec2 lake = {-31.0F, 24.0F};  // the lake's two lobes meet near here
    const auto lake_column = static_cast<cy::u32>(
        (lake.x - region.left_x) / (region.right_x - region.left_x) * kMinimapTexels);
    const auto lake_row = static_cast<cy::u32>((lake.y - region.top_z) /
                                               (region.bottom_z - region.top_z) * kMinimapTexels);
    CY_REQUIRE(map.terrain.height_at(lake.x, lake.y) < water);
    const cy::u32 lake_texel =
        baked[(static_cast<std::size_t>(lake_row) * kMinimapTexels) + lake_column];
    CY_CHECK_GT((lake_texel >> 16U) & 0xFFU, lake_texel & 0xFFU);  // blue over red
}

CY_TEST_CASE("samples/13-rts-api map: the minimap is blue's fog over the map, black where unseen") {
    MapContent map;
    LoadReport load;
    std::string detail;
    CY_REQUIRE(load_map(cy::system_allocator(cy::MemoryDomain::Assets), CY_RTS_MAP_PACKAGE, map,
                        load, detail)
                   .has_value());
    const mm::MinimapRegion region = whole_map(map);
    const MinimapBake bake = minimap_bake(map, region, kMinimapTexels);
    mm::TopographicMinimap minimap(cy::system_allocator(cy::MemoryDomain::Renderer));
    CY_REQUIRE(minimap.bake(bake.input, bake.style).has_value());

    // Blue's worker at the level's centre, seeing 7 m, walked there from (8, 16): the grid a run
    // on the map builds, as the frame's field packs it.
    namespace gfog = cy::gameplay::fog;
    const gfog::FogGridDescription grid = sample::rts::fog_grid_over(map_bounds(map));
    gfog::FogOfWar fog(cy::system_allocator(cy::MemoryDomain::World));
    CY_REQUIRE(fog.create(grid).has_value());
    gfog::FogUnit worker;
    worker.id = 1;
    worker.team = 1;
    worker.x_raw = sample::rts::fog_raw(8.0F);
    worker.z_raw = sample::rts::fog_raw(16.0F);
    worker.reveal_radius_raw = sample::rts::fog_raw(7.0F);
    CY_REQUIRE(fog.update({&worker, 1}, 1).has_value());
    worker.x_raw = sample::rts::fog_raw(24.0F);
    CY_REQUIRE(fog.update({&worker, 1}, 2).has_value());
    std::vector<cy::u8> states(static_cast<std::size_t>(grid.width) * grid.height);
    CY_REQUIRE(fog.write_states(1, {states.data(), states.size()}).has_value());
    fow::VisibilityField field(cy::system_allocator(cy::MemoryDomain::Renderer));
    CY_REQUIRE(field.create(grid.width, grid.height).has_value());
    CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
    field.snap();
    const cy::Aabb covered = sample::rts::fog_grid_bounds(grid);
    const fow::FogGridPlacement placement{covered.min.x, covered.min.z, 1.0F};
    CY_REQUIRE(minimap.compose(field.texels(), grid.width, grid.height, placement, {}).has_value());

    const auto texel_at = [&](f32 x, f32 z) {
        const auto column = static_cast<cy::u32>((x - region.left_x) /
                                                 (region.right_x - region.left_x) * kMinimapTexels);
        const auto row = static_cast<cy::u32>((z - region.top_z) /
                                              (region.bottom_z - region.top_z) * kMinimapTexels);
        return (static_cast<std::size_t>(row) * kMinimapTexels) + column;
    };
    // Where the worker stands now: the bake itself.
    const std::size_t seen = texel_at(24.25F, 16.25F);
    CY_CHECK_EQ(minimap.fogged()[seen], minimap.base()[seen]);
    // Where it stood: grey, its channels equal, darker than the bake.
    const std::size_t remembered = texel_at(8.25F, 16.25F);
    const cy::u32 grey = minimap.fogged()[remembered];
    CY_CHECK_EQ(grey & 0xFFU, (grey >> 8U) & 0xFFU);
    CY_CHECK_EQ(grey & 0xFFU, (grey >> 16U) & 0xFFU);
    CY_CHECK_GT(grey & 0xFFU, 0U);
    // The far corners, never seen: black.
    CY_CHECK_EQ(minimap.fogged()[texel_at(-40.0F, 60.0F)], 0xFF00'0000U);
    CY_CHECK_EQ(minimap.fogged()[texel_at(70.0F, -60.0F)], 0xFF00'0000U);
}

CY_TEST_CASE("samples/13-rts-api map: damaged, mistyped, newer and truncated assets are refused") {
    MapContent content;
    CookReport report;
    std::string detail;
    CY_REQUIRE(cook_map(small_cook(nullptr), content, report, detail).has_value());
    std::vector<u8> layout_payload;
    encode_layout(content.layout, layout_payload);
    const std::vector<u8> cooked = wrap(cy::assets::AssetKind::Binary, layout_payload);

    MapContent out;
    CY_REQUIRE(decode_asset(kLayoutAsset, cy::Span<const u8>(cooked.data(), cooked.size()), out)
                   .has_value());
    CY_CHECK_EQ(out.layout.placements.size(), content.layout.placements.size());

    // A byte of the payload changed: the header's hash refuses it before a decoder reads it.
    std::vector<u8> damaged = cooked;
    damaged[damaged.size() - 7] ^= 0x5AU;
    CY_CHECK_FALSE(
        decode_asset(kLayoutAsset, cy::Span<const u8>(damaged.data(), damaged.size()), out)
            .has_value());

    // The layout's bytes asked for as the terrain: the cooked kind says what they are.
    const std::string mistyped =
        refusal(decode_asset(kTerrainAsset, cy::Span<const u8>(cooked.data(), cooked.size()), out));
    CY_CHECK_NE(mistyped.find("kind"), std::string::npos);

    // A newer map format, correctly wrapped: the decoder refuses the version, by name.
    std::vector<u8> newer = layout_payload;
    newer[4] = static_cast<u8>(kFormatVersion + 1U);
    const std::vector<u8> newer_cooked = wrap(cy::assets::AssetKind::Binary, newer);
    const std::string version = refusal(decode_asset(
        kLayoutAsset, cy::Span<const u8>(newer_cooked.data(), newer_cooked.size()), out));
    CY_CHECK_NE(version.find("newer"), std::string::npos);

    // A payload cut short, correctly wrapped: the decoder runs out and says so.
    std::vector<u8> cut(layout_payload.begin(), layout_payload.end() - 9);
    const std::vector<u8> cut_cooked = wrap(cy::assets::AssetKind::Binary, cut);
    const std::string truncated = refusal(
        decode_asset(kLayoutAsset, cy::Span<const u8>(cut_cooked.data(), cut_cooked.size()), out));
    CY_CHECK_NE(truncated.find("truncated"), std::string::npos);
}

CY_TEST_CASE("samples/13-rts-api map: a malformed source fails at its line") {
    struct Case {
        const char* text;
        u32 line;
        const char* says;
    };
    const Case cases[] = {
        {"map \"x\"\nlayer 0 grass 0.3 0.4 0.2 noise 3 0.3\nbogus 1\n", 3, "unknown statement"},
        {"map \"x\"\nlayer 1 grass 0.3 0.4 0.2 noise 3 0.3\n", 2, "index order"},
        {"map \"x\"\nlayer 0 grass 0.3 0.4 0.2 noise 3 0.3\nroad 4 3 : 0 0 1 1\n", 3,
         "layer is not declared"},
        {"map \"x\"\nlayer 0 grass 0.3 0.4 0.2 noise 3 0.3\nmound 1 2 zero 4\n", 3, "number"},
        {"map \"x\"\nlayer 0 a 1 1 1 noise 1 0\nmaterial m 1 1 1 0.5 0 texture none\n", 3,
         "texture that is not declared"},
    };
    for (const Case& item : cases) {
        MapSource source;
        u32 line = 0;
        const cy::Status parsed = parse_map_source(item.text, source, line);
        CY_CHECK_FALSE(parsed.has_value());
        CY_CHECK_EQ(line, item.line);
        CY_CHECK_NE(refusal(parsed).find(item.says), std::string::npos);
    }
}
