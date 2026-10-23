// SPDX-License-Identifier: MIT
// Opacity groups, render transforms, and tab order. Issue #91, stages 2, 4 and 5.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/interaction.h>
#include <cy/ui/paint.h>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

const ui::Rect kScreen{0.0F, 0.0F, 640.0F, 480.0F};

[[nodiscard]] ElementId add(ElementStore& store, ElementId parent, ui::Rect rect,
                            u32 colour) noexcept {
    auto created = store.create(parent, Name::intern("panel"));
    if (!created) {
        return kNoElement;
    }
    store.layout_output(*created)->rect = rect;
    store.layout_output(*created)->clip = kScreen;
    store.paint(*created)->background = colour;
    return *created;
}

/// A panel at (100, 100) 200 by 100 with two opaque children overlapping in the middle.
struct Overlap {
    explicit Overlap(ElementStore& store) noexcept {
        panel = add(store, kNoElement, ui::Rect{100, 100, 200, 100}, 0xFF202020U);
        left = add(store, panel, ui::Rect{110, 110, 120, 80}, 0xFFC03030U);
        right = add(store, panel, ui::Rect{170, 110, 120, 80}, 0xFF3030C0U);
    }
    ElementId panel;
    ElementId left;
    ElementId right;
};

}  // namespace

CY_TEST_CASE("ui_groups: an opacity group flattens its subtree once, whole, and composites it") {
    // `ui-system`: "WHEN a panel with overlapping children is faded to 50 % THEN it SHALL be
    // composited once at 50 %, without the overlaps darkening."
    ElementStore store(allocator());
    Overlap scene(store);
    store.paint(scene.panel)->opacity = 0.5F;
    (void)store.set_flags(scene.panel, store.flags(scene.panel) | ElementFlags::OpacityGroup);
    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(store, kScreen, buffer, report).has_value());
    CY_CHECK_EQ(report.groups, 1U);
    CY_REQUIRE_EQ(buffer.groups().size(), 1U);
    const OpacityGroup& group = buffer.groups()[0];
    CY_CHECK_EQ(group.opacity, 0.5F);
    CY_CHECK_EQ(group.source, scene.panel);
    CY_CHECK_EQ(group.count, 3U);
    // Inside the group, the colours are whole: the opacity is the composite's, applied once.
    for (u32 index = group.first; index < group.first + group.count; ++index) {
        CY_CHECK_EQ(buffer.primitives()[index].colour >> 24U, 0xFFU);
    }
    CY_CHECK_EQ(group.bounds.x, 100.0F);
    CY_CHECK_EQ(group.bounds.width, 200.0F);
    // The main stream is the composite, alone: the group's own primitives are not in its batches.
    CY_REQUIRE_EQ(buffer.batches().size(), 1U);
    const Primitive& composite = buffer.primitives()[buffer.batches()[0].first];
    CY_CHECK_EQ(buffer.batches()[0].count, 1U);
    CY_CHECK_EQ(composite.atlas, kFirstGroupAtlas);
    CY_CHECK_EQ(composite.material, material_index(BuiltinMaterial::Image));
    CY_CHECK_EQ(composite.colour, scale_premultiplied(0xFFFFFFFFU, 0.5F));
    CY_REQUIRE_EQ(buffer.group_batches().size(), 1U);
    CY_CHECK_EQ(buffer.group_batches()[0].first, group.first);
}

CY_TEST_CASE("ui_groups: without the flag, or at full opacity, nothing changes") {
    ElementStore plain_store(allocator());
    Overlap plain(plain_store);
    plain_store.paint(plain.panel)->opacity = 0.5F;
    PrimitiveBuffer multiplied(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(plain_store, kScreen, multiplied, report).has_value());
    CY_CHECK_EQ(report.groups, 0U);
    CY_CHECK_EQ(multiplied.primitives().size(), 3U);
    // Multiplied down: each child at half, so their overlap blends twice — what the group fixes.
    CY_CHECK_EQ(multiplied.primitives()[1].colour, scale_premultiplied(0xFFC03030U, 0.5F));

    ElementStore whole_store(allocator());
    Overlap whole(whole_store);
    (void)whole_store.set_flags(whole.panel,
                                whole_store.flags(whole.panel) | ElementFlags::OpacityGroup);
    PrimitiveBuffer opaque(allocator());
    CY_REQUIRE(flatten(whole_store, kScreen, opaque, report).has_value());
    CY_CHECK_EQ(report.groups, 0U);
    CY_CHECK(opaque.groups().empty());
    CY_CHECK_EQ(opaque.primitives().size(), 3U);
}

CY_TEST_CASE("ui_groups: a group inside a group is listed first, and sampled by its parent") {
    ElementStore store(allocator());
    Overlap scene(store);
    const ElementId inner = add(store, scene.left, ui::Rect{120, 120, 40, 40}, 0xFF30C030U);
    (void)add(store, inner, ui::Rect{130, 130, 40, 40}, 0xFFFFFFFFU);
    for (const ElementId grouped : {scene.panel, inner}) {
        store.paint(grouped)->opacity = 0.5F;
        (void)store.set_flags(grouped, store.flags(grouped) | ElementFlags::OpacityGroup);
    }
    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(store, kScreen, buffer, report).has_value());
    CY_REQUIRE_EQ(buffer.groups().size(), 2U);
    CY_CHECK_EQ(buffer.groups()[0].source, inner);
    CY_CHECK_EQ(buffer.groups()[1].source, scene.panel);
    // The outer group's stream holds the inner group's composite, at the inner's opacity only.
    const OpacityGroup& outer = buffer.groups()[1];
    bool samples_inner = false;
    for (u32 index = outer.first; index < outer.first + outer.count; ++index) {
        const Primitive& primitive = buffer.primitives()[index];
        if (primitive.atlas == kFirstGroupAtlas) {
            samples_inner = true;
            CY_CHECK_EQ(primitive.colour, scale_premultiplied(0xFFFFFFFFU, 0.5F));
        }
    }
    CY_CHECK(samples_inner);
    CY_CHECK_EQ(buffer.groups()[0].opacity, 0.5F);
}

CY_TEST_CASE(
    "ui_transform: a render transform moves and scales a subtree, and layout never knows") {
    ElementStore store(allocator());
    const ElementId panel = add(store, kNoElement, ui::Rect{100, 100, 100, 100}, 0xFF202020U);
    const ElementId child = add(store, panel, ui::Rect{100, 100, 50, 50}, 0xFFFFFFFFU);
    store.paint(panel)->corner_radius = 4.0F;
    PaintData& paint = *store.paint(panel);
    paint.scale = 2.0F;
    paint.translate = Vec2{10.0F, 0.0F};
    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(store, kScreen, buffer, report).has_value());
    CY_REQUIRE_EQ(buffer.primitives().size(), 2U);
    // Scaled about the panel's centre (150, 150), then moved ten to the right.
    const ui::Rect& moved = buffer.primitives()[0].bounds;
    CY_CHECK_EQ(moved.x, 60.0F);
    CY_CHECK_EQ(moved.y, 50.0F);
    CY_CHECK_EQ(moved.width, 200.0F);
    CY_CHECK_EQ(buffer.primitives()[0].corner_radius, 8.0F);
    // The child goes with it, from its own place.
    CY_CHECK_EQ(buffer.primitives()[1].bounds.x, 60.0F);
    CY_CHECK_EQ(buffer.primitives()[1].bounds.width, 100.0F);
    // Layout's rects are what they were.
    CY_CHECK_EQ(store.layout_output(panel)->rect.x, 100.0F);
    CY_CHECK_EQ(store.layout_output(child)->rect.width, 50.0F);
}

CY_TEST_CASE("ui_transform: a clip set inside a moved subtree moves with it") {
    ElementStore store(allocator());
    const ElementId panel = add(store, kNoElement, ui::Rect{100, 100, 100, 100}, 0xFF202020U);
    (void)store.set_flags(panel, store.flags(panel) | ElementFlags::ClipsChildren);
    // The layout's clip for the panel's children is the panel's rect.
    const ElementId child = add(store, panel, ui::Rect{150, 150, 100, 100}, 0xFFFFFFFFU);
    store.layout_output(child)->clip = ui::Rect{100, 100, 100, 100};
    store.paint(panel)->translate = Vec2{200.0F, 0.0F};
    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(store, kScreen, buffer, report).has_value());
    CY_REQUIRE_EQ(buffer.primitives().size(), 2U);
    const ui::Rect clip = buffer.clips()[buffer.primitives()[1].clip];
    CY_CHECK_EQ(clip.x, 300.0F);
    CY_CHECK_EQ(clip.width, 100.0F);
}

CY_TEST_CASE("ui_focus: the tab order is document order within the active layer, and wraps") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, kScreen, 0U);
    ElementId buttons[4];
    for (u32 index = 0; index < 4; ++index) {
        buttons[index] = add(store, root, ui::Rect{static_cast<f32>(index) * 50.0F, 0, 40, 40}, 1U);
        (void)store.set_flags(buttons[index],
                              store.flags(buttons[index]) | ElementFlags::Focusable);
    }
    // A focusable element inside a collapsed parent is not in the order.
    const ElementId hidden = add(store, root, ui::Rect{0, 100, 40, 40}, 0U);
    const ElementId inside = add(store, hidden, ui::Rect{0, 100, 40, 40}, 1U);
    (void)store.set_flags(inside, store.flags(inside) | ElementFlags::Focusable);
    (void)store.set_flags(hidden, store.flags(hidden) | ElementFlags::Collapsed);
    Interaction interaction(allocator());
    CY_CHECK_EQ(interaction.focus_next(store, false), buttons[0]);
    CY_CHECK_EQ(interaction.focus_next(store, false), buttons[1]);
    CY_CHECK_EQ(interaction.focus_next(store, true), buttons[0]);
    CY_CHECK_EQ(interaction.focus_next(store, true), buttons[3]);   // wrapped backwards
    CY_CHECK_EQ(interaction.focus_next(store, false), buttons[0]);  // and forwards
    auto routed = interaction.route_action(store, UiAction::FocusNext);
    CY_REQUIRE(routed.has_value());
    CY_CHECK_EQ(routed->target, buttons[1]);
    CY_CHECK_EQ(std::string_view(ui_action_name(UiAction::FocusPrevious)),
                std::string_view("UI.FocusPrevious"));
}

CY_TEST_CASE("ui_focus: directional navigation skips what a collapsed ancestor hides") {
    // REGRESSION: navigation read only an element's own flags, so a focusable control on a tab
    // page that was collapsed — its rect stale from when it was shown — was still the nearest
    // thing "down", and focus went somewhere nothing was drawn.
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, kScreen, 0U);
    const ElementId top = add(store, root, ui::Rect{0, 0, 40, 40}, 1U);
    const ElementId page = add(store, root, ui::Rect{0, 50, 40, 40}, 0U);
    const ElementId on_page = add(store, page, ui::Rect{0, 50, 40, 40}, 1U);
    const ElementId bottom = add(store, root, ui::Rect{0, 200, 40, 40}, 1U);
    for (const ElementId element : {top, on_page, bottom}) {
        (void)store.set_flags(element, store.flags(element) | ElementFlags::Focusable);
    }
    (void)store.set_flags(page, store.flags(page) | ElementFlags::Collapsed);
    Interaction interaction(allocator());
    CY_REQUIRE(interaction.set_focus(store, top).has_value());
    CY_CHECK_EQ(interaction.navigate(store, UiAction::NavigateDown), bottom);
}
