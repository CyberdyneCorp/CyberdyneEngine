// SPDX-License-Identifier: MIT
// `unit.ui_toolkit`: every widget made, laid out and drawn in each of its states.

#include "fixture.h"

#include <cy/ui/paint.h>

#include <cmath>
#include <string_view>

using namespace cy;
using namespace cy::ui;
using namespace cy::ui::toolkit_test;

namespace {

[[nodiscard]] bool has(const ElementStore& store, ElementId element, ElementFlags flag) noexcept {
    return has_flag(store.flags(element), flag);
}

/// The background the store holds for an element, after the cascade, transitions and animation.
[[nodiscard]] u32 background(const ElementStore& store, ElementId element) noexcept {
    const PaintData* paint = store.paint(element);
    return paint != nullptr ? paint->background : 0U;
}

}  // namespace

CY_TEST_CASE("ui_toolkit: every widget kind is made, named for its type, and laid out") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId column = kit.make(kNoElement, WidgetKind::Column, "column");
    CY_REQUIRE(column.is_valid());
    for (u32 index = 0; index < static_cast<u32>(WidgetKind::Radio); ++index) {
        const auto kind = static_cast<WidgetKind>(index);
        WidgetDesc desc;
        desc.kind = kind;
        desc.text = "Item";
        const std::string_view items[] = {"One", "Two", "Three"};
        desc.items = Span<const std::string_view>(items, 3);
        auto made = kit.toolkit.create(column, desc);
        const std::string_view why = made.has_value() ? "" : made.error().message;
        CY_TEST_INFO(std::string_view(widget_kind_name(kind)), ": ", why);
        CY_REQUIRE(made.has_value());
        CY_CHECK_EQ(kit.toolkit.kind(*made), kind);
        CY_CHECK_EQ(kit.store.type_of(*made).text(), std::string_view(widget_kind_name(kind)));
        CY_CHECK_EQ(widget_kind_from_name(widget_kind_name(kind)), kind);
    }
    CY_REQUIRE(kit.frame());
    // Every kind but the dialog, which waits closed in the overlay, takes room in the column.
    Array<WidgetId> children(allocator());
    CY_REQUIRE(kit.toolkit.children(column, children).has_value());
    CY_CHECK_EQ(children.size(), static_cast<usize>(WidgetKind::Radio));
    // Every kind with content or padding of its own has a height; an empty container, an image with
    // no size and a closed dialog have none, which is right.
    for (const WidgetId child : children.span()) {
        const WidgetKind kind = kit.toolkit.kind(child);
        const bool sized = kind == WidgetKind::Label || kind == WidgetKind::RichText ||
                           kind == WidgetKind::Button || kind == WidgetKind::Toggle ||
                           kind == WidgetKind::Checkbox || kind == WidgetKind::RadioGroup ||
                           kind == WidgetKind::Slider || kind == WidgetKind::ProgressBar ||
                           kind == WidgetKind::TextField || kind == WidgetKind::Dropdown ||
                           kind == WidgetKind::ListView || kind == WidgetKind::Tabs ||
                           kind == WidgetKind::ScrollView;
        CY_TEST_INFO(std::string_view(widget_kind_name(kind)));
        CY_CHECK_EQ(kit.rect(child).height > 0.0F, sized);
    }
}

CY_TEST_CASE("ui_toolkit: a button is a box around its label, sized by the theme and its text") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId play = kit.make(row, WidgetKind::Button, "play", "Play");
    CY_REQUIRE(play.is_valid());
    CY_REQUIRE(kit.frame());
    // The theme's padding is 10 either side and 4 above and below; the text is four 6-unit cells
    // and one 13-unit line; and its minimum height is 24.
    const ui::Rect box = kit.rect(play);
    CY_CHECK_EQ(box.width, 10.0F + (4.0F * kCharWidth) + 10.0F);
    CY_CHECK_EQ(box.height, 24.0F);
    const ElementId label = kit.toolkit.part(play, "button-label");
    CY_REQUIRE(label.is_valid());
    CY_CHECK_EQ(kit.text.text_of(label), std::string_view("Play"));
    // Centred in the box.
    CY_CHECK_EQ(kit.rect(label).x, box.x + 10.0F);
    CY_CHECK_NEAR(kit.rect(label).y, box.y + ((24.0F - kLineHeight) * 0.5F), 0.01F);
    CY_CHECK(has(kit.store, play, ElementFlags::Focusable));
    CY_CHECK_EQ(kit.store.hit_test_mode(label), HitTestMode::Ignore);
}

CY_TEST_CASE("ui_toolkit: a toggle's knob and a checkbox's mark follow their state") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId column = kit.make(kNoElement, WidgetKind::Column);
    const WidgetId toggle = kit.make(column, WidgetKind::Toggle, "music", "Music");
    const WidgetId box = kit.make(column, WidgetKind::Checkbox, "hints", "Hints");
    CY_REQUIRE(kit.frame());
    const ElementId knob = kit.toolkit.part(toggle, "toggle-knob");
    const ElementId track = kit.toolkit.part(toggle, "toggle-track");
    const ElementId mark = kit.toolkit.part(box, "checkbox-mark");
    CY_CHECK_EQ(kit.rect(knob).x, kit.rect(track).x + 2.0F);
    CY_CHECK_FALSE(has(kit.store, mark, ElementFlags::Visible));
    const u32 off_colour = background(kit.store, track);

    CY_REQUIRE(kit.toolkit.set_checked(toggle, true).has_value());
    CY_REQUIRE(kit.toolkit.set_checked(box, true).has_value());
    // Long enough for the track's 0.12 s transition to finish.
    for (u32 frame = 0; frame < 20; ++frame) {
        CY_REQUIRE(kit.frame());
    }
    CY_CHECK_EQ(kit.rect(knob).right(), kit.rect(track).right() - 2.0F);
    CY_CHECK(has(kit.store, mark, ElementFlags::Visible));
    CY_CHECK(has(kit.store, toggle, ElementFlags::Checked));
    CY_CHECK_NE(background(kit.store, track), off_colour);
    CY_CHECK_EQ(kit.toolkit.value(toggle), 1.0F);
}

CY_TEST_CASE("ui_toolkit: a slider places its thumb and fill at its value, inside its track") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::Slider;
    desc.minimum = 0.0F;
    desc.maximum = 10.0F;
    desc.value = 2.5F;
    auto slider = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(slider.has_value());
    CY_REQUIRE(kit.toolkit.set_anchors(*slider, Vec2{0, 0}, Vec2{0, 0}, Vec2{20, 20},
                                       Vec2{234, 44})
                   .has_value());
    CY_REQUIRE(kit.frame());
    const ui::Rect root = kit.rect(*slider);
    CY_CHECK_EQ(root.width, 214.0F);
    const ui::Rect thumb = kit.rect(kit.toolkit.part(*slider, "slider-thumb"));
    const ui::Rect fill = kit.rect(kit.toolkit.part(*slider, "slider-fill"));
    // The thumb's centre runs from 7 to width - 7: a quarter of the way is 7 + 0.25 * 200.
    CY_CHECK_NEAR(thumb.x + (thumb.width * 0.5F), root.x + 7.0F + 50.0F, 0.01F);
    CY_CHECK_NEAR(fill.right(), root.x + 7.0F + 50.0F, 0.01F);
    CY_REQUIRE(kit.toolkit.set_value(*slider, 99.0F).has_value());
    CY_CHECK_EQ(kit.toolkit.value(*slider), 10.0F);
    CY_REQUIRE(kit.frame());
    CY_CHECK_NEAR(kit.rect(kit.toolkit.part(*slider, "slider-thumb")).right(), root.right(),
                  0.01F);
}

CY_TEST_CASE("ui_toolkit: a progress bar fills its fraction, and an empty one draws no fill") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::ProgressBar;
    desc.value = 0.25F;
    auto bar = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(bar.has_value());
    CY_REQUIRE(kit.toolkit.set_anchors(*bar, Vec2{0, 0}, Vec2{0, 0}, Vec2{0, 0}, Vec2{200, 10})
                   .has_value());
    CY_REQUIRE(kit.frame());
    const ElementId fill = kit.toolkit.part(*bar, "progress-fill");
    CY_CHECK_EQ(kit.rect(fill).width, 50.0F);
    CY_REQUIRE(kit.toolkit.set_value(*bar, 0.0F).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_FALSE(has(kit.store, fill, ElementFlags::Visible));
}

CY_TEST_CASE("ui_toolkit: a radio group is one radio per item, exactly one of them checked") {
    Kit kit;
    CY_REQUIRE(kit.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::RadioGroup;
    const std::string_view items[] = {"Easy", "Normal", "Hard"};
    desc.items = Span<const std::string_view>(items, 3);
    auto group = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(group.has_value());
    CY_REQUIRE(kit.toolkit.set_selected(*group, 2).has_value());
    CY_REQUIRE(kit.frame());
    Array<ElementId> radios(allocator());
    CY_REQUIRE(kit.store.children_of(*group, radios).has_value());
    CY_REQUIRE_EQ(radios.size(), 3U);
    for (usize index = 0; index < 3; ++index) {
        CY_CHECK_EQ(kit.toolkit.kind(radios[index]), WidgetKind::Radio);
        CY_CHECK_EQ(kit.toolkit.checked(radios[index]), index == 2);
        CY_CHECK_EQ(kit.text.text_of(kit.toolkit.part(radios[index], "radio-label")),
                    items[index]);
    }
    // A click on the first selects it and says so, once.
    kit.click(kit.centre(radios[0]));
    CY_CHECK_EQ(kit.toolkit.selected(*group), 0U);
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Changed, *group), 1U);
    CY_CHECK(kit.toolkit.checked(radios[0]));
    CY_CHECK_FALSE(kit.toolkit.checked(radios[2]));
    // Fewer items, fewer radios.
    const std::string_view two[] = {"On", "Off"};
    CY_REQUIRE(kit.toolkit.set_items(*group, Span<const std::string_view>(two, 2)).has_value());
    CY_REQUIRE(kit.store.children_of(*group, radios).has_value());
    CY_CHECK_EQ(radios.size(), 2U);
}

CY_TEST_CASE("ui_toolkit: a label measures its text and an image its natural size") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId label = kit.make(row, WidgetKind::Label, "gold", "Gold 1250");
    WidgetDesc image;
    image.kind = WidgetKind::Image;
    image.image_page = 3;
    image.image_uv = ui::Rect{0.25F, 0.0F, 0.25F, 0.5F};
    image.image_size = Vec2{32.0F, 32.0F};
    auto icon = kit.toolkit.create(row, image);
    CY_REQUIRE(icon.has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.rect(label).width, 9.0F * kCharWidth);
    CY_CHECK_EQ(kit.rect(*icon).width, 32.0F);
    const PaintData* paint = kit.store.paint(*icon);
    CY_CHECK_EQ(paint->material, material_index(BuiltinMaterial::Image));
    CY_CHECK_EQ(paint->atlas, 3U);
    CY_CHECK_EQ(paint->uv.x, 0.25F);
    CY_REQUIRE(kit.toolkit.set_text(label, "Gold 99999").has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.rect(label).width, 10.0F * kCharWidth);
}

CY_TEST_CASE("ui_toolkit: rich text is a run per size and per image, with colours per character") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId rich = kit.make(kNoElement, WidgetKind::RichText, "status",
                                   "Gold [color=#ffd34d]1250[/color] [img=2,0,0,1,1][size=26]x2[/size]");
    CY_REQUIRE(rich.is_valid());
    CY_REQUIRE(kit.frame());
    Array<ElementId> runs(allocator());
    CY_REQUIRE(kit.store.children_of(rich, runs).has_value());
    CY_REQUIRE_EQ(runs.size(), 3U);
    CY_CHECK_EQ(kit.store.type_of(runs[0]).text(), std::string_view("rich-run"));
    CY_CHECK_EQ(kit.text.text_of(runs[0]), std::string_view("Gold 1250 "));
    CY_CHECK_EQ(kit.store.type_of(runs[1]).text(), std::string_view("rich-image"));
    CY_CHECK_EQ(kit.store.paint(runs[1])->atlas, 2U);
    CY_CHECK_EQ(kit.text.text_of(runs[2]), std::string_view("x2"));
    // The 26-unit run is twice the built-in font's line.
    CY_CHECK_EQ(kit.rect(runs[2]).height, 26.0F);

    // The colour span covers "1250" and only it: paint the run and compare its glyphs.
    Array<Primitive> glyphs(allocator());
    CY_REQUIRE(kit.text.paint_content(runs[0], kit.rect(runs[0]), glyphs).has_value());
    u32 gold = 0;
    u32 plain = 0;
    for (const Primitive& glyph : glyphs.span()) {
        (glyph.colour == 0xFFFFD34DU ? gold : plain) += 1;
    }
    CY_CHECK_EQ(gold, 4U);
    CY_CHECK_EQ(plain, 4U);  // "Gold" — the spaces draw no glyph.
}

CY_TEST_CASE("ui_toolkit: disabled widgets take neither focus nor clicks, and say so in style") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId button = kit.make(row, WidgetKind::Button, "quit", "Quit");
    CY_REQUIRE(kit.toolkit.set_enabled(button, false).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK(has(kit.store, button, ElementFlags::Disabled));
    // The theme's `button:disabled` fades it.
    CY_CHECK_LT(kit.store.paint(button)->opacity, 1.0F);
    kit.click(kit.centre(button));
    CY_CHECK_EQ(kit.count_events(WidgetEventKind::Clicked), 0U);
    CY_CHECK_FALSE(kit.toolkit.focus(button).has_value());
}
