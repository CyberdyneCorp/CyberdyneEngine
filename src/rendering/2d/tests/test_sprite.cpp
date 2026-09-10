// The sort key, the batcher, and the primitive expansions. M8.b task 9.5.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/2d/sprite.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::rendering2d;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

[[nodiscard]] Draw2D sprite_at(u16 layer, i16 order, f32 y, u32 tiebreak, u16 material = 0,
                               u16 texture = 0) noexcept {
    Draw2D draw;
    draw.sort.layer = layer;
    draw.sort.order = order;
    draw.sort.y_sort = y;
    draw.sort.tiebreak = tiebreak;
    draw.material = material;
    draw.texture_set = texture;
    draw.destination = Rect2D{0.0F, y, 16.0F, 16.0F};
    draw.source = Rect2D{0.0F, 0.0F, 16.0F, 16.0F};
    return draw;
}

}  // namespace

CY_TEST_CASE(
    "sprite_sort: layer beats order, order beats Y, and Y only counts when the layer says") {
    // "2D draw order SHALL be determined by an explicit sort key composed of: layer, sort order
    // within the layer, an optional Y-sort value, and a stable tiebreak."
    const SortKey low_layer{0, 500, 1000.0F, 0};
    const SortKey high_layer{1, -500, 0.0F, 0};
    CY_CHECK_LT(low_layer.pack(true), high_layer.pack(true));

    const SortKey early{0, 0, 1000.0F, 0};
    const SortKey late{0, 5, 0.0F, 0};
    CY_CHECK_LT(early.pack(true), late.pack(true));

    // Y decides only within a layer that Y-sorts; without it the two are ordered by their tiebreak.
    const SortKey near_top{0, 0, 10.0F, 1};
    const SortKey near_bottom{0, 0, 500.0F, 2};
    CY_CHECK_LT(near_top.pack(true), near_bottom.pack(true));
    CY_CHECK_LT(near_top.pack(false), near_bottom.pack(false));
    // A negative Y sorts before a positive one, which a naive cast would get backwards.
    const SortKey above{0, 0, -50.0F, 0};
    CY_CHECK_LT(above.pack(true), near_top.pack(true));
}

CY_TEST_CASE("sprite_sort: a Y-sorted layer puts what is lower on screen in front") {
    // "WHEN a layer enables Y-sorting THEN entities lower on screen SHALL be drawn later, appearing
    // in front, updated as they move."
    Draw2D draws[3] = {sprite_at(0, 0, 300.0F, 1), sprite_at(0, 0, 100.0F, 2),
                       sprite_at(0, 0, 200.0F, 3)};
    Layer2D layer;
    layer.index = 0;
    layer.y_sorted = true;

    Array<u32> order(allocator());
    Array<Instance2D> instances(allocator());
    Array<Batch2D> batches(allocator());
    BatchReport report;
    CY_REQUIRE(build_batches(Span<const Draw2D>(draws, 3), Span<const Layer2D>(&layer, 1), order,
                             instances, batches, report)
                   .has_value());
    CY_REQUIRE_EQ(order.size(), 3U);
    CY_CHECK_EQ(draws[order[0]].sort.y_sort, 100.0F);
    CY_CHECK_EQ(draws[order[1]].sort.y_sort, 200.0F);
    CY_CHECK_EQ(draws[order[2]].sort.y_sort, 300.0F);

    // The same draws in a layer that does NOT Y-sort keep their submission order, by tiebreak.
    Draw2D unsorted[3] = {sprite_at(0, 0, 300.0F, 1), sprite_at(0, 0, 100.0F, 2),
                          sprite_at(0, 0, 200.0F, 3)};
    Layer2D plain;
    plain.index = 0;
    plain.y_sorted = false;
    CY_REQUIRE(build_batches(Span<const Draw2D>(unsorted, 3), Span<const Layer2D>(&plain, 1), order,
                             instances, batches, report)
                   .has_value());
    CY_CHECK_EQ(unsorted[order[0]].sort.tiebreak, 1U);
    CY_CHECK_EQ(unsorted[order[2]].sort.tiebreak, 3U);
}

CY_TEST_CASE("sprite_sort: identical keys keep a stable order between frames") {
    // "WHEN two sprites have identical sort keys THEN the tiebreak SHALL be stable across frames so
    // they do not flicker."
    Layer2D layer;
    Array<u32> order(allocator());
    Array<Instance2D> instances(allocator());
    Array<Batch2D> batches(allocator());
    BatchReport report;

    const Draw2D first_frame[2] = {sprite_at(0, 0, 0.0F, 7), sprite_at(0, 0, 0.0F, 4)};
    CY_REQUIRE(build_batches(Span<const Draw2D>(first_frame, 2), Span<const Layer2D>(&layer, 1),
                             order, instances, batches, report)
                   .has_value());
    const u32 first_order[2] = {order[0], order[1]};
    // The same two draws submitted in the OTHER order come out the same way round.
    const Draw2D second_frame[2] = {sprite_at(0, 0, 0.0F, 4), sprite_at(0, 0, 0.0F, 7)};
    CY_REQUIRE(build_batches(Span<const Draw2D>(second_frame, 2), Span<const Layer2D>(&layer, 1),
                             order, instances, batches, report)
                   .has_value());
    CY_CHECK_EQ(first_frame[first_order[0]].sort.tiebreak, second_frame[order[0]].sort.tiebreak);
    CY_CHECK_EQ(first_frame[first_order[1]].sort.tiebreak, second_frame[order[1]].sort.tiebreak);
}

CY_TEST_CASE("sprite_batch: each break reason is reported, and the dominant one is nameable") {
    // "WHEN a 2D scene has an unexpectedly high draw count THEN the diagnostic SHALL report the
    // dominant break reason."
    Draw2D draws[6];
    draws[0] = sprite_at(0, 0, 0.0F, 1, 0, 0);
    draws[1] = sprite_at(0, 1, 0.0F, 2, 1, 0);  // material change
    draws[2] = sprite_at(0, 2, 0.0F, 3, 1, 1);  // texture set change
    draws[3] = sprite_at(0, 3, 0.0F, 4, 2, 1);  // material change
    draws[4] = sprite_at(0, 4, 0.0F, 5, 3, 1);  // material change
    draws[5] = sprite_at(0, 5, 0.0F, 6, 3, 1);
    draws[5].scissored = true;
    draws[5].scissor = Rect2D{0.0F, 0.0F, 10.0F, 10.0F};  // scissor change

    Layer2D layer;
    Array<u32> order(allocator());
    Array<Instance2D> instances(allocator());
    Array<Batch2D> batches(allocator());
    BatchReport report;
    CY_REQUIRE(build_batches(Span<const Draw2D>(draws, 6), Span<const Layer2D>(&layer, 1), order,
                             instances, batches, report)
                   .has_value());
    CY_CHECK_EQ(report.batches, 6U);
    CY_CHECK_EQ(report.breaks[static_cast<usize>(BreakReason::Material)], 3U);
    CY_CHECK_EQ(report.breaks[static_cast<usize>(BreakReason::TextureSet)], 1U);
    CY_CHECK_EQ(report.breaks[static_cast<usize>(BreakReason::Scissor)], 1U);
    CY_CHECK_EQ(report.dominant_break(), BreakReason::Material);
}

CY_TEST_CASE("sprite_batch: a render target change is the break that is named first") {
    // Two things changed at once; the report names the expensive one, which is the one a developer
    // can do something about.
    Draw2D draws[2];
    draws[0] = sprite_at(0, 0, 0.0F, 1, 0, 0);
    draws[1] = sprite_at(0, 1, 0.0F, 2, 5, 0);
    draws[1].target = 3;

    Layer2D layer;
    Array<u32> order(allocator());
    Array<Instance2D> instances(allocator());
    Array<Batch2D> batches(allocator());
    BatchReport report;
    CY_REQUIRE(build_batches(Span<const Draw2D>(draws, 2), Span<const Layer2D>(&layer, 1), order,
                             instances, batches, report)
                   .has_value());
    CY_CHECK_EQ(report.breaks[static_cast<usize>(BreakReason::RenderTarget)], 1U);
    CY_CHECK_EQ(report.breaks[static_cast<usize>(BreakReason::Material)], 0U);
}

CY_TEST_CASE("sprite_batch: a flip becomes a negative source rectangle, not a shader branch") {
    Draw2D draw = sprite_at(0, 0, 0.0F, 1);
    draw.source = Rect2D{10.0F, 20.0F, 16.0F, 16.0F};
    draw.flip_x = true;

    Layer2D layer;
    Array<u32> order(allocator());
    Array<Instance2D> instances(allocator());
    Array<Batch2D> batches(allocator());
    BatchReport report;
    CY_REQUIRE(build_batches(Span<const Draw2D>(&draw, 1), Span<const Layer2D>(&layer, 1), order,
                             instances, batches, report)
                   .has_value());
    CY_REQUIRE_EQ(instances.size(), 1U);
    CY_CHECK_EQ(instances[0].source.x, 26.0F);
    CY_CHECK_EQ(instances[0].source.width, -16.0F);
}

CY_TEST_CASE("sprite_nine_slice: nine pieces, corners unstretched") {
    Draw2D draw = sprite_at(0, 0, 0.0F, 1);
    draw.kind = PrimitiveKind::NineSlice;
    draw.destination = Rect2D{0.0F, 0.0F, 200.0F, 100.0F};
    draw.source = Rect2D{0.0F, 0.0F, 48.0F, 48.0F};

    Array<Draw2D> pieces(allocator());
    CY_REQUIRE(expand_nine_slice(draw, Rect2D{8.0F, 8.0F, 8.0F, 8.0F}, pieces).has_value());
    CY_REQUIRE_EQ(pieces.size(), 9U);
    // The top-left corner keeps the border's size in both spaces: a corner that stretched is the
    // defect a nine-slice exists to prevent.
    CY_CHECK_EQ(pieces[0].destination.width, 8.0F);
    CY_CHECK_EQ(pieces[0].source.width, 8.0F);
    // The centre absorbed the difference.
    CY_CHECK_EQ(pieces[4].destination.width, 184.0F);
    CY_CHECK_EQ(pieces[4].source.width, 32.0F);
    // And every piece is an ordinary sprite, so it batches with what is around it.
    CY_CHECK_EQ(pieces[4].kind, PrimitiveKind::Sprite);
}

CY_TEST_CASE("sprite_line: a polyline expands to a strip, and the width curve is by distance") {
    const Vec2 points[3] = {Vec2{0.0F, 0.0F}, Vec2{10.0F, 0.0F}, Vec2{20.0F, 0.0F}};
    Array<Vec2> strip(allocator());
    CY_REQUIRE(expand_line(Span<const Vec2>(points, 3), 4.0F, LineJoint::Bevel, LineCap::None,
                           nullptr, nullptr, strip)
                   .has_value());
    CY_REQUIRE_EQ(strip.size(), 6U);
    // Two units either side of a horizontal line.
    CY_CHECK_NEAR(strip[0].y, 2.0F, 1e-4F);
    CY_CHECK_NEAR(strip[1].y, -2.0F, 1e-4F);

    // A taper: the width follows the distance along the line, so an uneven polyline still tapers
    // evenly.
    const auto taper = [](f32 t, void*) noexcept { return 1.0F - t; };
    Array<Vec2> tapered(allocator());
    CY_REQUIRE(expand_line(Span<const Vec2>(points, 3), 4.0F, LineJoint::Bevel, LineCap::None,
                           taper, nullptr, tapered)
                   .has_value());
    CY_CHECK_NEAR(tapered[0].y, 2.0F, 1e-4F);
    CY_CHECK_NEAR(tapered[4].y, 0.0F, 1e-4F);

    // A line of one point is refused rather than producing a degenerate strip.
    CY_CHECK_FALSE(expand_line(Span<const Vec2>(points, 1), 4.0F, LineJoint::Bevel, LineCap::None,
                               nullptr, nullptr, strip)
                       .has_value());
}

CY_TEST_CASE("sprite_animation: frames advance by their own durations and loop") {
    const f32 durations[3] = {0.1F, 0.2F, 0.1F};
    bool looped = false;
    CY_CHECK_EQ(animation_frame(Span<const f32>(durations, 3), 0.05F, true, looped), 0U);
    CY_CHECK_EQ(animation_frame(Span<const f32>(durations, 3), 0.15F, true, looped), 1U);
    CY_CHECK_EQ(animation_frame(Span<const f32>(durations, 3), 0.35F, true, looped), 2U);
    CY_CHECK_FALSE(looped);

    // Past the end: it wraps, and says so.
    CY_CHECK_EQ(animation_frame(Span<const f32>(durations, 3), 0.45F, true, looped), 0U);
    CY_CHECK(looped);

    // Not looping: it holds on the last frame rather than wrapping or going out of range.
    CY_CHECK_EQ(animation_frame(Span<const f32>(durations, 3), 10.0F, false, looped), 2U);
}
