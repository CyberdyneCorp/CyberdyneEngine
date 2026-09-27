// SPDX-License-Identifier: MIT
// `integration.game_backend_input` — ABI 1.3's `input_*` entries over a real input server.
// `add-swift-game-api`.
//
// OWNER: implementer A. Every case goes through `cy_get_interface`'s table with an `InputAdapter`
// bound on a real `cy::abi::Host`, so what is proven is what a Swift behaviour would see: the
// adapter's mapping AND the thunk in front of it.
//
// The case the entry exists for is the first one: a press and a release inside one tick must read
// as one press and one release, in a fixed step, and a replay that feeds the same events to a
// fresh server must read byte for byte the same.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/input_backend.h>
#include <cy/servers/input/server.h>
#include <cy/test/test.h>

#include <cstring>
#include <utility>

namespace {

using namespace cy::input;
using cy::Nanoseconds;

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

/// A server with two users, a keyboard and a mouse for user 0, a `select` action on Space pushed in
/// `gameplay`, and a `confirm` action on Enter in a `menu` context that is registered and NOT
/// pushed — plus the adapter and a host with it bound.
struct Rig {
    InputServer server{allocator()};
    cy::game_backend::InputAdapter adapter{server};
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    DeviceId keyboard;
    DeviceId mouse;
    ActionId select = kInvalidAction;
    ActionId confirm = kInvalidAction;
    ContextHandle menu;
    cy::u64 tick = 0;

    [[nodiscard]] bool build() noexcept {
        InputServerConfig config;
        config.users = 2;
        config.event_capacity = 256;
        if (!server.configure(config) || !server.initialize()) {
            return false;
        }
        select = declare("select", 1);
        confirm = declare("confirm", 2);
        keyboard = attach(DeviceKind::Keyboard, "kb");
        mouse = attach(DeviceKind::Mouse, "mouse");
        if (!server.finalize_declarations()) {
            return false;
        }
        MappingContext gameplay(allocator());
        gameplay.set_name(cy::Name::intern("gameplay"));
        MappingContext menu_context(allocator());
        menu_context.set_name(cy::Name::intern("menu"));
        if (!gameplay.add(simple(select, key_control(Key::Space))) ||
            !menu_context.add(simple(confirm, key_control(Key::Enter)))) {
            return false;
        }
        auto gameplay_handle = server.register_context(std::move(gameplay));
        auto menu_handle = server.register_context(std::move(menu_context));
        if (!gameplay_handle || !menu_handle) {
            return false;
        }
        menu = *menu_handle;
        cy::game_backend::bind(host, &adapter);
        return server.user(0).push_context(*gameplay_handle, 0).has_value();
    }

    ActionId declare(const char* name, cy::u32 stable) noexcept {
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

    void send(DeviceId device, Control control, Nanoseconds at, cy::f32 value) noexcept {
        DeviceEvent event;
        event.timestamp = at;
        event.device = device;
        event.control = control;
        event.value = value;
        server.submit(event);
    }

    /// One fixed step, the way a host runs it: the adapter sees the window, the server resolves.
    void step() noexcept {
        adapter.observe_pending();
        ++tick;
        server.resolve_tick(tick, static_cast<Nanoseconds>(tick) * 16 * kMs, 1.0F / 60.0F);
    }

    CyEngine engine() noexcept { return &host; }
};

CyInputActionState read_state(CyEngine engine, cy::u32 user, const char* name) noexcept {
    CyInputActionState state{};
    state.struct_size = sizeof(CyInputActionState);
    CY_CHECK_EQ(table().input_action_state_by_name(engine, user, name, &state), CY_RESULT_OK);
    return state;
}

/// Every field of two states identical. A `memcmp` of the whole struct would also compare its
/// padding, which nothing writes.
bool identical(const CyInputActionState& a, const CyInputActionState& b) noexcept {
    return a.struct_size == b.struct_size && a.flags == b.flags && a.value[0] == b.value[0] &&
           a.value[1] == b.value[1] && a.value[2] == b.value[2] && a.press_count == b.press_count &&
           a.release_count == b.release_count && a.tick == b.tick;
}

/// Run the press-and-release tick on a fresh rig and return what a fixed step reads.
CyInputActionState press_and_release_in_one_tick() noexcept {
    Rig rig;
    CY_REQUIRE(rig.build());
    rig.step();
    rig.send(rig.keyboard, key_control(Key::Space), 20 * kMs, 1.0F);
    rig.send(rig.keyboard, key_control(Key::Space), 24 * kMs, 0.0F);
    rig.step();
    const cy::abi::game::PhaseScope fixed(rig.host.game.clock, CY_PHASE_FIXED_UPDATE);
    return read_state(rig.engine(), 0, "select");
}

}  // namespace

CY_TEST_CASE("a press and a release inside one tick read as one of each, live and replayed") {
    const CyInputActionState live = press_and_release_in_one_tick();
    CY_CHECK_EQ(live.press_count, 1U);
    CY_CHECK_EQ(live.release_count, 1U);
    CY_CHECK((live.flags & CY_INPUT_ACTION_JUST_PRESSED) != 0U);
    CY_CHECK((live.flags & CY_INPUT_ACTION_JUST_RELEASED) != 0U);
    CY_CHECK((live.flags & CY_INPUT_ACTION_PRESSED) == 0U);  // the level is back up
    CY_CHECK_EQ(live.tick, 2U);

    // The replay: the same events into a fresh server. Byte-identical, which is what makes an `F`
    // read of an action the same on a replay and on a lockstep peer.
    const CyInputActionState replayed = press_and_release_in_one_tick();
    CY_CHECK(identical(live, replayed));
}

CY_TEST_CASE("an action is found by its declared name, and an unknown name is NOT_FOUND") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CyInputAction action = CY_INPUT_ACTION_INVALID;
    CY_CHECK_EQ(table().input_find_action(rig.engine(), "confirm", &action), CY_RESULT_OK);
    CY_CHECK_EQ(action, rig.confirm);
    CY_CHECK_EQ(table().input_find_action(rig.engine(), "never-declared-anywhere", &action),
                CY_RESULT_NOT_FOUND);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "never-declared-anywhere") != nullptr);
    CY_CHECK_EQ(action, rig.confirm);
}

CY_TEST_CASE(
    "a user the server does not have is OUT_OF_RANGE; an action index it lacks is not "
    "found") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CyInputActionState state{};
    CY_CHECK_EQ(table().input_action_state(rig.engine(), 2, rig.select, &state),
                CY_RESULT_OUT_OF_RANGE);
    CY_CHECK_EQ(table().input_action_state(rig.engine(), 0, 40, &state), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().input_action_state(rig.engine(), 0, CY_INPUT_ACTION_INVALID, &state),
                CY_RESULT_NOT_FOUND);
    CyInputPointer pointer{};
    CY_CHECK_EQ(table().input_pointer(rig.engine(), 9, &pointer), CY_RESULT_OUT_OF_RANGE);
    CY_CHECK_EQ(table().input_push_context(rig.engine(), 9, rig.menu.bits(), 0),
                CY_RESULT_OUT_OF_RANGE);
}

CY_TEST_CASE("a pushed context takes effect from the next tick and a popped one stops there") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CyInputContext menu = CY_INPUT_CONTEXT_NULL;
    CY_CHECK_EQ(table().input_find_context(rig.engine(), "menu", &menu), CY_RESULT_OK);
    CY_CHECK_EQ(menu, rig.menu.bits());
    CY_CHECK_EQ(table().input_find_context(rig.engine(), "inventory", &menu), CY_RESULT_NOT_FOUND);

    // Enter before the push is bound to nothing.
    rig.send(rig.keyboard, key_control(Key::Enter), 10 * kMs, 1.0F);
    rig.step();
    CY_CHECK_EQ(read_state(rig.engine(), 0, "confirm").flags & CY_INPUT_ACTION_PRESSED, 0U);

    {
        const cy::abi::game::PhaseScope fixed(rig.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(table().input_push_context(rig.engine(), 0, rig.menu.bits(), 5), CY_RESULT_OK);
        CY_CHECK_EQ(table().input_push_context(rig.engine(), 0, rig.menu.bits(), 5),
                    CY_RESULT_ALREADY_EXISTS);
    }
    // Not mid-tick: the state the fixed step reads is still the resolved one.
    CY_CHECK_EQ(read_state(rig.engine(), 0, "confirm").flags & CY_INPUT_ACTION_PRESSED, 0U);

    rig.send(rig.keyboard, key_control(Key::Enter), 20 * kMs, 0.0F);
    rig.send(rig.keyboard, key_control(Key::Enter), 21 * kMs, 1.0F);
    rig.step();
    const CyInputActionState pushed = read_state(rig.engine(), 0, "confirm");
    CY_CHECK((pushed.flags & CY_INPUT_ACTION_PRESSED) != 0U);
    CY_CHECK_EQ(pushed.press_count, 1U);

    CY_CHECK_EQ(table().input_pop_context(rig.engine(), 0, rig.menu.bits()), CY_RESULT_OK);
    CY_CHECK_EQ(table().input_pop_context(rig.engine(), 0, rig.menu.bits()), CY_RESULT_NOT_FOUND);
    rig.step();
    CY_CHECK_EQ(read_state(rig.engine(), 0, "confirm").flags & CY_INPUT_ACTION_PRESSED, 0U);

    // A value no registration produced is not a context.
    CY_CHECK_EQ(table().input_push_context(rig.engine(), 0, CY_INPUT_CONTEXT_NULL, 0),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().input_push_context(rig.engine(), 0, 0x700000001ULL, 0),
                CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("the pointer reports position, held buttons and the edges since the last frame") {
    Rig rig;
    CY_REQUIRE(rig.build());
    rig.send(rig.mouse, mouse_control(MouseControl::PositionX), 10 * kMs, 640.0F);
    rig.send(rig.mouse, mouse_control(MouseControl::PositionY), 10 * kMs, 360.0F);
    rig.send(rig.mouse, mouse_control(MouseControl::MoveX), 11 * kMs, 4.0F);
    rig.send(rig.mouse, mouse_control(MouseControl::Left), 12 * kMs, 1.0F);
    rig.send(rig.mouse, mouse_control(MouseControl::Right), 13 * kMs, 1.0F);
    rig.send(rig.mouse, mouse_control(MouseControl::Right), 14 * kMs, 0.0F);
    rig.send(rig.mouse, mouse_control(MouseControl::Wheel), 15 * kMs, -1.0F);
    rig.step();
    rig.send(rig.mouse, mouse_control(MouseControl::Wheel), 30 * kMs, -2.0F);
    rig.send(rig.mouse, mouse_control(MouseControl::MoveX), 31 * kMs, 3.0F);
    rig.step();  // two ticks in one frame: the edges add up across both
    rig.adapter.begin_frame();

    const cy::abi::game::PhaseScope frame(rig.host.game.clock, CY_PHASE_FRAME_UPDATE);
    CyInputPointer pointer{};
    CY_CHECK_EQ(table().input_pointer(rig.engine(), 0, &pointer), CY_RESULT_OK);
    CY_CHECK_EQ(pointer.flags, CY_INPUT_POINTER_PRESENT | CY_INPUT_POINTER_IN_WINDOW);
    CY_CHECK_EQ(pointer.position[0], 640.0F);
    CY_CHECK_EQ(pointer.position[1], 360.0F);
    CY_CHECK_EQ(pointer.buttons, CY_INPUT_BUTTON_LEFT);
    CY_CHECK_EQ(pointer.buttons_pressed, CY_INPUT_BUTTON_LEFT | CY_INPUT_BUTTON_RIGHT);
    CY_CHECK_EQ(pointer.buttons_released, CY_INPUT_BUTTON_RIGHT);
    CY_CHECK_EQ(pointer.delta[0], 7.0F);
    CY_CHECK_EQ(pointer.wheel[1], -3.0F);
    CY_CHECK_EQ(pointer.wheel[0], 0.0F);

    // The next frame with nothing new: still held, no edges.
    rig.adapter.begin_frame();
    rig.adapter.set_pointer_focus(0, true, true);
    CY_CHECK_EQ(table().input_pointer(rig.engine(), 0, &pointer), CY_RESULT_OK);
    CY_CHECK_EQ(pointer.buttons, CY_INPUT_BUTTON_LEFT);
    CY_CHECK_EQ(pointer.buttons_pressed, 0U);
    CY_CHECK_EQ(pointer.buttons_released, 0U);
    CY_CHECK_EQ(pointer.wheel[1], 0.0F);
    CY_CHECK((pointer.flags & CY_INPUT_POINTER_OVER_UI) != 0U);

    // A fixed step may not read it at all.
    const cy::abi::game::PhaseScope fixed(rig.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CY_CHECK_EQ(table().input_pointer(rig.engine(), 0, &pointer), CY_RESULT_PERMISSION_DENIED);
}

CY_TEST_CASE("a user without a pointing device has a pointer that is not present") {
    Rig rig;
    CY_REQUIRE(rig.build());
    CyInputPointer pointer{};
    pointer.struct_size = sizeof(pointer);
    CY_CHECK_EQ(table().input_pointer(rig.engine(), 1, &pointer), CY_RESULT_OK);
    CY_CHECK_EQ(pointer.flags, 0U);
    CY_CHECK_EQ(pointer.buttons, 0U);
    cy::u32 modifiers = 0xFFU;
    CY_CHECK_EQ(table().input_modifiers(rig.engine(), 1, &modifiers), CY_RESULT_OK);
    CY_CHECK_EQ(modifiers, 0U);
}

CY_TEST_CASE("the modifier keys held on the user's keyboard") {
    Rig rig;
    CY_REQUIRE(rig.build());
    rig.send(rig.keyboard, key_control(Key::RightShift), 5 * kMs, 1.0F);
    rig.send(rig.keyboard, key_control(Key::LeftAlt), 6 * kMs, 1.0F);
    rig.step();
    cy::u32 modifiers = 0;
    CY_CHECK_EQ(table().input_modifiers(rig.engine(), 0, &modifiers), CY_RESULT_OK);
    CY_CHECK_EQ(modifiers, CY_INPUT_MOD_SHIFT | CY_INPUT_MOD_ALT);
    rig.send(rig.keyboard, key_control(Key::RightShift), 20 * kMs, 0.0F);
    rig.send(rig.keyboard, key_control(Key::LeftControl), 21 * kMs, 1.0F);
    rig.step();
    CY_CHECK_EQ(table().input_modifiers(rig.engine(), 0, &modifiers), CY_RESULT_OK);
    CY_CHECK_EQ(modifiers, CY_INPUT_MOD_CTRL | CY_INPUT_MOD_ALT);
}
