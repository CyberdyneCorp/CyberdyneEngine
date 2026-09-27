// SPDX-License-Identifier: MIT
// `pipeline::InstanceHistory`: the previous rows per-object motion is derived from, on the host.
// Part of `unit.rendering_motion_blur`, because motion blur is the consumer that made them worth
// keeping — the rows are the prepass's input and the blur is what reads its output.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/pipeline/instance_history.h>
#include <cy/test/test.h>

#include <cstring>
#include <vector>

using namespace cy;
using namespace cy::rendering::pipeline;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

[[nodiscard]] InstanceTransform at(f32 x, f32 y, f32 z) noexcept {
    InstanceTransform row;
    row.row0[3] = x;
    row.row1[3] = y;
    row.row2[3] = z;
    return row;
}

[[nodiscard]] bool same(const InstanceTransform& a, const InstanceTransform& b) noexcept {
    return std::memcmp(&a, &b, sizeof(InstanceTransform)) == 0;
}

struct Step {
    std::vector<InstanceTransform> previous;
    InstanceHistoryReport report;
};

[[nodiscard]] Step advance(InstanceHistory& history, const std::vector<InstanceTransform>& rows,
                           const std::vector<u64>& ids = {}, Vec3 camera_motion = {},
                           bool cut = false) {
    Step step;
    step.previous.resize(rows.size());
    CY_REQUIRE(history
                   .advance(Span<const InstanceTransform>(rows.data(), rows.size()),
                            Span<const u64>(ids.data(), ids.size()), camera_motion, cut,
                            Span<InstanceTransform>(step.previous.data(), step.previous.size()))
                   .has_value());
    step.report = history.report();
    return step;
}

}  // namespace

CY_TEST_CASE("an instance's previous row is where it was last frame, and a new one has none") {
    InstanceHistory history(allocator());
    const std::vector<InstanceTransform> first = {at(0.0F, 0.0F, -5.0F), at(2.0F, 0.0F, -6.0F)};
    const Step opening = advance(history, first);
    // Nothing was seen before the first frame: every previous row IS the current one.
    CY_CHECK_EQ(opening.report.new_rows, 2U);
    CY_CHECK_EQ(opening.report.moved, 0U);
    CY_CHECK(same(opening.previous[0], first[0]));
    CY_CHECK(same(opening.previous[1], first[1]));

    const std::vector<InstanceTransform> second = {at(0.5F, 0.0F, -5.0F), at(2.0F, 0.0F, -6.0F)};
    const Step moving = advance(history, second);
    CY_CHECK_EQ(moving.report.new_rows, 0U);
    CY_CHECK_EQ(moving.report.moved, 1U);
    CY_CHECK(same(moving.previous[0], first[0]));
    // The still one's previous row is its current one, bit for bit: that is what keeps a still
    // frame's motion vectors exactly what they were before per-object motion existed.
    CY_CHECK(same(moving.previous[1], second[1]));
    CY_CHECK_EQ(history.kept(), 2U);
}

CY_TEST_CASE("last frame's rows are rebased to this frame's camera") {
    InstanceHistory history(allocator());
    (void)advance(history, {at(3.0F, 1.0F, -5.0F)});
    // The camera moved one metre along +x and the object stayed put in the world: its
    // camera-relative row moved one metre along -x, and its previous row, rebased, is that row.
    const std::vector<InstanceTransform> still = {at(2.0F, 1.0F, -5.0F)};
    const Step step = advance(history, still, {}, Vec3{1.0F, 0.0F, 0.0F});
    CY_CHECK(same(step.previous[0], still[0]));
    CY_CHECK_EQ(step.report.moved, 0U);

    const InstanceTransform rebased =
        rebase_previous_row(at(3.0F, 1.0F, -5.0F), Vec3{1.0F, 2.0F, 3.0F});
    CY_CHECK_EQ(rebased.row0[3], 2.0F);
    CY_CHECK_EQ(rebased.row1[3], -1.0F);
    CY_CHECK_EQ(rebased.row2[3], -8.0F);
}

CY_TEST_CASE("a cut, a recycled slot and a new slot carry no invented motion") {
    InstanceHistory history(allocator());
    (void)advance(history, {at(0.0F, 0.0F, -5.0F), at(1.0F, 0.0F, -5.0F)}, {10, 11});

    const std::vector<InstanceTransform> moved = {at(4.0F, 0.0F, -5.0F), at(5.0F, 0.0F, -5.0F),
                                                  at(6.0F, 0.0F, -5.0F)};
    // Slot 0 is the same instance, slot 1 was freed and handed to a new one, slot 2 is new.
    const Step step = advance(history, moved, {10, 99, 12});
    CY_CHECK(same(step.previous[0], at(0.0F, 0.0F, -5.0F)));
    CY_CHECK(same(step.previous[1], moved[1]));
    CY_CHECK(same(step.previous[2], moved[2]));
    CY_CHECK_EQ(step.report.new_rows, 2U);
    CY_CHECK_EQ(step.report.moved, 1U);

    // A cut: the history is meaningless, whatever the rows say.
    const std::vector<InstanceTransform> after = {at(9.0F, 0.0F, -5.0F), at(5.0F, 0.0F, -5.0F),
                                                  at(6.0F, 0.0F, -5.0F)};
    const Step cut = advance(history, after, {10, 99, 12}, {}, true);
    CY_CHECK_EQ(cut.report.new_rows, 3U);
    for (usize slot = 0; slot < after.size(); ++slot) {
        CY_CHECK(same(cut.previous[slot], after[slot]));
    }
}

CY_TEST_CASE("the history refuses rows it cannot pair") {
    InstanceHistory history(allocator());
    const std::vector<InstanceTransform> rows = {at(0.0F, 0.0F, -1.0F)};
    std::vector<InstanceTransform> previous(2);
    CY_CHECK_FALSE(history
                       .advance(Span<const InstanceTransform>(rows.data(), rows.size()), {}, Vec3{},
                                false, Span<InstanceTransform>(previous.data(), previous.size()))
                       .has_value());
    const std::vector<u64> ids = {1, 2};
    CY_CHECK_FALSE(history
                       .advance(Span<const InstanceTransform>(rows.data(), rows.size()),
                                Span<const u64>(ids.data(), ids.size()), Vec3{}, false,
                                Span<InstanceTransform>(previous.data(), 1))
                       .has_value());
}
