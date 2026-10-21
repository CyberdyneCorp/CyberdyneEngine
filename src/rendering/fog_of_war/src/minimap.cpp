// SPDX-License-Identifier: MIT
#include <cy/rendering/fog_of_war/minimap.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::fog_of_war {

u32 minimap_fog_texel(const FogSample& sample, const MinimapFogStyle& style) noexcept {
    const f32 cover = style.unexplored_alpha +
                      ((style.explored_alpha - style.unexplored_alpha) * sample.explored);
    const f32 alpha = std::clamp((1.0F - sample.visible) * cover, 0.0F, 1.0F);
    const auto premultiplied = [alpha](u32 channel) {
        return static_cast<u32>(std::lround(static_cast<f32>(channel) * alpha));
    };
    const u32 red = premultiplied((style.colour >> 16U) & 0xFFU);
    const u32 green = premultiplied((style.colour >> 8U) & 0xFFU);
    const u32 blue = premultiplied(style.colour & 0xFFU);
    const auto coverage = static_cast<u32>(std::lround(alpha * 255.0F));
    return red | (green << 8U) | (blue << 16U) | (coverage << 24U);
}

Status compose_minimap_fog(Span<const u8> texels, u32 grid_width, u32 grid_height,
                           const FogGridPlacement& placement, const MinimapRegion& region,
                           u32 width, u32 height, const MinimapFogStyle& style,
                           Span<u32> out) noexcept {
    if (width == 0 || height == 0 || out.size() != static_cast<usize>(width) * height) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the minimap overlay is not width * height texels");
    }
    if (texels.size() != static_cast<usize>(grid_width) * grid_height * kTexelBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "fog of war: the field's texels are not the grid's size");
    }
    for (u32 row = 0; row < height; ++row) {
        const f32 v = (static_cast<f32>(row) + 0.5F) / static_cast<f32>(height);
        const f32 z = region.top_z + ((region.bottom_z - region.top_z) * v);
        for (u32 column = 0; column < width; ++column) {
            const f32 u = (static_cast<f32>(column) + 0.5F) / static_cast<f32>(width);
            const f32 x = region.left_x + ((region.right_x - region.left_x) * u);
            const FogSample sample =
                sample_visibility(texels, grid_width, grid_height, placement, x, z);
            out[(static_cast<usize>(row) * width) + column] = minimap_fog_texel(sample, style);
        }
    }
    return ok();
}

}  // namespace cy::rendering::fog_of_war
