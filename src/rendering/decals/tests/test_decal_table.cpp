// SPDX-License-Identifier: MIT
// The decal table's layout, word for word. `unit.rendering_decals`.
//
// `cy/decal.slang` reads what `pack_decal_table` writes by offset, so a field that moves on one
// side and not the other draws a decal somewhere else with a plausible colour. These cases pin the
// words the shader reads to the values they came from.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/decals/decal_table.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstring>

namespace {

using cy::rendering::DecalInstance;
using cy::rendering::decals::DecalMaterial;
using cy::rendering::decals::DecalShape;
using cy::rendering::decals::DecalTableInput;
using cy::rendering::decals::kDecalHeaderWords;
using cy::rendering::decals::kDecalMagic;
using cy::rendering::decals::kDecalRecordWords;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

cy::f32 as_float(cy::u32 word) noexcept {
    cy::f32 value = 0.0F;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

DecalInstance decal_at(cy::Vec3 centre, cy::u64 id, cy::i32 order) noexcept {
    DecalInstance decal;
    decal.id = id;
    decal.center = centre;
    decal.half_extent = cy::Vec3{2.0F, 0.5F, 0.25F};
    decal.sort_order = order;
    return decal;
}

}  // namespace

CY_TEST_CASE("the header names the table, and the records are in application order") {
    DecalInstance decals[3] = {decal_at({1, 0, 0}, 1, 5), decal_at({2, 0, 0}, 2, -1),
                               decal_at({3, 0, 0}, 3, 5)};
    cy::u32 order[3] = {};
    CY_REQUIRE_EQ(cy::rendering::decal_application_order(cy::Span<const DecalInstance>(decals, 3),
                                                         cy::Span<cy::u32>(order, 3)),
                  3U);
    // Ascending sort order, ties on id: decal 2 (order -1), then 1 and 3 (order 5, ids 1 < 3).
    CY_CHECK_EQ(order[0], 1U);
    CY_CHECK_EQ(order[1], 0U);
    CY_CHECK_EQ(order[2], 2U);

    DecalMaterial material;
    DecalTableInput input;
    input.decals = cy::Span<const DecalInstance>(decals, 3);
    input.order = cy::Span<const cy::u32>(order, 3);
    input.materials = cy::Span<const DecalMaterial>(&material, 1);
    cy::Array<cy::u32> words(allocator());
    CY_REQUIRE(cy::rendering::decals::pack_decal_table(input, words).has_value());

    CY_CHECK_EQ(words.size(), static_cast<cy::usize>(kDecalHeaderWords + (3 * kDecalRecordWords)));
    CY_CHECK_EQ(words[0], kDecalMagic);
    CY_CHECK_EQ(words[2], 3U);
    CY_CHECK_EQ(words[3], kDecalRecordWords);
    CY_CHECK_EQ(words[4], kDecalHeaderWords);
    CY_CHECK_EQ(words[5], 0U);  // no lists in the table
    // Rank r is decals[order[r]]: the centres come out 2, 1, 3.
    for (cy::u32 rank = 0; rank < 3U; ++rank) {
        const cy::u32 base = kDecalHeaderWords + (rank * kDecalRecordWords);
        CY_CHECK_EQ(as_float(words[base]), decals[order[rank]].center.x);
    }
}

CY_TEST_CASE("a record carries its decal and its material, relative to the origin") {
    // A decal a kilometre out and a camera a metre beside it: what reaches the shader is the metre,
    // relative to the camera, and never the kilometre.
    DecalInstance decal = decal_at({1000.25F, 20.0F, -3000.5F}, 7, 0);
    decal.axis_x = cy::Vec3{0.0F, 0.0F, 3.0F};  // not unit: the packer normalises
    decal.fade_angle_radians = 1.0F;
    decal.fade_start = 10.0F;
    decal.fade_end = 30.0F;
    decal.channels = 0x5U;
    decal.normal_strength = 0.5F;
    decal.weights.normal = 0.8F;
    decal.weights.emission = 0.25F;
    decal.material_index = 1;
    DecalMaterial materials[2];
    materials[1].albedo = cy::Vec3{0.1F, 0.2F, 0.3F};
    materials[1].roughness = 0.7F;
    materials[1].metallic = 0.4F;
    materials[1].emission = cy::Vec3{5.0F, 6.0F, 7.0F};
    materials[1].shape = DecalShape::Ring;
    materials[1].shape_a = 0.6F;
    materials[1].shape_b = 0.1F;
    materials[1].relief_metres = 0.003F;
    materials[1].edge_softness = 0.2F;
    materials[1].seed = 100;
    const cy::u32 order = 0;

    DecalTableInput input;
    input.decals = cy::Span<const DecalInstance>(&decal, 1);
    input.order = cy::Span<const cy::u32>(&order, 1);
    input.materials = cy::Span<const DecalMaterial>(materials, 2);
    input.origin = cy::Vec3{999.25F, 20.0F, -3000.5F};
    cy::Array<cy::u32> words(allocator());
    CY_REQUIRE(cy::rendering::decals::pack_decal_table(input, words).has_value());
    const cy::u32* record = words.data() + kDecalHeaderWords;

    CY_CHECK_EQ(as_float(record[0]), 1.0F);
    CY_CHECK_EQ(as_float(record[1]), 0.0F);
    CY_CHECK_EQ(as_float(record[2]), 0.0F);
    CY_CHECK_NEAR(as_float(record[3]), std::cos(1.0F), 1.0e-6F);
    // Axis x, normalised, divided by its half extent of 2.
    CY_CHECK_EQ(as_float(record[4]), 0.0F);
    CY_CHECK_EQ(as_float(record[6]), 0.5F);
    // Axis z divided by 0.25.
    CY_CHECK_EQ(as_float(record[14]), 4.0F);
    CY_CHECK_NEAR(as_float(record[15]), 0.4F, 1.0e-6F);  // normal weight × strength
    CY_CHECK_EQ(as_float(record[16]), 0.1F);
    CY_CHECK_EQ(as_float(record[19]), 0.7F);
    CY_CHECK_EQ(as_float(record[20]), 5.0F);
    CY_CHECK_EQ(as_float(record[23]), 0.4F);
    CY_CHECK_EQ(as_float(record[24]), 10.0F);
    CY_CHECK_EQ(as_float(record[25]), 30.0F);
    CY_CHECK_EQ(as_float(record[27]), 0.25F);
    CY_CHECK_EQ(record[28], 0x5U);
    CY_CHECK_EQ(record[29], static_cast<cy::u32>(DecalShape::Ring));
    CY_CHECK_EQ(as_float(record[30]), 0.6F);
    CY_CHECK_EQ(as_float(record[31]), 0.1F);
    CY_CHECK_EQ(record[32], cy::rendering::pipeline::kNoMaterialTexture);
    CY_CHECK_EQ(as_float(record[33]), 0.003F);
    CY_CHECK_EQ(as_float(record[34]), 0.2F);
    CY_CHECK_EQ(record[35], 107U);  // the material's seed plus the decal's id
}

CY_TEST_CASE("the lists in the table are the assignment's decal lists, and nothing else") {
    cy::rendering::ClusterGridConfig config;
    config.tile_size = 32;
    config.slice_count = 4;
    const cy::Expected<cy::rendering::ClusterGrid, cy::Error> grid =
        cy::rendering::make_cluster_grid(config, 64, 64, 1.0F, 100.0F);
    CY_REQUIRE(grid.has_value());

    // A light and two decals: the table carries the decals' lists only, as ranks.
    cy::rendering::ClusterElement elements[3];
    elements[0].view_position = cy::Vec3{0.0F, 0.0F, -5.0F};
    elements[0].radius = 30.0F;
    elements[0].payload_index = 4;
    elements[1] = elements[0];
    elements[1].type = cy::rendering::ClusterElementType::Decal;
    elements[1].payload_index = 0;
    elements[1].radius = 2.0F;
    elements[2] = elements[1];
    elements[2].payload_index = 1;
    elements[2].view_position = cy::Vec3{3.0F, 3.0F, -5.0F};
    cy::rendering::ClusterAssignment assignment(allocator());
    CY_REQUIRE(cy::rendering::assign_clusters(
                   *grid, cy::Span<const cy::rendering::ClusterElement>(elements, 3), ~0U,
                   std::tan(0.5F), 1.0F, assignment)
                   .has_value());

    DecalInstance decals[2] = {decal_at({0, 0, -5}, 1, 0), decal_at({3, 3, -5}, 2, 1)};
    const cy::u32 order[2] = {0, 1};
    DecalMaterial material;
    DecalTableInput input;
    input.decals = cy::Span<const DecalInstance>(decals, 2);
    input.order = cy::Span<const cy::u32>(order, 2);
    input.materials = cy::Span<const DecalMaterial>(&material, 1);
    input.clusters = &assignment;
    input.grid = *grid;
    input.width = 64;
    input.height = 64;
    cy::Array<cy::u32> words(allocator());
    CY_REQUIRE(cy::rendering::decals::pack_decal_table(input, words).has_value());

    CY_CHECK_EQ(words[5], 1U);
    CY_CHECK_EQ(words[8], grid->dimensions[0]);
    CY_CHECK_EQ(words[10], grid->dimensions[2]);
    CY_CHECK_EQ(as_float(words[12]), grid->slice_scale);
    CY_CHECK_EQ(words[16], 64U);
    // The identity view looks down -Z: depth is -z, so the row is (0, 0, -1, 0).
    CY_CHECK_EQ(as_float(words[20]), -1.0F);
    CY_CHECK_EQ(as_float(words[21]), 0.0F);
    const cy::u32 lists = words[6];
    cy::u32 total = 0;
    for (cy::u32 cluster = 0; cluster < grid->cluster_count(); ++cluster) {
        const cy::rendering::ClusterHeader& header =
            assignment.headers[(cluster * cy::rendering::kClusterElementTypeCount) + 1U];
        const cy::u32 first = words[lists + (cluster * 2U)];
        const cy::u32 count = words[lists + (cluster * 2U) + 1U];
        CY_CHECK_EQ(count, header.count);
        for (cy::u32 entry = 0; entry < count; ++entry) {
            CY_CHECK_EQ(words[first + entry], assignment.indices[header.offset + entry]);
            CY_CHECK_LT(words[first + entry], 2U);  // a rank, never the light's payload 4
        }
        total += count;
    }
    CY_CHECK_GT(total, 0U);
    CY_CHECK_EQ(words.size(), static_cast<cy::usize>(words[7] + total));
}

CY_TEST_CASE("an order that is not the decals' and a material past the table are refused") {
    DecalInstance decals[2] = {decal_at({0, 0, 0}, 1, 0), decal_at({1, 0, 0}, 2, 0)};
    DecalMaterial material;
    cy::Array<cy::u32> words(allocator());

    const cy::u32 short_order[1] = {0};
    DecalTableInput input;
    input.decals = cy::Span<const DecalInstance>(decals, 2);
    input.order = cy::Span<const cy::u32>(short_order, 1);
    input.materials = cy::Span<const DecalMaterial>(&material, 1);
    CY_CHECK_FALSE(cy::rendering::decals::pack_decal_table(input, words).has_value());

    const cy::u32 past[2] = {0, 5};
    input.order = cy::Span<const cy::u32>(past, 2);
    CY_CHECK_FALSE(cy::rendering::decals::pack_decal_table(input, words).has_value());

    // In range and the right length, and still not a permutation: decal 1 would be drawn twice
    // and decal 2 never.
    const cy::u32 twice[2] = {0, 0};
    input.order = cy::Span<const cy::u32>(twice, 2);
    CY_CHECK_FALSE(cy::rendering::decals::pack_decal_table(input, words).has_value());

    const cy::u32 order[2] = {0, 1};
    input.order = cy::Span<const cy::u32>(order, 2);
    decals[1].material_index = 1;
    CY_CHECK_FALSE(cy::rendering::decals::pack_decal_table(input, words).has_value());
    decals[1].material_index = 0;
    CY_CHECK(cy::rendering::decals::pack_decal_table(input, words).has_value());
}
