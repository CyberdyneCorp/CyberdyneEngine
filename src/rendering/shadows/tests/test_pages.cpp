// Receiver-driven marking, compaction, and the staleness ordering that replaces a per-frame page
// cap. Task 8.1.
//
// The compaction case is the one worth reading: marks arriving in two different orders must produce
// the same compacted list, because the render order of dirty pages decides which of them fit inside
// a budget, and a set that reordered itself would make a shadow budget irreproducible.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/shadows/budget.h>
#include <cy/rendering/shadows/pages.h>

namespace {

using cy::rendering::PageRequest;
using cy::rendering::PageRequestSet;
using cy::rendering::ReceiverSample;
using cy::rendering::ShadowAddressSpace;
using cy::rendering::ShadowPageGeometry;
using cy::rendering::ShadowProjection;
using cy::rendering::StalePage;
using cy::rendering::UpdateClass;
using cy::rendering::VirtualPage;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// Two levels of one spot light, finest first.
void build_levels(ShadowAddressSpace (&levels)[2]) noexcept {
    for (cy::u32 index = 0; index < 2; ++index) {
        ShadowAddressSpace& space = levels[index];
        space.projection = ShadowProjection::Spot;
        space.light_slot = 1;
        space.level = static_cast<cy::u8>(index);
        space.basis = cy::rendering::shadow_basis(cy::Vec3{0.0F, 0.0F, -1.0F});
        space.position = cy::Vec3{0.0F, 0.0F, 0.0F};
        space.half_angle = 0.7F;
        space.extent = 100.0F;
        // The coarser level halves the logical resolution, which doubles the texel footprint.
        space.geometry = ShadowPageGeometry{128, index == 0 ? 4096U : 2048U};
    }
}

VirtualPage page_at(cy::u16 x, cy::u16 y) noexcept {
    VirtualPage page;
    page.light_slot = 1;
    page.x = x;
    page.y = y;
    return page;
}

}  // namespace

CY_TEST_CASE("a wall's worth of receiver samples compacts to a handful of pages") {
    PageRequestSet set(allocator());
    CY_REQUIRE(set.initialize(4096).has_value());

    ShadowAddressSpace levels[2];
    build_levels(levels);

    // Nine hundred samples across a small patch: the marks are many, the pages are few. That ratio
    // is the number that says marking is doing useful work.
    for (cy::u32 iy = 0; iy < 30; ++iy) {
        for (cy::u32 ix = 0; ix < 30; ++ix) {
            ReceiverSample sample;
            sample.world_position = cy::Vec3{(static_cast<cy::f32>(ix) * 0.01F) - 0.15F,
                                             (static_cast<cy::f32>(iy) * 0.01F) - 0.15F, -10.0F};
            sample.texel_world_size = 0.0F;  // wants the finest level available
            sample.importance = 0.5F;
            set.mark(cy::Span<const ShadowAddressSpace>(levels, 2), sample);
        }
    }
    CY_REQUIRE(set.compact().has_value());
    CY_CHECK_EQ(set.marks(), 900U);
    CY_CHECK_EQ(set.overflow(), 0U);
    CY_CHECK_GT(set.pages().size(), 0U);
    CY_CHECK_LT(set.pages().size(), 30U);

    cy::u32 total_marks = 0;
    for (const PageRequest& request : set.pages()) {
        total_marks += request.marks;
        CY_CHECK_EQ(static_cast<cy::u32>(request.page.level), 0U);
        CY_CHECK_NEAR(request.importance, 0.5F, 1e-6F);
    }
    CY_CHECK_EQ(total_marks, 900U);
}

CY_TEST_CASE("compaction is ordered by page identity, not by the order the marks arrived") {
    PageRequestSet forwards(allocator());
    PageRequestSet backwards(allocator());
    CY_REQUIRE(forwards.initialize(64).has_value());
    CY_REQUIRE(backwards.initialize(64).has_value());

    for (cy::u16 index = 0; index < 16; ++index) {
        forwards.mark_page(page_at(index, static_cast<cy::u16>(index * 2U)), 0.25F);
        backwards.mark_page(
            page_at(static_cast<cy::u16>(15U - index), static_cast<cy::u16>((15U - index) * 2U)),
            0.75F);
    }
    CY_REQUIRE(forwards.compact().has_value());
    CY_REQUIRE(backwards.compact().has_value());
    CY_REQUIRE_EQ(forwards.pages().size(), backwards.pages().size());
    for (cy::usize index = 0; index < forwards.pages().size(); ++index) {
        CY_CHECK_EQ(forwards.pages()[index].page.pack(), backwards.pages()[index].page.pack());
    }

    // Compaction is idempotent within a frame, and a duplicate mark merges rather than duplicating.
    forwards.mark_page(page_at(3, 6), 0.9F);
    CY_REQUIRE(forwards.compact().has_value());
    CY_CHECK_EQ(forwards.pages().size(), 16U);
    for (const PageRequest& request : forwards.pages()) {
        if (request.page.x == 3) {
            CY_CHECK_EQ(request.marks, 2U);
            CY_CHECK_NEAR(request.importance, 0.9F, 1e-6F);
        }
    }
}

CY_TEST_CASE("a distant receiver marks a coarser level than a near one") {
    PageRequestSet set(allocator());
    CY_REQUIRE(set.initialize(64).has_value());
    ShadowAddressSpace levels[2];
    build_levels(levels);
    const cy::Span<const ShadowAddressSpace> span(levels, 2);

    ReceiverSample near_sample;
    near_sample.world_position = cy::Vec3{0.0F, 0.0F, -5.0F};
    near_sample.texel_world_size = 0.0F;
    set.mark(span, near_sample);

    ReceiverSample far_sample;
    far_sample.world_position = cy::Vec3{0.1F, 0.1F, -80.0F};
    // A pixel eighty metres out wants a texel the fine level cannot afford to give it.
    far_sample.texel_world_size = 1.0F;
    set.mark(span, far_sample);

    CY_REQUIRE(set.compact().has_value());
    CY_REQUIRE_EQ(set.pages().size(), 2U);
    cy::u32 fine = 0;
    cy::u32 coarse = 0;
    for (const PageRequest& request : set.pages()) {
        if (request.page.level == 0) {
            ++fine;
        } else {
            ++coarse;
        }
    }
    CY_CHECK_EQ(fine, 1U);
    CY_CHECK_EQ(coarse, 1U);

    // A sample outside the light marks nothing at all.
    PageRequestSet empty(allocator());
    CY_REQUIRE(empty.initialize(8).has_value());
    ReceiverSample behind;
    behind.world_position = cy::Vec3{0.0F, 0.0F, 40.0F};
    empty.mark(span, behind);
    CY_REQUIRE(empty.compact().has_value());
    CY_CHECK_EQ(empty.pages().size(), 0U);
}

CY_TEST_CASE(
    "a critical page outranks everything, and staleness ordering follows what would show") {
    StalePage background;
    background.update_class = UpdateClass::Background;
    background.age = 200;
    background.importance = 1.0F;
    background.motion = 1.0F;

    StalePage critical;
    critical.update_class = UpdateClass::Critical;
    critical.age = 0;

    CY_CHECK_GT(staleness_priority(critical), staleness_priority(background));
    CY_CHECK_GT(staleness_priority(critical), cy::rendering::kCriticalPriority - 1.0F);

    StalePage near_hero = background;
    near_hero.importance = 1.0F;
    StalePage distant = background;
    distant.importance = 0.0F;
    CY_CHECK_GT(staleness_priority(near_hero), staleness_priority(distant));

    StalePage moving = background;
    StalePage still = background;
    still.motion = 0.0F;
    CY_CHECK_GT(staleness_priority(moving), staleness_priority(still));

    CY_CHECK_EQ(cy::rendering::max_stale_frames(UpdateClass::Critical), 0U);
    CY_CHECK_GT(cy::rendering::max_stale_frames(UpdateClass::Background),
                cy::rendering::max_stale_frames(UpdateClass::Dynamic));
}

CY_TEST_CASE(
    "pressure lengthens refresh intervals and coarsens pages, but not for hero receivers") {
    cy::rendering::ShadowBudget budget;
    CY_CHECK_EQ(cy::rendering::budgeted_stale_frames(budget, UpdateClass::Normal),
                cy::rendering::max_stale_frames(UpdateClass::Normal));

    // Drive it to its minimum with an allocation it cannot meet.
    budget.set_allocation_ms(0.4F);
    for (cy::u32 frame = 0; frame < 64; ++frame) {
        budget.report_measured_ms(2.0F);
        (void)budget.update();
    }
    CY_REQUIRE(budget.at_minimum());
    CY_CHECK_GT(cy::rendering::budgeted_stale_frames(budget, UpdateClass::Normal),
                cy::rendering::max_stale_frames(UpdateClass::Normal));
    // Critical is not negotiable at any lever position.
    CY_CHECK_EQ(cy::rendering::budgeted_stale_frames(budget, UpdateClass::Critical), 0U);

    // Background receivers lose resolution; a hero receiver keeps it. That is the whole of "high
    // importance receivers SHALL retain shadow quality while background shadows degrade".
    CY_CHECK_GT(static_cast<cy::u32>(apply_resolution_bias(budget, 1, 0.1F, 8)), 1U);
    CY_CHECK_EQ(static_cast<cy::u32>(apply_resolution_bias(budget, 1, 0.95F, 8)), 1U);
    // The bias is clamped to the levels that exist.
    CY_CHECK_LE(static_cast<cy::u32>(apply_resolution_bias(budget, 8, 0.0F, 8)), 8U);
}
