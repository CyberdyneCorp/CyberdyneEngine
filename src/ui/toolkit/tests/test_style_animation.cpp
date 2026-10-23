// SPDX-License-Identifier: MIT
// `unit.ui_toolkit`: sheets and themes over the widgets, state transitions, and animations by time.

#include "fixture.h"

#include <cy/ui/toolkit/animation.h>

#include <string_view>

using namespace cy;
using namespace cy::ui;
using namespace cy::ui::toolkit_test;

namespace {

/// Each channel of `colour` strictly between the same channel of `from` and `to`, wherever the
/// two differ.
[[nodiscard]] bool strictly_between(u32 colour, u32 from, u32 to) noexcept {
    bool any = false;
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        const u32 a = (from >> shift) & 0xFFU;
        const u32 b = (to >> shift) & 0xFFU;
        const u32 c = (colour >> shift) & 0xFFU;
        if (a == b) {
            continue;
        }
        any = true;
        const u32 low = a < b ? a : b;
        const u32 high = a < b ? b : a;
        if (c <= low || c >= high) {
            return false;
        }
    }
    return any;
}

}  // namespace

CY_TEST_CASE("ui_style: a variant resolves from itself first and falls back to the base") {
    // `ui-system`: "WHEN a button declares the variant 'danger' THEN properties SHALL resolve from
    // the danger variant first, falling back to the base button style."
    Kit kit;
    CY_REQUIRE(kit.started);
    CY_REQUIRE(kit.toolkit
                   .set_stylesheet("button { background-color: #203040; corner-radius: 7; }\n"
                                   "button.danger { background-color: #a02020; }\n")
                   .has_value());
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId plain = kit.make(row, WidgetKind::Button, "ok", "OK");
    WidgetDesc desc;
    desc.kind = WidgetKind::Button;
    desc.text = "Delete";
    desc.classes = "danger";
    auto danger = kit.toolkit.create(row, desc);
    CY_REQUIRE(danger.has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.store.paint(plain)->background, 0xFF203040U);
    CY_CHECK_EQ(kit.store.paint(*danger)->background, 0xFFA02020U);
    // Not in the variant: from the base button.
    CY_CHECK_EQ(kit.store.paint(*danger)->corner_radius, 7.0F);
    // And the theme under both: the border width the sheet never mentions.
    CY_CHECK_EQ(kit.store.paint(*danger)->border_width, 1.0F);
}

CY_TEST_CASE("ui_style: a 0.1 s hover transition interpolates, and is strictly between midway") {
    // `ui-system`: "WHEN a button is hovered with a 0.1 s transition THEN its background colour
    // SHALL interpolate rather than switching."
    Kit kit;
    CY_REQUIRE(kit.started);
    CY_REQUIRE(kit.toolkit
                   .set_stylesheet("button { background-color: #000000; transition-duration: 0.1s;"
                                   " transition-timing-function: linear; }\n"
                                   "button:hover { background-color: #c08040; }\n")
                   .has_value());
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId button = kit.make(row, WidgetKind::Button, "go", "Go");
    CY_REQUIRE(kit.frame());
    CY_REQUIRE_EQ(kit.store.paint(button)->background, 0xFF000000U);
    (void)kit.toolkit.pointer(PointerInput{kit.centre(button), false, {}, 0});
    // The frame that sees the hover starts the transition; half of 0.1 s later it is midway.
    CY_REQUIRE(kit.frame(0.0F));
    CY_REQUIRE(kit.frame(0.05F));
    const u32 midway = kit.store.paint(button)->background;
    CY_CHECK(strictly_between(midway, 0xFF000000U, 0xFFC08040U));
    CY_CHECK_EQ(midway, mix_colour(0xFF000000U, 0xFFC08040U, 0.5F));
    CY_REQUIRE(kit.frame(0.06F));
    CY_CHECK_EQ(kit.store.paint(button)->background, 0xFFC08040U);
    // A paint transition relays out nothing.
    CY_CHECK_EQ(kit.report.layout.measured, 0U);
}

CY_TEST_CASE("ui_style: a theme swap restyles everything, and an inline declaration still wins") {
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId a = kit.make(row, WidgetKind::Button, "a", "A");
    const WidgetId b = kit.make(row, WidgetKind::Button, "b", "B");
    CY_REQUIRE(kit.toolkit.set_inline_style(b, "background-color: #123456").has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.store.paint(b)->background, 0xFF123456U);
    CY_REQUIRE(kit.toolkit.set_theme("button { background-color: #ff0000; padding-left: 30; }")
                   .has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.store.paint(a)->background, 0xFFFF0000U);
    CY_CHECK_EQ(kit.store.paint(b)->background, 0xFF123456U);
    // A size that changed relaid out: thirty units of padding on the left now.
    CY_CHECK_EQ(kit.rect(kit.toolkit.part(a, "button-label")).x, kit.rect(a).x + 30.0F);
    // An inline declaration outside the closed property set is refused, by name.
    CY_CHECK_FALSE(kit.toolkit.set_inline_style(a, "float: left").has_value());
}

CY_TEST_CASE("ui_style: a part takes its widget's states, so one selector styles it") {
    Kit kit;
    CY_REQUIRE(kit.started);
    CY_REQUIRE(kit.toolkit.set_stylesheet("slider-thumb:focus { background-color: #00ff00; }")
                   .has_value());
    WidgetDesc desc;
    desc.kind = WidgetKind::Slider;
    auto slider = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(slider.has_value());
    CY_REQUIRE(kit.frame());
    const ElementId thumb = kit.toolkit.part(*slider, "slider-thumb");
    CY_CHECK_NE(kit.store.paint(thumb)->background, 0xFF00FF00U);
    CY_REQUIRE(kit.toolkit.focus(*slider).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.store.paint(thumb)->background, 0xFF00FF00U);
}

CY_TEST_CASE("ui_style: a straight `.cyss` colour with alpha is premultiplied as it is read") {
    // REGRESSION: `parse_colour` said "premultiplied on the way in" and did not premultiply, so a
    // half-transparent red reached the device as full red under half alpha — an additive glow, not
    // a blend.
    StyleSheet sheet(allocator());
    CY_REQUIRE(parse_stylesheet("panel { background-color: #ff000080; }", sheet).has_value());
    CY_REQUIRE_EQ(sheet.declarations().size(), 1U);
    CY_CHECK_EQ(sheet.declarations()[0].value.colour, 0x80800000U);
    // And durations take their CSS units.
    StyleSheet times(allocator());
    CY_REQUIRE(
        parse_stylesheet("panel { transition-duration: 150ms; transition-delay: 0.2s; }", times)
            .has_value());
    CY_REQUIRE(times.diagnostics().empty());
    CY_CHECK_NEAR(times.declarations()[0].value.number, 0.15F, 1e-6F);
    CY_CHECK_NEAR(times.declarations()[1].value.number, 0.2F, 1e-6F);
}

CY_TEST_CASE("ui_animation: every easing starts at zero and ends at one, and moves the right way") {
    for (u32 index = 0; index < static_cast<u32>(Easing::Count); ++index) {
        const auto easing = static_cast<Easing>(index);
        CY_TEST_INFO(std::string_view(easing_name(easing)));
        CY_CHECK_EQ(ease(easing, 0.0F), 0.0F);
        CY_CHECK_EQ(ease(easing, 1.0F), 1.0F);
        CY_CHECK_EQ(easing_from_name(easing_name(easing)), easing);
    }
    CY_CHECK_EQ(ease(Easing::Linear, 0.25F), 0.25F);
    CY_CHECK_LT(ease(Easing::EaseIn, 0.25F), 0.25F);
    CY_CHECK_GT(ease(Easing::EaseOut, 0.25F), 0.25F);
    CY_CHECK_NEAR(ease(Easing::EaseInOut, 0.5F), 0.5F, 1e-3F);
    // A spring and a back curve overshoot before they settle.
    bool overshoots = false;
    for (f32 t = 0.0F; t < 1.0F; t += 0.01F) {
        overshoots = overshoots || ease(Easing::Spring, t) > 1.0F;
    }
    CY_CHECK(overshoots);
    CY_CHECK_GT(ease(Easing::Back, 0.6F), 1.0F);
}

CY_TEST_CASE("ui_animation: a fade, a slide and a pop are sampled by time, and relay out nothing") {
    // `ui-system`: "WHEN an element's opacity or colour animates THEN no layout work SHALL occur
    // for any frame of the animation."
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId panel = kit.make(kNoElement, WidgetKind::Panel, "menu");
    CY_REQUIRE(
        kit.toolkit.set_anchors(panel, Vec2{0, 0}, Vec2{0, 0}, Vec2{100, 100}, Vec2{300, 200})
            .has_value());
    CY_REQUIRE(kit.frame());
    Animation fade;
    fade.property = AnimatedProperty::Opacity;
    fade.from = 0.0F;
    fade.to = 1.0F;
    fade.duration = 0.4F;
    fade.easing = Easing::Linear;
    CY_REQUIRE(kit.toolkit.animate(panel, fade).has_value());
    CY_CHECK_EQ(kit.store.paint(panel)->opacity, 0.0F);  // from the first frame
    CY_REQUIRE(kit.toolkit.slide(panel, Vec2{-80.0F, 0.0F}, 0.4F).has_value());
    for (u32 frame = 1; frame <= 4; ++frame) {
        CY_REQUIRE(kit.frame(0.1F));
        CY_CHECK_EQ(kit.report.layout.measured, 0U);
        CY_CHECK_NEAR(kit.store.paint(panel)->opacity, 0.25F * static_cast<f32>(frame), 1e-5F);
    }
    CY_CHECK_EQ(kit.store.paint(panel)->translate.x, 0.0F);
    CY_CHECK_EQ(kit.toolkit.running_animations(), 0U);
    // The layout never moved: the slide was the render transform.
    CY_CHECK_EQ(kit.rect(panel).x, 100.0F);

    CY_REQUIRE(kit.toolkit.pop(panel, 0.2F).has_value());
    CY_REQUIRE(kit.frame(0.1F));
    CY_CHECK_NE(kit.store.paint(panel)->scale, 1.0F);
    CY_REQUIRE(kit.frame(0.1F));
    CY_CHECK_EQ(kit.store.paint(panel)->scale, 1.0F);
    CY_CHECK_EQ(kit.report.layout.measured, 0U);
}

CY_TEST_CASE("ui_animation: a staggered entrance delays each item by one more step") {
    // `ui-system`: "WHEN a menu opens with a staggered slide-and-fade THEN each item SHALL animate
    // with an incremental delay, evaluated inside the UI system."
    Kit kit;
    CY_REQUIRE(kit.started);
    const WidgetId column = kit.make(kNoElement, WidgetKind::Column);
    WidgetId items[4];
    for (WidgetId& item : items) {
        item = kit.make(column, WidgetKind::Button, {}, "Item");
    }
    CY_REQUIRE(kit.frame());
    Animation fade;
    fade.property = AnimatedProperty::Opacity;
    fade.from = 0.0F;
    fade.to = 1.0F;
    fade.duration = 0.2F;
    fade.easing = Easing::Linear;
    CY_REQUIRE(
        kit.toolkit.animate_staggered(Span<const WidgetId>(items, 4), fade, 0.1F).has_value());
    CY_REQUIRE(kit.frame(0.15F));
    // 0.15 s in: the first is 75 % there, the second 25 %, the third and fourth still waiting.
    CY_CHECK_NEAR(kit.store.paint(items[0])->opacity, 0.75F, 1e-5F);
    CY_CHECK_NEAR(kit.store.paint(items[1])->opacity, 0.25F, 1e-5F);
    CY_CHECK_EQ(kit.store.paint(items[2])->opacity, 0.0F);
    CY_CHECK_EQ(kit.store.paint(items[3])->opacity, 0.0F);
    CY_REQUIRE(kit.frame(0.5F));
    for (const WidgetId item : items) {
        CY_CHECK_EQ(kit.store.paint(item)->opacity, 1.0F);
    }
}

CY_TEST_CASE(
    "ui_animation: reduced motion ends decorative animations at once, keeps state feedback") {
    // `ui-system`: "WHEN the platform reports a reduced-motion preference THEN decorative
    // animations SHALL be shortened or disabled while state feedback remains."
    Kit kit;
    CY_REQUIRE(kit.started);
    kit.toolkit.settings().reduced_motion = true;
    const WidgetId row = kit.make(kNoElement, WidgetKind::Row);
    const WidgetId button = kit.make(row, WidgetKind::Button, "go", "Go");
    CY_REQUIRE(kit.frame());
    CY_REQUIRE(kit.toolkit.fade(button, 0.0F, 1.0F, 2.0F).has_value());
    CY_REQUIRE(kit.frame(0.01F));
    CY_CHECK_EQ(kit.store.paint(button)->opacity, 1.0F);
    // The hover still changes the button — state feedback is not decorative.
    const u32 idle = kit.store.paint(button)->background;
    (void)kit.toolkit.pointer(PointerInput{kit.centre(button), false, {}, 0});
    for (u32 frame = 0; frame < 12; ++frame) {
        CY_REQUIRE(kit.frame());
    }
    CY_CHECK_NE(kit.store.paint(button)->background, idle);
}
