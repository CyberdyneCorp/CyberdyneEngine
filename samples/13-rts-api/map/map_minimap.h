// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_MAP_MINIMAP_H
#define CY_SAMPLE_RTS_API_MAP_MINIMAP_H
// map_minimap.h — what the RTS test map hands the engine's topographic minimap
// (cy::rendering-minimap): its height field, the colours its painted layers baked into the albedo,
// the road layer's coverage and the lake's surface. ABI 1.16's battle shows it on the HUD
// (host/map_render.h); integration.rts_map holds its contours to the map's heights.

#include <cy/rendering/minimap/topographic.h>

#include <vector>

#include "map_format.h"

namespace sample::rts::map {

/// Metres between two of the minimap's contour lines: the map's hills rise ten.
inline constexpr cy::f32 kContourMetres = 1.0F;
/// The terrain layer the source's `road` statements paint (content/rts/frontier.cymap).
inline constexpr cy::u8 kRoadLayer = 1;

/// A bake's input and the storage its spans point into: keep it alive while the input is used.
struct MinimapBake {
    std::vector<cy::f32> heights;
    std::vector<cy::u32> colours;
    std::vector<cy::u8> roads;
    cy::rendering::minimap::TopographicInput input;
    cy::rendering::minimap::TopographicStyle style;
};

/// The input for a `texels`-square minimap of `region`: the terrain's samples as the height field,
/// the cooked albedo averaged under each texel as its colour, the road layer's weight under it,
/// and the map's water level.
[[nodiscard]] MinimapBake minimap_bake(const MapContent& content,
                                       const cy::rendering::minimap::MinimapRegion& region,
                                       cy::u32 texels);

}  // namespace sample::rts::map

#endif  // CY_SAMPLE_RTS_API_MAP_MINIMAP_H
