// Measure, arrange, the three models, and resolution independence. M8.b task 9.1.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/layout.h>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

[[nodiscard]] ElementId add(ElementStore& store, ElementId parent, const char* type) noexcept {
    auto created = store.create(parent, Name::intern(type));
    return created.has_value() ? created.value() : kNoElement;
}

/// A measurer that answers a fixed size per element, so layout cases are arithmetic rather than
/// typography — and so this suite needs no font server.
class FixedMeasurer final : public ContentMeasurer {
public:
    Vec2 measure_content(ElementId element, Vec2 available) noexcept override {
        ++calls;
        (void)available;
        for (usize index = 0; index < count; ++index) {
            if (elements[index] == element) {
                return sizes[index];
            }
        }
        return Vec2{0.0F, 0.0F};
    }

    [[nodiscard]] bool declare(ElementId element, Vec2 size) noexcept {
        if (count >= 16) {
            return false;
        }
        elements[count] = element;
        sizes[count] = size;
        ++count;
        return true;
    }

    ElementId elements[16];
    Vec2 sizes[16];
    usize count = 0;
    u32 calls = 0;
};

[[nodiscard]] ScaleSettings settings_1080p() noexcept {
    ScaleSettings settings;
    settings.mode = ScaleMode::FixedPixel;
    settings.reference = Vec2{1920.0F, 1080.0F};
    return settings;
}

}  // namespace

CY_TEST_CASE("ui_layout: a flex row distributes surplus by grow, respecting maximums") {
    // "WHEN a flex row has more width than its children's minimum THEN surplus SHALL be distributed
    // by flex grow factors, respecting each child's maximum."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId left = add(store, root, "panel");
    const ElementId right = add(store, root, "panel");

    store.layout_input(root)->model = LayoutModel::Flex;
    store.layout_input(root)->direction = FlexDirection::Row;
    store.layout_input(root)->align = Align::Start;
    store.layout_input(left)->preferred = Vec2{100.0F, 50.0F};
    store.layout_input(left)->flex_grow = 1.0F;
    store.layout_input(right)->preferred = Vec2{100.0F, 50.0F};
    store.layout_input(right)->flex_grow = 3.0F;
    store.layout_input(right)->maximum = Vec2{250.0F, 1000.0F};

    LayoutReport report;
    CY_REQUIRE(layout(store, settings_1080p(), Vec2{1000.0F, 400.0F}, nullptr, report).has_value());

    // 800 surplus over 1000 wide: one part to the left, three to the right — but the right hits its
    // maximum of 250 and the rest is simply not distributed rather than exceeding it.
    CY_CHECK_NEAR(store.layout_output(left)->rect.width, 300.0F, 0.5F);
    CY_CHECK_NEAR(store.layout_output(right)->rect.width, 250.0F, 0.5F);
    CY_CHECK_NEAR(store.layout_output(left)->rect.x, 0.0F, 0.5F);
    CY_CHECK_NEAR(store.layout_output(right)->rect.x, 300.0F, 0.5F);
}

CY_TEST_CASE("ui_layout: justify and gap place the leftover where CSS would") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId first = add(store, root, "panel");
    const ElementId second = add(store, root, "panel");
    store.layout_input(root)->gap = 20.0F;
    store.layout_input(root)->justify = Justify::End;
    store.layout_input(root)->align = Align::Start;
    store.layout_input(first)->preferred = Vec2{100.0F, 40.0F};
    store.layout_input(second)->preferred = Vec2{100.0F, 40.0F};

    LayoutReport report;
    CY_REQUIRE(layout(store, settings_1080p(), Vec2{500.0F, 200.0F}, nullptr, report).has_value());
    // 500 − (100 + 20 + 100) = 280 of leftover, all before the first child.
    CY_CHECK_NEAR(store.layout_output(first)->rect.x, 280.0F, 0.5F);
    CY_CHECK_NEAR(store.layout_output(second)->rect.x, 400.0F, 0.5F);
}

CY_TEST_CASE("ui_layout: an anchored element stays in its corner across a resize") {
    // "WHEN a minimap is positioned absolutely against the bottom-right corner with a fixed size
    // THEN it SHALL remain anchored there across window resizes and aspect changes."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId minimap = add(store, root, "panel");
    store.layout_input(root)->model = LayoutModel::Absolute;
    LayoutInput* input = store.layout_input(minimap);
    input->anchor_min = Vec2{1.0F, 1.0F};
    input->anchor_max = Vec2{1.0F, 1.0F};
    input->offset_min = Vec2{-260.0F, -160.0F};
    input->offset_max = Vec2{-20.0F, -20.0F};

    LayoutReport report;
    CY_REQUIRE(
        layout(store, settings_1080p(), Vec2{1920.0F, 1080.0F}, nullptr, report).has_value());
    const Rect wide = store.layout_output(minimap)->rect;
    CY_CHECK_NEAR(wide.right(), 1900.0F, 0.5F);
    CY_CHECK_NEAR(wide.bottom(), 1060.0F, 0.5F);
    CY_CHECK_NEAR(wide.width, 240.0F, 0.5F);

    // A different window, a different aspect: the same distance from the same corner, the same
    // size.
    store.mark(root, Dirty::Arrange);
    CY_REQUIRE(
        layout(store, settings_1080p(), Vec2{1280.0F, 1024.0F}, nullptr, report).has_value());
    const Rect narrow = store.layout_output(minimap)->rect;
    CY_CHECK_NEAR(narrow.right(), 1260.0F, 0.5F);
    CY_CHECK_NEAR(narrow.bottom(), 1004.0F, 0.5F);
    CY_CHECK_NEAR(narrow.width, 240.0F, 0.5F);
}

CY_TEST_CASE("ui_layout: a grid places children in tracks and honours a span") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    store.layout_input(root)->model = LayoutModel::Grid;
    store.layout_input(root)->grid_columns = 2;
    store.layout_input(root)->gap = 10.0F;

    const ElementId a = add(store, root, "panel");
    const ElementId b = add(store, root, "panel");
    const ElementId c = add(store, root, "panel");
    store.layout_input(c)->grid_column_span = 2;

    LayoutReport report;
    CY_REQUIRE(layout(store, settings_1080p(), Vec2{410.0F, 200.0F}, nullptr, report).has_value());
    const Rect first = store.layout_output(a)->rect;
    const Rect second = store.layout_output(b)->rect;
    const Rect third = store.layout_output(c)->rect;
    CY_CHECK_NEAR(first.width, 200.0F, 0.5F);
    CY_CHECK_NEAR(second.x, 210.0F, 0.5F);
    CY_CHECK_NEAR(first.y, second.y, 0.5F);
    // The spanning child is on the next row and twice as wide, plus the gap it swallowed.
    CY_CHECK_GT(third.y, first.y);
    CY_CHECK_NEAR(third.width, 410.0F, 0.5F);
}

CY_TEST_CASE("ui_layout: text drives layout through the measurer, and only where it must") {
    // "WHEN a label's text changes and its desired size grows THEN the measure pass SHALL propagate
    // the change upward and the arrange pass SHALL re-layout affected ancestors only."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId row = add(store, root, "panel");
    const ElementId label = add(store, row, "label");
    const ElementId other = add(store, root, "panel");
    store.layout_input(other)->preferred = Vec2{50.0F, 50.0F};

    FixedMeasurer measurer;
    CY_REQUIRE(measurer.declare(label, Vec2{80.0F, 20.0F}));

    LayoutReport first;
    CY_REQUIRE(layout(store, settings_1080p(), Vec2{800.0F, 600.0F}, &measurer, first).has_value());
    CY_CHECK_NEAR(store.layout_output(row)->rect.width, 80.0F, 0.5F);
    CY_CHECK_GT(first.measured, 0U);

    // NOTHING DIRTY: the next frame does no work at all. "WHEN no UI state changes in a frame THEN
    // no layout or paint work SHALL be performed."
    LayoutReport idle;
    CY_REQUIRE(layout(store, settings_1080p(), Vec2{800.0F, 600.0F}, &measurer, idle).has_value());
    CY_CHECK_EQ(idle.measured, 0U);
    CY_CHECK_EQ(idle.arranged, 0U);

    // The label grows: it, its row and the root re-measure, and the unrelated sibling does not.
    measurer.sizes[0] = Vec2{200.0F, 20.0F};
    store.mark(label, Dirty::Measure);
    LayoutReport grown;
    CY_REQUIRE(layout(store, settings_1080p(), Vec2{800.0F, 600.0F}, &measurer, grown).has_value());
    CY_CHECK_NEAR(store.layout_output(row)->rect.width, 200.0F, 0.5F);
    CY_CHECK_LE(grown.measured, 4U);
}

CY_TEST_CASE("ui_layout: a collapsed element takes part in nothing") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId visible = add(store, root, "panel");
    const ElementId hidden = add(store, root, "panel");
    store.layout_input(visible)->preferred = Vec2{100.0F, 40.0F};
    store.layout_input(hidden)->preferred = Vec2{100.0F, 40.0F};
    CY_REQUIRE(store.set_flags(hidden, ElementFlags::Collapsed).has_value());

    LayoutReport report;
    CY_REQUIRE(layout(store, settings_1080p(), Vec2{500.0F, 200.0F}, nullptr, report).has_value());
    CY_CHECK_NEAR(store.layout_output(root)->desired.x, 100.0F, 0.5F);
    CY_CHECK_GT(report.skipped, 0U);
}

CY_TEST_CASE("ui_layout: the clip narrows down the tree and never widens") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId scroller = add(store, root, "panel");
    const ElementId content = add(store, scroller, "panel");
    store.layout_input(root)->model = LayoutModel::Absolute;
    store.layout_input(scroller)->anchor_min = Vec2{0.0F, 0.0F};
    store.layout_input(scroller)->anchor_max = Vec2{0.0F, 0.0F};
    store.layout_input(scroller)->offset_max = Vec2{200.0F, 100.0F};
    store.layout_input(content)->preferred = Vec2{1000.0F, 1000.0F};
    CY_REQUIRE(
        store.set_flags(scroller, ElementFlags::Visible | ElementFlags::ClipsChildren).has_value());

    LayoutReport report;
    CY_REQUIRE(layout(store, settings_1080p(), Vec2{800.0F, 600.0F}, nullptr, report).has_value());
    const Rect clip = store.layout_output(content)->clip;
    CY_CHECK_LE(clip.width, 200.5F);
    CY_CHECK_LE(clip.height, 100.5F);
}

CY_TEST_CASE("ui_layout: the scale strategy is one number, computed once") {
    ScaleSettings settings;
    settings.reference = Vec2{1920.0F, 1080.0F};

    settings.mode = ScaleMode::FixedPixel;
    CY_CHECK_EQ(resolve_scale(settings, Vec2{3840.0F, 2160.0F}), 1.0F);

    settings.mode = ScaleMode::ScaleWithWidth;
    CY_CHECK_NEAR(resolve_scale(settings, Vec2{3840.0F, 1080.0F}), 2.0F, 1e-5F);

    settings.mode = ScaleMode::ScaleWithSmaller;
    CY_CHECK_NEAR(resolve_scale(settings, Vec2{3840.0F, 1080.0F}), 1.0F, 1e-5F);

    settings.mode = ScaleMode::Match;
    settings.match = 0.5F;
    CY_CHECK_NEAR(resolve_scale(settings, Vec2{3840.0F, 1080.0F}), 1.5F, 1e-5F);

    // THE DPI SCALE AND THE PLAYER'S OWN SCALE MULTIPLY IN, ONCE. "WHEN the display reports a 2×
    // scale THEN UI SHALL render at twice the pixel density."
    settings.mode = ScaleMode::FixedPixel;
    settings.dpi_scale = 2.0F;
    settings.user_scale = 1.25F;
    CY_CHECK_NEAR(resolve_scale(settings, Vec2{1920.0F, 1080.0F}), 2.5F, 1e-5F);
}
