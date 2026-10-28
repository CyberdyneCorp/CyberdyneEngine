// SPDX-License-Identifier: MIT
// unit.render_minimap — the topographic minimap on the host: contours where the levels cross,
// hillshading by the light, water, roads, the fog over it, the icons, and the fog stage skipped
// while the field stands still. openspec/changes/add-rts-combat-and-minimap.
//
// THE CONTOUR CASES ARE EXHAUSTIVE: for every pair of neighbouring texels of a field, a level
// between them puts a line on the higher one, and a line is never anywhere a level does not cross.
// That is what "the contour lines are at the interval" means, held for every texel rather than for
// a sample of them.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/fog_of_war/visibility_field.h>
#include <cy/rendering/minimap/topographic.h>
#include <cy/test/test.h>

#include <cmath>
#include <utility>
#include <vector>

using namespace cy;
using namespace cy::rendering::minimap;
namespace fow = cy::rendering::fog_of_war;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// A rolling field: two crossing waves and a mound, metres, over `size` texels a side.
[[nodiscard]] std::vector<f32> rolling(u32 size) {
    std::vector<f32> heights(static_cast<usize>(size) * size);
    for (u32 row = 0; row < size; ++row) {
        for (u32 column = 0; column < size; ++column) {
            const f32 x = static_cast<f32>(column);
            const f32 z = static_cast<f32>(row);
            const f32 dx = x - 20.0F;
            const f32 dz = z - 26.0F;
            heights[(static_cast<usize>(row) * size) + column] =
                (3.0F * std::sin(x * 0.11F)) + (2.5F * std::cos(z * 0.09F)) +
                (9.0F * std::exp(-((dx * dx) + (dz * dz)) / 180.0F));
        }
    }
    return heights;
}

[[nodiscard]] i64 level(f32 h, f32 interval) noexcept {
    return static_cast<i64>(std::floor(h / interval));
}

[[nodiscard]] u8 red(u32 texel) noexcept {
    return static_cast<u8>(texel & 0xFFU);
}
[[nodiscard]] u8 green(u32 texel) noexcept {
    return static_cast<u8>((texel >> 8U) & 0xFFU);
}
[[nodiscard]] u8 blue(u32 texel) noexcept {
    return static_cast<u8>((texel >> 16U) & 0xFFU);
}

/// A height grid one metre a sample, its origin at the world origin.
[[nodiscard]] HeightGrid grid_of(const std::vector<f32>& heights, u32 size) noexcept {
    HeightGrid grid;
    grid.heights = Span<const f32>(heights.data(), heights.size());
    grid.columns = size;
    grid.rows = size;
    return grid;
}

/// The region whose texel centres are the grid's samples: `size` texels over `size` metres,
/// offset half a texel.
[[nodiscard]] MinimapRegion on_samples(u32 size) noexcept {
    return MinimapRegion{-0.5F, static_cast<f32>(size) - 0.5F, -0.5F,
                         static_cast<f32>(size) - 0.5F};
}

}  // namespace

CY_TEST_CASE("minimap: a line on every texel a level crosses below, and nowhere else") {
    constexpr u32 kSize = 64;
    const std::vector<f32> heights = rolling(kSize);
    TopographicStyle style;
    style.contour_interval = 1.5F;
    style.index_every = 4;
    std::vector<Contour> contours(heights.size());
    CY_REQUIRE(classify_contours({heights.data(), heights.size()}, kSize, kSize, style,
                                 {contours.data(), contours.size()})
                   .has_value());
    const auto h = [&](u32 c, u32 r) { return heights[(static_cast<usize>(r) * kSize) + c]; };
    const auto mark = [&](u32 c, u32 r) { return contours[(static_cast<usize>(r) * kSize) + c]; };
    u32 lines = 0;
    u32 index_lines = 0;
    for (u32 row = 0; row < kSize; ++row) {
        for (u32 column = 0; column < kSize; ++column) {
            const f32 here = h(column, row);
            bool crossed = false;
            bool index = false;
            const std::pair<i32, i32> steps[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto& [dc, dr] : steps) {
                const i32 c = static_cast<i32>(column) + dc;
                const i32 r = static_cast<i32>(row) + dr;
                if (c < 0 || r < 0 || !std::cmp_less(c, kSize) || !std::cmp_less(r, kSize)) {
                    continue;
                }
                const f32 there = h(static_cast<u32>(c), static_cast<u32>(r));
                // A level L with there < L <= here: this texel is the higher one it crosses.
                for (i64 k = level(there, 1.5F) + 1; k <= level(here, 1.5F); ++k) {
                    crossed = true;
                    index = index || k % 4 == 0;
                }
            }
            const Contour got = mark(column, row);
            CY_CHECK_EQ(got != Contour::None, crossed);
            CY_CHECK_EQ(got == Contour::Index, crossed && index);
            lines += got != Contour::None ? 1U : 0U;
            index_lines += got == Contour::Index ? 1U : 0U;
        }
    }
    // A real field: many lines, some of them index lines, most of the ground clear.
    CY_CHECK_GT(lines, 200U);
    CY_CHECK_GT(index_lines, 20U);
    CY_CHECK_LT(lines, static_cast<u32>(heights.size() / 2U));
}

CY_TEST_CASE("minimap: on a ramp the lines stand exactly a contour interval apart") {
    // 0.25 m a texel along x: a 2 m interval is a line every eighth column, on the column whose
    // height first reaches the level.
    constexpr u32 kWidth = 80;
    constexpr u32 kHeight = 4;
    std::vector<f32> heights(static_cast<usize>(kWidth) * kHeight);
    for (u32 row = 0; row < kHeight; ++row) {
        for (u32 column = 0; column < kWidth; ++column) {
            heights[(static_cast<usize>(row) * kWidth) + column] =
                (0.25F * static_cast<f32>(column)) + 0.1F;
        }
    }
    TopographicStyle style;
    style.contour_interval = 2.0F;
    std::vector<Contour> contours(heights.size());
    CY_REQUIRE(classify_contours({heights.data(), heights.size()}, kWidth, kHeight, style,
                                 {contours.data(), contours.size()})
                   .has_value());
    std::vector<u32> columns;
    for (u32 column = 0; column < kWidth; ++column) {
        if (contours[column] != Contour::None) {
            columns.push_back(column);
        }
    }
    // Levels 2, 4, ... 18: the first column at or above each, 0.1 + 0.25 c >= 2k.
    CY_REQUIRE_EQ(columns.size(), 9U);
    for (usize index = 0; index < columns.size(); ++index) {
        const f32 level_metres = 2.0F * static_cast<f32>(index + 1U);
        CY_CHECK_EQ(columns[index], static_cast<u32>(std::ceil((level_metres - 0.1F) / 0.25F)));
    }
    // The tenth metre is the fifth level: an index contour.
    CY_CHECK(contours[columns[4]] == Contour::Index);
    CY_CHECK(contours[columns[3]] == Contour::Line);
}

CY_TEST_CASE("minimap: flat ground is its colour, a slope to the light brighter, away darker") {
    constexpr u32 kSize = 32;
    // A flat floor at 0.5 m with a ridge running along z at x = 16: its west face looks to the
    // light (which comes from the image's top left), its east face away.
    std::vector<f32> heights(static_cast<usize>(kSize) * kSize, 0.5F);
    for (u32 row = 0; row < kSize; ++row) {
        for (u32 column = 10; column < 22; ++column) {
            const f32 d = std::fabs(static_cast<f32>(column) - 16.0F);
            heights[(static_cast<usize>(row) * kSize) + column] = 0.5F + ((6.0F - d) * 0.3F);
        }
    }
    TopographicInput input;
    input.heights = grid_of(heights, kSize);
    input.region = on_samples(kSize);
    input.width = kSize;
    input.height = kSize;
    std::vector<u32> colours(heights.size(), 0xFF40'8060U);  // r 0x60, g 0x80, b 0x40
    input.colours = Span<const u32>(colours.data(), colours.size());
    TopographicStyle style;
    style.contour_interval = 100.0F;  // no lines
    std::vector<u32> out(heights.size());
    CY_REQUIRE(bake_topographic(input, style, {out.data(), out.size()}).has_value());
    const u32 flat = out[(16U * kSize) + 3U];
    CY_CHECK_EQ(red(flat), 0x60);
    CY_CHECK_EQ(green(flat), 0x80);
    CY_CHECK_EQ(blue(flat), 0x40);
    CY_CHECK_EQ(flat >> 24U, 0xFFU);
    const u32 lit = out[(16U * kSize) + 13U];
    const u32 shaded = out[(16U * kSize) + 19U];
    CY_CHECK_GT(green(lit), green(flat));
    CY_CHECK_LT(green(shaded), green(flat));
}

CY_TEST_CASE("minimap: water by its depth, a shore at its edge, and roads laid over the ground") {
    constexpr u32 kSize = 24;
    // A bowl: deepest in the middle, dry at its rim.
    std::vector<f32> heights(static_cast<usize>(kSize) * kSize);
    for (u32 row = 0; row < kSize; ++row) {
        for (u32 column = 0; column < kSize; ++column) {
            const f32 dx = static_cast<f32>(column) - 12.0F;
            const f32 dz = static_cast<f32>(row) - 12.0F;
            heights[(static_cast<usize>(row) * kSize) + column] =
                (std::sqrt((dx * dx) + (dz * dz)) * 0.5F) - 4.0F;
        }
    }
    std::vector<u8> roads(heights.size(), 0);
    for (u32 column = 0; column < kSize; ++column) {
        roads[column] = 255;  // a road along the top row
    }
    TopographicInput input;
    input.heights = grid_of(heights, kSize);
    input.region = on_samples(kSize);
    input.width = kSize;
    input.height = kSize;
    input.roads = Span<const u8>(roads.data(), roads.size());
    TopographicStyle style;
    style.water_level = 0.0F;
    style.contour_interval = 50.0F;
    style.shade_strength = 0.0F;
    std::vector<u32> out(heights.size());
    CY_REQUIRE(bake_topographic(input, style, {out.data(), out.size()}).has_value());
    // The deepest point is the deep colour; a point a metre deep is between the two.
    const u32 deepest = out[(12U * kSize) + 12U];
    CY_CHECK_EQ(red(deepest), 0x1F);
    CY_CHECK_EQ(blue(deepest), 0x86);
    const u32 shallower = out[(12U * kSize) + 18U];  // six texels out: a metre deep
    CY_CHECK_GT(red(shallower), 0x1F);
    CY_CHECK_LT(red(shallower), 0x5C);
    // The shore: the first dry texel out from the water, x = 20 (4 m - 4 = 0, dry at the level).
    std::vector<Contour> contours(heights.size());
    CY_REQUIRE(classify_contours({heights.data(), heights.size()}, kSize, kSize, style,
                                 {contours.data(), contours.size()})
                   .has_value());
    CY_CHECK(contours[(12U * kSize) + 20U] == Contour::Shore);
    CY_CHECK(contours[(12U * kSize) + 22U] == Contour::None);
    CY_CHECK(contours[(12U * kSize) + 12U] == Contour::None);
    // The road: the ground's colour mixed most of the way to the road's.
    const u32 road = out[3];
    const u32 off_road = out[(2U * kSize) + 3U];
    CY_CHECK_GT(red(road), red(off_road));
    CY_CHECK_GT(red(road), 0xB0);
}

CY_TEST_CASE("minimap: through the fog, visible is the bake, explored grey, unexplored black") {
    // An 8 x 8 field one metre a cell: the west third visible, the middle explored, the east never.
    constexpr u32 kCells = 9;
    std::vector<u8> states(static_cast<usize>(kCells) * kCells);
    for (u32 z = 0; z < kCells; ++z) {
        for (u32 x = 0; x < kCells; ++x) {
            u8 state = fow::kCellUnexplored;
            if (x < 3) {
                state = fow::kCellVisible;
            } else if (x < 6) {
                state = fow::kCellExplored;
            }
            states[(static_cast<usize>(z) * kCells) + x] = state;
        }
    }
    fow::VisibilityField field(allocator());
    CY_REQUIRE(field.create(kCells, kCells).has_value());
    CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
    field.snap();
    constexpr u32 kImage = 18;  // two texels a cell
    std::vector<u32> base(static_cast<usize>(kImage) * kImage, 0xFF20'A0C0U);
    std::vector<u32> out(base.size());
    const MinimapRegion region{0.0F, 9.0F, 0.0F, 9.0F};
    CY_REQUIRE(apply_minimap_fog({base.data(), base.size()}, kImage, kImage, region, field.texels(),
                                 kCells, kCells, fow::FogGridPlacement{}, MinimapFogLook{},
                                 {out.data(), out.size()})
                   .has_value());
    const u32 seen = out[(9U * kImage) + 1U];  // x 0.75 m: deep in the visible third
    CY_CHECK_EQ(seen, base[0]);
    const u32 remembered = out[(9U * kImage) + 9U];  // x 4.75 m: the explored middle
    CY_CHECK_EQ(red(remembered), green(remembered));
    CY_CHECK_EQ(green(remembered), blue(remembered));
    const f32 luma = (0.2126F * 0xC0) + (0.7152F * 0xA0) + (0.0722F * 0x20);
    CY_CHECK_EQ(static_cast<f32>(red(remembered)), std::round(luma * 0.55F));
    const u32 unknown = out[(9U * kImage) + 17U];  // x 8.75 m: never explored
    CY_CHECK_EQ(unknown, 0xFF00'0000U);
}

CY_TEST_CASE("minimap: icons stamp at their world point, outlined, and off the map not at all") {
    constexpr u32 kImage = 40;
    std::vector<u32> image(static_cast<usize>(kImage) * kImage, 0xFF80'8080U);
    const MinimapRegion region{-20.0F, 20.0F, -20.0F, 20.0F};  // a metre a texel
    const MinimapIcon icons[] = {
        {0.0F, 0.0F, 0x2050E0U, IconShape::Dot, 3.0F, true},
        {10.0F, -10.0F, 0xE03020U, IconShape::Square, 2.0F, false},
        {50.0F, 0.0F, 0xFFFFFFU, IconShape::Dot, 3.0F, true},
    };
    draw_icons({icons, 3}, region, kImage, kImage, {image.data(), image.size()});
    // The dot: its centre the fill, its rim the outline.
    const u32 centre = image[(20U * kImage) + 20U];
    CY_CHECK_EQ(red(centre), 0x20);
    CY_CHECK_EQ(blue(centre), 0xE0);
    const u32 rim = image[(20U * kImage) + 22U];
    CY_CHECK_LT(blue(rim), 0x20);
    // The square, at (30, 10) in texels, no outline.
    CY_CHECK_EQ(red(image[(10U * kImage) + 30U]), 0xE0);
    CY_CHECK_EQ(red(image[(11U * kImage) + 31U]), 0xE0);
    // Nothing else touched: the third icon is off the map.
    CY_CHECK_EQ(image[(20U * kImage) + 39U], 0xFF80'8080U);
    CY_CHECK_EQ(image[0], 0xFF80'8080U);
}

CY_TEST_CASE("minimap: the fog stage runs when the field changes and is reused while it stands") {
    constexpr u32 kSize = 16;
    const std::vector<f32> heights = rolling(kSize);
    TopographicMinimap map(allocator());
    TopographicInput input;
    input.heights = grid_of(heights, kSize);
    input.region = on_samples(kSize);
    input.width = kSize;
    input.height = kSize;
    CY_REQUIRE(map.bake(input, TopographicStyle{}).has_value());
    std::vector<u8> states(static_cast<usize>(kSize) * kSize, fow::kCellVisible);
    fow::VisibilityField field(allocator());
    CY_REQUIRE(field.create(kSize, kSize).has_value());
    CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
    field.snap();
    fow::FogGridPlacement placement;
    placement.origin_x = -0.5F;
    placement.origin_z = -0.5F;
    const MinimapIcon unit{3.0F, 3.0F, 0x0000FFU, IconShape::Dot, 1.0F, false};
    for (int frame = 0; frame < 5; ++frame) {
        CY_REQUIRE(map.compose(field.texels(), kSize, kSize, placement, {&unit, 1}).has_value());
    }
    CY_CHECK_EQ(map.fog_passes(), 1U);
    CY_CHECK_EQ(map.reused(), 4U);
    // All visible: the picture is the bake, but for the icon.
    CY_CHECK_EQ(map.image()[(10U * kSize) + 10U], map.base()[(10U * kSize) + 10U]);
    CY_CHECK_EQ(blue(map.image()[(3U * kSize) + 3U]), 0xFF);
    // The field changes: the fog stage runs again, and the picture shows it.
    states[(10U * kSize) + 10U] = fow::kCellUnexplored;
    CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
    field.snap();
    CY_REQUIRE(map.compose(field.texels(), kSize, kSize, placement, {}).has_value());
    CY_CHECK_EQ(map.fog_passes(), 2U);
    CY_CHECK_NE(map.image()[(10U * kSize) + 10U], map.base()[(10U * kSize) + 10U]);
}

CY_TEST_CASE("minimap: redrawing only the changed cells is the whole fog stage, texel for texel") {
    // The incremental fog stage against the whole one: a field whose units' sight moves a few
    // cells at a time, composed after every move, must give exactly the picture a full pass over
    // the same field gives — and draw far fewer texels to get it. (`fog_stage_fades` holds the
    // same over a field that is still fading.) The image is 1.5 texels a cell, so texel centres
    // fall both on and between cells and the bilinear sample reads neighbours.
    constexpr u32 kCells = 16;
    constexpr u32 kImage = 24;
    const std::vector<f32> heights = rolling(kCells);
    TopographicMinimap map(allocator());
    TopographicInput input;
    input.heights = grid_of(heights, kCells);
    input.region = MinimapRegion{0.0F, 16.0F, 0.0F, 16.0F};
    input.width = kImage;
    input.height = kImage;
    CY_REQUIRE(map.bake(input, TopographicStyle{}).has_value());
    fow::VisibilityField field(allocator());
    CY_REQUIRE(field.create(kCells, kCells).has_value());
    std::vector<u8> states(static_cast<usize>(kCells) * kCells, fow::kCellUnexplored);
    std::vector<u32> whole(static_cast<usize>(kImage) * kImage);
    // The texels drawn after the first composition, which draws the whole image.
    u64 first_texels = 0;
    u64 later_full_texels = 0;
    for (u32 step = 0; step < 5; ++step) {
        // A scout walks east along row 10, seeing two cells round it; what it saw stays explored.
        for (u32 z = 0; z < kCells; ++z) {
            for (u32 x = 0; x < kCells; ++x) {
                u8& cell = states[(static_cast<usize>(z) * kCells) + x];
                const bool near =
                    (x + 2U >= step * 2U && x <= (step * 2U) + 2U) && (z + 2U >= 10U && z <= 12U);
                if (near) {
                    cell = fow::kCellVisible;
                } else if (cell == fow::kCellVisible) {
                    cell = fow::kCellExplored;
                }
            }
        }
        CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
        field.snap();
        CY_REQUIRE(
            map.compose(field.texels(), kCells, kCells, fow::FogGridPlacement{}, {}).has_value());
        CY_REQUIRE(apply_minimap_fog(map.base(), kImage, kImage, input.region, field.texels(),
                                     kCells, kCells, fow::FogGridPlacement{}, map.look,
                                     {whole.data(), whole.size()})
                       .has_value());
        if (step == 0) {
            first_texels = map.refogged();
        } else {
            later_full_texels += whole.size();
        }
        usize differing = 0;
        for (usize index = 0; index < whole.size(); ++index) {
            differing += map.fogged()[index] != whole[index] ? 1U : 0U;
        }
        CY_CHECK_EQ(differing, 0U);
    }
    CY_CHECK_EQ(first_texels, whole.size());
    CY_CHECK_LT((map.refogged() - first_texels) * 4U, later_full_texels);
}

CY_TEST_CASE("minimap: fog_stage_fades — a field still fading is redrawn exactly where it moved") {
    constexpr u32 kCells = 8;
    constexpr u32 kImage = 24;
    const std::vector<f32> heights = rolling(kCells);
    TopographicMinimap map(allocator());
    TopographicInput input;
    input.heights = grid_of(heights, kCells);
    input.region = MinimapRegion{0.0F, 8.0F, 0.0F, 8.0F};
    input.width = kImage;
    input.height = kImage;
    CY_REQUIRE(map.bake(input, TopographicStyle{}).has_value());
    fow::VisibilityField field(allocator());
    CY_REQUIRE(field.create(kCells, kCells).has_value());
    std::vector<u8> states(static_cast<usize>(kCells) * kCells, fow::kCellUnexplored);
    std::vector<u32> whole(static_cast<usize>(kImage) * kImage);
    for (u32 step = 0; step < 6; ++step) {
        states[(3U * kCells) + step] = fow::kCellVisible;
        CY_REQUIRE(field.set_states({states.data(), states.size()}).has_value());
        field.advance(0.1F);
        CY_REQUIRE(
            map.compose(field.texels(), kCells, kCells, fow::FogGridPlacement{}, {}).has_value());
        CY_REQUIRE(apply_minimap_fog(map.base(), kImage, kImage, input.region, field.texels(),
                                     kCells, kCells, fow::FogGridPlacement{}, map.look,
                                     {whole.data(), whole.size()})
                       .has_value());
        usize differing = 0;
        for (usize index = 0; index < whole.size(); ++index) {
            differing += map.fogged()[index] != whole[index] ? 1U : 0U;
        }
        CY_CHECK_EQ(differing, 0U);
    }
}

CY_TEST_CASE("minimap: what the bake and the fog refuse") {
    std::vector<f32> heights(16, 0.0F);
    std::vector<Contour> contours(15);
    TopographicStyle style;
    CY_CHECK(!classify_contours({heights.data(), heights.size()}, 4, 4, style,
                                {contours.data(), contours.size()})
                  .has_value());
    contours.resize(16);
    style.contour_interval = 0.0F;
    CY_CHECK(!classify_contours({heights.data(), heights.size()}, 4, 4, style,
                                {contours.data(), contours.size()})
                  .has_value());
    TopographicInput input;
    input.width = 4;
    input.height = 4;
    std::vector<u32> colours(3);
    input.colours = Span<const u32>(colours.data(), colours.size());
    std::vector<u32> out(16);
    CY_CHECK(!bake_topographic(input, TopographicStyle{}, {out.data(), out.size()}).has_value());
    TopographicMinimap map(allocator());
    CY_CHECK(!map.compose({}, 0, 0, fow::FogGridPlacement{}, {}).has_value());
}
