// SPDX-License-Identifier: MIT
// The lightmap's mip chain over an atlas drawn by hand. `integration.render_lightmap_mips`.
//
// `rendering-global-illumination` — "UV2 and chart packing": "padding sufficient for bilinear
// filtering and mip generation at the bake resolution".
//
// ================================================================================================
// THE ATLAS
// ================================================================================================
//
// One 512 page (a block is four texels, so two mip levels are protected and the gutter is four):
//
//   A  blocks (0, 0) 8 x 8    one red chart
//   B  blocks (8, 0) 8 x 8    one green chart, touching A
//   C  blocks (0, 8) 16 x 8   two charts of ONE object, blue and white, eight texels apart — the
//                             chart gap `required_chart_gap` asks for at two levels — placed so the
//                             gap straddles the level-1 and level-2 grids unevenly
//
// Every padding texel holds its nearest chart's colour, as the bake's dilation leaves it. The
// shadow mask holds the same colours. Every colour is exact in half precision, so ANY blend of two
// charts shows as a value that is not the chart's.
//
// ================================================================================================
// WHAT "READS ONLY ITS OWN CHART" MEANS
// ================================================================================================
//
// At every protected level, a bilinear tap at the centre of every texel a chart covers — the
// coarse-level read the frame makes of that surface — returns that chart's colour exactly. The
// control is a plain 2 x 2 box chain over the same atlas, which the same taps catch bleeding.

#include <cy/rendering/lightmap_bake/bake.h>
#include <cy/rendering/lightmap_bake/mips.h>
#include <cy/test/test.h>

#include <array>
#include <vector>

namespace {

using namespace cy::rendering::lightmap_bake;  // NOLINT(google-build-using-namespace)
using cy::f32;
using cy::u32;
using cy::usize;
using cy::Vec2;
using cy::Vec4;

constexpr u32 kPage = 512;
constexpr u32 kLevels = 2;

struct Chart {
    u32 x0;
    u32 y0;
    u32 x1;
    u32 y1;
    Vec4 colour;
};

/// Where rectangle C's two charts end and start: blue covers x in [4, blue_end), white
/// [white_start, 60). The default is the gap the header describes; the sweep moves it across the
/// coarse grids.
struct Split {
    u32 blue_end = 29;
    u32 white_start = 37;
};

using Charts = std::array<Chart, 4>;

[[nodiscard]] Charts charts_for(Split split) noexcept {
    return Charts{{
        {4, 4, 28, 28, Vec4{1.0F, 0.0F, 0.0F, 1.0F}},                   // A, red
        {36, 4, 60, 28, Vec4{0.0F, 1.0F, 0.0F, 1.0F}},                  // B, green
        {4, 36, split.blue_end, 60, Vec4{0.0F, 0.0F, 1.0F, 1.0F}},      // C, blue
        {split.white_start, 36, 60, 60, Vec4{1.0F, 1.0F, 1.0F, 0.5F}},  // C, white
    }};
}

/// Which chart a texel of rectangle C is nearest, the way the bake's dilation leaves padding: the
/// closer covered column, blue (the lower id) on a tie.
[[nodiscard]] u32 nearest_in_c(Split split, u32 x) noexcept {
    const u32 to_blue = x + 1U > split.blue_end ? x + 1U - split.blue_end : 0U;
    const u32 to_white = split.white_start > x ? split.white_start - x : 0U;
    return to_blue <= to_white ? 2U : 3U;
}

[[nodiscard]] u32 owner_at(Split split, u32 x, u32 y) noexcept {
    if (y < 32U) {
        return x < 32U ? 0U : (x < 64U ? 1U : kNoChart);
    }
    if (y < 64U && x < 64U) {
        return nearest_in_c(split, x);
    }
    return kNoChart;
}

[[nodiscard]] bool covered_by(const Chart& chart, u32 x, u32 y) noexcept {
    return x >= chart.x0 && x < chart.x1 && y >= chart.y0 && y < chart.y1;
}

struct HandAtlas {
    BakedLightmap lightmap;
    std::vector<u32> charts;
    Charts layout;

    explicit HandAtlas(Split split = Split{}) : layout(charts_for(split)) {
        lightmap.mode = LightmapMode::Irradiance;
        lightmap.page_size = kPage;
        lightmap.pages = 1;
        lightmap.gutter_texels = 4;
        lightmap.mip_levels = kLevels;
        for (LightmapTexels* texels : {&lightmap.texels, &lightmap.shadow_mask}) {
            texels->width = kPage;
            texels->height = kPage;
            texels->planes = 1;
            CY_REQUIRE(texels->texels.resize(usize{kPage} * kPage).has_value());
        }
        CY_REQUIRE(lightmap.shadow_lights.push_back(7).has_value());
        const AtlasPlacement placements[3] = {
            {0, 0, 0, 8, 8, false}, {0, 8, 0, 8, 8, false}, {0, 0, 8, 16, 8, false}};
        for (const AtlasPlacement& placement : placements) {
            CY_REQUIRE(lightmap.addresses.push_back(encode_address(placement)).has_value());
        }
        charts.assign(usize{kPage} * kPage, kNoChart);
        for (u32 y = 0; y < kPage; ++y) {
            for (u32 x = 0; x < kPage; ++x) {
                const usize at = (usize{y} * kPage) + x;
                const u32 owner = owner_at(split, x, y);
                const Vec4 colour = owner == kNoChart ? Vec4{} : layout[owner].colour;
                lightmap.texels.texels[at] = colour;
                lightmap.shadow_mask.texels[at] = Vec4{colour.w, colour.z, colour.y, colour.x};
                if (owner != kNoChart && covered_by(layout[owner], x, y)) {
                    charts[at] = owner;
                }
            }
        }
    }
};

[[nodiscard]] bool same(Vec4 a, Vec4 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/// Taps at the centre of every covered texel of every chart, at `level` of `texels`, that do not
/// return the chart's own value.
[[nodiscard]] u32 bleeding_taps(const Charts& layout, const LightmapTexels& texels, u32 level,
                                bool mask) {
    u32 wrong = 0;
    for (const Chart& chart : layout) {
        const Vec4 expected =
            mask ? Vec4{chart.colour.w, chart.colour.z, chart.colour.y, chart.colour.x}
                 : chart.colour;
        for (u32 y = chart.y0; y < chart.y1; ++y) {
            for (u32 x = chart.x0; x < chart.x1; ++x) {
                const f32 scale = 1.0F / static_cast<f32>(1U << level);
                const Vec2 at{(static_cast<f32>(x) + 0.5F) * scale,
                              (static_cast<f32>(y) + 0.5F) * scale};
                wrong += same(sample_plane(texels, 0, at), expected) ? 0U : 1U;
            }
        }
    }
    return wrong;
}

/// The control: a plain 2 x 2 box over the whole atlas, level after level.
[[nodiscard]] LightmapTexels box_level(const LightmapTexels& fine) {
    LightmapTexels coarse;
    coarse.width = fine.width / 2U;
    coarse.height = fine.height / 2U;
    coarse.planes = 1;
    CY_REQUIRE(coarse.texels.resize(usize{coarse.width} * coarse.height).has_value());
    for (u32 y = 0; y < coarse.height; ++y) {
        for (u32 x = 0; x < coarse.width; ++x) {
            const Vec4 sum = fine.texels[fine.index(0, 2 * x, 2 * y)] +
                             fine.texels[fine.index(0, (2 * x) + 1, 2 * y)] +
                             fine.texels[fine.index(0, 2 * x, (2 * y) + 1)] +
                             fine.texels[fine.index(0, (2 * x) + 1, (2 * y) + 1)];
            coarse.texels[coarse.index(0, x, y)] = sum * 0.25F;
        }
    }
    return coarse;
}

}  // namespace

CY_TEST_CASE("every level of the chain reads only its own chart, where a box filter bleeds") {
    HandAtlas atlas;
    CY_REQUIRE(build_lightmap_mips(atlas.lightmap, {atlas.charts.data(), atlas.charts.size()})
                   .has_value());

    LightmapTexels box = box_level(atlas.lightmap.texels);
    for (u32 level = 1; level <= kLevels; ++level) {
        const LightmapTexels& texels = lightmap_level(atlas.lightmap, level);
        const LightmapTexels& mask = shadow_mask_level(atlas.lightmap, level);
        CY_CHECK_EQ(texels.width, kPage >> level);
        CY_CHECK_EQ(texels.height, kPage >> level);
        CY_CHECK_EQ(mask.width, kPage >> level);
        const u32 chained = bleeding_taps(atlas.layout, texels, level, false);
        const u32 chained_mask = bleeding_taps(atlas.layout, mask, level, true);
        const u32 boxed = bleeding_taps(atlas.layout, box, level, false);
        CY_TEST_MESSAGE("level " << level << ": " << chained << " planes and " << chained_mask
                                 << " mask taps read another chart; a box filter, " << boxed);
        CY_CHECK_EQ(chained, 0U);
        CY_CHECK_EQ(chained_mask, 0U);
        // The control: the taps do see a bleed when there is one. Level 1 of a box filter bleeds
        // nowhere a tap reaches; level 2 does, at the white chart's edge across the gap.
        if (level == kLevels) {
            CY_CHECK_GT(boxed, 0U);
        }
        box = box_level(box);
    }
}

CY_TEST_CASE("wherever the chart gap falls on the coarse grids, no tap reads the other chart") {
    // The gap `required_chart_gap` asks for at two levels (eight texels) and one more, moved one
    // texel at a time across a level-2 texel: which chart owns the coarse padding texel between the
    // two depends on where the gap falls, and a tap from either side reaches it for some offset.
    u32 layouts = 0;
    for (const u32 gap : {8U, 9U}) {
        for (u32 blue_end = 29; blue_end <= 32; ++blue_end) {
            HandAtlas atlas(Split{blue_end, blue_end + gap});
            CY_REQUIRE(
                build_lightmap_mips(atlas.lightmap, {atlas.charts.data(), atlas.charts.size()})
                    .has_value());
            for (u32 level = 1; level <= kLevels; ++level) {
                const u32 wrong = bleeding_taps(atlas.layout, lightmap_level(atlas.lightmap, level),
                                                level, false);
                CY_TEST_MESSAGE("gap " << gap << " from x = " << blue_end << ", level " << level
                                       << ": " << wrong << " taps read the other chart");
                CY_CHECK_EQ(wrong, 0U);
            }
            layouts += 1U;
        }
    }
    CY_CHECK_EQ(layouts, 8U);
}

CY_TEST_CASE("the chain is only as deep as the padding protects, and one chart per texel") {
    HandAtlas atlas;
    atlas.lightmap.mip_levels = 3;  // a four-texel block protects two
    CY_CHECK_FALSE(build_lightmap_mips(atlas.lightmap, {atlas.charts.data(), atlas.charts.size()})
                       .has_value());
    atlas.lightmap.mip_levels = kLevels;
    CY_CHECK_FALSE(build_lightmap_mips(atlas.lightmap, {atlas.charts.data(), 16}).has_value());

    // No levels asked for: no chain, and the base is every level there is.
    atlas.lightmap.mip_levels = 0;
    CY_REQUIRE(build_lightmap_mips(atlas.lightmap, {atlas.charts.data(), atlas.charts.size()})
                   .has_value());
    CY_CHECK(atlas.lightmap.mip_texels[0].texels.empty());
    CY_CHECK_EQ(&lightmap_level(atlas.lightmap, 1), &atlas.lightmap.texels);
    CY_CHECK_EQ(atlas.lightmap.device_bytes(), cy::u64{kPage} * kPage * 8U);
}

CY_TEST_CASE("the chain costs a third of the base on the device, and the base is untouched") {
    HandAtlas atlas;
    const Vec4 before = atlas.lightmap.texels.texels[(usize{40} * kPage) + 33];
    CY_REQUIRE(build_lightmap_mips(atlas.lightmap, {atlas.charts.data(), atlas.charts.size()})
                   .has_value());
    CY_CHECK(same(atlas.lightmap.texels.texels[(usize{40} * kPage) + 33], before));
    const cy::u64 base = cy::u64{kPage} * kPage * 8U;
    CY_CHECK_EQ(atlas.lightmap.device_bytes(), base + (base / 4U) + (base / 16U));
    CY_CHECK_EQ(atlas.lightmap.shadow_mask_bytes(), base + (base / 4U) + (base / 16U));
}
