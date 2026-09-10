// The batching case the requirement states at its own scale. M8.b task 9.5.
//
// INTEGRATION, NOT UNIT, and deliberately so. `rendering-2d`'s scenario names FIVE THOUSAND
// sprites, and sorting and batching that many measured 1.01 ms on this machine — over the unit
// tier's one-millisecond budget by a hair. Hard rule 7 of the milestone's brief says a case that
// expensive belongs in the tier above rather than in the one whose budget it sits on the edge of,
// and shaving the case down to four thousand to fit would be testing a number the specification
// does not state.
//
// What the case is worth keeping at full size for: it is the only place the batcher is driven at
// the scale it was designed for, and the batch count it asserts — ONE — is the property the whole
// sort key exists to produce.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/2d/sprite.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::rendering2d;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

[[nodiscard]] Draw2D sprite_at(u16 layer, i16 order, f32 y, u32 tiebreak) noexcept {
    Draw2D draw;
    draw.sort.layer = layer;
    draw.sort.order = order;
    draw.sort.y_sort = y;
    draw.sort.tiebreak = tiebreak;
    draw.destination = Rect2D{0.0F, y, 16.0F, 16.0F};
    draw.source = Rect2D{0.0F, 0.0F, 16.0F, 16.0F};
    return draw;
}

}  // namespace

CY_TEST_CASE("sprite_batch_scale: five thousand sprites on one atlas are a handful of draws") {
    // "WHEN 5 000 sprites share an atlas and material THEN they SHALL be submitted as a small
    // number of instanced draws."
    Array<Draw2D> draws(allocator());
    // Reserved and filled without a per-item assertion: five thousand `CY_REQUIRE`s cost more than
    // the batcher does and would put this case in the tier above for the wrong reason.
    CY_REQUIRE(draws.resize(5000U).has_value());
    for (u32 index = 0; index < 5000U; ++index) {
        draws[index] = sprite_at(0, 0, static_cast<f32>(index % 400U), index & 0xFFFU);
    }
    Layer2D layer;
    layer.y_sorted = true;

    Array<u32> order(allocator());
    Array<Instance2D> instances(allocator());
    Array<Batch2D> batches(allocator());
    BatchReport report;
    CY_REQUIRE(build_batches(draws.span(), Span<const Layer2D>(&layer, 1), order, instances,
                             batches, report)
                   .has_value());
    CY_CHECK_EQ(report.draws, 5000U);
    CY_CHECK_EQ(report.instances, 5000U);
    // ONE DRAW. One pipeline, one material, one texture set, no scissor: nothing breaks the batch.
    CY_CHECK_EQ(report.batches, 1U);
}
