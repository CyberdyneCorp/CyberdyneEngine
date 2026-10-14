// SPDX-License-Identifier: MIT
// A mesh as the GI scene holds it: cards pooled per cell and facing, and a signed distance grid
// from its triangles. Issue #35, stage 4.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/mesh_cards.h>

#include <cmath>
#include <cstdio>
#include <numbers>

namespace {

using cy::Aabb;
using cy::Array;
using cy::f32;
using cy::Mat4;
using cy::Span;
using cy::u32;
using cy::usize;
using cy::Vec3;
using cy::rendering::gi::append_mesh_cards;
using cy::rendering::gi::box_distance;
using cy::rendering::gi::MeshCardSettings;
using cy::rendering::gi::MeshDistanceField;
using cy::rendering::gi::MeshFieldSettings;
using cy::rendering::gi::Surfel;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// A box of half-extent `h` about the origin, wound outward.
struct BoxMesh {
    Vec3 positions[8]{};
    u32 indices[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                       3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};

    explicit BoxMesh(Vec3 h) noexcept {
        // Corners 1, 2, 5 and 6 are at +x, 2, 3, 6 and 7 at +y, 4 to 7 at +z: the order the
        // indices are wound outward over.
        constexpr bool kPlusX[8] = {false, true, true, false, false, true, true, false};
        constexpr bool kPlusY[8] = {false, false, true, true, false, false, true, true};
        for (u32 corner = 0; corner < 8; ++corner) {
            positions[corner] = Vec3{kPlusX[corner] ? h.x : -h.x, kPlusY[corner] ? h.y : -h.y,
                                     corner >= 4U ? h.z : -h.z};
        }
    }

    [[nodiscard]] Span<const Vec3> vertices() const noexcept { return {positions, 8}; }
    [[nodiscard]] Span<const u32> triangles() const noexcept { return {indices, 36}; }
};

/// A cylinder's side as a fan of slivers: `segments` quads from the base to `height`, outward.
struct CylinderSide {
    Array<Vec3> positions{allocator()};
    Array<u32> indices{allocator()};

    CylinderSide(f32 radius, f32 height, u32 segments) noexcept {
        for (u32 segment = 0; segment < segments; ++segment) {
            const f32 angle = 2.0F * std::numbers::pi_v<f32> * static_cast<f32>(segment) /
                              static_cast<f32>(segments);
            (void)positions.push_back(
                Vec3{radius * std::cos(angle), 0.0F, radius * std::sin(angle)});
            (void)positions.push_back(
                Vec3{radius * std::cos(angle), height, radius * std::sin(angle)});
        }
        for (u32 segment = 0; segment < segments; ++segment) {
            const u32 a = segment * 2U;
            const u32 b = ((segment + 1U) % segments) * 2U;
            const u32 quad[6] = {a, a + 1U, b + 1U, a, b + 1U, b};
            for (const u32 index : quad) {
                (void)indices.push_back(index);
            }
        }
    }
};

[[nodiscard]] f32 total_area(const Array<Surfel>& cards) noexcept {
    f32 area = 0.0F;
    for (const Surfel& card : cards.span()) {
        area += card.area;
    }
    return area;
}

}  // namespace

CY_TEST_CASE("a box's grid is the box's distance at every sample, signed") {
    const Vec3 half{1.0F, 0.6F, 0.8F};
    const BoxMesh box(half);
    MeshDistanceField field;
    MeshFieldSettings settings;
    settings.dimension = 16;
    settings.margin_metres = 0.5F;
    CY_REQUIRE(field.build(box.vertices(), box.triangles(), settings).has_value());
    const cy::rendering::gi::AssetDistanceField asset = field.asset();
    CY_REQUIRE_EQ(asset.dimension, 16U);
    CY_CHECK(std::abs(asset.bounds.min.x + 1.5F) < 1.0e-6F);
    CY_CHECK(std::abs(asset.bounds.max.y - 1.1F) < 1.0e-6F);
    const Vec3 size = asset.bounds.size();
    f32 worst = 0.0F;
    u32 inside = 0;
    for (u32 z = 0; z < 16; ++z) {
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                const Vec3 point{asset.bounds.min.x + (size.x * static_cast<f32>(x) / 15.0F),
                                 asset.bounds.min.y + (size.y * static_cast<f32>(y) / 15.0F),
                                 asset.bounds.min.z + (size.z * static_cast<f32>(z) / 15.0F)};
                const f32 stored = asset.distances[(((z * 16U) + y) * 16U) + x];
                const f32 exact = box_distance(point, half);
                worst = std::max(worst, std::abs(stored - exact));
                inside += stored < 0.0F ? 1U : 0U;
            }
        }
    }
    std::fprintf(stderr, "box grid: worst |stored - exact| %.3g m, %u samples inside\n",
                 static_cast<double>(worst), inside);
    CY_CHECK(worst < 1.0e-5F);
    CY_CHECK(inside > 0U);
}

CY_TEST_CASE("an open plane's grid is negative below it and positive above") {
    const Vec3 positions[4] = {
        {-2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, -2.0F}, {2.0F, 0.0F, 2.0F}, {-2.0F, 0.0F, 2.0F}};
    const u32 indices[6] = {0, 2, 1, 0, 3, 2};
    MeshDistanceField field;
    MeshFieldSettings settings;
    settings.dimension = 9;
    settings.margin_metres = 1.0F;
    CY_REQUIRE(field.build({positions, 4}, {indices, 6}, settings).has_value());
    const cy::rendering::gi::AssetDistanceField asset = field.asset();
    const f32 step = asset.bounds.size().y / 8.0F;
    for (u32 y = 0; y < 9; ++y) {
        const f32 height = asset.bounds.min.y + (step * static_cast<f32>(y));
        // The column through the plane's centre.
        const f32 stored = asset.distances[(((4U * 9U) + y) * 9U) + 4U];
        if (std::abs(height) > 1.0e-4F) {
            CY_CHECK(std::abs(stored - height) < 1.0e-5F);
        }
    }
}

CY_TEST_CASE("a box's cards cover its faces once, on them and facing out") {
    const BoxMesh box(Vec3{1.0F, 1.0F, 1.0F});
    Array<Surfel> cards(allocator());
    MeshCardSettings settings;
    settings.spacing_metres = 0.5F;
    settings.albedo = Vec3{0.3F, 0.4F, 0.5F};
    settings.instance_id = 7;
    CY_REQUIRE(append_mesh_cards(box.vertices(), box.triangles(),
                                 Mat4::from_translation(Vec3{4.0F, 0.0F, 0.0F}), settings, cards)
                   .has_value());
    // Six faces of four cells by four.
    CY_CHECK_EQ(cards.size(), static_cast<usize>(96));
    CY_CHECK(std::abs(total_area(cards) - 24.0F) < 1.0e-3F);
    for (const Surfel& card : cards.span()) {
        const Vec3 local = card.position - Vec3{4.0F, 0.0F, 0.0F};
        CY_CHECK(std::abs(box_distance(local, Vec3{1.0F, 1.0F, 1.0F})) < 1.0e-4F);
        // Out of the face it lies on.
        CY_CHECK(dot(card.normal, local) > 0.99F);
        // A piece pools by where its centre falls, so a cell's card is its cell's surface to within
        // the pieces that straddle the cell's edge: a third of the spacing each.
        CY_CHECK(std::abs(card.area - 0.25F) < 0.05F);
        CY_CHECK_EQ(card.instance_id, 7U);
        CY_CHECK(card.albedo.y == 0.4F);
    }
}

CY_TEST_CASE("a column of slivers has the cards its surface needs, not its triangles") {
    // A pillar: 0.42 m in radius, 4.4 m tall, 32 slivers 8 cm wide. A card per sub-triangle of
    // each sliver, cut by its 4.4 m edge, would be thousands.
    const CylinderSide pillar(0.42F, 4.4F, 32);
    Array<Surfel> cards(allocator());
    MeshCardSettings settings;
    settings.spacing_metres = 0.5F;
    CY_REQUIRE(append_mesh_cards(pillar.positions.span(), pillar.indices.span(), Mat4::identity(),
                                 settings, cards)
                   .has_value());
    const f32 surface = 2.0F * 0.42F * std::sin(std::numbers::pi_v<f32> / 32.0F) * 32.0F * 4.4F;
    const f32 per_card = 0.25F;
    std::fprintf(stderr, "pillar: %zu cards for %.2f m2\n", cards.size(),
                 static_cast<double>(surface));
    CY_CHECK(std::abs(total_area(cards) - surface) < 1.0e-2F);
    CY_CHECK(static_cast<f32>(cards.size()) < 4.0F * surface / per_card);
    CY_CHECK(static_cast<f32>(cards.size()) > surface / per_card);
    for (const Surfel& card : cards.span()) {
        // On the side, facing away from the axis.
        const f32 radial =
            std::sqrt((card.position.x * card.position.x) + (card.position.z * card.position.z));
        CY_CHECK(radial > 0.35F);
        CY_CHECK(radial < 0.43F);
        CY_CHECK((card.normal.x * card.position.x) + (card.normal.z * card.position.z) > 0.0F);
    }
}

CY_TEST_CASE("a region keeps only the cards inside it, and two sides doubles them") {
    const Vec3 positions[4] = {
        {-5.0F, 0.0F, -5.0F}, {5.0F, 0.0F, -5.0F}, {5.0F, 0.0F, 5.0F}, {-5.0F, 0.0F, 5.0F}};
    const u32 indices[6] = {0, 2, 1, 0, 3, 2};
    MeshCardSettings settings;
    settings.spacing_metres = 1.0F;
    settings.clip_to_region = true;
    settings.region = Aabb::from_min_max(Vec3{-2.0F, -1.0F, -2.0F}, Vec3{2.0F, 1.0F, 2.0F});
    Array<Surfel> cards(allocator());
    CY_REQUIRE(append_mesh_cards({positions, 4}, {indices, 6}, Mat4::identity(), settings, cards)
                   .has_value());
    CY_CHECK_EQ(cards.size(), static_cast<usize>(16));
    // The pieces straddling the region's edge: a third of a metre wide along its 16 m perimeter.
    CY_CHECK(std::abs(total_area(cards) - 16.0F) < 16.0F * (1.0F / 3.0F) * 0.5F);
    for (const Surfel& card : cards.span()) {
        CY_CHECK(card.normal.y > 0.99F);
    }
    settings.two_sided = true;
    settings.emission = Vec3{1.0F, 1.0F, 1.0F};
    Array<Surfel> both(allocator());
    CY_REQUIRE(append_mesh_cards({positions, 4}, {indices, 6}, Mat4::identity(), settings, both)
                   .has_value());
    CY_CHECK_EQ(both.size(), static_cast<usize>(32));
    u32 below = 0;
    for (const Surfel& card : both.span()) {
        if (card.normal.y < -0.99F) {
            ++below;
            // Only the front face emits.
            CY_CHECK(card.emission.x == 0.0F);
        }
    }
    CY_CHECK_EQ(below, 16U);
}

CY_TEST_CASE("a mesh that is not whole triangles is refused") {
    const Vec3 positions[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    const u32 broken[2] = {0, 1};
    const u32 past[3] = {0, 1, 3};
    Array<Surfel> cards(allocator());
    CY_CHECK(
        !append_mesh_cards({positions, 3}, {broken, 2}, Mat4::identity(), {}, cards).has_value());
    CY_CHECK(
        !append_mesh_cards({positions, 3}, {past, 3}, Mat4::identity(), {}, cards).has_value());
    MeshDistanceField field;
    CY_CHECK(!field.build({positions, 3}, {past, 3}, {}).has_value());
    MeshFieldSettings tiny;
    tiny.dimension = 1;
    const u32 whole[3] = {0, 1, 2};
    CY_CHECK(!field.build({positions, 3}, {whole, 3}, tiny).has_value());
    CY_CHECK(cards.empty());
}
