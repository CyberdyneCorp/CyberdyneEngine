// SPDX-License-Identifier: MIT
// The editor's terrain brushes, evaluated by the modifier stack over an author's region: each brush
// changes only its footprint, removing a stroke restores the region byte for byte, a smooth across
// a tile boundary leaves the boundary shared, and a hole reaches rendering and collision. Issue
// #29, "Terrain (finish)".
//
// "Only its footprint" is measured against the brush's own arithmetic stated here — a sample
// farther than the radius from every dab — rather than by asking `brush_weight()`, because a test
// that asks the implementation where the footprint is passes on a footprint that is too large.

#include <cy/test/test.h>

#include <cy/terrain/region.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

namespace {

constexpr cy::f32 kRadius = 8.0F;

[[nodiscard]] RegionDescription region() noexcept {
    RegionDescription value;
    value.terrain = 29;
    value.tiles = 2;
    value.extent = 128.0F;  // one metre between samples
    return value;
}

/// Rolling ground, so a smooth and a flatten have something to change.
[[nodiscard]] TerrainGenerator rolling() noexcept {
    TerrainGenerator value;
    value.period = 32.0F;
    value.amplitude = 6.0F;
    value.octaves = 3;
    value.base_height = 100.0F;
    return value;
}

/// A short stroke of three dabs that crosses the boundary between the two columns of tiles.
constexpr BrushPoint kStroke[3] = {{56.0, 40.0, 1.0F}, {64.0, 44.0, 1.0F}, {72.0, 48.0, 0.75F}};

[[nodiscard]] Modifier brush(BrushOp op) noexcept {
    Modifier modifier;
    modifier.kind = ModifierKind::Brush;
    modifier.name = brush_op_name(op);
    modifier.brush = op;
    modifier.radius = kRadius;
    modifier.amplitude = 0.8F;
    modifier.falloff = 0.5F;
    modifier.layer = 2;
    modifier.height = 96.0F;
    modifier.iterations = kBrushSmoothPasses;
    return modifier;
}

void build(ModifierStack& stack, const Modifier* extra) {
    stack.set_generator(rolling());
    if (extra != nullptr) {
        cy::Expected<cy::u32, cy::Error> index = stack.add(*extra);
        CY_REQUIRE(index.has_value());
        CY_REQUIRE(
            stack.add_brush(index.value(), cy::Span<const BrushPoint>(kStroke, 3)).has_value());
    }
}

[[nodiscard]] RegionSnapshot evaluate(const Modifier* extra) {
    ModifierStack stack(test::allocator(), region_layout(region()), 0x29);
    build(stack, extra);
    cy::Expected<RegionSnapshot, cy::Error> snapshot =
        evaluate_region(test::allocator(), stack, region());
    CY_REQUIRE(snapshot.has_value());
    return std::move(snapshot.value());
}

/// True when a position is at least the radius from every dab: outside the footprint.
[[nodiscard]] bool outside(cy::f64 x, cy::f64 z) noexcept {
    return std::ranges::all_of(kStroke, [&](const BrushPoint& dab) {
        const cy::f64 dx = x - dab.x;
        const cy::f64 dz = z - dab.z;
        return std::sqrt((dx * dx) + (dz * dz)) >= static_cast<cy::f64>(kRadius);
    });
}

struct HeightDiff {
    cy::u32 changed_outside = 0;
    cy::u32 changed_inside = 0;
    cy::i64 signed_change = 0;
};

[[nodiscard]] HeightDiff compare_heights(const RegionSnapshot& before,
                                         const RegionSnapshot& after) noexcept {
    HeightDiff diff;
    const cy::f64 spacing = static_cast<cy::f64>(region().extent) / (before.edge - 1);
    for (cy::u32 z = 0; z < before.edge; ++z) {
        for (cy::u32 x = 0; x < before.edge; ++x) {
            const cy::usize at = (static_cast<cy::usize>(z) * before.edge) + x;
            if (before.heights[at] == after.heights[at]) {
                continue;
            }
            if (outside(x * spacing, z * spacing)) {
                ++diff.changed_outside;
            } else {
                ++diff.changed_inside;
                diff.signed_change += static_cast<cy::i64>(after.heights[at]) -
                                      static_cast<cy::i64>(before.heights[at]);
            }
        }
    }
    return diff;
}

[[nodiscard]] cy::f64 roughness(const RegionSnapshot& snapshot) noexcept {
    // Sum of squared differences between horizontal neighbours inside the footprint.
    cy::f64 total = 0.0;
    const cy::f64 spacing = static_cast<cy::f64>(region().extent) / (snapshot.edge - 1);
    for (cy::u32 z = 0; z < snapshot.edge; ++z) {
        for (cy::u32 x = 0; x + 1 < snapshot.edge; ++x) {
            if (outside(x * spacing, z * spacing)) {
                continue;
            }
            const cy::usize at = (static_cast<cy::usize>(z) * snapshot.edge) + x;
            const cy::f64 step = static_cast<cy::f64>(snapshot.heights[at + 1]) -
                                 static_cast<cy::f64>(snapshot.heights[at]);
            total += step * step;
        }
    }
    return total;
}

[[nodiscard]] bool same_bytes(const RegionSnapshot& a, const RegionSnapshot& b) noexcept {
    if (a.edge != b.edge || a.heights.size() != b.heights.size() ||
        a.texels.size() != b.texels.size()) {
        return false;
    }
    for (cy::usize index = 0; index < a.heights.size(); ++index) {
        if (a.heights[index] != b.heights[index]) {
            return false;
        }
    }
    for (cy::usize index = 0; index < a.texels.size(); ++index) {
        if (!(a.texels[index] == b.texels[index]) || a.holes[index] != b.holes[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

CY_TEST_CASE(
    "a brush dab's weight is one in its core, falls to zero at its radius, and stays zero") {
    CY_CHECK_EQ(brush_falloff(0.0F, 8.0F, 0.5F), 1.0F);
    CY_CHECK_EQ(brush_falloff(4.0F, 8.0F, 0.5F), 1.0F);
    const cy::f32 soft = brush_falloff(6.0F, 8.0F, 0.5F);
    CY_CHECK_GT(soft, 0.0F);
    CY_CHECK_LT(soft, 1.0F);
    CY_CHECK_EQ(brush_falloff(8.0F, 8.0F, 0.5F), 0.0F);
    CY_CHECK_EQ(brush_falloff(9.0F, 8.0F, 0.5F), 0.0F);
    CY_CHECK_EQ(brush_falloff(7.99F, 8.0F, 0.0F), 1.0F);  // a hard disc
}

CY_TEST_CASE("raise and lower change only the stroke's footprint, in their own direction") {
    const RegionSnapshot base = evaluate(nullptr);
    for (const BrushOp op : {BrushOp::Raise, BrushOp::Lower}) {
        const Modifier modifier = brush(op);
        const RegionSnapshot after = evaluate(&modifier);
        const HeightDiff diff = compare_heights(base, after);
        CY_TEST_MESSAGE(brush_op_name(op));
        CY_CHECK_EQ(diff.changed_outside, 0U);
        CY_CHECK_GT(diff.changed_inside, 100U);
        if (op == BrushOp::Raise) {
            CY_CHECK_GT(diff.signed_change, 0);
        } else {
            CY_CHECK_LT(diff.signed_change, 0);
        }
    }
}

CY_TEST_CASE("smooth changes only the stroke's footprint and makes it smoother") {
    const RegionSnapshot base = evaluate(nullptr);
    const Modifier modifier = brush(BrushOp::Smooth);
    const RegionSnapshot after = evaluate(&modifier);
    const HeightDiff diff = compare_heights(base, after);
    CY_CHECK_EQ(diff.changed_outside, 0U);
    CY_CHECK_GT(diff.changed_inside, 50U);
    CY_CHECK_LT(roughness(after), roughness(base));
}

CY_TEST_CASE("flatten changes only the stroke's footprint and moves it toward its target") {
    const RegionSnapshot base = evaluate(nullptr);
    const Modifier modifier = brush(BrushOp::Flatten);
    const RegionSnapshot after = evaluate(&modifier);
    const HeightDiff diff = compare_heights(base, after);
    CY_CHECK_EQ(diff.changed_outside, 0U);
    CY_CHECK_GT(diff.changed_inside, 100U);

    const TileLayout layout = region_layout(region());
    const cy::u16 target = quantise_height(layout, modifier.height);
    cy::u32 closer = 0;
    cy::u32 farther = 0;
    for (cy::usize index = 0; index < base.heights.size(); ++index) {
        const cy::i32 was = std::abs(static_cast<cy::i32>(base.heights[index]) - target);
        const cy::i32 now = std::abs(static_cast<cy::i32>(after.heights[index]) - target);
        closer += (now < was) ? 1U : 0U;
        farther += (now > was) ? 1U : 0U;
    }
    CY_CHECK_GT(closer, 100U);
    CY_CHECK_EQ(farther, 0U);
}

CY_TEST_CASE("paint writes its layer into the footprint's texels and nowhere else") {
    const RegionSnapshot base = evaluate(nullptr);
    const Modifier modifier = brush(BrushOp::Paint);
    const RegionSnapshot after = evaluate(&modifier);
    CY_CHECK_EQ(compare_heights(base, after).changed_inside, 0U);  // paint moves no ground
    const cy::u32 quads = base.edge - 1;
    const cy::f64 spacing = static_cast<cy::f64>(region().extent) / quads;
    cy::u32 painted = 0;
    cy::u32 changed_outside = 0;
    for (cy::u32 z = 0; z < quads; ++z) {
        for (cy::u32 x = 0; x < quads; ++x) {
            const MaterialTexel& was = base.texels[(static_cast<cy::usize>(z) * quads) + x];
            const MaterialTexel& now = after.texels[(static_cast<cy::usize>(z) * quads) + x];
            if (outside(x * spacing, z * spacing)) {
                changed_outside += (was == now) ? 0U : 1U;
                continue;
            }
            bool has_layer = false;
            cy::u32 sum = 0;
            for (cy::u32 slot = 0; slot < kMaxTexelLayers; ++slot) {
                has_layer = has_layer || (now.layer[slot] == 2 && now.weight[slot] > 0);
                sum += now.weight[slot];
            }
            painted += has_layer ? 1U : 0U;
            CY_REQUIRE_EQ(sum, 255U);
        }
    }
    CY_CHECK_EQ(changed_outside, 0U);
    CY_CHECK_GT(painted, 100U);
}

CY_TEST_CASE("painting a texel twice with one layer keeps it one layer and sums to 255") {
    MaterialTexel texel{{0, 0, 0, 0}, {255, 0, 0, 0}};
    paint_texel(texel, 3, 0.5F);
    paint_texel(texel, 3, 0.5F);
    CY_CHECK_EQ(texel.used(), 2U);
    CY_CHECK_EQ(texel.layer[0], 3);
    CY_CHECK_EQ(static_cast<cy::u32>(texel.weight[0]) + texel.weight[1], 255U);
    paint_texel(texel, 3, 1.0F);
    CY_CHECK_EQ(texel.used(), 1U);
    CY_CHECK_EQ(texel.weight[0], 255);
}

CY_TEST_CASE("a hole brush removes rendering and collision exactly where it cuts") {
    const RegionSnapshot base = evaluate(nullptr);
    const Modifier modifier = brush(BrushOp::Hole);
    const RegionSnapshot after = evaluate(&modifier);
    CY_CHECK_EQ(compare_heights(base, after).changed_inside, 0U);

    const cy::u32 quads = base.edge - 1;
    const cy::f64 spacing = static_cast<cy::f64>(region().extent) / quads;
    cy::u32 holes = 0;
    cy::u32 outside_holes = 0;
    for (cy::u32 z = 0; z < quads; ++z) {
        for (cy::u32 x = 0; x < quads; ++x) {
            const bool cut = after.holes[(static_cast<cy::usize>(z) * quads) + x] != 0;
            holes += cut ? 1U : 0U;
            outside_holes += (cut && outside(x * spacing, z * spacing)) ? 1U : 0U;
        }
    }
    CY_CHECK_EQ(base.rendered_hole_quads, 0U);
    CY_CHECK_EQ(base.collision_holes, 0U);
    CY_CHECK_GT(holes, 100U);
    CY_CHECK_EQ(outside_holes, 0U);
    // Rendering leaves exactly the cut quads open, two triangles each.
    CY_CHECK_EQ(after.rendered_hole_quads, holes);
    CY_CHECK_EQ(base.rendered_triangles - after.rendered_triangles, 2U * holes);
    // Collision reaches physics with the sentinel over the cut.
    CY_CHECK_GT(after.collision_holes, 0U);
    // The joined render mesh is what meshing emitted, three indices per triangle.
    CY_CHECK_EQ(after.indices.size(), static_cast<cy::usize>(after.rendered_triangles) * 3U);
    CY_CHECK_EQ(after.positions.size(), after.normals.size());
}

CY_TEST_CASE("removing a stroke restores the region's heights, weights and holes byte for byte") {
    // What an editor undo sends the engine: the stack as it was before the stroke.
    const RegionSnapshot before = evaluate(nullptr);
    for (const BrushOp op : {BrushOp::Raise, BrushOp::Lower, BrushOp::Smooth, BrushOp::Flatten,
                             BrushOp::Paint, BrushOp::Hole}) {
        const Modifier modifier = brush(op);
        const RegionSnapshot stroked = evaluate(&modifier);
        CY_TEST_MESSAGE(brush_op_name(op));
        CY_CHECK_FALSE(same_bytes(before, stroked));
        const RegionSnapshot undone = evaluate(nullptr);
        CY_CHECK(same_bytes(before, undone));
    }
}

CY_TEST_CASE("a disabled brush changes nothing") {
    const RegionSnapshot before = evaluate(nullptr);
    Modifier modifier = brush(BrushOp::Raise);
    modifier.enabled = false;
    CY_CHECK(same_bytes(before, evaluate(&modifier)));
}

CY_TEST_CASE("a smooth across a tile boundary leaves the two tiles agreeing on it") {
    // Each tile is evaluated over its own padded grid. The shared column agrees only if the halo
    // covers the dab radius plus every Jacobi pass, which is what this case can see fail.
    ModifierStack stack(test::allocator(), region_layout(region()), 0x29);
    const Modifier modifier = brush(BrushOp::Smooth);
    build(stack, &modifier);
    cy::Expected<TerrainTile, cy::Error> left =
        stack.evaluate(test::allocator(), TileCoord{0, 0, 0});
    cy::Expected<TerrainTile, cy::Error> right =
        stack.evaluate(test::allocator(), TileCoord{1, 0, 0});
    CY_REQUIRE(left.has_value());
    CY_REQUIRE(right.has_value());
    cy::u32 disagreements = 0;
    for (cy::u32 j = 0; j < kTileVerts; ++j) {
        disagreements +=
            (left.value().stored(kTileQuads, j) == right.value().stored(0, j)) ? 0U : 1U;
    }
    CY_CHECK_EQ(disagreements, 0U);
}

CY_TEST_CASE("a flatten target read from the stack is the ground under the point") {
    ModifierStack stack(test::allocator(), region_layout(region()), 0x29);
    build(stack, nullptr);
    const RegionSnapshot base = evaluate(nullptr);
    cy::Expected<cy::f32, cy::Error> height =
        region_height_at(test::allocator(), stack, region(), 70.2, 33.9);
    CY_REQUIRE(height.has_value());
    const cy::u16 expected = base.heights[(34U * base.edge) + 70U];
    CY_CHECK_EQ(quantise_height(region_layout(region()), height.value()), expected);
    // The far edge is the last lattice line, which the last tile owns.
    cy::Expected<cy::f32, cy::Error> corner =
        region_height_at(test::allocator(), stack, region(), 128.0, 128.0);
    CY_REQUIRE(corner.has_value());
    CY_CHECK_EQ(quantise_height(region_layout(region()), corner.value()),
                base.heights[base.heights.size() - 1]);
}

CY_TEST_CASE("a region refuses a size it cannot evaluate") {
    RegionDescription none = region();
    none.tiles = 0;
    CY_CHECK_FALSE(validate_region(none).has_value());
    RegionDescription many = region();
    many.tiles = kMaxRegionTiles + 1;
    CY_CHECK_FALSE(validate_region(many).has_value());
    RegionDescription empty = region();
    empty.extent = 0.0F;
    CY_CHECK_FALSE(validate_region(empty).has_value());
    CY_CHECK(validate_region(region()).has_value());
}
