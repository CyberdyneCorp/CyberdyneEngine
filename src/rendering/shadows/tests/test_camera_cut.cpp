// The camera cut: two thousand dirty pages against an allocation that affords about a hundred.
// Task 8.1.
//
// Integration rather than unit, and the reason is the point of the case: `virtual-shadows` forbids
// a fixed cap on pages per frame because "a hard cap turns a camera cut into a stall", so the
// scenario that proves the alternative works has to be a real cut — two thousand pages, sorted by
// priority, with the allocation spent down. That is milliseconds of honest work in an unoptimised
// build, and the taxonomy in `testing-and-quality` places a test this expensive in the suite above
// `unit`. Shrinking the cut to fit a one-millisecond budget would be certifying a different
// scenario from the one the requirement is about.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/shadows/pages.h>

namespace {

using cy::rendering::select_pages_to_render;
using cy::rendering::StalePage;
using cy::rendering::UpdateClass;
using cy::rendering::VirtualPage;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

VirtualPage page_at(cy::u16 x, cy::u16 y) noexcept {
    VirtualPage page;
    page.light_slot = 1;
    page.x = x;
    page.y = y;
    return page;
}

}  // namespace

CY_TEST_CASE("a camera cut is spread over frames by priority, and critical pages never wait") {
    // Two thousand dirty pages against an allocation that affords about a hundred: the requirement
    // is that this produces several frames of refresh rather than one long frame.
    cy::Array<StalePage> pages(allocator());
    CY_REQUIRE(pages.reserve(2000).has_value());
    for (cy::u32 index = 0; index < 2000; ++index) {
        StalePage page;
        page.page = page_at(static_cast<cy::u16>(index % 64U), static_cast<cy::u16>(index / 64U));
        page.update_class = index < 4 ? UpdateClass::Critical : UpdateClass::Background;
        page.age = index % 17U;
        page.importance = static_cast<cy::f32>(index % 10U) / 10.0F;
        page.last_cost_ms = 0.01F;
        CY_REQUIRE(pages.push_back(page).has_value());
    }

    cy::Array<VirtualPage> selected(allocator());
    CY_REQUIRE(selected.resize(2000).has_value());
    const cy::rendering::PageSelection selection =
        select_pages_to_render(pages.span(), 1.0F, 0.02F, selected.span().data(), 2000);

    CY_CHECK_GT(selection.selected, 0U);
    CY_CHECK_LT(selection.selected, 2000U);
    CY_CHECK_GT(selection.deferred, 0U);
    CY_CHECK_LE(selection.spend_ms, 1.05F);

    // Every critical page is in the selection, wherever it sat in the input. The four ids are
    // recomputed rather than searched for in the reordered input, because `select_pages_to_render`
    // sorts in place and a scan of the whole input per selected page would be the only quadratic
    // thing in this suite.
    cy::u32 critical_selected = 0;
    for (cy::u32 index = 0; index < 4; ++index) {
        const cy::u64 wanted =
            page_at(static_cast<cy::u16>(index % 64U), static_cast<cy::u16>(index / 64U)).pack();
        for (cy::u32 taken = 0; taken < selection.selected; ++taken) {
            if (selected.span()[taken].pack() == wanted) {
                ++critical_selected;
                break;
            }
        }
    }
    CY_CHECK_EQ(critical_selected, 4U);
}
