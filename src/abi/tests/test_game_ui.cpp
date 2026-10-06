// SPDX-License-Identifier: MIT
// ABI 1.6's interface entries against a fake backend, and `ui_event` through
// `BehaviourRuntime::ui_event`.
//
// The real interface — elements in a `cy::ui::ElementStore`, a click routed through
// `cy::ui::Interaction` — is the adapter's, and integration.game_backend_ui tests it.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

using cy::f32;
using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

constexpr CyUiElement kRoot = 0x0000000100000000ULL;
constexpr CyUiElement kMade = 0x0000000100000007ULL;

/// Records what reached it. Every element but the root and `kMade` is NOT_FOUND.
class FakeUi final : public cy::abi::game::UiBackend {
public:
    CyUiElementDesc desc{};
    const char* name = "";
    CyUiLayout layout{};
    CyUiStyle style{};
    char text[32] = {};
    u32 colour = 0;
    u32 scale = 0;
    u32 page = 0;
    f32 uv[4] = {};
    f32 progress = -1.0F;
    CyUiVisibility visibility = CY_UI_VISIBLE;
    f32 opacity = -1.0F;
    CyUiElement focused = CY_UI_ELEMENT_NULL;
    u32 writes = 0;

    [[nodiscard]] CyUiElement root() const noexcept override { return kRoot; }
    CyResult create(CyUiElement parent, const CyUiElementDesc& whole,
                    CyUiElement& out) noexcept override {
        if (parent != kRoot) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such parent");
        }
        desc = whole;
        name = whole.name == nullptr ? "" : whole.name;
        out = kMade;
        return CY_RESULT_OK;
    }
    CyResult destroy(CyUiElement element) noexcept override { return known(element); }
    CyResult set_layout(CyUiElement element, const CyUiLayout& value) noexcept override {
        layout = value;
        return known(element);
    }
    CyResult set_style(CyUiElement element, const CyUiStyle& value) noexcept override {
        style = value;
        return known(element);
    }
    CyResult set_text(CyUiElement element, const char* utf8, u32 value,
                      u32 pixel_scale) noexcept override {
        (void)std::snprintf(text, sizeof(text), "%s", utf8);
        colour = value;
        scale = pixel_scale;
        return known(element);
    }
    CyResult set_image(CyUiElement element, u32 atlas_page,
                       const f32 (&rect)[4]) noexcept override {
        page = atlas_page;
        std::memcpy(uv, rect, sizeof(uv));
        return known(element);
    }
    CyResult set_progress(CyUiElement element, f32 value) noexcept override {
        progress = value;
        return known(element);
    }
    CyResult set_visibility(CyUiElement element, CyUiVisibility value) noexcept override {
        visibility = value;
        return known(element);
    }
    CyResult set_opacity(CyUiElement element, f32 value) noexcept override {
        opacity = value;
        return known(element);
    }
    CyResult rect(CyUiElement element, f32 (&out)[4]) const noexcept override {
        if (element != kMade) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such element");
        }
        out[0] = 10.0F;
        out[1] = 20.0F;
        out[2] = 30.0F;
        out[3] = 40.0F;
        return CY_RESULT_OK;
    }
    [[nodiscard]] CyUiElement hit_test(f32 x, f32 y) const noexcept override {
        return x >= 10.0F && x < 40.0F && y >= 20.0F && y < 60.0F ? kMade : CY_UI_ELEMENT_NULL;
    }
    [[nodiscard]] CyUiElement focus() const noexcept override { return focused; }
    CyResult set_focus(CyUiElement element) noexcept override {
        if (element != CY_UI_ELEMENT_NULL && element != kMade) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such element");
        }
        focused = element;
        return CY_RESULT_OK;
    }

private:
    CyResult known(CyUiElement element) noexcept {
        ++writes;
        return element == kMade ? CY_RESULT_OK
                                : cy::abi::report(CY_RESULT_NOT_FOUND, "no such element");
    }
};

/// A host with the fake bound, in the frame phase unless a case says otherwise.
struct Bound {
    cy::abi::Host host{allocator()};
    FakeUi ui;
    Bound() noexcept { host.game.ui = &ui; }
};

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

// --- `ui_event` through the runtime --------------------------------------------------------------

CyUiEvent g_seen[4] = {};
u32 g_seen_count = 0;
CyPhase g_phase_seen = CY_PHASE_NONE;
cy::abi::Host* g_host = nullptr;

extern "C" CyInstance ui_probe_create(CyEngine, CyEntity, void*) {
    static int instance = 0;
    return &instance;
}
extern "C" void ui_probe_destroy(CyInstance, void*) {}
extern "C" void ui_probe_event(CyInstance, const CyUiEvent* event, void*) {
    if (g_seen_count < 4U) {
        g_seen[g_seen_count++] = *event;
    }
    g_phase_seen = g_host != nullptr ? g_host->game.clock.phase : CY_PHASE_NONE;
}

CyBehaviourVTable ui_vtable() noexcept {
    CyBehaviourVTable vtable{};
    vtable.struct_size = sizeof(vtable);
    vtable.create = &ui_probe_create;
    vtable.destroy = &ui_probe_destroy;
    vtable.ui_event = &ui_probe_event;
    return vtable;
}

CyUiEvent click(CyEntity owner) noexcept {
    CyUiEvent event{};
    event.struct_size = sizeof(event);
    event.kind = CY_UI_EVENT_CLICK;
    event.element = kMade;
    event.owner = owner;
    event.position[0] = 12.0F;
    event.position[1] = 24.0F;
    event.button = CY_INPUT_BUTTON_LEFT;
    return event;
}

}  // namespace

CY_TEST_CASE("the 1.6 table appends the interface after every 1.5 entry and ends with it") {
    const CyInterface& iface = table();
    CY_CHECK_GE(iface.header.abi_minor, 6U);
    const bool entries[] = {
        iface.ui_root != nullptr,           iface.ui_create != nullptr,
        iface.ui_destroy != nullptr,        iface.ui_set_layout != nullptr,
        iface.ui_set_style != nullptr,      iface.ui_set_text != nullptr,
        iface.ui_set_image != nullptr,      iface.ui_set_progress != nullptr,
        iface.ui_set_visibility != nullptr, iface.ui_set_opacity != nullptr,
        iface.ui_element_rect != nullptr,   iface.ui_hit_test != nullptr,
        iface.ui_focus != nullptr,          iface.ui_set_focus != nullptr,
    };
    CY_CHECK_EQ(sizeof(entries) / sizeof(entries[0]), 14U);
    for (const bool set : entries) {
        CY_CHECK(set);
    }
    CY_CHECK_EQ(offsetof(CyInterface, ui_root),
                offsetof(CyInterface, character_state) + sizeof(void*));
    CY_CHECK_EQ(offsetof(CyInterface, ui_set_focus) + sizeof(void*), sizeof(CyInterface));
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, ui_event) + sizeof(void*), sizeof(CyBehaviourVTable));
}

CY_TEST_CASE(
    "every interface entry is unavailable with no interface bound, and refuses a null "
    "engine") {
    cy::abi::Host host(allocator());
    const CyInterface& iface = table();
    CyUiElement out = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_root(&host, &out), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.ui_set_progress(&host, kMade, 0.5F), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.ui_focus(&host, &out), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.ui_root(nullptr, &out), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_destroy(nullptr, kMade), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("the interface is presentation: a fixed step is refused, none and frame are not") {
    Bound bound;
    const CyInterface& iface = table();
    CyUiElement out = CY_UI_ELEMENT_NULL;
    {
        const cy::abi::game::PhaseScope fixed(bound.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(iface.ui_root(&bound.host, &out), CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.ui_set_text(&bound.host, kMade, "1250", 0xFFFFFFFFU, 1),
                    CY_RESULT_PERMISSION_DENIED);
        const f32 at[2] = {12.0F, 24.0F};
        CY_CHECK_EQ(iface.ui_hit_test(&bound.host, at, &out), CY_RESULT_PERMISSION_DENIED);
        CY_CHECK(std::strstr(cy::abi::last_error_message(), "ui_hit_test") != nullptr);
        CY_CHECK_EQ(bound.ui.writes, 0U);
    }
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FRAME_UPDATE}) {
        const cy::abi::game::PhaseScope scope(bound.host.game.clock, phase);
        out = CY_UI_ELEMENT_NULL;
        CY_CHECK_EQ(iface.ui_root(&bound.host, &out), CY_RESULT_OK);
        CY_CHECK_EQ(out, kRoot);
    }
}

CY_TEST_CASE("ui_create reads a description of any size and names the element's kind and owner") {
    Bound bound;
    const CyInterface& iface = table();
    CyUiElementDesc desc{};
    desc.kind = CY_UI_BUTTON;
    desc.name = "build-button";
    desc.owner = 42;
    CyUiElement made = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_create(&bound.host, kRoot, &desc, &made), CY_RESULT_OK);
    CY_CHECK_EQ(made, kMade);
    CY_CHECK_EQ(bound.ui.desc.kind, static_cast<u32>(CY_UI_BUTTON));
    CY_CHECK_EQ(bound.ui.desc.owner, 42U);
    CY_CHECK_EQ(bound.ui.desc.struct_size, sizeof(CyUiElementDesc));
    CY_CHECK(std::strcmp(bound.ui.name, "build-button") == 0);

    // A caller compiled against a description that ended at `name` gets a null owner.
    desc.struct_size = static_cast<uint32_t>(offsetof(CyUiElementDesc, owner));
    CY_CHECK_EQ(iface.ui_create(&bound.host, kRoot, &desc, &made), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.desc.owner, CY_ENTITY_NULL);

    // An unknown kind, a malformed size, and the missing pointers are refused before the backend.
    desc.struct_size = 0;
    desc.kind = CY_UI_BUTTON + 1U;
    made = 99;
    CY_CHECK_EQ(iface.ui_create(&bound.host, kRoot, &desc, &made), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(made, 99U);
    desc.kind = CY_UI_PANEL;
    desc.struct_size = 2;
    CY_CHECK_EQ(iface.ui_create(&bound.host, kRoot, &desc, &made), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_create(&bound.host, kRoot, nullptr, &made), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_create(&bound.host, kRoot, &desc, nullptr), CY_RESULT_INVALID_ARGUMENT);
    // The backend's own refusal is passed through, the output untouched.
    desc.struct_size = 0;
    CY_CHECK_EQ(iface.ui_create(&bound.host, 12345, &desc, &made), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(made, 99U);
}

CY_TEST_CASE("ui_set_layout passes a whole layout and refuses an unknown enumerator or a NaN") {
    Bound bound;
    const CyInterface& iface = table();
    CyUiLayout layout{};
    layout.model = CY_UI_LAYOUT_ABSOLUTE;
    layout.anchor_min[0] = 1.0F;
    layout.offset_max[1] = -6.0F;
    layout.grid_columns = 3;
    CY_CHECK_EQ(iface.ui_set_layout(&bound.host, kMade, &layout), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.layout.model, static_cast<u32>(CY_UI_LAYOUT_ABSOLUTE));
    CY_CHECK_EQ(bound.ui.layout.anchor_min[0], 1.0F);
    CY_CHECK_EQ(bound.ui.layout.offset_max[1], -6.0F);
    CY_CHECK_EQ(bound.ui.layout.grid_columns, 3U);
    CY_CHECK_EQ(iface.ui_set_layout(&bound.host, 5, &layout), CY_RESULT_NOT_FOUND);

    const u32 writes = bound.ui.writes;
    CyUiLayout bad = layout;
    bad.align = CY_UI_ALIGN_END + 1U;
    CY_CHECK_EQ(iface.ui_set_layout(&bound.host, kMade, &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = layout;
    bad.model = CY_UI_LAYOUT_ABSOLUTE + 1U;
    CY_CHECK_EQ(iface.ui_set_layout(&bound.host, kMade, &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = layout;
    bad.padding[2] = kNaN;
    CY_CHECK_EQ(iface.ui_set_layout(&bound.host, kMade, &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = layout;
    bad.offset_min[0] = std::numeric_limits<f32>::infinity();
    CY_CHECK_EQ(iface.ui_set_layout(&bound.host, kMade, &bad), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_set_layout(&bound.host, kMade, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.ui.writes, writes);
}

CY_TEST_CASE("style, text, image, progress, visibility and opacity reach the backend checked") {
    Bound bound;
    const CyInterface& iface = table();
    CyUiStyle style{};
    style.background = 0xC80E141CU;
    style.border_colour = 0xFF3C5064U;
    style.border_width = 1.0F;
    style.corner_radius = 3.0F;
    style.flags = CY_UI_STYLE_CLIP_CHILDREN;
    CY_CHECK_EQ(iface.ui_set_style(&bound.host, kMade, &style), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.style.background, 0xC80E141CU);
    CY_CHECK_EQ(bound.ui.style.flags, CY_UI_STYLE_CLIP_CHILDREN);
    style.corner_radius = -1.0F;
    CY_CHECK_EQ(iface.ui_set_style(&bound.host, kMade, &style), CY_RESULT_INVALID_ARGUMENT);
    style.corner_radius = kNaN;
    CY_CHECK_EQ(iface.ui_set_style(&bound.host, kMade, &style), CY_RESULT_INVALID_ARGUMENT);

    // A zero pixel scale is 1; null text is refused, "" is not.
    CY_CHECK_EQ(iface.ui_set_text(&bound.host, kMade, "1250", 0xFFFFD34DU, 0), CY_RESULT_OK);
    CY_CHECK(std::strcmp(bound.ui.text, "1250") == 0);
    CY_CHECK_EQ(bound.ui.colour, 0xFFFFD34DU);
    CY_CHECK_EQ(bound.ui.scale, 1U);
    CY_CHECK_EQ(iface.ui_set_text(&bound.host, kMade, "", 0xFFFFFFFFU, 2), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.scale, 2U);
    CY_CHECK_EQ(iface.ui_set_text(&bound.host, kMade, nullptr, 0, 1), CY_RESULT_INVALID_ARGUMENT);

    // A null rectangle is the whole page.
    CY_CHECK_EQ(iface.ui_set_image(&bound.host, kMade, 2, nullptr), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.page, 2U);
    CY_CHECK_EQ(bound.ui.uv[2], 1.0F);
    CY_CHECK_EQ(bound.ui.uv[3], 1.0F);
    const f32 quarter[4] = {0.5F, 0.5F, 0.25F, 0.25F};
    CY_CHECK_EQ(iface.ui_set_image(&bound.host, kMade, 3, quarter), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.uv[0], 0.5F);
    const f32 broken[4] = {0.0F, kNaN, 1.0F, 1.0F};
    CY_CHECK_EQ(iface.ui_set_image(&bound.host, kMade, 3, broken), CY_RESULT_INVALID_ARGUMENT);

    // Progress is clamped rather than refused; a NaN is refused.
    CY_CHECK_EQ(iface.ui_set_progress(&bound.host, kMade, 0.64F), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.progress, 0.64F);
    CY_CHECK_EQ(iface.ui_set_progress(&bound.host, kMade, 1.5F), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.progress, 1.0F);
    CY_CHECK_EQ(iface.ui_set_progress(&bound.host, kMade, -2.0F), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.progress, 0.0F);
    CY_CHECK_EQ(iface.ui_set_progress(&bound.host, kMade, kNaN), CY_RESULT_INVALID_ARGUMENT);

    CY_CHECK_EQ(iface.ui_set_visibility(&bound.host, kMade, CY_UI_COLLAPSED), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.visibility, CY_UI_COLLAPSED);
    CY_CHECK_EQ(iface.ui_set_visibility(&bound.host, kMade, CY_UI_COLLAPSED + 1U),
                CY_RESULT_INVALID_ARGUMENT);

    // Opacity is a fraction; outside [0, 1] it is a mistake, not a request to clamp.
    CY_CHECK_EQ(iface.ui_set_opacity(&bound.host, kMade, 0.5F), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.opacity, 0.5F);
    CY_CHECK_EQ(iface.ui_set_opacity(&bound.host, kMade, 1.5F), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_set_opacity(&bound.host, kMade, kNaN), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.ui.opacity, 0.5F);
}

CY_TEST_CASE("rects, hits and focus are read back, and a miss is null rather than a failure") {
    Bound bound;
    const CyInterface& iface = table();
    f32 rect[4] = {};
    CY_CHECK_EQ(iface.ui_element_rect(&bound.host, kMade, rect), CY_RESULT_OK);
    CY_CHECK_EQ(rect[2], 30.0F);
    CY_CHECK_EQ(iface.ui_element_rect(&bound.host, 5, rect), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(iface.ui_element_rect(&bound.host, kMade, nullptr), CY_RESULT_INVALID_ARGUMENT);

    CyUiElement hit = 99;
    const f32 inside[2] = {12.0F, 24.0F};
    CY_CHECK_EQ(iface.ui_hit_test(&bound.host, inside, &hit), CY_RESULT_OK);
    CY_CHECK_EQ(hit, kMade);
    const f32 outside[2] = {200.0F, 24.0F};
    CY_CHECK_EQ(iface.ui_hit_test(&bound.host, outside, &hit), CY_RESULT_OK);
    CY_CHECK_EQ(hit, CY_UI_ELEMENT_NULL);
    const f32 nowhere[2] = {kNaN, 0.0F};
    CY_CHECK_EQ(iface.ui_hit_test(&bound.host, nowhere, &hit), CY_RESULT_INVALID_ARGUMENT);

    CyUiElement focus = 99;
    CY_CHECK_EQ(iface.ui_focus(&bound.host, &focus), CY_RESULT_OK);
    CY_CHECK_EQ(focus, CY_UI_ELEMENT_NULL);
    CY_CHECK_EQ(iface.ui_set_focus(&bound.host, kMade), CY_RESULT_OK);
    CY_CHECK_EQ(iface.ui_focus(&bound.host, &focus), CY_RESULT_OK);
    CY_CHECK_EQ(focus, kMade);
    CY_CHECK_EQ(iface.ui_set_focus(&bound.host, CY_UI_ELEMENT_NULL), CY_RESULT_OK);
    CY_CHECK_EQ(bound.ui.focused, CY_UI_ELEMENT_NULL);
    CY_CHECK_EQ(iface.ui_set_focus(&bound.host, 5), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("an interface event reaches the behaviours on its owner, in the frame phase") {
    cy::abi::Host host(allocator());
    g_host = &host;
    cy::abi::BehaviourRuntime runtime(allocator(), host);
    g_seen_count = 0;
    CY_REQUIRE(host.register_behaviour("Clicker", ui_vtable()).has_value());
    CY_REQUIRE(runtime.create("Clicker", 7).has_value());
    CY_REQUIRE(runtime.create("Clicker", 8).has_value());

    CY_CHECK_EQ(runtime.ui_event(click(7)), 1U);
    CY_REQUIRE_EQ(g_seen_count, 1U);
    CY_CHECK_EQ(g_seen[0].owner, 7U);
    CY_CHECK_EQ(g_seen[0].element, kMade);
    CY_CHECK_EQ(g_seen[0].button, CY_INPUT_BUTTON_LEFT);
    CY_CHECK_EQ(g_seen[0].position[1], 24.0F);
    CY_CHECK_EQ(g_phase_seen, CY_PHASE_FRAME_UPDATE);
    // The phase is the frame's only for the delivery.
    CY_CHECK_EQ(host.game.clock.phase, CY_PHASE_NONE);

    // Nobody owns entity 9: the event is dropped, not delivered to whoever is first.
    CY_CHECK_EQ(runtime.ui_event(click(9)), 0U);
    CY_CHECK_EQ(g_seen_count, 1U);
    g_host = nullptr;
}

CY_TEST_CASE(
    "a module compiled before 1.6 takes no interface event, and is not read past its "
    "vtable") {
    cy::abi::Host host(allocator());
    cy::abi::BehaviourRuntime runtime(allocator(), host);
    g_seen_count = 0;
    // A 1.5 vtable ends at `exit_tree`. The bytes after it are garbage the engine must not read.
    CyBehaviourVTable old = ui_vtable();
    old.struct_size = static_cast<uint32_t>(offsetof(CyBehaviourVTable, ui_event));
    CY_REQUIRE(host.register_behaviour("Old", old).has_value());
    CY_REQUIRE(runtime.create("Old", 7).has_value());
    CY_CHECK_EQ(runtime.ui_event(click(7)), 0U);
    CY_CHECK_EQ(g_seen_count, 0U);
}
