// SPDX-License-Identifier: MIT
// map_minimap.cpp — the RTS test map as the topographic minimap reads it. See map_minimap.h.

#include "map_minimap.h"

#include <algorithm>

namespace sample::rts::map {
namespace {

namespace mm = cy::rendering::minimap;
using cy::f32;
using cy::u32;
using cy::u8;
using cy::Vec2;

/// The cooked albedo averaged over the texel at `at`: the painted layers' colours.
[[nodiscard]] u32 albedo_under(const MapContent& content, Vec2 at, u32 texels) noexcept {
    const TextureData& albedo = content.albedo;
    const TerrainData& land = content.terrain;
    const f32 span = land.spacing * static_cast<f32>(land.edge - 1U);
    if (albedo.width == 0 || albedo.height == 0 || span <= 0.0F ||
        albedo.pixels.size() < static_cast<std::size_t>(albedo.width) * albedo.height * 4U) {
        return 0xFF7A'8A8AU;
    }
    // The albedo's texel (u, v) lies over (origin + u * span / width, origin + v * span / height).
    const f32 u = std::clamp((at.x - land.origin_x) / span, 0.0F, 1.0F);
    const f32 v = std::clamp((at.y - land.origin_z) / span, 0.0F, 1.0F);
    const u32 block = std::max(1U, albedo.width / std::max(texels, 1U));
    const u32 x0 =
        std::min(static_cast<u32>(u * static_cast<f32>(albedo.width)), albedo.width - block);
    const u32 y0 =
        std::min(static_cast<u32>(v * static_cast<f32>(albedo.height)), albedo.height - block);
    u32 sum[3] = {};
    for (u32 y = y0; y < y0 + block; ++y) {
        for (u32 x = x0; x < x0 + block; ++x) {
            const u8* texel =
                albedo.pixels.data() + (((static_cast<std::size_t>(y) * albedo.width) + x) * 4U);
            sum[0] += texel[0];
            sum[1] += texel[1];
            sum[2] += texel[2];
        }
    }
    const u32 count = block * block;
    return (sum[0] / count) | ((sum[1] / count) << 8U) | ((sum[2] / count) << 16U) | 0xFF00'0000U;
}

/// How much of the quad under `at` the road layer covers, 0 to 255.
[[nodiscard]] u8 road_under(const TerrainData& land, Vec2 at) noexcept {
    if (land.edge < 2U) {
        return 0;
    }
    const u32 quads = land.edge - 1U;
    const f32 fx = (at.x - land.origin_x) / land.spacing;
    const f32 fz = (at.y - land.origin_z) / land.spacing;
    if (fx < 0.0F || fz < 0.0F || fx >= static_cast<f32>(quads) || fz >= static_cast<f32>(quads)) {
        return 0;
    }
    const std::size_t quad = (static_cast<std::size_t>(fz) * quads) + static_cast<std::size_t>(fx);
    if (((quad + 1U) * 4U) > land.texel_layers.size() ||
        ((quad + 1U) * 4U) > land.texel_weights.size()) {
        return 0;
    }
    u32 cover = 0;
    for (u32 slot = 0; slot < 4U; ++slot) {
        cover += land.texel_layers[(quad * 4U) + slot] == kRoadLayer
                     ? land.texel_weights[(quad * 4U) + slot]
                     : 0U;
    }
    return static_cast<u8>(std::min<u32>(cover, 255U));
}

}  // namespace

MinimapBake minimap_bake(const MapContent& content, const mm::MinimapRegion& region, u32 texels) {
    MinimapBake bake;
    const TerrainData& land = content.terrain;
    bake.heights.resize(static_cast<std::size_t>(land.edge) * land.edge);
    for (u32 j = 0; j < land.edge; ++j) {
        for (u32 i = 0; i < land.edge; ++i) {
            bake.heights[(static_cast<std::size_t>(j) * land.edge) + i] = land.sample(i, j);
        }
    }
    bake.colours.resize(static_cast<std::size_t>(texels) * texels);
    bake.roads.resize(bake.colours.size());
    for (u32 row = 0; row < texels; ++row) {
        for (u32 column = 0; column < texels; ++column) {
            const Vec2 at = mm::texel_point(region, texels, texels, column, row);
            const std::size_t texel = (static_cast<std::size_t>(row) * texels) + column;
            bake.colours[texel] = albedo_under(content, at, texels);
            bake.roads[texel] = road_under(land, at);
        }
    }
    bake.input.heights =
        mm::HeightGrid{cy::Span<const f32>(bake.heights.data(), bake.heights.size()),
                       land.edge,
                       land.edge,
                       land.origin_x,
                       land.origin_z,
                       land.spacing};
    bake.input.region = region;
    bake.input.width = texels;
    bake.input.height = texels;
    bake.input.colours = cy::Span<const u32>(bake.colours.data(), bake.colours.size());
    bake.input.roads = cy::Span<const u8>(bake.roads.data(), bake.roads.size());
    bake.style.contour_interval = kContourMetres;
    bake.style.water_level = content.layout.water_level;
    return bake;
}

}  // namespace sample::rts::map
