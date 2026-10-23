// SPDX-License-Identifier: MIT
// `unit.ui_toolkit`: the virtualised list, and widgets bound to game values.

#include "fixture.h"

#include <cstdio>
#include <string_view>

using namespace cy;
using namespace cy::ui;
using namespace cy::ui::toolkit_test;

namespace {

/// An inventory the list asks for rows of: "Item N".
class Inventory final : public ListSource {
public:
    explicit Inventory(u32 count) noexcept : count_(count) {}
    [[nodiscard]] u32 count() const noexcept override { return count_; }
    [[nodiscard]] Status text(u32 index, Array<char>& out) const noexcept override {
        ++asked;
        char text[32] = {};
        const int length = std::snprintf(text, sizeof(text), "Item %u", index);
        return out.append(Span<const char>(text, static_cast<usize>(length)));
    }
    mutable u32 asked = 0;

private:
    u32 count_;
};

/// A game value: a number, a flag or a text the game sets, and a flag for "changed".
class GameValue final : public BindingSource {
public:
    [[nodiscard]] BoundValue read() noexcept override {
        dirty = false;
        return value;
    }
    [[nodiscard]] bool changed() const noexcept override { return dirty; }
    [[nodiscard]] Status write(const BoundValue& next) noexcept override {
        value = next;
        ++writes;
        return ok();
    }
    void set_number(f32 number) noexcept {
        value.kind = BoundValue::Kind::Number;
        value.number = number;
        dirty = true;
    }
    BoundValue value;
    bool dirty = false;
    u32 writes = 0;
};

[[nodiscard]] WidgetId make_list(Kit& kit, const Inventory& inventory) noexcept {
    WidgetDesc desc;
    desc.kind = WidgetKind::ListView;
    desc.id = "inventory";
    desc.row_height = 20.0F;
    auto list = kit.toolkit.create(kNoElement, desc);
    if (!list) {
        return kNoElement;
    }
    (void)kit.toolkit.set_anchors(*list, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{200, 100});
    (void)kit.toolkit.set_list_source(*list, &inventory);
    return *list;
}

}  // namespace

CY_TEST_CASE("ui_list: a list realises the rows it can show, whatever it holds") {
    Kit kit;
    CY_REQUIRE(kit.started);
    Inventory inventory(1000);
    const WidgetId list = make_list(kit, inventory);
    CY_REQUIRE(list.is_valid());
    CY_REQUIRE(kit.frame());
    CY_REQUIRE(kit.frame());
    // A 98-unit window over 20-unit rows is five rows visible, a sixth partly, plus two overscan.
    const ElementId content = kit.toolkit.part(list, "list-content");
    Array<ElementId> rows(allocator());
    CY_REQUIRE(kit.store.children_of(content, rows).has_value());
    CY_CHECK_LE(rows.size(), 10U);
    CY_CHECK_LE(inventory.asked, 10U);
    CY_CHECK_EQ(kit.rect(content).height, 1000.0F * 20.0F);
    CY_CHECK_EQ(kit.text.text_of(kit.toolkit.part(rows[0], "list-row-label")),
                std::string_view("Item 0"));

    // Scrolled to the middle: the rows are reused for the items there, and nothing else is asked.
    inventory.asked = 0;
    CY_REQUIRE(kit.toolkit.set_scroll(list, 10000.0F).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_LE(inventory.asked, 10U);
    Array<ElementId> after(allocator());
    CY_REQUIRE(kit.store.children_of(content, after).has_value());
    // The pool is the window's rows and two either side, however far the list scrolled.
    CY_CHECK_LE(after.size(), 10U);
    bool shows_500 = false;
    for (const ElementId row : after.span()) {
        const ui::Rect rect = kit.rect(row);
        if (kit.text.text_of(kit.toolkit.part(row, "list-row-label")) ==
            std::string_view("Item 500")) {
            shows_500 = true;
            CY_CHECK_NEAR(rect.y, kit.rect(list).y + 1.0F, 1.01F);
        }
    }
    CY_CHECK(shows_500);
}

CY_TEST_CASE("ui_list: up and down move the selection, keep it in view, and leave at the ends") {
    Kit kit;
    CY_REQUIRE(kit.started);
    Inventory inventory(50);
    const WidgetId above = kit.make(kNoElement, WidgetKind::Button, "above", "Above");
    CY_REQUIRE(kit.toolkit.set_anchors(above, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{80, 24})
                   .has_value());
    const WidgetId list = make_list(kit, inventory);
    CY_REQUIRE(kit.toolkit.set_anchors(list, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 40}, Vec2{200, 140})
                   .has_value());
    CY_REQUIRE(kit.frame());
    CY_REQUIRE(kit.toolkit.focus(list).has_value());
    for (u32 press = 0; press < 12; ++press) {
        CY_CHECK(kit.toolkit.action(UiAction::NavigateDown));
    }
    CY_CHECK_EQ(kit.toolkit.selected(list), 12U);
    CY_CHECK_EQ(kit.toolkit.focused(), list);
    // Row 12 spans 240 to 260: scrolled so its bottom is at the window's.
    CY_CHECK_GE(kit.toolkit.scroll(list), 260.0F - 100.0F);
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Changed, list), 12U);
    CY_CHECK(kit.toolkit.action(UiAction::Accept));
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Submitted, list), 1U);
    // The selected row draws selected.
    CY_REQUIRE(kit.frame());
    const ElementId content = kit.toolkit.part(list, "list-content");
    Array<ElementId> rows(allocator());
    CY_REQUIRE(kit.store.children_of(content, rows).has_value());
    u32 checked = 0;
    for (const ElementId row : rows.span()) {
        checked += kit.toolkit.checked(row) ? 1U : 0U;
    }
    CY_CHECK_EQ(checked, 1U);
    // From the top, up leaves the list for what is above it.
    CY_REQUIRE(kit.toolkit.set_selected(list, 0).has_value());
    CY_CHECK(kit.toolkit.action(UiAction::NavigateDown));
    CY_CHECK(kit.toolkit.action(UiAction::NavigateUp));
    CY_CHECK_EQ(kit.toolkit.selected(list), 0U);
    CY_CHECK(kit.toolkit.action(UiAction::NavigateUp));
    CY_CHECK_EQ(kit.toolkit.focused(), above);
}

CY_TEST_CASE("ui_list: a click on a row selects it") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::ListView;
    const std::string_view items[] = {"Sword", "Shield", "Potion"};
    desc.items = Span<const std::string_view>(items, 3);
    auto list = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(list.has_value());
    CY_REQUIRE(kit.toolkit.set_anchors(*list, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{200, 100})
                   .has_value());
    CY_REQUIRE(kit.frame());
    CY_REQUIRE(kit.frame());
    // Row 2 is 44 to 66 below the list's top edge and its one-unit border.
    kit.click(Vec2{50.0F, 1.0F + 44.0F + 10.0F});
    CY_CHECK_EQ(kit.toolkit.selected(*list), 2U);
    CY_CHECK_EQ(kit.toolkit.focused(), *list);
    Array<char> text(allocator());
    CY_REQUIRE(kit.toolkit.item_text(*list, 2, text).has_value());
    CY_CHECK_EQ(std::string_view(text.data(), text.size()), std::string_view("Potion"));
}

CY_TEST_CASE("ui_binding: a bound value updates the widget when it changes, and not otherwise") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::ProgressBar;
    auto health = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(health.has_value());
    GameValue value;
    value.set_number(0.75F);
    CY_REQUIRE(kit.toolkit.bind(*health, WidgetProperty::Value, value).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.toolkit.value(*health), 0.75F);
    CY_CHECK_EQ(kit.report.bindings_applied, 1U);
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.report.bindings_applied, 0U);  // unchanged: nothing applied
    value.set_number(0.5F);
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.toolkit.value(*health), 0.5F);
    // A change that is the same value is not an update.
    value.set_number(0.5F);
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.report.bindings_applied, 0U);
}

CY_TEST_CASE("ui_binding: two-way writes the player's edits back, and waits while they edit") {
    // `ui-system`: "editing the field SHALL write back, and external changes SHALL update the field
    // unless it is being edited."
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::Slider;
    desc.maximum = 10.0F;
    desc.step = 1.0F;
    auto slider = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(slider.has_value());
    CY_REQUIRE(kit.toolkit.set_anchors(*slider, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{214, 24})
                   .has_value());
    GameValue volume;
    volume.set_number(4.0F);
    CY_REQUIRE(kit.toolkit.bind(*slider, WidgetProperty::Value, volume, BindingMode::TwoWay)
                   .has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.toolkit.value(*slider), 4.0F);
    // The player drags it to 7: written back.
    CY_CHECK(kit.toolkit.pointer(PointerInput{Vec2{7.0F + 140.0F, 12.0F}, true, {}, 0}));
    CY_CHECK_EQ(volume.value.number, 7.0F);
    CY_CHECK_GE(volume.writes, 1U);
    // The game changes it while the player still holds the thumb: the slider keeps the player's.
    volume.set_number(2.0F);
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.toolkit.value(*slider), 7.0F);
    // Released: the game's change arrives.
    (void)kit.toolkit.pointer(PointerInput{Vec2{7.0F + 140.0F, 12.0F}, false, {}, 0});
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.toolkit.value(*slider), 2.0F);
}

CY_TEST_CASE("ui_binding: a text, a flag and a selection bind by kind") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId column = kit.make(kNoElement, WidgetKind::Column);
    const WidgetId label = kit.make(column, WidgetKind::Label, "gold", "0");
    const WidgetId toggle = kit.make(column, WidgetKind::Toggle, "music", "Music");
    GameValue gold;
    gold.value.kind = BoundValue::Kind::Text;
    gold.value.text = Name::intern("1250");
    gold.dirty = true;
    GameValue music;
    music.value.kind = BoundValue::Kind::Boolean;
    music.value.boolean = true;
    music.dirty = true;
    CY_REQUIRE(kit.toolkit.bind(label, WidgetProperty::Text, gold).has_value());
    CY_REQUIRE(kit.toolkit.bind(toggle, WidgetProperty::Checked, music, BindingMode::TwoWay)
                   .has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.toolkit.text(label), std::string_view("1250"));
    CY_CHECK(kit.toolkit.checked(toggle));
    CY_REQUIRE(kit.toolkit.focus(toggle).has_value());
    CY_CHECK(kit.toolkit.action(UiAction::Accept));
    CY_CHECK_FALSE(music.value.boolean);
    // Unbound, the game's changes stop arriving.
    kit.toolkit.unbind(label);
    gold.value.text = Name::intern("0");
    gold.dirty = true;
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.toolkit.text(label), std::string_view("1250"));
}
