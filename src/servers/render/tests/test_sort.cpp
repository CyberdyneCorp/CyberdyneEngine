// Deterministic submission order. Task 4.1.5, design.md §6.
//
// The requirement is "WHEN the same snapshot is rendered twice THEN the recorded command stream
// SHALL be identical". These cases check the three properties that make that true and that would
// each, on their own, be enough to break it:
//
//   * the key's fields are ordered by cost and the layer is the most significant
//   * transparency sorts back to front and opacity front to back
//   * the ordering is TOTAL, so the sorted sequence does not depend on the input order

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/mesh.h>
#include <cy/servers/render/sort.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::render;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

[[nodiscard]] DrawItem draw(SortLayer layer, u32 pipeline, u32 material, u32 mesh, f32 depth,
                            u64 stable_id) noexcept {
    DrawItem item;
    item.key = make_sort_key(DrawKeyInputs{layer, pipeline, material, mesh, depth});
    item.stable_id = stable_id;
    return item;
}

}  // namespace

CY_TEST_CASE("the layer is the most significant field, so buckets never interleave") {
    const u64 background =
        make_sort_key(DrawKeyInputs{SortLayer::Background, 0xFFFF, 0xFFFF, 0xFFFF, 10000.0F});
    const u64 opaque = make_sort_key(DrawKeyInputs{SortLayer::Opaque, 0, 0, 0, 0.0F});
    const u64 overlay = make_sort_key(DrawKeyInputs{SortLayer::Overlay, 0, 0, 0, 0.0F});
    // Whatever a background draw's pipeline, material, mesh and depth are, it precedes every opaque
    // draw. That is what makes the sky a bucket rather than a special case in the submitter.
    CY_CHECK_LT(background, opaque);
    CY_CHECK_LT(opaque, overlay);
    CY_CHECK_EQ(sort_key_layer(background), SortLayer::Background);
    CY_CHECK_EQ(sort_key_layer(overlay), SortLayer::Overlay);
}

CY_TEST_CASE("the key's fields read back, which is what automatic instancing merges on") {
    const u64 key = make_sort_key(DrawKeyInputs{SortLayer::Opaque, 1234, 5678, 9012, 4.0F});
    CY_CHECK_EQ(sort_key_pipeline(key), 1234U);
    CY_CHECK_EQ(sort_key_material(key), 5678U);
    CY_CHECK_EQ(sort_key_mesh(key), 9012U);
}

CY_TEST_CASE("opaque draws sort by state first and depth last") {
    // The trade design.md §6 and this file's header record: after a depth prepass, front-to-back
    // ordering buys nothing the prepass has not already bought, so state sorting wins.
    const u64 near_second_pipeline = make_sort_key(DrawKeyInputs{SortLayer::Opaque, 2, 0, 0, 1.0F});
    const u64 far_first_pipeline = make_sort_key(DrawKeyInputs{SortLayer::Opaque, 1, 0, 0, 900.0F});
    CY_CHECK_LT(far_first_pipeline, near_second_pipeline);

    // Within one pipeline and material, nearer draws first.
    const u64 near = make_sort_key(DrawKeyInputs{SortLayer::Opaque, 1, 1, 1, 1.0F});
    const u64 far = make_sort_key(DrawKeyInputs{SortLayer::Opaque, 1, 1, 1, 900.0F});
    CY_CHECK_LT(near, far);
}

CY_TEST_CASE(
    "transparent draws sort back to front, and the direction is not the caller's to choose") {
    // Blending is not commutative. The direction is decided by the layer, so a caller cannot pass a
    // flag the wrong way round — there is no flag.
    const u64 near = make_sort_key(DrawKeyInputs{SortLayer::Transparent, 1, 1, 1, 1.0F});
    const u64 far = make_sort_key(DrawKeyInputs{SortLayer::Transparent, 1, 1, 1, 900.0F});
    CY_CHECK_LT(far, near);
}

CY_TEST_CASE("depth quantisation is monotone and needs no far plane") {
    // The engine's default far plane is infinite, so a quantisation that divided by one would have
    // nothing to divide by. The IEEE bit trick is monotone over every non-negative float.
    CY_CHECK_LE(quantise_depth(0.0F), quantise_depth(0.001F));
    CY_CHECK_LE(quantise_depth(0.001F), quantise_depth(1.0F));
    CY_CHECK_LE(quantise_depth(1.0F), quantise_depth(1000.0F));
    CY_CHECK_LT(quantise_depth(1.0F), quantise_depth(1.0e30F));
    // Behind the camera and NaN both sort to the front rather than to an arbitrary bucket.
    CY_CHECK_EQ(quantise_depth(-5.0F), 0U);
}

CY_TEST_CASE("the sorted order does not depend on the order the draws were published in") {
    // THE HEADLINE. Two publication orders, one sorted sequence — which is the whole of "the same
    // snapshot rendered twice produces the same command stream", because publication order is the
    // only thing that differs between two runs of a correct renderer.
    cy::Array<DrawItem> forward(allocator());
    cy::Array<DrawItem> backward(allocator());
    for (u32 index = 0; index < 16; ++index) {
        // Deliberately colliding keys: eight draws share a key and are separated only by their
        // stable ids, which is the case an unstable sort would get wrong.
        const DrawItem item = draw(SortLayer::Opaque, index % 2U, 7, 9, 5.0F, 1000U + index);
        CY_REQUIRE(forward.push_back(item).has_value());
    }
    for (cy::usize index = forward.size(); index > 0; --index) {
        CY_REQUIRE(backward.push_back(forward[index - 1]).has_value());
    }

    sort_draws(forward.span());
    sort_draws(backward.span());

    CY_REQUIRE_EQ(forward.size(), backward.size());
    for (cy::usize index = 0; index < forward.size(); ++index) {
        CY_CHECK_EQ(forward[index].key, backward[index].key);
        CY_CHECK_EQ(forward[index].stable_id, backward[index].stable_id);
    }
    CY_CHECK(draws_are_ordered(forward.span()));
    CY_CHECK_EQ(submission_fingerprint(forward.span()), submission_fingerprint(backward.span()));
}

CY_TEST_CASE("the fingerprint changes when the order does") {
    // A number that did not notice a reordering would be a number a golden test could not use.
    cy::Array<DrawItem> draws(allocator());
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 1, 1, 1, 1.0F, 10)).has_value());
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 1, 1, 1, 1.0F, 20)).has_value());
    const u64 before = submission_fingerprint(draws.span());

    const DrawItem first = draws[0];
    draws[0] = draws[1];
    draws[1] = first;
    CY_CHECK_NE(submission_fingerprint(draws.span()), before);
}

CY_TEST_CASE("adjacent draws sharing pipeline, material and mesh merge into one instanced batch") {
    // `rendering-geometry-and-resources`: "WHEN 500 entities share one mesh and material THEN
    // submission SHALL merge them into instanced draws without the content author doing anything."
    // Five hundred at unit scale is twenty; what is asserted is that the merge is a linear scan
    // over a sorted list and produces one batch.
    cy::Array<DrawItem> draws(allocator());
    for (u32 index = 0; index < 20; ++index) {
        CY_REQUIRE(
            draws.push_back(draw(SortLayer::Opaque, 3, 4, 5, 2.0F, 100U + index)).has_value());
    }
    // One odd draw out, with a different mesh: it must not join the batch.
    CY_REQUIRE(draws.push_back(draw(SortLayer::Opaque, 3, 4, 6, 2.0F, 999)).has_value());
    sort_draws(draws.span());

    cy::Array<InstancedBatch> batches(allocator());
    const auto count = build_instanced_batches(draws.span(), batches);
    CY_REQUIRE(count.has_value());
    CY_CHECK_EQ(*count, 2U);
    CY_CHECK_EQ(batches[0].draw_count, 20U);
    CY_CHECK_EQ(batches[0].mesh, 5U);
    CY_CHECK_EQ(batches[1].draw_count, 1U);
    CY_CHECK_EQ(batches[1].mesh, 6U);
}

CY_TEST_CASE("an empty draw list produces no batches and no error") {
    cy::Array<InstancedBatch> batches(allocator());
    const auto count = build_instanced_batches(cy::Span<const DrawItem>(), batches);
    CY_REQUIRE(count.has_value());
    CY_CHECK_EQ(*count, 0U);
    CY_CHECK(batches.empty());
}
