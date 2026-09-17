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

CY_TEST_CASE("two views needing one page mark it once, and the secondary view weighs less") {
    // "Views that share a light — stereo eyes, split screen, a reflection probe and the main
    // camera — SHALL merge their shadow page requirements into one request set, and each page SHALL
    // be rendered once and sampled by every view that needs it. Views SHALL contribute demand
    // weighted by their own priority, so a minimap or an editor thumbnail does not raise shadow
    // quality to main-camera levels."
    //
    // There is no merge STEP to test: two views mark the same page id and compaction merges them,
    // which is the requirement satisfied by construction. So what this case is written against is
    // the two ways that construction can be wrong — a set that emits the shared page TWICE, which
    // is one page rasterised once per view, and a merge that keeps the LAST view's weight instead
    // of the largest, which is a reflection probe quietly deciding the main camera's shadow
    // quality. The main camera therefore marks FIRST and the probe SECOND: written the other way
    // round, a last-writer merge would pass.
    PageRequestSet set(allocator());
    CY_REQUIRE(set.initialize(64).has_value());

    ShadowAddressSpace levels[2];
    build_levels(levels);
    const cy::Span<const ShadowAddressSpace> span(levels, 2);

    const cy::Vec3 shared{0.0F, 0.0F, -10.0F};

    ReceiverSample camera;
    camera.world_position = shared;
    camera.texel_world_size = 0.0F;
    camera.importance = 1.0F;
    set.mark(span, camera);

    ReceiverSample probe;
    probe.world_position = shared;
    probe.texel_world_size = 0.0F;
    probe.importance = 0.2F;
    set.mark(span, probe);

    // A page only the probe needs, so the two weights can be told apart downstream.
    ReceiverSample probe_only = probe;
    probe_only.world_position = cy::Vec3{4.0F, 4.0F, -10.0F};
    set.mark(span, probe_only);

    CY_REQUIRE(set.compact().has_value());
    CY_CHECK_EQ(set.marks(), 3U);
    CY_REQUIRE_EQ(set.pages().size(), 2U);

    const PageRequest* shared_page = nullptr;
    const PageRequest* probe_page = nullptr;
    for (const PageRequest& request : set.pages()) {
        if (request.marks == 2U) {
            shared_page = &request;
        } else {
            probe_page = &request;
        }
    }
    // Rendered ONCE and sampled by both: one entry carrying both views' marks.
    CY_REQUIRE(shared_page != nullptr);
    CY_REQUIRE(probe_page != nullptr);
    if (shared_page == nullptr || probe_page == nullptr) {
        // `CY_REQUIRE` reports and continues under -fno-exceptions, so the failure above has to
        // stop this case by hand or the dereferences below turn a red assertion into a SIGSEGV —
        // which is what the compaction mutation this case was proved against actually produced.
        return;
    }
    CY_CHECK_EQ(probe_page->marks, 1U);
    // The main camera's demand decides the shared page; the probe's does not dilute it.
    CY_CHECK_NEAR(shared_page->importance, 1.0F, 1e-6F);
    CY_CHECK_NEAR(probe_page->importance, 0.2F, 1e-6F);

    // And the weight is what the rest of the system spends on. Under pressure the page only the
    // probe asked for coarsens, and the one the main camera shares does not — "the probe's demand
    // SHALL carry lower priority and select coarser pages".
    cy::rendering::ShadowBudget budget;
    budget.set_allocation_ms(0.4F);
    for (cy::u32 frame = 0; frame < 64; ++frame) {
        budget.report_measured_ms(2.0F);
        (void)budget.update();
    }
    CY_REQUIRE(budget.at_minimum());
    CY_CHECK_GT(static_cast<cy::u32>(apply_resolution_bias(budget, 0, probe_page->importance, 6)),
                static_cast<cy::u32>(apply_resolution_bias(budget, 0, shared_page->importance, 6)));

    // The same ordering decides which of the two is refreshed first when the frame cannot afford
    // both: equal age, equal class, and the weight the views contributed is the only difference.
    StalePage shared_stale;
    shared_stale.page = shared_page->page;
    shared_stale.importance = shared_page->importance;
    shared_stale.age = 4;
    StalePage probe_stale;
    probe_stale.page = probe_page->page;
    probe_stale.importance = probe_page->importance;
    probe_stale.age = 4;
    CY_CHECK_GT(staleness_priority(shared_stale), staleness_priority(probe_stale));
}
