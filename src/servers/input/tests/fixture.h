#pragma once
// The shared fixture for CyberInput's suites. Section 4.1.
//
// One fixture rather than one per file, because every case needs the same four things — a server, a
// user, a device of the class its bindings name, and a context — and a copy of that in six files is
// six places to change when the server's bring-up order does.
//
// WHAT IS NOT HERE, AND IS THE POINT: no window, no display server, no ECS world, no job system, no
// GPU. `input-and-actions` — "Input SHALL function with no devices present ... Absence of a device
// backend SHALL NOT prevent the system from initialising" — so the tests run the same path a
// dedicated server does, and the device below is one the test invented rather than one the platform
// found.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/input/server.h>
#include <cy/test/test.h>

namespace cy::input_test {

using namespace cy::input;

// `cy::input` does not re-export the core vocabulary and should not: these are declarations local
// to the suites, so that a case reads `Vec3` without every file repeating the same four lines.
using cy::Nanoseconds;
using cy::Quat;
using cy::Vec2;
using cy::Vec3;

inline cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// One millisecond, as nanoseconds. Every timestamp in these suites is a multiple of it, so that a
/// case reads as "at 5 ms" rather than as a nine-digit literal.
inline constexpr Nanoseconds kMs = 1'000'000;

struct Fixture {
    Fixture() noexcept : server(allocator()) {}

    [[nodiscard]] bool start(u32 users = 1) noexcept {
        InputServerConfig config;
        config.users = users;
        config.event_capacity = 256;
        return server.configure(config).has_value() && server.initialize().has_value();
    }

    /// Declare an action and return its dense id. The stable id is derived from the declaration
    /// order here only because a test has no identity manifest; in a project it comes from the
    /// authored asset — see action.h.
    [[nodiscard]] ActionId declare(const char* name, u32 stable, ActionValueType type,
                                   bool in_frame = false, f32 buffer_seconds = 0.0F) noexcept {
        ActionDeclaration declaration;
        declaration.name = Name::intern(name);
        declaration.stable_id = ActionStableId{stable};
        declaration.type = type;
        declaration.in_command_frame = in_frame;
        declaration.buffer_window_seconds = buffer_seconds;
        auto declared = server.actions().declare(declaration);
        return declared ? *declared : kInvalidAction;
    }

    /// Connect a device of `kind` and give it to `user`. Returns a null id on failure, which every
    /// caller checks with `CY_REQUIRE`.
    [[nodiscard]] DeviceId attach(DeviceKind kind, u32 user = 0,
                                  const char* hardware = "test-device") noexcept {
        DeviceDescription description;
        description.kind = kind;
        description.hardware_id = Name::intern(hardware);
        description.display_name = description.hardware_id;
        auto connected = server.devices().connect(description, 0);
        if (!connected) {
            return DeviceId{};
        }
        if (!server.assign(*connected, user, 0).has_value()) {
            return DeviceId{};
        }
        return *connected;
    }

    /// Everything after the declarations: size the users' records, register the context, push it.
    [[nodiscard]] bool install(MappingContext&& context, u32 user = 0, i32 priority = 0,
                               FocusLayer layer = FocusLayer::Gameplay) noexcept {
        if (!server.finalize_declarations().has_value()) {
            return false;
        }
        auto registered = server.register_context(std::move(context));
        if (!registered) {
            return false;
        }
        installed = *registered;
        return server.user(user).push_context(installed, priority, layer).has_value();
    }

    void press(DeviceId device, Control control, Nanoseconds at, f32 value = 1.0F) noexcept {
        DeviceEvent event;
        event.timestamp = at;
        event.device = device;
        event.control = control;
        event.value = value;
        server.submit(event);
    }

    void release(DeviceId device, Control control, Nanoseconds at) noexcept {
        press(device, control, at, 0.0F);
    }

    InputServer server;
    ContextHandle installed;
};

/// A one-control digital binding. The shape most cases want, spelled once.
[[nodiscard]] inline Binding simple(ActionId action, Control control,
                                    TriggerKind trigger = TriggerKind::Down) noexcept {
    Binding binding;
    binding.action = action;
    binding.kind = BindingKind::Simple;
    binding.component_count = 1;
    binding.components[0].control = control;
    binding.components[0].weight = Vec3{1.0F, 0.0F, 0.0F};
    binding.trigger.kind = trigger;
    return binding;
}

/// Four keys as one two-dimensional axis: `input-and-actions`' "Keys become an axis".
[[nodiscard]] inline Binding wasd(ActionId action, Key left, Key right, Key down, Key up) noexcept {
    Binding binding;
    binding.action = action;
    binding.kind = BindingKind::Axis2D;
    binding.component_count = 4;
    binding.components[0] = BindingComponent{key_control(left), Vec3{-1.0F, 0.0F, 0.0F}};
    binding.components[1] = BindingComponent{key_control(right), Vec3{1.0F, 0.0F, 0.0F}};
    binding.components[2] = BindingComponent{key_control(down), Vec3{0.0F, -1.0F, 0.0F}};
    binding.components[3] = BindingComponent{key_control(up), Vec3{0.0F, 1.0F, 0.0F}};
    binding.trigger.kind = TriggerKind::Down;
    return binding;
}

}  // namespace cy::input_test
