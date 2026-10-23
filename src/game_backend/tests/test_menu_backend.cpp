// SPDX-License-Identifier: MIT
// `integration.game_backend_menu` — ABI 1.13 over the engine: rebinding through a real input
// server's `RebindOperation`, the player's profile saved and loaded into a fresh server, the
// display settings and the player's files. Every case goes through `cy_get_interface`'s table on a
// real `cy::abi::Host`, so what is proven is what a Swift settings screen sees.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/input_backend.h>
#include <cy/game_backend/menu_backends.h>
#include <cy/servers/input/server.h>
#include <cy/test/test.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace cy::input;
using cy::Nanoseconds;
using cy::u32;

constexpr Nanoseconds kMs = 1'000'000;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

Binding simple(ActionId action, Control control) noexcept {
    Binding binding;
    binding.action = action;
    binding.kind = BindingKind::Simple;
    binding.component_count = 1;
    binding.components[0].control = control;
    binding.components[0].weight = cy::Vec3{1.0F, 0.0F, 0.0F};
    binding.trigger.kind = TriggerKind::Down;
    return binding;
}

/// A game's controls: Jump on Space and the gamepad's south button, Interact on E and west,
/// Forward on W — one keyboard and one gamepad, the adapter bound on a host.
struct Rig {
    InputServer server{allocator()};
    cy::game_backend::InputAdapter adapter{server};
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    DeviceId keyboard;
    DeviceId pad;
    ActionId jump = kInvalidAction;
    ActionId interact = kInvalidAction;
    ActionId forward = kInvalidAction;
    cy::u64 tick = 0;

    [[nodiscard]] bool build() noexcept {
        InputServerConfig config;
        config.users = 1;
        config.event_capacity = 256;
        if (!server.configure(config) || !server.initialize()) {
            return false;
        }
        jump = declare("Game.Jump", 11);
        interact = declare("Game.Interact", 12);
        forward = declare("Game.Forward", 13);
        keyboard = attach(DeviceKind::Keyboard, "kb");
        pad = attach(DeviceKind::Gamepad, "pad");
        if (!server.finalize_declarations()) {
            return false;
        }
        MappingContext game(allocator());
        game.set_name(cy::Name::intern("Game"));
        if (!game.add(simple(jump, key_control(Key::Space))) ||
            !game.add(simple(jump, gamepad_control(GamepadControl::South))) ||
            !game.add(simple(interact, key_control(Key::E))) ||
            !game.add(simple(interact, gamepad_control(GamepadControl::West))) ||
            !game.add(simple(forward, key_control(Key::W)))) {
            return false;
        }
        auto handle = server.register_context(std::move(game));
        if (!handle) {
            return false;
        }
        cy::game_backend::bind(host, &adapter);
        return server.user(0).push_context(*handle, 0).has_value();
    }

    ActionId declare(const char* name, u32 stable) noexcept {
        ActionDeclaration declaration;
        declaration.name = cy::Name::intern(name);
        declaration.stable_id = ActionStableId{stable};
        auto declared = server.actions().declare(declaration);
        return declared ? *declared : kInvalidAction;
    }

    DeviceId attach(DeviceKind kind, const char* hardware) noexcept {
        DeviceDescription description;
        description.kind = kind;
        description.hardware_id = cy::Name::intern(hardware);
        auto connected = server.devices().connect(description, 0);
        if (!connected || !server.assign(*connected, 0, 0)) {
            return DeviceId{};
        }
        return *connected;
    }

    /// Press and release `control` within one fixed step, the way a host runs it.
    void tap(DeviceId device, Control control) noexcept {
        for (const cy::f32 value : {1.0F, 0.0F}) {
            DeviceEvent event;
            event.timestamp = (static_cast<Nanoseconds>(tick) * 16 * kMs) + (value > 0 ? 1 : 2);
            event.device = device;
            event.control = control;
            event.value = value;
            server.submit(event);
        }
        step();
    }

    void step() noexcept {
        adapter.observe_pending();
        ++tick;
        server.resolve_tick(tick, static_cast<Nanoseconds>(tick) * 16 * kMs, 1.0F / 60.0F);
    }

    [[nodiscard]] std::string binding(ActionId action, u32 scheme, u32 slot = 0) noexcept {
        char path[64] = {};
        u32 size = 0;
        if (table().input_binding(&host, 0, action, scheme, slot, path, sizeof(path), &size) !=
            CY_RESULT_OK) {
            return "(none)";
        }
        return {path, size};
    }

    [[nodiscard]] CyInputRebind poll() noexcept {
        CyInputRebind state{};
        state.struct_size = sizeof(state);
        CY_CHECK_EQ(table().input_rebind_poll(&host, 0, &state), CY_RESULT_OK);
        return state;
    }

    [[nodiscard]] bool pressed(ActionId action) const noexcept {
        return server.user(0).action_state(action).pressed() ||
               server.user(0).action_state(action).press_count > 0;
    }
};

}  // namespace

CY_TEST_CASE("menu: a key rebound through the input system's flow is the action's from then on") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CY_CHECK_EQ(rig.binding(rig.jump, CY_INPUT_SCHEME_KEYBOARD_MOUSE),
                std::string("keyboard/space"));
    CY_CHECK_EQ(rig.poll().status, static_cast<u32>(CY_INPUT_REBIND_IDLE));
    CY_REQUIRE_EQ(table().input_rebind_begin(&rig.host, 0, rig.jump, CY_INPUT_SCHEME_KEYBOARD_MOUSE,
                                             0, CY_INPUT_CONFLICT_SWAP),
                  CY_RESULT_OK);
    CY_CHECK_EQ(rig.poll().status, static_cast<u32>(CY_INPUT_REBIND_LISTENING));
    // A gamepad button is another scheme: ignored, and the rebind keeps listening.
    rig.tap(rig.pad, gamepad_control(GamepadControl::North));
    CY_CHECK_EQ(rig.poll().status, static_cast<u32>(CY_INPUT_REBIND_LISTENING));
    rig.tap(rig.keyboard, key_control(Key::J));
    const CyInputRebind done = rig.poll();
    CY_CHECK_EQ(done.status, static_cast<u32>(CY_INPUT_REBIND_APPLIED));
    CY_CHECK_EQ(std::string_view(done.control), std::string_view("keyboard/j"));
    CY_CHECK_EQ(done.conflict[0], '\0');
    CY_CHECK_EQ(rig.binding(rig.jump, CY_INPUT_SCHEME_KEYBOARD_MOUSE), std::string("keyboard/j"));
    // The gamepad's binding is untouched, and J now jumps where Space no longer does.
    CY_CHECK_EQ(rig.binding(rig.jump, CY_INPUT_SCHEME_GAMEPAD), std::string("gamepad/south"));
    rig.tap(rig.keyboard, key_control(Key::J));
    CY_CHECK(rig.pressed(rig.jump));
    rig.tap(rig.keyboard, key_control(Key::Space));
    CY_CHECK_FALSE(rig.pressed(rig.jump));
}

CY_TEST_CASE("menu: a control another action holds is swapped, refused or cancelled by policy") {
    Rig rig;
    CY_REQUIRE(rig.build());
    // SWAP, against an AUTHORED binding: Jump takes E, and Interact takes Jump's Space.
    CY_REQUIRE_EQ(table().input_rebind_begin(&rig.host, 0, rig.jump, CY_INPUT_SCHEME_KEYBOARD_MOUSE,
                                             0, CY_INPUT_CONFLICT_SWAP),
                  CY_RESULT_OK);
    rig.tap(rig.keyboard, key_control(Key::E));
    const CyInputRebind swapped = rig.poll();
    CY_CHECK_EQ(swapped.status, static_cast<u32>(CY_INPUT_REBIND_APPLIED));
    CY_CHECK_EQ(std::string_view(swapped.conflict), std::string_view("Game.Interact"));
    CY_CHECK_EQ(rig.binding(rig.jump, CY_INPUT_SCHEME_KEYBOARD_MOUSE), std::string("keyboard/e"));
    CY_CHECK_EQ(rig.binding(rig.interact, CY_INPUT_SCHEME_KEYBOARD_MOUSE),
                std::string("keyboard/space"));

    // REJECT: W is Forward's, so Interact stays where it was and the rebind ends in CONFLICT.
    CY_REQUIRE_EQ(
        table().input_rebind_begin(&rig.host, 0, rig.interact, CY_INPUT_SCHEME_KEYBOARD_MOUSE, 0,
                                   CY_INPUT_CONFLICT_REJECT),
        CY_RESULT_OK);
    rig.tap(rig.keyboard, key_control(Key::W));
    const CyInputRebind refused = rig.poll();
    CY_CHECK_EQ(refused.status, static_cast<u32>(CY_INPUT_REBIND_CONFLICT));
    CY_CHECK_EQ(std::string_view(refused.conflict), std::string_view("Game.Forward"));
    CY_CHECK_EQ(rig.binding(rig.interact, CY_INPUT_SCHEME_KEYBOARD_MOUSE),
                std::string("keyboard/space"));

    // Escape cancels, and a gamepad's Back does on a gamepad rebind.
    CY_REQUIRE_EQ(
        table().input_rebind_begin(&rig.host, 0, rig.interact, CY_INPUT_SCHEME_KEYBOARD_MOUSE, 0,
                                   CY_INPUT_CONFLICT_SWAP),
        CY_RESULT_OK);
    rig.tap(rig.keyboard, key_control(Key::Escape));
    CY_CHECK_EQ(rig.poll().status, static_cast<u32>(CY_INPUT_REBIND_CANCELLED));
    CY_REQUIRE_EQ(table().input_rebind_begin(&rig.host, 0, rig.interact, CY_INPUT_SCHEME_GAMEPAD, 0,
                                             CY_INPUT_CONFLICT_SWAP),
                  CY_RESULT_OK);
    rig.tap(rig.pad, gamepad_control(GamepadControl::Back));
    CY_CHECK_EQ(rig.poll().status, static_cast<u32>(CY_INPUT_REBIND_CANCELLED));
    CY_CHECK_EQ(rig.binding(rig.interact, CY_INPUT_SCHEME_GAMEPAD), std::string("gamepad/west"));
    // And the game can cancel one itself.
    CY_REQUIRE_EQ(
        table().input_rebind_begin(&rig.host, 0, rig.forward, CY_INPUT_SCHEME_KEYBOARD_MOUSE, 0, 0),
        CY_RESULT_OK);
    CY_REQUIRE_EQ(table().input_rebind_cancel(&rig.host, 0), CY_RESULT_OK);
    CY_CHECK_EQ(rig.poll().status, static_cast<u32>(CY_INPUT_REBIND_CANCELLED));
}

CY_TEST_CASE("menu: the player's profile is saved as text and loaded into a fresh game") {
    std::string saved;
    {
        Rig rig;
        CY_REQUIRE(rig.build());
        u32 size = 0;
        CY_REQUIRE_EQ(table().input_profile_save(&rig.host, 0, nullptr, 0, &size), CY_RESULT_OK);
        CY_CHECK_EQ(size, 0U);  // nothing changed, nothing to keep
        CY_REQUIRE_EQ(
            table().input_rebind_begin(&rig.host, 0, rig.interact, CY_INPUT_SCHEME_GAMEPAD, 0,
                                       CY_INPUT_CONFLICT_SWAP),
            CY_RESULT_OK);
        rig.tap(rig.pad, gamepad_control(GamepadControl::North));
        CY_REQUIRE_EQ(rig.poll().status, static_cast<u32>(CY_INPUT_REBIND_APPLIED));
        CY_REQUIRE_EQ(table().input_profile_save(&rig.host, 0, nullptr, 0, &size), CY_RESULT_OK);
        std::string text(size, '\0');
        CY_REQUIRE_EQ(table().input_profile_save(&rig.host, 0, text.data(), size + 1, &size),
                      CY_RESULT_OK);
        saved = text;
    }
    CY_CHECK_EQ(saved, std::string("cyinputprofile 1\n12 gamepad 0 gamepad/north Game.Interact\n"));

    Rig fresh;
    CY_REQUIRE(fresh.build());
    CY_CHECK_EQ(fresh.binding(fresh.interact, CY_INPUT_SCHEME_GAMEPAD),
                std::string("gamepad/west"));
    CY_REQUIRE_EQ(table().input_profile_load(&fresh.host, 0, saved.data(), saved.size()),
                  CY_RESULT_OK);
    CY_CHECK_EQ(fresh.binding(fresh.interact, CY_INPUT_SCHEME_GAMEPAD),
                std::string("gamepad/north"));
    fresh.tap(fresh.pad, gamepad_control(GamepadControl::North));
    CY_CHECK(fresh.pressed(fresh.interact));

    // A profile that does not read changes nothing, naming its line.
    constexpr std::string_view kBroken =
        "cyinputprofile 1\n12 gamepad 0 gamepad/south X\nnonsense\n";
    CY_CHECK_EQ(table().input_profile_load(&fresh.host, 0, kBroken.data(), kBroken.size()),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK(std::string_view(cy::abi::last_error_message()).find("line 3") !=
             std::string_view::npos);
    CY_CHECK_EQ(fresh.binding(fresh.interact, CY_INPUT_SCHEME_GAMEPAD),
                std::string("gamepad/north"));
    // A line naming an action this build does not have is skipped, not refused.
    constexpr std::string_view kOld = "cyinputprofile 1\n99 keyboard 0 keyboard/k Gone\n";
    CY_CHECK_EQ(table().input_profile_load(&fresh.host, 0, kOld.data(), kOld.size()), CY_RESULT_OK);
    CY_CHECK_EQ(fresh.binding(fresh.interact, CY_INPUT_SCHEME_GAMEPAD),
                std::string("gamepad/west"));
    // Reset: every authored binding again.
    CY_REQUIRE_EQ(table().input_profile_load(&fresh.host, 0, saved.data(), saved.size()),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(table().input_profile_reset(&fresh.host, 0), CY_RESULT_OK);
    CY_CHECK_EQ(fresh.binding(fresh.interact, CY_INPUT_SCHEME_GAMEPAD),
                std::string("gamepad/west"));
}

CY_TEST_CASE("menu: display settings move only to an offered mode, and a change is counted") {
    cy::game_backend::VideoSettingsAdapter video(allocator());
    const CyVideoMode modes[] = {{1280, 720}, {1920, 1080}};
    CyVideoSettings initial{sizeof(CyVideoSettings), 1280, 720, CY_VIDEO_QUALITY_HIGH,
                            CY_VIDEO_UPSCALE_NATIVE, 0};
    CyVideoSettings bad = initial;
    bad.width = 800;
    CY_CHECK_FALSE(video.start(cy::Span<const CyVideoMode>(modes, 2), bad).has_value());
    CY_REQUIRE(video.start(cy::Span<const CyVideoMode>(modes, 2), initial).has_value());
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    cy::game_backend::bind(host, &video, nullptr);
    CyVideoSettings next = initial;
    next.width = 1920;
    next.height = 1080;
    next.upscale = CY_VIDEO_UPSCALE_QUALITY;
    CY_REQUIRE_EQ(table().video_set_settings(&host, &next), CY_RESULT_OK);
    CY_CHECK_EQ(video.revision(), 1U);
    CY_CHECK_EQ(video.current().upscale, static_cast<u32>(CY_VIDEO_UPSCALE_QUALITY));
    CY_REQUIRE_EQ(table().video_set_settings(&host, &next), CY_RESULT_OK);
    CY_CHECK_EQ(video.revision(), 1U);  // the same settings again are no change
    next.width = 2560;
    CY_CHECK_EQ(table().video_set_settings(&host, &next), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(video.current().width, 1920U);
    CY_CHECK_EQ(table().user_data_read(&host, "a.cfg", nullptr, 0, nullptr), CY_RESULT_UNAVAILABLE);
}

CY_TEST_CASE("menu: a player's file is written whole into the directory and read back") {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cy-game-backend-user-data";
    std::filesystem::remove_all(root);
    cy::game_backend::UserDataDirectory files(allocator());
    CY_REQUIRE(files.start((root / "nested").string()).has_value());
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    cy::game_backend::bind(host, nullptr, &files);
    constexpr std::string_view kText = "music=0.4\nsfx=0.9\n";
    CY_REQUIRE_EQ(table().user_data_write(&host, "settings.cfg", kText.data(), kText.size()),
                  CY_RESULT_OK);
    CY_CHECK(std::filesystem::exists(root / "nested" / "settings.cfg"));
    cy::u64 size = 0;
    char buffer[64] = {};
    CY_REQUIRE_EQ(table().user_data_read(&host, "settings.cfg", buffer, sizeof(buffer), &size),
                  CY_RESULT_OK);
    CY_CHECK_EQ(std::string_view(buffer, size), kText);
    CY_CHECK_EQ(table().user_data_read(&host, "other.cfg", nullptr, 0, &size), CY_RESULT_NOT_FOUND);
    std::filesystem::remove_all(root);
}
