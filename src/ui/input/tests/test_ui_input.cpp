// SPDX-License-Identifier: MIT
// `unit.ui_input`: the toolkit driven by the input server's actions — a keyboard and a gamepad
// reaching the same button as `UI.Accept`, directional and tab-order navigation from both, the
// prompt glyph following the device, and gameplay suppressed while the interface holds the input.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/input/server.h>
#include <cy/test/test.h>
#include <cy/ui/input/ui_input.h>
#include <cy/ui/text/builtin_font.h>

#include <string_view>
#include <utility>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

constexpr Nanoseconds kMs = 1'000'000;

/// An input server with a keyboard and a gamepad for one player, the interface's context and a
/// game's — `Confirm` on Enter and on the gamepad's south button, the same controls as UI.Accept —
/// and a toolkit with the bridge between them.
struct Rig {
    Rig() noexcept {
        cy::text::TextServerConfig text_config;
        text_config.atlas.initial_extent = 256;
        text_config.atlas.maximum_extent = 256;
        input::InputServerConfig config;
        config.users = 1;
        config.event_capacity = 256;
        started = server.configure(config).has_value() && server.initialize().has_value() &&
                  text_server.start(text_config).has_value() &&
                  text.start(text_server, builtin_font(), 1).has_value() &&
                  toolkit.start().has_value() && declare() && attach() && contexts();
        settings.mode = ScaleMode::FixedPixel;
        settings.reference = Vec2{800.0F, 600.0F};
    }

    [[nodiscard]] bool declare() noexcept {
        if (!declare_ui_actions(server.actions(), actions)) {
            return false;
        }
        input::ActionDeclaration declaration;
        declaration.name = Name::intern("Confirm");
        declaration.stable_id = input::ActionStableId{1};
        auto declared = server.actions().declare(declaration);
        confirm = declared ? *declared : input::kInvalidAction;
        return declared.has_value() && server.finalize_declarations().has_value();
    }

    [[nodiscard]] bool attach() noexcept {
        for (const input::DeviceKind kind : {input::DeviceKind::Keyboard, input::DeviceKind::Gamepad}) {
            input::DeviceDescription description;
            description.kind = kind;
            description.hardware_id = Name::intern(kind == input::DeviceKind::Keyboard ? "kb" : "pad");
            description.display_name = description.hardware_id;
            auto connected = server.devices().connect(description, 0);
            if (!connected || !server.assign(*connected, 0, 0).has_value()) {
                return false;
            }
            (kind == input::DeviceKind::Keyboard ? keyboard : gamepad) = *connected;
        }
        return true;
    }

    [[nodiscard]] bool contexts() noexcept {
        auto ui = make_ui_context(allocator(), actions);
        if (!ui) {
            return false;
        }
        auto ui_handle = server.register_context(std::move(*ui));
        input::MappingContext game(allocator());
        for (const input::Control control : {input::key_control(input::Key::Enter),
                                             input::gamepad_control(input::GamepadControl::South)}) {
            input::Binding binding;
            binding.action = confirm;
            binding.component_count = 1;
            binding.components[0].control = control;
            if (!game.add(binding)) {
                return false;
            }
        }
        auto game_handle = server.register_context(std::move(game));
        return ui_handle && game_handle &&
               server.user(0).push_context(*game_handle, 0, input::FocusLayer::Gameplay).has_value() &&
               bridge.start(actions, *ui_handle).has_value();
    }

    /// One tick: the events so far resolved, then the bridge pumped, then a toolkit frame.
    void tick() noexcept {
        ++ticks;
        server.resolve_tick(ticks, static_cast<Nanoseconds>(ticks) * 10 * kMs, 1.0F / 60.0F);
        (void)bridge.pump(&report);
        (void)toolkit.update(1.0F / 60.0F, settings, Vec2{800.0F, 600.0F});
    }

    /// Press and release a control across two ticks.
    void tap(input::DeviceId device, input::Control control) noexcept {
        input::DeviceEvent down;
        down.device = device;
        down.control = control;
        down.value = 1.0F;
        down.timestamp = (static_cast<Nanoseconds>(ticks) * 10 * kMs) + kMs;
        server.submit(down);
        tick();
        input::DeviceEvent up = down;
        up.value = 0.0F;
        up.timestamp += 5 * kMs;
        server.submit(up);
        tick();
    }

    void hold(input::DeviceId device, input::Control control, f32 value) noexcept {
        input::DeviceEvent event;
        event.device = device;
        event.control = control;
        event.value = value;
        event.timestamp = (static_cast<Nanoseconds>(ticks) * 10 * kMs) + kMs;
        server.submit(event);
        tick();
    }

    [[nodiscard]] u32 clicks(WidgetId widget) const noexcept {
        u32 count = 0;
        for (const WidgetEvent& event : toolkit.events()) {
            count += (event.kind == WidgetEventKind::Clicked && event.widget == widget) ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] bool confirmed() const noexcept {
        return server.user(0).action_state(confirm).just_pressed();
    }

    input::InputServer server{allocator()};
    cy::text::TextServer text_server;
    TextPainter text{allocator()};
    ElementStore store{allocator()};
    Toolkit toolkit{allocator(), store, text};
    UiInputActions actions;
    UiInputBridge bridge{toolkit, server, 0};
    input::ActionId confirm = input::kInvalidAction;
    input::DeviceId keyboard;
    input::DeviceId gamepad;
    ScaleSettings settings;
    UiInputReport report;
    u64 ticks = 0;
    bool started = false;
};

[[nodiscard]] WidgetId button(Rig& rig, WidgetId parent, const char* id) noexcept {
    WidgetDesc desc;
    desc.kind = WidgetKind::Button;
    desc.id = id;
    desc.text = id;
    auto made = rig.toolkit.create(parent, desc);
    return made ? *made : kNoElement;
}

}  // namespace

CY_TEST_CASE("ui_input: Enter, the gamepad's south button and a click each reach a button as UI.Accept") {
    // `ui-system`: "WHEN a button handles UI.Accept THEN it SHALL respond to Enter, left click,
    // gamepad south button, or tap, with no device-specific code."
    Rig rig;
    CY_REQUIRE(rig.started);
    WidgetDesc row;
    row.kind = WidgetKind::Row;
    auto menu = rig.toolkit.create(kNoElement, row);
    CY_REQUIRE(menu.has_value());
    const WidgetId play = button(rig, *menu, "play");
    rig.bridge.set_menu_mode(true);  // a menu screen: the interface has the input
    rig.tick();
    CY_REQUIRE(rig.toolkit.focus(play).has_value());

    rig.tap(rig.keyboard, input::key_control(input::Key::Enter));
    CY_CHECK_EQ(rig.clicks(play), 1U);
    rig.tap(rig.gamepad, input::gamepad_control(input::GamepadControl::South));
    CY_CHECK_EQ(rig.clicks(play), 2U);
    // A click and a tap are the pointer's, through the same toolkit and the same event.
    const LayoutOutput* out = rig.store.layout_output(play);
    const Vec2 centre{out->rect.x + (out->rect.width * 0.5F), out->rect.y + (out->rect.height * 0.5F)};
    (void)rig.toolkit.pointer(PointerInput{centre, true, {}, 0});
    (void)rig.toolkit.pointer(PointerInput{centre, false, {}, 0});
    CY_CHECK_EQ(rig.clicks(play), 3U);
}

CY_TEST_CASE("ui_input: a modal suppresses the game's action on the same key, and gives it back") {
    // `ui-system`: "WHEN a modal interface layer is open THEN its context SHALL consume the actions it
    // uses, and gameplay bound to the same controls SHALL NOT observe them."
    Rig rig;
    CY_REQUIRE(rig.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::Dialog;
    desc.title = "Quit?";
    auto dialog = rig.toolkit.create(kNoElement, desc);
    CY_REQUIRE(dialog.has_value());
    const WidgetId yes = button(rig, *dialog, "yes");
    rig.tick();

    // In play: Enter is the game's, and the interface's bindings are not even evaluated.
    rig.hold(rig.keyboard, input::key_control(input::Key::Enter), 1.0F);
    CY_CHECK(rig.confirmed());
    CY_CHECK_EQ(rig.report.fired, 0U);
    CY_CHECK_EQ(rig.server.user(0).focus(), input::FocusLayer::Gameplay);
    rig.hold(rig.keyboard, input::key_control(input::Key::Enter), 0.0F);

    CY_REQUIRE(rig.toolkit.open(*dialog).has_value());
    rig.tick();  // the pump moves the user's focus to the interface
    CY_CHECK_EQ(rig.server.user(0).focus(), input::FocusLayer::Interface);
    rig.hold(rig.keyboard, input::key_control(input::Key::Enter), 1.0F);
    CY_CHECK_FALSE(rig.confirmed());
    CY_CHECK_EQ(rig.server.user(0).outcome(rig.confirm), input::ActionOutcome::SuppressedByFocus);
    CY_CHECK_EQ(rig.clicks(yes), 1U);
    rig.hold(rig.keyboard, input::key_control(input::Key::Enter), 0.0F);

    // Escape closes it; the next tick the game has its key back.
    rig.tap(rig.keyboard, input::key_control(input::Key::Escape));
    CY_CHECK_FALSE(rig.toolkit.is_open(*dialog));
    rig.tick();
    CY_CHECK_EQ(rig.server.user(0).focus(), input::FocusLayer::Gameplay);
    rig.hold(rig.keyboard, input::key_control(input::Key::Enter), 1.0F);
    CY_CHECK(rig.confirmed());
}

CY_TEST_CASE("ui_input: the d-pad, the stick and the keyboard navigate; Tab and Shift+Tab walk the order") {
    Rig rig;
    CY_REQUIRE(rig.started);
    WidgetDesc grid;
    grid.kind = WidgetKind::Grid;
    grid.columns = 2;
    auto made = rig.toolkit.create(kNoElement, grid);
    CY_REQUIRE(made.has_value());
    const WidgetId a = button(rig, *made, "a");
    const WidgetId b = button(rig, *made, "b");
    const WidgetId c = button(rig, *made, "c");
    const WidgetId d = button(rig, *made, "d");
    (void)d;
    rig.bridge.set_menu_mode(true);
    rig.tick();
    rig.tap(rig.gamepad, input::gamepad_control(input::GamepadControl::DpadDown));
    CY_CHECK_EQ(rig.toolkit.focused(), a);  // the first press focuses the first control
    rig.tap(rig.gamepad, input::gamepad_control(input::GamepadControl::DpadRight));
    CY_CHECK_EQ(rig.toolkit.focused(), b);
    // The left stick pushed down past half way is one press, however long it is held.
    rig.hold(rig.gamepad, input::gamepad_control(input::GamepadControl::LeftStickX), -0.9F);
    CY_CHECK_EQ(rig.toolkit.focused(), a);
    rig.tick();
    rig.tick();
    CY_CHECK_EQ(rig.toolkit.focused(), a);
    rig.hold(rig.gamepad, input::gamepad_control(input::GamepadControl::LeftStickX), 0.0F);
    rig.tap(rig.keyboard, input::key_control(input::Key::Down));
    CY_CHECK_EQ(rig.toolkit.focused(), c);
    rig.tap(rig.keyboard, input::key_control(input::Key::Tab));
    CY_CHECK_EQ(rig.toolkit.focused(), d);
    // Shift+Tab is the chord, not a Tab as well.
    rig.hold(rig.keyboard, input::key_control(input::Key::LeftShift), 1.0F);
    rig.tap(rig.keyboard, input::key_control(input::Key::Tab));
    CY_CHECK_EQ(rig.toolkit.focused(), c);
    rig.hold(rig.keyboard, input::key_control(input::Key::LeftShift), 0.0F);
    CY_CHECK(rig.toolkit.focus_visible());
}

CY_TEST_CASE("ui_input: the prompt glyph follows the device last used") {
    // `ui-system`: "WHEN the player switches from keyboard to gamepad THEN prompts SHALL update to the
    // gamepad's glyphs."
    Rig rig;
    CY_REQUIRE(rig.started);
    rig.tick();
    CY_CHECK_EQ(std::string_view(ui_action_glyph(UiAction::Accept, rig.bridge.scheme())),
                std::string_view("Enter"));
    // The scheme detector's hysteresis: the gamepad has to hold the lead for its dwell time — a
    // stick held over thirty ticks, still moving at the end of them — before the prompts change.
    rig.hold(rig.gamepad, input::gamepad_control(input::GamepadControl::RightStickX), 0.8F);
    for (u32 tick = 0; tick < 30; ++tick) {
        rig.tick();
    }
    CY_CHECK_EQ(rig.bridge.scheme(), input::SchemeKind::KeyboardMouse);
    rig.hold(rig.gamepad, input::gamepad_control(input::GamepadControl::RightStickX), 0.9F);
    CY_CHECK_EQ(rig.bridge.scheme(), input::SchemeKind::Gamepad);
    CY_CHECK_EQ(std::string_view(ui_action_glyph(UiAction::Accept, rig.bridge.scheme())),
                std::string_view("A"));
    CY_CHECK_EQ(std::string_view(ui_action_glyph(UiAction::NextTab, input::SchemeKind::Gamepad)),
                std::string_view("RB"));
}

CY_TEST_CASE("ui_input: a focused text field is text entry, and keeps its own left and right") {
    Rig rig;
    CY_REQUIRE(rig.started);
    WidgetDesc desc;
    desc.kind = WidgetKind::TextField;
    desc.text = "abc";
    auto field = rig.toolkit.create(kNoElement, desc);
    CY_REQUIRE(field.has_value());
    rig.tick();
    CY_REQUIRE(rig.toolkit.focus(*field).has_value());
    rig.tick();
    CY_CHECK(rig.server.user(0).text_entry_active());
    CY_CHECK_EQ(rig.server.user(0).focus(), input::FocusLayer::Interface);
    rig.tap(rig.keyboard, input::key_control(input::Key::Left));
    CY_CHECK_EQ(rig.toolkit.focused(), *field);
    CY_CHECK_EQ(rig.report.fired, 0U);
}
