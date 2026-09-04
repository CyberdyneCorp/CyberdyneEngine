// The draw list, the radix sort, and the per-draw instance record. Task 4.3.2.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/forward/diagnostics.h>
#include <cy/rendering/forward/draw_list.h>

namespace {

using cy::rendering::DrawList;
using cy::rendering::DrawSortScratch;
using cy::rendering::DrawSurface;
using cy::rendering::VisibleInstance;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// One surface per instance, taken from a table the caller owns — which is the shape the real
/// callback has, with the mesh table standing in for this array.
struct SurfaceTable {
    DrawSurface surfaces[64];
};

cy::Span<const DrawSurface> surface_of(const VisibleInstance& instance, void* user) noexcept {
    auto* table = static_cast<SurfaceTable*>(user);
    return {&table->surfaces[instance.slot], 1};
}

VisibleInstance make_visible(cy::u32 slot, cy::u64 stable_id, cy::f32 depth) noexcept {
    VisibleInstance instance;
    instance.slot = slot;
    instance.gpu_slot = slot + 100;
    instance.stable_id = stable_id;
    instance.view_depth = depth;
    return instance;
}

}  // namespace

CY_TEST_CASE("the layer comes from the blend mode, never from the caller") {
    // A material never chooses its own bucket: two materials with the same blending cannot end up
    // in different ones, which is what `render::sort_layer_for` is for.
    SurfaceTable table;
    table.surfaces[0].blend = cy::render::BlendMode::Opaque;
    table.surfaces[1].blend = cy::render::BlendMode::Translucent;
    table.surfaces[2].blend = cy::render::BlendMode::Masked;

    VisibleInstance visible[3] = {make_visible(0, 1, 5.0F), make_visible(1, 2, 5.0F),
                                  make_visible(2, 3, 5.0F)};
    DrawList list(allocator());
    CY_REQUIRE(cy::rendering::build_draw_list(cy::Span<const VisibleInstance>(visible, 3),
                                              &surface_of, &table, list)
                   .has_value());
    CY_REQUIRE_EQ(list.items.size(), 3U);
    CY_CHECK_EQ(cy::render::sort_key_layer(list.items[0].key), cy::render::SortLayer::Opaque);
    CY_CHECK_EQ(cy::render::sort_key_layer(list.items[1].key), cy::render::SortLayer::Transparent);
    CY_CHECK_EQ(cy::render::sort_key_layer(list.items[2].key), cy::render::SortLayer::Masked);
}

CY_TEST_CASE("the radix sort produces exactly the order the render server's reference does") {
    // THE ASSERTION THE FAST PATH EXISTS FOR. `render::sort_draws` is a comparison sort over the
    // total order `(key, stable_id, surface)` and is obviously correct; this one is linear. Sorting
    // one list both ways and comparing is a stronger statement than either alone.
    SurfaceTable table;
    for (cy::u32 index = 0; index < 64; ++index) {
        table.surfaces[index].pipeline = (index * 7U) % 5U;
        table.surfaces[index].material = (index * 13U) % 7U;
        table.surfaces[index].mesh = index % 3U;
        table.surfaces[index].surface = index % 2U;
        table.surfaces[index].blend =
            (index % 4U == 0U) ? cy::render::BlendMode::Translucent : cy::render::BlendMode::Opaque;
    }
    VisibleInstance visible[64];
    for (cy::u32 index = 0; index < 64; ++index) {
        // Depths that repeat, so keys collide and the tie-break is exercised rather than avoided.
        visible[index] = make_visible(index, 1000U - index, static_cast<cy::f32>(index % 8U));
    }

    DrawList list(allocator());
    CY_REQUIRE(cy::rendering::build_draw_list(cy::Span<const VisibleInstance>(visible, 64),
                                              &surface_of, &table, list)
                   .has_value());
    cy::Array<cy::render::DrawItem> reference(allocator());
    CY_REQUIRE(reference.append(list.items.span()).has_value());
    cy::render::sort_draws(reference.span());

    DrawSortScratch scratch(allocator());
    CY_REQUIRE(cy::rendering::radix_sort_draws(list.items.span(), scratch).has_value());

    CY_REQUIRE_EQ(list.items.size(), reference.size());
    for (cy::usize index = 0; index < reference.size(); ++index) {
        CY_CHECK_EQ(list.items[index].key, reference[index].key);
        CY_CHECK_EQ(list.items[index].stable_id, reference[index].stable_id);
        CY_CHECK_EQ(list.items[index].surface, reference[index].surface);
    }
    CY_CHECK(cy::render::draws_are_ordered(list.items.span()));
    // And the two orders have the same fingerprint, which is the number a golden test records.
    CY_CHECK_EQ(cy::render::submission_fingerprint(list.items.span()),
                cy::render::submission_fingerprint(reference.span()));
}

CY_TEST_CASE("the sort does not depend on the order the cull produced") {
    // design.md §6: a frame's order must come from the sort key and the stable id, never from
    // publication order. Two shuffles of one visible set must sort to the same list.
    SurfaceTable table;
    for (cy::u32 index = 0; index < 32; ++index) {
        table.surfaces[index].pipeline = index % 3U;
        table.surfaces[index].material = index % 4U;
    }
    VisibleInstance forward[32];
    VisibleInstance backward[32];
    for (cy::u32 index = 0; index < 32; ++index) {
        forward[index] = make_visible(index, index + 1U, static_cast<cy::f32>(index % 5U));
        backward[31U - index] = forward[index];
    }

    DrawList first(allocator());
    DrawList second(allocator());
    DrawSortScratch scratch(allocator());
    CY_REQUIRE(cy::rendering::build_draw_list(cy::Span<const VisibleInstance>(forward, 32),
                                              &surface_of, &table, first)
                   .has_value());
    CY_REQUIRE(cy::rendering::build_draw_list(cy::Span<const VisibleInstance>(backward, 32),
                                              &surface_of, &table, second)
                   .has_value());
    CY_REQUIRE(cy::rendering::sort_draw_list(first, scratch).has_value());
    CY_REQUIRE(cy::rendering::sort_draw_list(second, scratch).has_value());

    CY_CHECK_EQ(cy::render::submission_fingerprint(first.items.span()),
                cy::render::submission_fingerprint(second.items.span()));
    // The per-draw records moved with the items, so draw i's `first_instance` is still i.
    for (cy::usize index = 0; index < first.items.size(); ++index) {
        CY_CHECK_EQ(first.instances[index].instance_slot, first.items[index].instance_slot);
        CY_CHECK_EQ(second.instances[index].instance_slot, second.items[index].instance_slot);
    }
}

CY_TEST_CASE("sorting groups the draws it promises to group") {
    // "WHEN the opaque list is sorted and submitted THEN all draws sharing a pipeline SHALL be
    // adjacent, and within them all sharing a material." Measured exactly: the number of
    // transitions along the order equals the number of distinct values if and only if every group
    // is contiguous.
    SurfaceTable table;
    for (cy::u32 index = 0; index < 48; ++index) {
        table.surfaces[index].pipeline = index % 4U;
        table.surfaces[index].material = index % 6U;
        table.surfaces[index].mesh = index % 3U;
    }
    VisibleInstance visible[48];
    for (cy::u32 index = 0; index < 48; ++index) {
        visible[index] = make_visible(index, index + 1U, static_cast<cy::f32>(index));
    }

    DrawList list(allocator());
    DrawSortScratch scratch(allocator());
    CY_REQUIRE(cy::rendering::build_draw_list(cy::Span<const VisibleInstance>(visible, 48),
                                              &surface_of, &table, list)
                   .has_value());

    const cy::rendering::SortKeyDistribution unsorted =
        cy::rendering::analyse_sort_keys(list.items.span());
    CY_CHECK_FALSE(unsorted.grouped());

    CY_REQUIRE(cy::rendering::sort_draw_list(list, scratch).has_value());
    const cy::rendering::SortKeyDistribution sorted =
        cy::rendering::analyse_sort_keys(list.items.span());
    CY_CHECK(sorted.grouped());
    CY_CHECK_EQ(sorted.draws, 48U);
    // On a grouped list the run count IS the distinct count: four pipelines, four runs.
    CY_CHECK_EQ(sorted.pipeline_runs, 4U);
    CY_CHECK_LT(sorted.pipeline_runs, unsorted.pipeline_runs);
    CY_CHECK_EQ(sorted.layer_draws[static_cast<cy::u32>(cy::render::SortLayer::Opaque)], 48U);
}

CY_TEST_CASE("transparent draws come back to front and opaque ones front to back") {
    SurfaceTable table;
    for (cy::u32 index = 0; index < 8; ++index) {
        table.surfaces[index].blend =
            index < 4 ? cy::render::BlendMode::Opaque : cy::render::BlendMode::Translucent;
    }
    VisibleInstance visible[8];
    for (cy::u32 index = 0; index < 8; ++index) {
        // Depths 1, 2, 3, 4 in each group.
        visible[index] = make_visible(index, index + 1U, static_cast<cy::f32>((index % 4U) + 1U));
    }

    DrawList list(allocator());
    DrawSortScratch scratch(allocator());
    CY_REQUIRE(cy::rendering::build_draw_list(cy::Span<const VisibleInstance>(visible, 8),
                                              &surface_of, &table, list)
                   .has_value());
    CY_REQUIRE(cy::rendering::sort_draw_list(list, scratch).has_value());

    // Opaque first (the layer is the key's most significant field), then transparent.
    CY_CHECK_EQ(cy::render::sort_key_layer(list.items[0].key), cy::render::SortLayer::Opaque);
    CY_CHECK_EQ(cy::render::sort_key_layer(list.items[7].key), cy::render::SortLayer::Transparent);
    // Within the transparent group, back to front: the furthest is drawn first. Blending is not
    // commutative, so this one is correctness rather than performance.
    CY_CHECK_EQ(list.instances[4].instance_slot, list.items[4].instance_slot);
    CY_CHECK_GT(list.items[4].key, 0U);
}

CY_TEST_CASE("the LOD level and its fade survive the round trip through one word") {
    for (cy::u32 level = 0; level < 8; ++level) {
        const cy::u32 packed = cy::rendering::pack_lod_and_fade(level, 0.5F);
        CY_CHECK_EQ(cy::rendering::unpack_lod(packed), level);
        CY_CHECK_NEAR(cy::rendering::unpack_fade(packed), 0.5F, 1.0F / 255.0F);
    }
    CY_CHECK_EQ(cy::rendering::unpack_fade(cy::rendering::pack_lod_and_fade(0, 1.0F)), 1.0F);
    CY_CHECK_EQ(cy::rendering::unpack_fade(cy::rendering::pack_lod_and_fade(0, 0.0F)), 0.0F);
}

CY_TEST_CASE("an empty list sorts without touching anything") {
    DrawList list(allocator());
    DrawSortScratch scratch(allocator());
    CY_CHECK(cy::rendering::sort_draw_list(list, scratch).has_value());
    CY_CHECK_EQ(cy::rendering::analyse_sort_keys(list.items.span()).draws, 0U);
}
