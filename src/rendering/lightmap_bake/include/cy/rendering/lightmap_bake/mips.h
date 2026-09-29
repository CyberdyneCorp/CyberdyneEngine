// SPDX-License-Identifier: MIT
#pragma once
// The lightmap's mip chain: the levels the atlas's gutter and chart padding were laid out for,
// each one filtered and dilated per chart so no chart bleeds into another at any level.
//
// `rendering-global-illumination` — "UV2 and chart packing": "Charts SHALL be packed into atlases
// with padding sufficient for bilinear filtering and mip generation at the bake resolution".
//
// ================================================================================================
// WHY A BOX FILTER IS NOT ENOUGH
// ================================================================================================
//
// Rectangles sit on the block grid, and a block is at least 2^levels texels, so a 2 x 2 box never
// straddles two OBJECTS at a protected level. It does straddle two CHARTS of one object: the
// padding between them is dilated half from each side, and a coarse texel over the middle of it
// averages both. A bilinear tap at a chart's edge at that level then reads the other chart's light
// — a seam that appears only as the camera backs away. So each level is built the way the base
// was dilated:
//
//   1. a coarse texel takes the mean of the fine texels of ONE chart — the chart most of its four
//      fine texels belong to (lowest id on a tie) — and belongs to that chart;
//   2. a coarse texel none of whose fine texels belongs to a chart is padding, and belongs to the
//      chart NEAREST its footprint at the base resolution (Chebyshev distance in base texels,
//      lowest id on a tie) — the ownership the base dilation gives each padding texel, carried
//      down. It takes its owner's value by dilation: one ring per pass outward from the owner's
//      filled texels.
//
// Nearest, rather than "the chart most of its neighbours belong to": in a gap two charts split
// unevenly across the coarse grid, a padding texel between them can have one neighbour of each,
// and the tie went to the chart whose taps never reach it — leaving the chart whose taps do to read
// its neighbour's light. The chart padding `report.required_chart_gap` asks for — two texels of the
// coarsest protected level — is what makes the nearest chart the one whose taps reach the texel.
//
// Every read and every write stays inside one rectangle, so an object never bleeds into another.
//
// ================================================================================================
// WHERE THE CHARTS COME FROM
// ================================================================================================
//
// The bake: every covered texel's chart id from the rasteriser, which is what the base dilation
// and the denoiser were bounded by. A chart is a caller's notion here: `build_lightmap_mips` takes
// one id per base texel and `kNoChart` for padding, and the tests hand it charts they drew.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::lightmap_bake {

struct BakedLightmap;
struct LightmapTexels;

/// A texel no chart covers: padding, gutter, or the space between rectangles.
inline constexpr u32 kNoChart = ~0U;

/// Levels below the base one chain can hold: log2 of the largest block (4096 / 128 texels).
inline constexpr u32 kMaxLightmapMipLevels = 5;

/// Build levels 1 to `lightmap.mip_levels` of the planes and of the shadow mask into
/// `lightmap.mips`.
///
/// `charts` holds one id per base texel, `kNoChart` where no chart covers it. Fails with
/// `InvalidArgument` when it does not, or when `mip_levels` exceeds what the page's block allows.
[[nodiscard]] Status build_lightmap_mips(BakedLightmap& lightmap, Span<const u32> charts) noexcept;

/// The planes of one level: 0 is the base (`lightmap.texels`).
[[nodiscard]] const LightmapTexels& lightmap_level(const BakedLightmap& lightmap,
                                                   u32 level) noexcept;
/// The shadow mask of one level: 0 is the base (`lightmap.shadow_mask`).
[[nodiscard]] const LightmapTexels& shadow_mask_level(const BakedLightmap& lightmap,
                                                      u32 level) noexcept;

}  // namespace cy::rendering::lightmap_bake
