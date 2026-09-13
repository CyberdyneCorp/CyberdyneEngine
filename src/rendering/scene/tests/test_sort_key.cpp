// Deterministic submission order. Task 4.1.5, design.md §6.
//
// The case that matters is the last one, and it is the reason this file exists at M3 rather than at
// M9: the same draws, published in a different order, must sort to the same command stream. At M3
// that reads as pedantry. At M9 it is the difference between a golden image that reproduces and one
// that is flaky for reasons nobody can find.

#include <cy/test/test.h>

#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/scene/sort_key.h>

namespace {

using cy::rendering::DrawItem;
using cy::rendering::DrawSortInput;
using cy::rendering::DrawSortKey;
using cy::rendering::make_sort_key;
using cy::rendering::sort_draws;
using cy::rendering::SortLayer;

DrawItem draw(SortLayer layer, cy::u64 program, cy::u64 mesh, cy::u64 identity, cy::f32 depth,
              cy::u32 slot) noexcept {
    DrawSortInput input;
    input.layer = layer;
    input.program_identity = program;
    input.mesh_identity = mesh;
    input.instance_identity = identity;
    input.depth = depth;
    return DrawItem{make_sort_key(input), slot, 0};
}

}  // namespace

CY_TEST_CASE("sort key: layers never interleave") {
    const DrawItem prepass = draw(SortLayer::DepthPrepass, 1, 1, 1, 0.1f, 0);
    const DrawItem opaque = draw(SortLayer::Opaque, 9999, 9999, 2, 0.9f, 1);
    const DrawItem masked = draw(SortLayer::Masked, 1, 1, 3, 0.9f, 2);
    const DrawItem transparent = draw(SortLayer::Transparent, 1, 1, 4, 0.9f, 3);
    const DrawItem overlay = draw(SortLayer::Overlay, 1, 1, 5, 0.9f, 4);

    CY_CHECK(prepass.key < opaque.key);
    CY_CHECK(opaque.key < masked.key);
    CY_CHECK(masked.key < transparent.key);
    CY_CHECK(transparent.key < overlay.key);
}

CY_TEST_CASE("sort key: opaque draws sort near to far") {
    // Reversed Z: the near plane is depth 1. Front to back is therefore descending depth, which is
    // exactly the sign that is invisible when it is wrong — early-Z hides it and the frame is
    // merely slower.
    const DrawItem near_draw = draw(SortLayer::Opaque, 7, 7, 1, 0.9f, 0);
    const DrawItem far_draw = draw(SortLayer::Opaque, 7, 7, 2, 0.1f, 1);
    CY_CHECK(near_draw.key < far_draw.key);
}

CY_TEST_CASE("sort key: transparent draws sort far to near, and depth outranks state") {
    const DrawItem far_draw = draw(SortLayer::Transparent, 7, 7, 1, 0.1f, 0);
    const DrawItem near_draw = draw(SortLayer::Transparent, 7, 7, 2, 0.9f, 1);
    CY_CHECK(far_draw.key < near_draw.key);

    // Two different programs, the far one drawn first regardless: back-to-front is a correctness
    // requirement and batching is not.
    const DrawItem far_other_program = draw(SortLayer::Transparent, 123456, 7, 3, 0.1f, 2);
    const DrawItem near_same_program = draw(SortLayer::Transparent, 7, 7, 4, 0.9f, 3);
    CY_CHECK(far_other_program.key < near_same_program.key);
}

CY_TEST_CASE("sort key: opaque draws group by program before depth") {
    cy::FixedArray<DrawItem, 4> draws;
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 100, 1, 1, 0.9f, 0)).has_value());
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 200, 1, 2, 0.8f, 1)).has_value());
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 100, 1, 3, 0.7f, 2)).has_value());
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 200, 1, 4, 0.6f, 3)).has_value());
    sort_draws(draws.span());

    // The two draws sharing a program are adjacent — the "Instance shares a pipeline" scenario —
    // and within the pair the nearer one comes first.
    const cy::u64 first_program = draws[0].key.primary >> 45U;
    CY_CHECK_EQ(draws[1].key.primary >> 45U, first_program);
    CY_CHECK_NE(draws[2].key.primary >> 45U, first_program);
    CY_CHECK_EQ(draws[3].key.primary >> 45U, draws[2].key.primary >> 45U);
}

CY_TEST_CASE("sort key: a surface index saturates rather than wrapping into the depth field") {
    DrawSortInput input;
    input.layer = SortLayer::Opaque;
    input.surface = 4096;
    const DrawSortKey saturated = make_sort_key(input);

    input.surface = 31;
    const DrawSortKey maximum = make_sort_key(input);
    CY_CHECK_EQ(saturated.primary, maximum.primary);
}

CY_TEST_CASE("sort key: publication order does not reach the ordering") {
    // The whole requirement, as a measurement. Two thousand draws, built in two different orders
    // with two different slot assignments, must produce the same sorted key sequence.
    constexpr cy::u32 kCount = 1000;
    cy::Array<DrawItem> forward(cy::system_allocator(cy::MemoryDomain::Renderer));
    cy::Array<DrawItem> backward(cy::system_allocator(cy::MemoryDomain::Renderer));
    CY_REQUIRE(forward.reserve(kCount).has_value());
    CY_REQUIRE(backward.reserve(kCount).has_value());

    for (cy::u32 i = 0; i < kCount; ++i) {
        const cy::u64 identity = 0x1000 + i;
        const cy::f32 depth = static_cast<cy::f32>(i % 97) / 97.0f;
        CY_REQUIRE(forward.push_back(draw(SortLayer::Opaque, i % 13, i % 7, identity, depth, i))
                       .has_value());
    }
    for (cy::u32 i = kCount; i > 0; --i) {
        const cy::u32 index = i - 1;
        const cy::u64 identity = 0x1000 + index;
        const cy::f32 depth = static_cast<cy::f32>(index % 97) / 97.0f;
        // A different slot for the same content: this is what a level streamed in a different
        // order, or an entity destroyed in a previous session, actually produces.
        CY_REQUIRE(backward
                       .push_back(draw(SortLayer::Opaque, index % 13, index % 7, identity, depth,
                                       kCount - index))
                       .has_value());
    }

    sort_draws(forward.span());
    sort_draws(backward.span());

    CY_REQUIRE_EQ(forward.size(), backward.size());
    for (cy::usize i = 0; i < forward.size(); ++i) {
        CY_CHECK(forward[i].key == backward[i].key);
    }
    CY_CHECK(cy::rendering::verify_total_order(forward.span()));
}

CY_TEST_CASE("sort key: the process hash seed does not reach the ordering") {
    // `cy::hash_seed()` is randomised per process in development builds on purpose. A sort id built
    // from it would reorder draws between two runs of the same frame — the defect this file exists
    // to prevent, introduced by the mechanism meant to catch it.
    const cy::u64 seed_before = cy::hash_seed();
    const DrawSortKey first = make_sort_key(DrawSortInput{SortLayer::Opaque, 42, 43, 44, 2, 0.5f});
    cy::set_hash_seed(seed_before ^ 0xDEADBEEFULL);
    const DrawSortKey second = make_sort_key(DrawSortInput{SortLayer::Opaque, 42, 43, 44, 2, 0.5f});
    cy::set_hash_seed(seed_before);

    CY_CHECK(first == second);
}

CY_TEST_CASE("sort key: two draws claiming one identity are reported, not silently ordered") {
    cy::FixedArray<DrawItem, 2> draws;
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 1, 1, 5, 0.5f, 0)).has_value());
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 1, 1, 5, 0.5f, 1)).has_value());
    sort_draws(draws.span());

    CY_CHECK_FALSE(cy::rendering::verify_total_order(draws.span()));
    CY_CHECK_EQ(cy::rendering::first_duplicate_key(draws.span()), 1U);
}
