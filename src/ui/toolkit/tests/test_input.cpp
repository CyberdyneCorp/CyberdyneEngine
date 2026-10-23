// SPDX-License-Identifier: MIT
// `unit.ui_toolkit`: the pointer, focus navigation by keyboard and gamepad, the focus ring, and
// what the interface consumes before the game sees it.

#include "fixture.h"

#include <string_view>

using namespace cy;
using namespace cy::ui;
using namespace cy::ui::toolkit_test;

namespace {

/// A three-by-two grid of buttons, named b0..b5 in reading order.
struct Grid {
    explicit Grid(Kit& kit) noexcept {
        WidgetDesc desc;
        desc.kind = WidgetKind::Grid;
        desc.columns = 3;
        auto made = kit.toolkit.create(kNoElement, desc);
        grid = made.has_value() ? *made : kNoElement;
        (void)kit.toolkit.set_anchors(grid, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{300, 60});
        const char* names[] = {"b0", "b1", "b2", "b3", "b4", "b5"};
        for (u32 index = 0; index < 6; ++index) {
            buttons[index] = kit.make(grid, WidgetKind::Button, names[index], names[index]);
        }
    }
    WidgetId grid;
    WidgetId buttons[6];
};

}  // namespace

CY_TEST_CASE("ui_toolkit: hover and press are states the theme draws, and a click is an event") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId button = kit.make(row, WidgetKind::Button, "go", "Go");
    CY_REQUIRE(kit.frame());
    const u32 idle = kit.store.paint(button)->background;
    const Vec2 over = kit.centre(button);

    CY_CHECK(kit.toolkit.pointer(PointerInput{over, false, {}, 0}));  // over a button: consumed
    for (u32 frame = 0; frame < 12; ++frame) {
        CY_REQUIRE(kit.frame());
    }
    CY_CHECK(has_flag(kit.store.flags(button), ElementFlags::Hovered));
    const u32 hovered = kit.store.paint(button)->background;
    CY_CHECK_NE(hovered, idle);

    CY_CHECK(kit.toolkit.pointer(PointerInput{over, true, {}, 0}));
    for (u32 frame = 0; frame < 12; ++frame) {
        CY_REQUIRE(kit.frame());
    }
    CY_CHECK(has_flag(kit.store.flags(button), ElementFlags::Pressed));
    CY_CHECK_NE(kit.store.paint(button)->background, hovered);
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Clicked), 0U);  // not until released
    CY_CHECK(kit.toolkit.pointer(PointerInput{over, false, {}, 0}));
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Clicked, button), 1U);
    // A pointer click focuses without the ring: the ring is for keyboards and gamepads.
    CY_CHECK_EQ(kit.toolkit.focused(), button);
    CY_CHECK_FALSE(kit.toolkit.focus_visible());

    // A press dragged off and released elsewhere is not a click.
    kit.toolkit.clear_events();
    CY_CHECK(kit.toolkit.pointer(PointerInput{over, true, {}, 0}));
    (void)kit.toolkit.pointer(PointerInput{Vec2{700.0F, 500.0F}, true, {}, 0});
    (void)kit.toolkit.pointer(PointerInput{Vec2{700.0F, 500.0F}, false, {}, 0});
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Clicked), 0U);
    // Over nothing, the pointer is the game's.
    CY_CHECK_FALSE(kit.toolkit.pointer(PointerInput{Vec2{700.0F, 500.0F}, false, {}, 0}));
}

CY_TEST_CASE("ui_toolkit: a slider is dragged by the pointer and stepped by a gamepad") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::Slider;
    desc.minimum = 0.0F;
    desc.maximum = 100.0F;
    desc.step = 5.0F;
    auto slider = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(slider.has_value());
    CY_REQUIRE(kit.toolkit.set_anchors(*slider, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{214, 24})
                   .has_value());
    CY_REQUIRE(kit.frame());
    // The track's 200 units run from x = 7 to 207: pressing at 57 is a quarter of the way.
    CY_CHECK(kit.toolkit.pointer(PointerInput{Vec2{57.0F, 12.0F}, true, {}, 0}));
    CY_CHECK_EQ(kit.toolkit.value(*slider), 25.0F);
    // Dragged well past the end it keeps the pointer — capture — and clamps.
    CY_CHECK(kit.toolkit.pointer(PointerInput{Vec2{600.0F, 300.0F}, true, {}, 0}));
    CY_CHECK_EQ(kit.toolkit.value(*slider), 100.0F);
    (void)kit.toolkit.pointer(PointerInput{Vec2{600.0F, 300.0F}, false, {}, 0});
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Changed, *slider), 2U);
    // Focused, left and right step it; up and down still navigate.
    CY_CHECK(kit.toolkit.action(UiAction::NavigateLeft));
    CY_CHECK_EQ(kit.toolkit.value(*slider), 95.0F);
}

CY_TEST_CASE("ui_toolkit: the tab order walks the widgets in document order, and wraps") {
    Kit kit;
    CY_REQUIRE(kit.started);
    Grid grid(kit);
    CY_REQUIRE(kit.frame());
    WidgetId visited[7];
    for (WidgetId& seen : visited) {
        CY_CHECK(kit.toolkit.action(UiAction::FocusNext));
        seen = kit.toolkit.focused();
    }
    for (u32 index = 0; index < 6; ++index) {
        CY_CHECK_EQ(visited[index], grid.buttons[index]);
    }
    CY_CHECK_EQ(visited[6], grid.buttons[0]);  // wrapped
    CY_CHECK(kit.toolkit.action(UiAction::FocusPrevious));
    CY_CHECK_EQ(kit.toolkit.focused(), grid.buttons[5]);
    CY_CHECK(kit.toolkit.focus_visible());
    // Six focus changes raised six gains and six losses' worth of events, paired.
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::FocusGained),
                kit.count_events(WidgetEventKind::FocusLost) + 1U);
}

CY_TEST_CASE("ui_toolkit: a gamepad navigates by direction, and the ring follows focus") {
    Kit kit;
    CY_REQUIRE(kit.started);
    Grid grid(kit);
    CY_REQUIRE(kit.frame());
    // The first press with nothing focused focuses the first control.
    CY_CHECK(kit.toolkit.action(UiAction::NavigateDown));
    CY_CHECK_EQ(kit.toolkit.focused(), grid.buttons[0]);
    CY_CHECK(kit.toolkit.action(UiAction::NavigateRight));
    CY_CHECK_EQ(kit.toolkit.focused(), grid.buttons[1]);
    CY_CHECK(kit.toolkit.action(UiAction::NavigateDown));
    CY_CHECK_EQ(kit.toolkit.focused(), grid.buttons[4]);
    CY_CHECK(kit.toolkit.action(UiAction::NavigateLeft));
    CY_CHECK_EQ(kit.toolkit.focused(), grid.buttons[3]);
    // At the edge, nothing further that way: focus stays, and the press is still the interface's.
    CY_CHECK(kit.toolkit.action(UiAction::NavigateLeft));
    CY_CHECK_EQ(kit.toolkit.focused(), grid.buttons[3]);
    CY_REQUIRE(kit.frame());
    const ElementId ring = kit.store.roots()[1];
    (void)ring;
    // The ring: three units out from the focused button, on top of everything.
    const ui::Rect focused = kit.rect(grid.buttons[3]);
    bool found = false;
    for (const ElementId root : kit.store.roots()) {
        Array<ElementId> children(allocator());
        CY_REQUIRE(kit.store.children_of(root, children).has_value());
        for (const ElementId child : children.span()) {
            if (kit.store.type_of(child).text() == std::string_view("focus-ring")) {
                found = true;
                CY_CHECK(has_flag(kit.store.flags(child), ElementFlags::Visible));
                CY_CHECK_EQ(kit.rect(child).x, focused.x - 3.0F);
                CY_CHECK_EQ(kit.rect(child).width, focused.width + 6.0F);
                CY_CHECK_GT(kit.store.paint(child)->border_width, 0.0F);
            }
        }
    }
    CY_CHECK(found);
    // A declared neighbour wins over geometry.
    CY_REQUIRE(kit.toolkit.set_neighbour(grid.buttons[3], UiAction::NavigateUp, grid.buttons[2])
                   .has_value());
    CY_CHECK(kit.toolkit.action(UiAction::NavigateUp));
    CY_CHECK_EQ(kit.toolkit.focused(), grid.buttons[2]);
    // Accept is a click.
    CY_CHECK(kit.toolkit.action(UiAction::Accept));
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Clicked, grid.buttons[2]), 1U);
}

CY_TEST_CASE(
    "ui_toolkit: a modal dialog takes focus, keeps it, blocks what is under it, and gives it "
    "back") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId settings = kit.make(row, WidgetKind::Button, "settings", "Settings");
    WidgetDesc desc;
    desc.kind = WidgetKind::Dialog;
    desc.id = "confirm";
    desc.title = "Quit?";
    auto dialog = kit.toolkit.create(row, desc);
    CY_REQUIRE(dialog.has_value());
    const WidgetId yes = kit.make(*dialog, WidgetKind::Button, "yes", "Yes");
    const WidgetId no = kit.make(*dialog, WidgetKind::Button, "no", "No");
    CY_REQUIRE(kit.toolkit.focus(settings).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.toolkit.find(kNoElement, "no"), no);
    CY_CHECK_EQ(kit.toolkit.find(row, "yes"), yes);

    CY_REQUIRE(kit.toolkit.open(*dialog).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK(kit.toolkit.is_open(*dialog));
    CY_CHECK(kit.toolkit.captures_input());
    CY_CHECK_EQ(kit.toolkit.focused(), yes);
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Opened, *dialog), 1U);
    // Focus does not leave the dialog, by navigation or by tab order.
    for (u32 step = 0; step < 4; ++step) {
        CY_CHECK(kit.toolkit.action(UiAction::FocusNext));
        const WidgetId now = kit.toolkit.focused();
        CY_CHECK((now == yes || now == no));
    }
    CY_CHECK_FALSE(kit.toolkit.focus(settings).has_value());
    // A click on the button beneath does nothing and is consumed: the modal blocks it.
    kit.toolkit.clear_events();
    CY_CHECK(kit.toolkit.pointer(PointerInput{kit.centre(settings), true, {}, 0}));
    CY_CHECK(kit.toolkit.pointer(PointerInput{kit.centre(settings), false, {}, 0}));
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Clicked), 0U);
    // Any action is the interface's while the modal is up.
    CY_CHECK(kit.toolkit.action(UiAction::Context));
    // Cancel closes it, and focus is back on the button that opened it.
    CY_CHECK(kit.toolkit.action(UiAction::Cancel));
    CY_CHECK_FALSE(kit.toolkit.is_open(*dialog));
    CY_CHECK_EQ(kit.toolkit.focused(), settings);
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Closed, *dialog), 1U);
    CY_CHECK_FALSE(kit.toolkit.captures_input());
    // With nothing modal, an action nothing handles is the game's.
    CY_CHECK_FALSE(kit.toolkit.action(UiAction::Context));
}

CY_TEST_CASE("ui_toolkit: a drop-down opens a list in a layer of its own and picks from it") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::Dropdown;
    desc.id = "quality";
    const std::string_view items[] = {"Low", "Medium", "High"};
    desc.items = Span<const std::string_view>(items, 3);
    auto dropdown = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(dropdown.has_value());
    CY_REQUIRE(
        kit.toolkit.set_anchors(*dropdown, Vec2{0, 0}, Vec2{0, 0}, Vec2{20, 20}, Vec2{180, 44})
            .has_value());
    CY_REQUIRE(kit.toolkit.set_selected(*dropdown, 1).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.text.text_of(kit.toolkit.part(*dropdown, "dropdown-label")),
                std::string_view("Medium"));
    CY_REQUIRE(kit.toolkit.focus(*dropdown).has_value());
    CY_CHECK(kit.toolkit.action(UiAction::Accept));
    CY_REQUIRE(kit.frame());
    CY_CHECK(kit.toolkit.is_open(*dropdown));
    // The selected option has focus; down moves to the next, and accepting it picks it and closes.
    const WidgetId first = kit.toolkit.focused();
    CY_CHECK_EQ(kit.toolkit.kind(first), WidgetKind::Option);
    CY_CHECK_EQ(kit.toolkit.text(first), std::string_view("Medium"));
    // The list sits under the drop-down.
    CY_CHECK_GE(kit.rect(first).y, kit.rect(*dropdown).bottom());
    CY_CHECK(kit.toolkit.action(UiAction::NavigateDown));
    CY_CHECK_EQ(kit.toolkit.text(kit.toolkit.focused()), std::string_view("High"));
    CY_CHECK(kit.toolkit.action(UiAction::Accept));
    CY_CHECK_FALSE(kit.toolkit.is_open(*dropdown));
    CY_CHECK_EQ(kit.toolkit.selected(*dropdown), 2U);
    CY_CHECK_EQ(kit.toolkit.focused(), *dropdown);
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Changed, *dropdown), 1U);
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.text.text_of(kit.toolkit.part(*dropdown, "dropdown-label")),
                std::string_view("High"));
    // Opened again and dismissed by a click outside: closed, nothing chosen, the click consumed.
    CY_REQUIRE(kit.toolkit.open(*dropdown).has_value());
    CY_REQUIRE(kit.frame());
    kit.toolkit.clear_events();
    CY_CHECK(kit.toolkit.pointer(PointerInput{Vec2{700.0F, 500.0F}, true, {}, 0}));
    CY_CHECK_FALSE(kit.toolkit.is_open(*dropdown));
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Changed), 0U);
}

CY_TEST_CASE("ui_toolkit: tabs switch by click and by the shoulder buttons, one page at a time") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId tabs = kit.make(kNoElement, WidgetKind::Tabs, "tabs");
    CY_REQUIRE(kit.toolkit.set_anchors(tabs, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{400, 300})
                   .has_value());
    WidgetDesc page;
    page.kind = WidgetKind::Column;
    page.title = "Video";
    auto video = kit.toolkit.create(tabs, page);
    page.title = "Audio";
    auto audio = kit.toolkit.create(tabs, page);
    CY_REQUIRE(video.has_value());
    CY_REQUIRE(audio.has_value());
    const WidgetId volume = kit.make(*audio, WidgetKind::Button, "volume", "Volume");
    CY_REQUIRE(kit.frame());
    CY_CHECK(kit.toolkit.visible(*video));
    CY_CHECK_FALSE(kit.toolkit.visible(*audio));
    const ElementId bar = kit.toolkit.part(tabs, "tab-bar");
    Array<ElementId> buttons(allocator());
    CY_REQUIRE(kit.store.children_of(bar, buttons).has_value());
    CY_REQUIRE_EQ(buttons.size(), 2U);
    CY_CHECK_EQ(kit.toolkit.text(buttons[1]), std::string_view("Audio"));
    // A control on a hidden page is not a place focus may go.
    CY_CHECK_FALSE(kit.toolkit.focus(volume).has_value());
    CY_CHECK(kit.toolkit.action(UiAction::NextTab));
    CY_CHECK_EQ(kit.toolkit.selected(tabs), 1U);
    CY_CHECK(kit.toolkit.visible(*audio));
    CY_CHECK_FALSE(kit.toolkit.visible(*video));
    CY_CHECK(kit.toolkit.checked(buttons[1]));
    CY_CHECK(kit.toolkit.action(UiAction::NextTab));  // wraps
    CY_CHECK_EQ(kit.toolkit.selected(tabs), 0U);
    CY_REQUIRE(kit.frame());
    kit.click(kit.centre(buttons[1]));
    CY_CHECK_EQ(kit.toolkit.selected(tabs), 1U);
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Changed, tabs), 3U);
}

CY_TEST_CASE("ui_toolkit: a scroll view clips, scrolls by wheel and by focus moving into it") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId view = kit.make(kNoElement, WidgetKind::ScrollView, "scroll");
    CY_REQUIRE(kit.toolkit.set_anchors(view, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{200, 100})
                   .has_value());
    WidgetId rows[10];
    for (WidgetId& row : rows) {
        row = kit.make(view, WidgetKind::Button, {}, "Row");
    }
    CY_REQUIRE(kit.frame());
    // Ten 24-unit buttons and nine 6-unit gaps: 294 units in a window the theme's padding of four
    // above and below makes 92 units tall, from y = 4 to 96.
    const ElementId port = kit.toolkit.part(view, "scroll-viewport");
    CY_CHECK(has_flag(kit.store.flags(port), ElementFlags::ClipsChildren));
    CY_CHECK_EQ(kit.store.layout_output(rows[9])->clip.bottom(), 96.0F);
    CY_CHECK(kit.toolkit.pointer(PointerInput{Vec2{50.0F, 50.0F}, false, Vec2{0.0F, 1.0F}, 0}));
    CY_CHECK_EQ(kit.toolkit.scroll(view), 40.0F);
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.rect(rows[0]).y, 4.0F - 40.0F);
    // Not past the end.
    (void)kit.toolkit.pointer(PointerInput{Vec2{50.0F, 50.0F}, false, Vec2{0.0F, 50.0F}, 0});
    CY_CHECK_EQ(kit.toolkit.scroll(view), 294.0F - 92.0F);
    CY_REQUIRE(kit.toolkit.set_scroll(view, 0.0F).has_value());
    CY_REQUIRE(kit.frame());
    // Focus moved to the last row brings it into view.
    CY_REQUIRE(kit.toolkit.focus(rows[9]).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_LE(kit.rect(rows[9]).bottom(), 96.0F + 0.01F);
    CY_CHECK_GE(kit.rect(rows[9]).y, 4.0F);
}

CY_TEST_CASE("ui_toolkit: a tooltip shows after the pointer rests, beside it, and hides on leave") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId button = kit.make(row, WidgetKind::Button, "save", "Save");
    CY_REQUIRE(kit.toolkit.set_tooltip(button, "Save the game").has_value());
    CY_REQUIRE(kit.frame());
    ElementId tooltip;
    Array<ElementId> overlay(allocator());
    CY_REQUIRE(kit.store.children_of(kit.store.roots()[1], overlay).has_value());
    for (const ElementId element : overlay.span()) {
        if (kit.store.type_of(element).text() == std::string_view("tooltip")) {
            tooltip = element;
        }
    }
    CY_REQUIRE(tooltip.is_valid());
    const Vec2 over = kit.centre(button);
    (void)kit.toolkit.pointer(PointerInput{over, false, {}, 0});
    CY_REQUIRE(kit.frame(0.2F));
    CY_CHECK_FALSE(has_flag(kit.store.flags(tooltip), ElementFlags::Visible));
    CY_REQUIRE(kit.frame(0.4F));
    CY_CHECK(has_flag(kit.store.flags(tooltip), ElementFlags::Visible));
    CY_CHECK_EQ(kit.rect(tooltip).x, over.x + 12.0F);
    (void)kit.toolkit.pointer(PointerInput{Vec2{700.0F, 500.0F}, false, {}, 0});
    CY_REQUIRE(kit.frame());
    CY_CHECK_FALSE(has_flag(kit.store.flags(tooltip), ElementFlags::Visible));
}
