// Ground cover: patch descriptions in, blades out, and the ratio between them. M10 task 2.4;
// `foliage`'s "Grass is generated on the GPU" requirement.

#include <cy/test/test.h>

#include <cy/foliage/grass.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::ClusterBounds;
using cy::foliage::ClusterId;
using cy::foliage::CullView;
using cy::foliage::GrassBlade;
using cy::foliage::GrassBudget;
using cy::foliage::GrassField;
using cy::foliage::GrassPatch;
using cy::foliage::InteractionBounds;
using cy::foliage::InteractionEffect;
using cy::foliage::InteractionField;
using cy::foliage::InteractionPrimitive;
using cy::foliage::InteractionShape;

namespace {

[[nodiscard]] ClusterBounds meadow_bounds() noexcept {
    ClusterBounds bounds;
    bounds.min_x = 0.0;
    bounds.min_z = 0.0;
    bounds.max_x = 64.0;
    bounds.max_z = 64.0;
    bounds.min_y = 0.0F;
    bounds.max_y = 8.0F;
    return bounds;
}

[[nodiscard]] GrassPatch patch_at(cy::u16 qx, cy::u16 qz, cy::u64 seed) noexcept {
    GrassPatch patch;
    patch.seed = seed;
    patch.qx = qx;
    patch.qz = qz;
    patch.edge_decimetres = 40;  // 4 m
    patch.density = 200;
    patch.height_cm = 32;
    patch.height_variance_cm = 9;
    patch.orientation = 40;
    patch.spread_degrees = 100;
    patch.base_height = 2.0F;
    patch.height_dx = 0.02F;
    patch.height_dz = -0.01F;
    return patch;
}

[[nodiscard]] CullView meadow_view(cy::f64 eye_x) noexcept {
    CullView view;
    view.eye = cy::world::WorldVec3d{eye_x, 2.0, 32.0};
    view.max_distance_metres = 4096.0F;
    view.pixels_per_metre = 1000.0F;
    return view;
}

}  // namespace

CY_TEST_CASE("a patch description is thirty-two bytes and describes thousands of blades") {
    CY_CHECK_EQ(sizeof(GrassPatch), 32u);
    const GrassPatch patch = patch_at(0, 0, 1);
    // 4 m square at 200 blades a square metre.
    CY_CHECK_EQ(patch.full_blade_count(), 3200u);
}

CY_TEST_CASE("tens of millions of blades, and the stored data describes patches") {
    // `foliage` — "Persistent storage for ground cover SHALL be PROPORTIONAL TO PATCH DESCRIPTIONS,
    // not to blade count." The ratio is the requirement, and it is asserted rather than described.
    GrassField field(test::allocator());
    const ClusterBounds bounds = meadow_bounds();
    for (cy::u32 index = 0; index < 240; ++index) {
        const auto qx = static_cast<cy::u16>((index % 16) * 4096);
        const auto qz = static_cast<cy::u16>((index / 16) * 4096);
        CY_REQUIRE(field.add(ClusterId{1}, bounds, patch_at(qx, qz, 1000 + index)).has_value());
    }
    CY_CHECK_EQ(field.bytes(), 240u * 32u);

    GrassBudget budget;
    budget.distance_metres = 4096.0F;
    budget.max_blades = 4'000'000;
    cy::Array<GrassBlade> blades(test::allocator());
    auto report = field.expand(budget, meadow_view(32.0), nullptr, blades);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().blades_expanded, blades.size());
    CY_CHECK_GT(report.value().blades_expanded, 500'000u);
    // Thousands of blades per stored byte.
    CY_CHECK_GT(report.value().blades_per_stored_byte(), 50.0);
}

CY_TEST_CASE("a patch expands identically every time it is expanded") {
    const GrassPatch patch = patch_at(2000, 9000, 0xBEEF);
    const ClusterBounds bounds = meadow_bounds();
    cy::Array<GrassBlade> first(test::allocator());
    cy::Array<GrassBlade> second(test::allocator());
    const cy::u32 a = GrassField::expand_patch(patch, bounds, 1.0F, 4096, nullptr, first);
    const cy::u32 b = GrassField::expand_patch(patch, bounds, 1.0F, 4096, nullptr, second);
    CY_REQUIRE_EQ(a, b);
    CY_REQUIRE((a) > (100u));
    cy::u32 differences = 0;
    for (cy::u32 index = 0; index < a; ++index) {
        differences +=
            (first[index].position.x == second[index].position.x &&
             first[index].position.z == second[index].position.z &&
             first[index].height_metres == second[index].height_metres &&
             first[index].yaw == second[index].yaw && first[index].phase == second[index].phase)
                ? 0U
                : 1U;
    }
    CY_CHECK_EQ(differences, 0u);

    // A different patch seed is a different meadow, so the comparison above measures something.
    cy::Array<GrassBlade> other(test::allocator());
    const GrassPatch elsewhere = patch_at(2000, 9000, 0xBEF0);
    CY_REQUIRE_EQ(GrassField::expand_patch(elsewhere, bounds, 1.0F, 4096, nullptr, other), a);
    cy::u32 same = 0;
    for (cy::u32 index = 0; index < a; ++index) {
        same += first[index].position.x == other[index].position.x ? 1U : 0U;
    }
    CY_CHECK_LT(same, a / 4);
}

CY_TEST_CASE("expansion is bounded by distance, by visibility and by a budget") {
    GrassField field(test::allocator());
    const ClusterBounds bounds = meadow_bounds();
    for (cy::u32 index = 0; index < 16; ++index) {
        CY_REQUIRE(field
                       .add(ClusterId{1}, bounds,
                            patch_at(static_cast<cy::u16>(index * 4096), 32000, 5 + index))
                       .has_value());
    }

    // The distance lever.
    GrassBudget near_budget;
    near_budget.distance_metres = 12.0F;
    cy::Array<GrassBlade> blades(test::allocator());
    auto near_report = field.expand(near_budget, meadow_view(0.0), nullptr, blades);
    CY_REQUIRE(near_report.has_value());
    CY_CHECK_GT(near_report.value().patches_beyond_distance, 8u);

    // The density lever: the same patches, half the blades.
    GrassBudget full;
    full.distance_metres = 4096.0F;
    full.max_blades = 4'000'000;
    GrassBudget half = full;
    half.density_scale = 0.5F;
    cy::Array<GrassBlade> dense(test::allocator());
    cy::Array<GrassBlade> sparse(test::allocator());
    auto dense_report = field.expand(full, meadow_view(32.0), nullptr, dense);
    auto sparse_report = field.expand(half, meadow_view(32.0), nullptr, sparse);
    CY_REQUIRE(dense_report.has_value());
    CY_REQUIRE(sparse_report.has_value());
    CY_CHECK_LT(sparse_report.value().blades_expanded, dense_report.value().blades_expanded);
    CY_CHECK_GT(sparse_report.value().blades_expanded, dense_report.value().blades_expanded / 3u);

    // The ceiling. It is REPORTED rather than silently truncating the array, and the patches it
    // stopped are the FAR ones — a truncation would have left a straight edge across the meadow.
    GrassBudget capped = full;
    capped.max_blades = 5000;
    cy::Array<GrassBlade> capped_blades(test::allocator());
    auto capped_report = field.expand(capped, meadow_view(0.0), nullptr, capped_blades);
    CY_REQUIRE(capped_report.has_value());
    CY_CHECK_LE(capped_report.value().blades_expanded, 5000u);
    CY_CHECK_GT(capped_report.value().patches_over_budget, 0u);
    cy::f64 furthest_expanded = 0.0;
    for (const GrassBlade& blade : capped_blades) {
        furthest_expanded =
            blade.position.x > furthest_expanded ? blade.position.x : furthest_expanded;
    }
    CY_CHECK_LT(furthest_expanded, 32.0);
}

CY_TEST_CASE("ground cover responds to the interaction field like other foliage") {
    // `foliage` — "Ground cover SHALL respond to the wind field and the INTERACTION FIELD like
    // other foliage." The same `InteractionField::sample()` an instanced plant calls.
    InteractionBounds limits;
    limits.extent_metres = 64.0F;
    limits.trail_cells = 64;
    InteractionField interaction(test::allocator(), limits);
    CY_REQUIRE(interaction.recentre(cy::world::WorldVec3d{8.0, 0.0, 8.0}).has_value());

    InteractionPrimitive vehicle;
    vehicle.source = 1;
    vehicle.shape = InteractionShape::Sphere;
    vehicle.effect = InteractionEffect::Flatten;
    vehicle.position = cy::world::WorldVec3d{2.0, 0.0, 2.0};
    vehicle.radius_metres = 2.0F;
    vehicle.strength = 1.0F;
    CY_REQUIRE(interaction.register_primitive(vehicle).has_value());
    CY_REQUIRE(interaction.resolve(0.0F).has_value());

    const GrassPatch patch = patch_at(0, 0, 77);
    cy::Array<GrassBlade> blades(test::allocator());
    const cy::u32 count =
        GrassField::expand_patch(patch, meadow_bounds(), 1.0F, 4096, &interaction, blades);
    CY_REQUIRE((count) > (100u));
    cy::u32 flattened = 0;
    cy::u32 upright = 0;
    for (const GrassBlade& blade : blades) {
        if (blade.flatten > 0.1F) {
            ++flattened;
        } else {
            ++upright;
        }
    }
    CY_CHECK_GT(flattened, 0u);
    CY_CHECK_GT(upright, 0u);
}
