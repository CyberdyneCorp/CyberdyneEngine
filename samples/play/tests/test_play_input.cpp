// SPDX-License-Identifier: MIT
// `unit.play_host` — a player's rebinding file and the input script. See bindings_file.h and
// input_script.h.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/input/server.h>
#include <cy/test/test.h>

#include <utility>

#include "bindings_file.h"
#include "input_script.h"

using namespace cy::input;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// A server with `player.jump` on Space and `weapon.fire` on the left button, user 0's.
struct Rig {
    InputServer server{allocator()};
    DeviceId keyboard;
    DeviceId mouse;
    ActionId jump = kInvalidAction;
    ActionId fire = kInvalidAction;

    [[nodiscard]] bool build() noexcept {
        InputServerConfig config;
        config.event_capacity = 64;
        if (!server.configure(config) || !server.initialize()) {
            return false;
        }
        jump = declare("player.jump", 1);
        fire = declare("weapon.fire", 2);
        if (!server.finalize_declarations()) {
            return false;
        }
        keyboard = connect(DeviceKind::Keyboard, "kb");
        mouse = connect(DeviceKind::Mouse, "mouse");
        MappingContext context(allocator());
        context.set_name(cy::Name::intern("play"));
        if (!context.add(simple(jump, key_control(Key::Space), SchemeKind::KeyboardMouse)) ||
            !context.add(simple(fire, mouse_control(MouseControl::Left),
                                SchemeKind::KeyboardMouse))) {
            return false;
        }
        auto handle = server.register_context(std::move(context));
        return handle && server.user(0).push_context(*handle, 0).has_value();
    }

    ActionId declare(const char* name, cy::u32 stable) noexcept {
        ActionDeclaration declaration;
        declaration.name = cy::Name::intern(name);
        declaration.stable_id = ActionStableId{stable};
        auto declared = server.actions().declare(declaration);
        return declared ? *declared : kInvalidAction;
    }

    DeviceId connect(DeviceKind kind, const char* hardware) noexcept {
        DeviceDescription description;
        description.kind = kind;
        description.hardware_id = cy::Name::intern(hardware);
        auto connected = server.devices().connect(description, 0);
        if (!connected || !server.assign(*connected, 0, 0)) {
            return DeviceId{};
        }
        return *connected;
    }

    static Binding simple(ActionId action, Control control, SchemeKind scheme) noexcept {
        Binding binding;
        binding.action = action;
        binding.kind = BindingKind::Simple;
        binding.schemes = scheme_mask(scheme);
        binding.component_count = 1;
        binding.components[0].control = control;
        binding.trigger.kind = TriggerKind::Down;
        return binding;
    }

    [[nodiscard]] bool pressed_after(Control control, cy::u64 tick) noexcept {
        DeviceEvent down;
        down.device = control.kind == DeviceKind::Keyboard ? keyboard : mouse;
        down.control = control;
        down.value = 1.0F;
        down.timestamp = static_cast<cy::Nanoseconds>(tick) * 1000;
        server.submit(down);
        server.resolve_tick(tick, static_cast<cy::Nanoseconds>(tick) * 1000, 1.0F / 60.0F);
        const bool jumped = server.user(0).action_state(jump).pressed();
        DeviceEvent up = down;
        up.value = 0.0F;
        server.submit(up);
        server.resolve_tick(tick + 1, static_cast<cy::Nanoseconds>(tick + 1) * 1000, 1.0F / 60.0F);
        return jumped;
    }
};

}  // namespace

CY_TEST_CASE("a rebinding file moves an action to another key, over the authored binding") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CY_CHECK(rig.pressed_after(key_control(Key::Space), 1));
    const cy::play::BindingsResult rebound =
        cy::play::apply_bindings(rig.server, 0, "# the player's choice\nplayer.jump keyboard/f\n");
    CY_REQUIRE(rebound.error.empty());
    CY_CHECK_EQ(rebound.applied, 1U);
    // F jumps now, and Space no longer does: the override replaced the primary binding.
    CY_CHECK(rig.pressed_after(key_control(Key::F), 3));
    CY_CHECK_FALSE(rig.pressed_after(key_control(Key::Space), 5));
}

CY_TEST_CASE("a rebinding file with a typo is refused whole, naming the line") {
    Rig rig;
    CY_REQUIRE(rig.build());
    const cy::play::BindingsResult unknown_action =
        cy::play::apply_bindings(rig.server, 0, "weapon.fire mouse/right\nplayer.jmup keyboard/f\n");
    CY_CHECK_EQ(unknown_action.applied, 0U);
    CY_CHECK(unknown_action.error.find("line 2") != std::string::npos);
    const cy::play::BindingsResult unknown_control =
        cy::play::apply_bindings(rig.server, 0, "player.jump keyboard/nosuchkey\n");
    CY_CHECK(unknown_control.error.find("line 1") != std::string::npos);
    // Nothing of either file was stored: Space still jumps, the left button still fires.
    CY_CHECK_EQ(rig.server.user(0).profile().count(), 0U);
}

CY_TEST_CASE("an input script reads every verb and refuses a line it does not know") {
    cy::play::InputScript script;
    CY_REQUIRE(script
                   .parse("# frame what\n"
                          "30 key w down\n"
                          "30 mouse move 120 -40\n"
                          "31 mouse to 640 360\n"
                          "40 mouse left down\n"
                          "50 focus lost\n"
                          "51 focus gained\n"
                          "60 pad attach\n"
                          "61 pad axis leftStickY 1\n"
                          "62 pad button south down\n"
                          "55 window 1600 900\n"
                          "10 key space up\n"
                          "900 quit\n")
                   .has_value());
    CY_CHECK_EQ(script.size(), 12U);
    // Without the platform it can still say when the run ends, and refuses to push anything.
    const auto early = script.run(9, cy::play::ScriptTarget{});
    CY_REQUIRE(early.has_value());
    CY_CHECK_FALSE(*early);
    CY_CHECK_FALSE(script.run(10, cy::play::ScriptTarget{}).has_value());

    cy::play::InputScript bad;
    CY_CHECK_FALSE(bad.parse("10 key nosuchkey down\n").has_value());
    CY_CHECK_FALSE(bad.parse("10 dance\n").has_value());

    cy::play::InputScript ends;
    CY_REQUIRE(ends.parse("5 quit\n").has_value());
    const auto ended = ends.run(5, cy::play::ScriptTarget{});
    CY_REQUIRE(ended.has_value());
    CY_CHECK(*ended);
}
