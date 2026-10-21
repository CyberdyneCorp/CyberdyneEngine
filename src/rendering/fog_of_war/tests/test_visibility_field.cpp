// SPDX-License-Identifier: MIT
// unit.render_fog_of_war — fog of war's host half: the visibility field the frame uploads, its
// fade, its smoothing, the shading, the minimap overlay and the constants the dispatch reads.
// No device: render.fog_of_war holds the dispatch to the twins these cases pin.

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/fog/fog_of_war.h>
#include <cy/rendering/fog_of_war/fog_of_war_pass.h>
#include <cy/rendering/fog_of_war/minimap.h>
#include <cy/rendering/fog_of_war/visibility_field.h>
#include <cy/test/test.h>

#include <cmath>
#include <vector>

using namespace cy;
using namespace cy::rendering::fog_of_war;
namespace gfog = cy::gameplay::fog;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

constexpr i64 kMetre = gfog::kOneMetreRaw;

[[nodiscard]] gfog::FogUnit scout(u64 id, gameplay::TeamId team, i64 x, i64 z,
                                  i64 radius) noexcept {
    gfog::FogUnit unit;
    unit.id = id;
    unit.team = team;
    unit.x_raw = (x * kMetre) + (kMetre / 2);
    unit.z_raw = (z * kMetre) + (kMetre / 2);
    unit.reveal_radius_raw = radius * kMetre;
    return unit;
}

/// A field of `width` x `height` cells whose states are `states`, settled.
[[nodiscard]] bool settled_field(VisibilityField& field, u32 width, u32 height,
                                 const std::vector<u8>& states) {
    return field.create(width, height).has_value() &&
           field.set_states({states.data(), states.size()}).has_value() && (field.snap(), true);
}

}  // namespace

CY_TEST_CASE("fog of war: the visibility texture is the gameplay grid, cell for cell") {
    // The simulation: a blue scout walked from (4, 4) to (12, 4) on a 16 x 8 grid.
    gfog::FogOfWar fog(allocator());
    gfog::FogGridDescription grid;
    grid.width = 16;
    grid.height = 8;
    CY_REQUIRE(fog.create(grid).has_value());
    gfog::FogUnit unit = scout(1, 1, 4, 4, 2);
    CY_REQUIRE(fog.update({&unit, 1}, 1).has_value());
    unit.x_raw += 8 * kMetre;
    CY_REQUIRE(fog.update({&unit, 1}, 2).has_value());
    std::vector<u8> states(16U * 8U);
    CY_REQUIRE(fog.write_states(1, {states.data(), states.size()}).has_value());

    VisibilityField field(allocator());
    CY_REQUIRE(settled_field(field, 16, 8, states));
    const Span<const u8> texels = field.texels();
    u32 visible = 0;
    u32 explored = 0;
    for (u32 z = 0; z < 8U; ++z) {
        for (u32 x = 0; x < 16U; ++x) {
            const u8* texel = &texels[((z * 16U) + x) * kTexelBytes];
            const gfog::CellState state = fog.cell(1, x, z);
            CY_CHECK_EQ(texel[0], state == gfog::CellState::Visible ? 255U : 0U);
            CY_CHECK_EQ(texel[1], state == gfog::CellState::Unexplored ? 0U : 255U);
            CY_CHECK_EQ(texel[2], 0U);
            CY_CHECK_EQ(texel[3], 255U);
            visible += texel[0] == 255U ? 1U : 0U;
            explored += texel[1] == 255U && texel[0] == 0U ? 1U : 0U;
        }
    }
    CY_CHECK_EQ(visible, 13U);
    CY_CHECK_EQ(explored, 13U);
    CY_CHECK(field.settled());
    // A state byte the simulation never writes is refused, and a short span too.
    u8 wrong[16U * 8U] = {};
    wrong[3] = 3;
    CY_CHECK_FALSE(field.set_states(wrong).has_value());
    CY_CHECK_FALSE(field.set_states({states.data(), states.size() - 1U}).has_value());
}

CY_TEST_CASE("fog of war: a revealed cell fades in and a cell left behind fades out") {
    VisibilityField field(allocator());
    FogFadeSettings fade;
    fade.reveal_seconds = 0.2F;
    fade.hide_seconds = 0.8F;
    CY_REQUIRE(field.create(2, 1, fade).has_value());
    const u8 seen[2] = {kCellVisible, kCellUnexplored};
    CY_REQUIRE(field.set_states(seen).has_value());
    field.advance(0.1F);
    CY_CHECK_NEAR(field.visible(0, 0), 0.5F, 1.0e-6F);
    CY_CHECK_NEAR(field.explored(0, 0), 0.5F, 1.0e-6F);
    CY_CHECK_FALSE(field.settled());
    field.advance(0.1F);
    CY_CHECK_EQ(field.visible(0, 0), 1.0F);
    CY_CHECK(field.settled());
    // Left behind: four times slower out than in, and the cell stays explored throughout.
    const u8 left[2] = {kCellExplored, kCellUnexplored};
    CY_REQUIRE(field.set_states(left).has_value());
    field.advance(0.2F);
    CY_CHECK_NEAR(field.visible(0, 0), 0.75F, 1.0e-6F);
    CY_CHECK_EQ(field.explored(0, 0), 1.0F);
    CY_CHECK_EQ(field.texels()[0], 191U);
    field.advance(10.0F);
    CY_CHECK_EQ(field.visible(0, 0), 0.0F);
    CY_CHECK(field.settled());
    // A snap is the state at once: what a first frame or a team switch draws.
    CY_REQUIRE(field.set_states(seen).has_value());
    field.snap();
    CY_CHECK_EQ(field.visible(0, 0), 1.0F);
    CY_CHECK_EQ(field.explored(1, 0), 0.0F);
}

CY_TEST_CASE("fog of war: the sampled field is continuous across a cell boundary") {
    // Two visible cells beside two unexplored ones: the four taps half a cell apart make the level
    // a straight ramp two cells wide, which the smoothstep eases — its slope never steeper than
    // 1.5 times the ramp's half a level a cell — and a cell whose neighbours share its state reads
    // its state exactly.
    VisibilityField field(allocator());
    CY_REQUIRE(
        settled_field(field, 4, 1, {kCellVisible, kCellVisible, kCellUnexplored, kCellUnexplored}));
    FogGridPlacement placement;
    placement.cell_size = 2.0F;
    constexpr u32 kSteps = 384;
    f32 previous = 1.0F;
    f32 steepest = 0.0F;
    for (u32 step = 0; step <= kSteps; ++step) {
        // From cell 0's centre (1 m) to cell 3's (7 m): three cells, six metres.
        const f32 x = 1.0F + (6.0F * static_cast<f32>(step) / static_cast<f32>(kSteps));
        const FogSample sample = sample_visibility(field.texels(), 4, 1, placement, x, 1.0F);
        steepest = std::max(steepest, std::fabs(sample.visible - previous));
        previous = sample.visible;
    }
    const f32 per_step = (1.5F * 0.5F * 3.0F) / static_cast<f32>(kSteps);
    CY_CHECK_LE(steepest, per_step * 1.0001F);
    CY_CHECK_EQ(sample_visibility(field.texels(), 4, 1, placement, 1.0F, 1.0F).visible, 1.0F);
    CY_CHECK_EQ(sample_visibility(field.texels(), 4, 1, placement, 7.0F, 1.0F).visible, 0.0F);
    // Halfway between the visible and the unexplored, the level is one half, eased or not.
    CY_CHECK_NEAR(sample_visibility(field.texels(), 4, 1, placement, 4.0F, 1.0F).visible, 0.5F,
                  1.0e-6F);
    // A visible cell beside the boundary is no longer fully visible at its centre: the ramp is two
    // cells wide, which is what rounds the corners of the cells.
    const f32 beside = sample_visibility(field.texels(), 4, 1, placement, 3.0F, 1.0F).visible;
    CY_CHECK_GT(beside, 0.5F);
    CY_CHECK_LT(beside, 1.0F);
    // Off the grid reads the edge cell rather than wrapping or reading nothing.
    CY_CHECK_EQ(sample_visibility(field.texels(), 4, 1, placement, -50.0F, 1.0F).visible, 1.0F);
    CY_CHECK_EQ(sample_visibility(field.texels(), 4, 1, placement, 50.0F, 9.0F).explored, 0.0F);
}

CY_TEST_CASE(
    "fog of war: a visible surface is untouched, an explored one greyed, an unknown one hidden") {
    const Vec3 red{0.8F, 0.1F, 0.05F};
    FogOfWarStyle style;
    const Vec3 seen = shade_fog(red, FogSample{1.0F, 1.0F}, style);
    CY_CHECK_EQ(seen.x, red.x);
    CY_CHECK_EQ(seen.y, red.y);
    CY_CHECK_EQ(seen.z, red.z);
    const Vec3 explored = shade_fog(red, FogSample{0.0F, 1.0F}, style);
    const f32 luma = (0.2126F * red.x) + (0.7152F * red.y) + (0.0722F * red.z);
    CY_CHECK_NEAR(explored.x, (luma + ((red.x - luma) * 0.2F)) * 0.45F, 1.0e-6F);
    CY_CHECK_NEAR(explored.y, (luma + ((red.y - luma) * 0.2F)) * 0.45F, 1.0e-6F);
    // Greyer as well as darker: the spread between channels shrank by the saturation.
    CY_CHECK_NEAR(explored.x - explored.y, (red.x - red.y) * 0.2F * 0.45F, 1.0e-6F);
    style.unexplored_colour = Vec3{0.01F, 0.02F, 0.03F};
    const Vec3 hidden = shade_fog(red, FogSample{0.0F, 0.0F}, style);
    CY_CHECK_EQ(hidden.x, 0.01F);
    CY_CHECK_EQ(hidden.z, 0.03F);
}

CY_TEST_CASE("fog of war: the minimap overlay is the field the frame samples, north row first") {
    // Three visible columns, three explored, three unexplored; the two rows identical.
    std::vector<u8> states;
    for (u32 row = 0; row < 2U; ++row) {
        for (const u8 state : {kCellVisible, kCellExplored, kCellUnexplored}) {
            states.insert(states.end(), 3U, state);
        }
    }
    VisibilityField field(allocator());
    CY_REQUIRE(settled_field(field, 9, 2, states));
    FogGridPlacement placement;
    placement.cell_size = 10.0F;
    // Ninety texels across 90 m starting half a metre in: texel i's centre is at x = i + 1.
    MinimapRegion region{0.5F, 90.5F, 20.0F, 0.0F};
    MinimapFogStyle style;
    style.colour = 0x204060U;
    std::vector<u32> overlay(90U * 4U);
    CY_REQUIRE(compose_minimap_fog(field.texels(), 9, 2, placement, region, 90, 4, style,
                                   {overlay.data(), overlay.size()})
                   .has_value());
    for (u32 row = 0; row < 4U; ++row) {
        // The middle cell of each band: x = 15, 45 and 75 metres, texels 14, 44 and 74.
        const u32 clear = overlay[(row * 90U) + 14U];
        const u32 dim = overlay[(row * 90U) + 44U];
        const u32 dark = overlay[(row * 90U) + 74U];
        CY_CHECK_EQ(clear, 0U);
        CY_CHECK_EQ(dim >> 24U, 140U);
        CY_CHECK_EQ(dark, 0xFF604020U);
        CY_CHECK_EQ(dark, minimap_fog_texel(FogSample{0.0F, 0.0F}, style));
        // Premultiplied: no channel above the coverage.
        CY_CHECK_LE(dim & 0xFFU, dim >> 24U);
        // And monotone across each boundary: the fog only thickens eastward.
        for (u32 column = 1; column < 90U; ++column) {
            CY_CHECK_GE(overlay[(row * 90U) + column] >> 24U,
                        overlay[(row * 90U) + column - 1U] >> 24U);
        }
    }
    // Which world z is row 0 is the region's: a field visible in the south and unexplored in the
    // north is dark on the minimap's first row and clear on its last.
    VisibilityField tall(allocator());
    CY_REQUIRE(settled_field(tall, 1, 6,
                             {kCellVisible, kCellVisible, kCellVisible, kCellUnexplored,
                              kCellUnexplored, kCellUnexplored}));
    std::vector<u32> column(6);
    CY_REQUIRE(compose_minimap_fog(tall.texels(), 1, 6, placement, MinimapRegion{0, 10, 60, 0}, 1,
                                   6, style, {column.data(), column.size()})
                   .has_value());
    CY_CHECK_EQ(column[0] >> 24U, 255U);
    CY_CHECK_EQ(column[5], 0U);
    CY_CHECK_FALSE(compose_minimap_fog(tall.texels(), 1, 6, placement, region, 1, 5, style,
                                       {column.data(), column.size()})
                       .has_value());
    CY_CHECK(minimap_shows(2));
    CY_CHECK(minimap_shows(1));
    CY_CHECK_FALSE(minimap_shows(0));
}

CY_TEST_CASE("fog of war: the constants reconstruct a surface's world position from its depth") {
    FogOfWarView view;
    view.projection = perspective_reversed_z(0.9F, 16.0F / 9.0F, 0.1F, 200.0F);
    const Vec3 forward = normalize(Vec3{0.2F, -0.8F, -0.55F});
    view.relative_view = look_at(Vec3{0.0F, 0.0F, 0.0F}, forward, Vec3{0.0F, 1.0F, 0.0F});
    view.eye = Vec3{12.0F, 30.0F, 44.0F};
    FogGridPlacement placement{-16.0F, -16.0F, 2.0F};
    Expected<FogOfWarConstants, Error> packed =
        pack_fog_of_war_constants(view, placement, FogOfWarStyle{}, 480, 270, 32, 32);
    CY_REQUIRE(packed.has_value());
    const FogOfWarConstants& words = *packed;
    const Vec3 points[] = {Vec3{15.0F, 0.0F, 20.0F}, Vec3{3.0F, 1.0F, 30.0F},
                           Vec3{20.0F, 0.5F, 18.0F}};
    for (const Vec3 point : points) {
        const Vec3 relative = point - view.eye;
        const Vec4 clip =
            (view.projection * view.relative_view) * Vec4{relative.x, relative.y, relative.z, 1.0F};
        const Vec4 ndc{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, 1.0F};
        // The dispatch's arithmetic, word for word.
        const Vec4 back = (words.words[0] * ndc.x) + (words.words[1] * ndc.y) +
                          (words.words[2] * ndc.z) + (words.words[3] * ndc.w);
        const Vec3 world{(back.x / back.w) + words.words[4].x, (back.y / back.w) + words.words[4].y,
                         (back.z / back.w) + words.words[4].z};
        CY_CHECK_NEAR(world.x, point.x, 2.0e-3F);
        CY_CHECK_NEAR(world.y, point.y, 2.0e-3F);
        CY_CHECK_NEAR(world.z, point.z, 2.0e-3F);
    }
    CY_CHECK_EQ(words.words[5].z, 0.5F);
    CY_CHECK_EQ(words.words[8].z, 32.0F);
    // The refusals: a cell of no size, a style out of range, a singular camera, a zero extent.
    CY_CHECK_FALSE(pack_fog_of_war_constants(view, FogGridPlacement{0, 0, 0.0F}, FogOfWarStyle{},
                                             480, 270, 32, 32)
                       .has_value());
    FogOfWarStyle bright;
    bright.explored_brightness = 1.5F;
    CY_CHECK_FALSE(
        pack_fog_of_war_constants(view, placement, bright, 480, 270, 32, 32).has_value());
    FogOfWarView flat = view;
    flat.projection = Mat4::zero();
    CY_CHECK_FALSE(
        pack_fog_of_war_constants(flat, placement, FogOfWarStyle{}, 480, 270, 32, 32).has_value());
    CY_CHECK_FALSE(
        pack_fog_of_war_constants(view, placement, FogOfWarStyle{}, 0, 270, 32, 32).has_value());
}
