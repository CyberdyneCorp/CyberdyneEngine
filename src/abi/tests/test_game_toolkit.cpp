// SPDX-License-Identifier: MIT
// ABI 1.12's widget toolkit entries against a fake backend: what each thunk checks, and that a
// backend with no toolkit answers UNAVAILABLE. The real toolkit behind them is
// integration.game_backend_ui's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

constexpr CyUiElement kRoot = 0x0000000100000000ULL;
constexpr CyUiElement kWidget = 0x0000000100000009ULL;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

/// A 1.6 backend with no toolkit: every 1.12 virtual is the UNAVAILABLE default.
class Bare final : public cy::abi::game::UiBackend {
public:
    [[nodiscard]] CyUiElement root() const noexcept override { return kRoot; }
    CyResult create(CyUiElement, const CyUiElementDesc&, CyUiElement&) noexcept override {
        return CY_RESULT_OK;
    }
    CyResult destroy(CyUiElement) noexcept override { return CY_RESULT_OK; }
    CyResult set_layout(CyUiElement, const CyUiLayout&) noexcept override { return CY_RESULT_OK; }
    CyResult set_style(CyUiElement, const CyUiStyle&) noexcept override { return CY_RESULT_OK; }
    CyResult set_text(CyUiElement, const char*, u32, u32) noexcept override { return CY_RESULT_OK; }
    CyResult set_image(CyUiElement, u32, const f32 (&)[4]) noexcept override {
        return CY_RESULT_OK;
    }
    CyResult set_progress(CyUiElement, f32) noexcept override { return CY_RESULT_OK; }
    CyResult set_visibility(CyUiElement, CyUiVisibility) noexcept override { return CY_RESULT_OK; }
    CyResult set_opacity(CyUiElement, f32) noexcept override { return CY_RESULT_OK; }
    CyResult rect(CyUiElement, f32 (&)[4]) const noexcept override { return CY_RESULT_OK; }
    [[nodiscard]] CyUiElement hit_test(f32, f32) const noexcept override {
        return CY_UI_ELEMENT_NULL;
    }
    [[nodiscard]] CyUiElement focus() const noexcept override { return CY_UI_ELEMENT_NULL; }
    CyResult set_focus(CyUiElement) noexcept override { return CY_RESULT_OK; }
};

/// Records what reached it. Only `kWidget` exists.
class FakeToolkit final : public cy::abi::game::UiBackend {
public:
    CyUiWidgetDesc widget{};
    std::vector<std::string> items;
    std::string text = "pur\xC3\xA9";  // five bytes, the last code point two
    std::string sheet;
    std::string classes;
    std::string document;
    std::string path;
    f32 value = 0.0F;
    u32 selected = 0;
    bool enabled = true;
    bool open = false;
    CyUiAnimation animation{};
    u32 action = 99;
    u32 writes = 0;

    [[nodiscard]] CyUiElement root() const noexcept override { return kRoot; }
    CyResult create(CyUiElement, const CyUiElementDesc&, CyUiElement&) noexcept override {
        return CY_RESULT_OK;
    }
    CyResult destroy(CyUiElement) noexcept override { return CY_RESULT_OK; }
    CyResult set_layout(CyUiElement, const CyUiLayout&) noexcept override { return CY_RESULT_OK; }
    CyResult set_style(CyUiElement, const CyUiStyle&) noexcept override { return CY_RESULT_OK; }
    CyResult set_text(CyUiElement, const char*, u32, u32) noexcept override { return CY_RESULT_OK; }
    CyResult set_image(CyUiElement, u32, const f32 (&)[4]) noexcept override {
        return CY_RESULT_OK;
    }
    CyResult set_progress(CyUiElement, f32) noexcept override { return CY_RESULT_OK; }
    CyResult set_visibility(CyUiElement, CyUiVisibility) noexcept override { return CY_RESULT_OK; }
    CyResult set_opacity(CyUiElement, f32) noexcept override { return CY_RESULT_OK; }
    CyResult rect(CyUiElement, f32 (&)[4]) const noexcept override { return CY_RESULT_OK; }
    [[nodiscard]] CyUiElement hit_test(f32, f32) const noexcept override {
        return CY_UI_ELEMENT_NULL;
    }
    [[nodiscard]] CyUiElement focus() const noexcept override { return CY_UI_ELEMENT_NULL; }
    CyResult set_focus(CyUiElement) noexcept override { return CY_RESULT_OK; }

    CyResult load_document(CyUiElement, const CyUiDocumentDesc& desc,
                           CyUiElement& out) noexcept override {
        ++writes;
        if (desc.text != nullptr) {
            document.assign(desc.text, desc.text_size);
        } else {
            path = desc.path;
        }
        out = kWidget;
        return CY_RESULT_OK;
    }
    CyResult load_stylesheet(const char* utf8, u64 size) noexcept override {
        ++writes;
        sheet.assign(utf8, size);
        return CY_RESULT_OK;
    }
    CyResult find(CyUiElement, const char* id, CyUiElement& out) const noexcept override {
        out = std::string_view(id) == "volume" ? kWidget : CY_UI_ELEMENT_NULL;
        return CY_RESULT_OK;
    }
    CyResult create_widget(CyUiElement, const CyUiWidgetDesc& desc,
                           CyUiElement& out) noexcept override {
        ++writes;
        widget = desc;
        items.clear();
        for (u32 index = 0; index < desc.item_count; ++index) {
            items.emplace_back(desc.items[index]);
        }
        out = kWidget;
        return CY_RESULT_OK;
    }
    CyResult widget_set_text(CyUiElement element, const char* utf8) noexcept override {
        text = utf8;
        return known(element);
    }
    CyResult widget_text(CyUiElement element, const char*& out, u32& size) const noexcept override {
        out = text.data();
        size = static_cast<u32>(text.size());
        return element == kWidget ? CY_RESULT_OK
                                  : cy::abi::report(CY_RESULT_NOT_FOUND, "no such widget");
    }
    CyResult widget_set_value(CyUiElement element, f32 next) noexcept override {
        value = next;
        return known(element);
    }
    CyResult widget_value(CyUiElement element, f32& out) const noexcept override {
        out = value;
        return element == kWidget ? CY_RESULT_OK
                                  : cy::abi::report(CY_RESULT_NOT_FOUND, "no such widget");
    }
    CyResult widget_set_selected(CyUiElement element, u32 index) noexcept override {
        selected = index;
        return known(element);
    }
    CyResult widget_selected(CyUiElement element, u32& out) const noexcept override {
        out = selected;
        return element == kWidget ? CY_RESULT_OK
                                  : cy::abi::report(CY_RESULT_NOT_FOUND, "no such widget");
    }
    CyResult widget_set_items(CyUiElement element, const char* const* list,
                              u32 count) noexcept override {
        items.assign(list, list + count);
        return known(element);
    }
    CyResult widget_set_enabled(CyUiElement element, bool on) noexcept override {
        enabled = on;
        return known(element);
    }
    CyResult widget_set_classes(CyUiElement element, const char* names) noexcept override {
        classes = names;
        return known(element);
    }
    CyResult widget_open(CyUiElement element, bool opened) noexcept override {
        open = opened;
        return known(element);
    }
    CyResult widget_animate(CyUiElement element, const CyUiAnimation& started) noexcept override {
        animation = started;
        return known(element);
    }
    CyResult navigate(u32 delivered, bool& consumed) noexcept override {
        action = delivered;
        consumed = delivered == CY_UI_ACTION_ACCEPT;
        return CY_RESULT_OK;
    }

private:
    CyResult known(CyUiElement element) noexcept {
        ++writes;
        return element == kWidget ? CY_RESULT_OK
                                  : cy::abi::report(CY_RESULT_NOT_FOUND, "no such widget");
    }
};

template <class Backend>
struct Bound {
    cy::abi::Host host{allocator()};
    Backend ui;
    Bound() noexcept { host.game.ui = &ui; }
};

}  // namespace

CY_TEST_CASE("the widget entries are unavailable without a toolkit, and presentation-only") {
    const CyInterface& iface = table();
    Bound<Bare> bare;
    CyUiElement out = CY_UI_ELEMENT_NULL;
    CyUiWidgetDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.kind = CY_UI_WIDGET_BUTTON;
    CY_CHECK_EQ(iface.ui_create_widget(&bare.host, CY_UI_ELEMENT_NULL, &desc, &out),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "toolkit") != nullptr);
    u32 consumed = 7;
    CY_CHECK_EQ(iface.ui_navigate(&bare.host, CY_UI_ACTION_ACCEPT, &consumed),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.ui_load_stylesheet(&bare.host, "x {}", 4), CY_RESULT_UNAVAILABLE);

    Bound<FakeToolkit> bound;
    {
        const cy::abi::game::PhaseScope fixed(bound.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(iface.ui_create_widget(&bound.host, CY_UI_ELEMENT_NULL, &desc, &out),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.ui_widget_set_value(&bound.host, kWidget, 1.0F),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.ui_navigate(&bound.host, CY_UI_ACTION_ACCEPT, &consumed),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(bound.ui.writes, 0U);
    }
    cy::abi::Host unbound(allocator());
    CY_CHECK_EQ(iface.ui_find(&unbound, CY_UI_ELEMENT_NULL, "a", &out), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.ui_find(nullptr, CY_UI_ELEMENT_NULL, "a", &out), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("ui_create_widget passes a whole description and refuses what no widget can be") {
    const CyInterface& iface = table();
    Bound<FakeToolkit> bound;
    const char* items[] = {"Low", "High"};
    CyUiWidgetDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.kind = CY_UI_WIDGET_DROPDOWN;
    desc.id = "quality";
    desc.items = items;
    desc.item_count = 2;
    desc.flags = CY_UI_WIDGET_DISABLED;
    desc.owner = 42;
    CyUiElement out = CY_UI_ELEMENT_NULL;
    CY_REQUIRE_EQ(iface.ui_create_widget(&bound.host, CY_UI_ELEMENT_NULL, &desc, &out),
                  CY_RESULT_OK);
    CY_CHECK_EQ(out, kWidget);
    CY_CHECK_EQ(bound.ui.widget.kind, CY_UI_WIDGET_DROPDOWN);
    CY_CHECK_EQ(bound.ui.widget.owner, 42U);
    CY_REQUIRE_EQ(bound.ui.items.size(), 2U);
    CY_CHECK_EQ(bound.ui.items[1], "High");

    // A description from an older module — just the kind — is read whole, zeros after it.
    struct Short {
        u32 struct_size;
        u32 kind;
    } older{sizeof(Short), CY_UI_WIDGET_SLIDER};
    CY_REQUIRE_EQ(iface.ui_create_widget(&bound.host, CY_UI_ELEMENT_NULL,
                                         reinterpret_cast<const CyUiWidgetDesc*>(&older), &out),
                  CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.widget.kind, CY_UI_WIDGET_SLIDER);
    CY_CHECK(bound.ui.widget.id == nullptr);

    const auto refused = [&iface, &bound](CyUiWidgetDesc bad) {
        CyUiElement ignored = CY_UI_ELEMENT_NULL;
        return iface.ui_create_widget(&bound.host, CY_UI_ELEMENT_NULL, &bad, &ignored) ==
               CY_RESULT_INVALID_ARGUMENT;
    };
    CyUiWidgetDesc bad = desc;
    bad.kind = CY_UI_WIDGET_DIALOG + 1U;
    CY_CHECK(refused(bad));
    bad = desc;
    bad.kind = CY_UI_WIDGET_IMAGE;  // a document's, not this entry's
    CY_CHECK(refused(bad));
    bad = desc;
    bad.flags = 0x80U;
    CY_CHECK(refused(bad));
    bad = desc;
    bad.value = kNaN;
    CY_CHECK(refused(bad));
    bad = desc;
    bad.step = -1.0F;
    CY_CHECK(refused(bad));
    bad = desc;
    bad.items = nullptr;
    CY_CHECK(refused(bad));
    const char* holes[] = {"Low", nullptr};
    bad = desc;
    bad.items = holes;
    CY_CHECK(refused(bad));
    CY_CHECK_EQ(iface.ui_create_widget(&bound.host, CY_UI_ELEMENT_NULL, nullptr, &out),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("a document is its text or its path, and a sheet is its bytes") {
    const CyInterface& iface = table();
    Bound<FakeToolkit> bound;
    const std::string_view text = "cyui 1\ncolumn #menu\n";
    CyUiDocumentDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.text = text.data();
    desc.text_size = text.size();
    CyUiElement root = CY_UI_ELEMENT_NULL;
    CY_REQUIRE_EQ(iface.ui_load_document(&bound.host, CY_UI_ELEMENT_NULL, &desc, &root),
                  CY_RESULT_OK);
    CY_CHECK_EQ(root, kWidget);
    CY_CHECK_EQ(bound.ui.document, text);
    desc.text = nullptr;
    desc.path = "ui/menu.cyui";
    CY_REQUIRE_EQ(iface.ui_load_document(&bound.host, CY_UI_ELEMENT_NULL, &desc, &root),
                  CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.path, "ui/menu.cyui");
    // Both, neither, or a reserved word set: refused.
    desc.text = text.data();
    CY_CHECK_EQ(iface.ui_load_document(&bound.host, CY_UI_ELEMENT_NULL, &desc, &root),
                CY_RESULT_INVALID_ARGUMENT);
    desc.text = nullptr;
    desc.path = nullptr;
    CY_CHECK_EQ(iface.ui_load_document(&bound.host, CY_UI_ELEMENT_NULL, &desc, &root),
                CY_RESULT_INVALID_ARGUMENT);
    desc.path = "a";
    desc.reserved = 1;
    CY_CHECK_EQ(iface.ui_load_document(&bound.host, CY_UI_ELEMENT_NULL, &desc, &root),
                CY_RESULT_INVALID_ARGUMENT);
    // A sheet is its bytes, not a C string: the size bounds it.
    CY_REQUIRE_EQ(iface.ui_load_stylesheet(&bound.host, "button {}EXTRA", 9), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.sheet, "button {}");
    CY_CHECK_EQ(iface.ui_load_stylesheet(&bound.host, nullptr, 3), CY_RESULT_INVALID_ARGUMENT);
    CyUiElement found = CY_UI_ELEMENT_NULL;
    CY_REQUIRE_EQ(iface.ui_find(&bound.host, CY_UI_ELEMENT_NULL, "volume", &found), CY_RESULT_OK);
    CY_CHECK_EQ(found, kWidget);
    CY_CHECK_EQ(iface.ui_find(&bound.host, CY_UI_ELEMENT_NULL, nullptr, &found),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("a widget's text comes back whole, or cut at a code point when the buffer is short") {
    const CyInterface& iface = table();
    Bound<FakeToolkit> bound;
    u32 size = 0;
    CY_REQUIRE_EQ(iface.ui_widget_text(&bound.host, kWidget, nullptr, 0, &size), CY_RESULT_OK);
    CY_CHECK_EQ(size, 5U);
    char whole[8] = {};
    CY_REQUIRE_EQ(iface.ui_widget_text(&bound.host, kWidget, whole, sizeof(whole), &size),
                  CY_RESULT_OK);
    CY_CHECK_EQ(std::string_view(whole), "pur\xC3\xA9");
    // Five bytes into a five-byte buffer: four fit, and the fourth is half the é — so three.
    char short_buffer[5] = {'x', 'x', 'x', 'x', 'x'};
    CY_REQUIRE_EQ(iface.ui_widget_text(&bound.host, kWidget, short_buffer, 5, &size), CY_RESULT_OK);
    CY_CHECK_EQ(std::string_view(short_buffer), "pur");
    CY_CHECK_EQ(size, 5U);
    CY_CHECK_EQ(iface.ui_widget_text(&bound.host, kWidget, nullptr, 4, &size),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_widget_text(&bound.host, 3, whole, 8, &size), CY_RESULT_NOT_FOUND);
    CY_REQUIRE_EQ(iface.ui_widget_set_text(&bound.host, kWidget, "Ada"), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.text, "Ada");
    CY_CHECK_EQ(iface.ui_widget_set_text(&bound.host, kWidget, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("values, selections, items, classes, opening and enabling reach the widget checked") {
    const CyInterface& iface = table();
    Bound<FakeToolkit> bound;
    CY_REQUIRE_EQ(iface.ui_widget_set_value(&bound.host, kWidget, 0.25F), CY_RESULT_OK);
    f32 value = 0.0F;
    CY_REQUIRE_EQ(iface.ui_widget_value(&bound.host, kWidget, &value), CY_RESULT_OK);
    CY_CHECK_EQ(value, 0.25F);
    CY_CHECK_EQ(iface.ui_widget_set_value(&bound.host, kWidget, kNaN), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_widget_value(&bound.host, kWidget, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_REQUIRE_EQ(iface.ui_widget_set_selected(&bound.host, kWidget, 3), CY_RESULT_OK);
    u32 selected = 0;
    CY_REQUIRE_EQ(iface.ui_widget_selected(&bound.host, kWidget, &selected), CY_RESULT_OK);
    CY_CHECK_EQ(selected, 3U);
    const char* items[] = {"a", "b", "c"};
    CY_REQUIRE_EQ(iface.ui_widget_set_items(&bound.host, kWidget, items, 3), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.items.size(), 3U);
    CY_REQUIRE_EQ(iface.ui_widget_set_items(&bound.host, kWidget, nullptr, 0), CY_RESULT_OK);
    CY_CHECK(bound.ui.items.empty());
    CY_CHECK_EQ(iface.ui_widget_set_items(&bound.host, kWidget, nullptr, 2),
                CY_RESULT_INVALID_ARGUMENT);
    CY_REQUIRE_EQ(iface.ui_widget_set_classes(&bound.host, kWidget, "danger wide"), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.classes, "danger wide");
    CY_REQUIRE_EQ(iface.ui_widget_set_classes(&bound.host, kWidget, nullptr), CY_RESULT_OK);
    CY_CHECK(bound.ui.classes.empty());
    CY_REQUIRE_EQ(iface.ui_widget_open(&bound.host, kWidget, 1), CY_RESULT_OK);
    CY_CHECK(bound.ui.open);
    CY_REQUIRE_EQ(iface.ui_widget_set_enabled(&bound.host, kWidget, 0), CY_RESULT_OK);
    CY_CHECK_FALSE(bound.ui.enabled);
    CY_CHECK_EQ(iface.ui_widget_open(&bound.host, 77, 1), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("an animation is checked whole, and navigation answers whether it was consumed") {
    const CyInterface& iface = table();
    Bound<FakeToolkit> bound;
    CyUiAnimation fade{};
    fade.struct_size = sizeof(fade);
    fade.property = CY_UI_ANIMATE_OPACITY;
    fade.to = 1.0F;
    fade.duration = 0.25F;
    fade.easing = CY_UI_EASING_SPRING;
    CY_REQUIRE_EQ(iface.ui_widget_animate(&bound.host, kWidget, &fade), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.animation.easing, CY_UI_EASING_SPRING);
    const auto refused = [&iface, &bound](CyUiAnimation bad) {
        return iface.ui_widget_animate(&bound.host, kWidget, &bad) == CY_RESULT_INVALID_ARGUMENT;
    };
    CyUiAnimation bad = fade;
    bad.property = CY_UI_ANIMATE_BACKGROUND + 1U;
    CY_CHECK(refused(bad));
    bad = fade;
    bad.easing = CY_UI_EASING_BACK + 1U;
    CY_CHECK(refused(bad));
    bad = fade;
    bad.duration = -1.0F;
    CY_CHECK(refused(bad));
    bad = fade;
    bad.delay = kNaN;
    CY_CHECK(refused(bad));
    bad = fade;
    bad.flags = 0x10U;
    CY_CHECK(refused(bad));

    u32 consumed = 9;
    CY_REQUIRE_EQ(iface.ui_navigate(&bound.host, CY_UI_ACTION_ACCEPT, &consumed), CY_RESULT_OK);
    CY_CHECK_EQ(consumed, 1U);
    CY_REQUIRE_EQ(iface.ui_navigate(&bound.host, CY_UI_ACTION_FOCUS_PREVIOUS, &consumed),
                  CY_RESULT_OK);
    CY_CHECK_EQ(consumed, 0U);
    CY_CHECK_EQ(bound.ui.action, static_cast<u32>(CY_UI_ACTION_FOCUS_PREVIOUS));
    CY_CHECK_EQ(iface.ui_navigate(&bound.host, CY_UI_ACTION_FOCUS_PREVIOUS + 1U, &consumed),
                CY_RESULT_INVALID_ARGUMENT);
    // The answer is optional.
    CY_CHECK_EQ(iface.ui_navigate(&bound.host, CY_UI_ACTION_CANCEL, nullptr), CY_RESULT_OK);
}
