// SPDX-License-Identifier: MIT
// `integration.sdl3_injector` — synthetic input through SDL's own queue. See sdl3_event_injector.h.
//
// What a play-mode test, a capture and a replay push their events through, so this suite holds the
// one property they all rest on: an injected event reaches the input server exactly as a person's
// would — the same key, the mouse's Y flipped once, a pad's stick and trigger in the engine's
// convention. Like `integration.sdl3_input` it needs no display: SDL's event and gamepad subsystems
// are independent of video, so it runs headless in CI.

#include <SDL3/SDL.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/platform/sdl3_event_injector.h>
#include <cy/platform/sdl3_input_source.h>
#include <cy/test/test.h>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// SDL's event subsystem for one case, as in test_input_source.cpp.
struct SdlEvents {
    SdlEvents() noexcept { initialised = SDL_InitSubSystem(SDL_INIT_EVENTS); }
    ~SdlEvents() {
        if (initialised) {
            SDL_QuitSubSystem(SDL_INIT_EVENTS);
        }
    }

    SdlEvents(const SdlEvents&) = delete;
    SdlEvents& operator=(const SdlEvents&) = delete;

    bool initialised = false;
};

struct Fixture {
    Fixture() noexcept : server(allocator()) {}

    [[nodiscard]] bool start() noexcept {
        cy::input::InputServerConfig config;
        config.event_capacity = 64;
        return server.configure(config).has_value() && server.initialize().has_value();
    }

    cy::input::InputServer server;
    cy::Sdl3InputSource source;
};

}  // namespace
// --- The injector: synthetic input that takes the path a person's does ---------------------------

CY_TEST_CASE("sdl3 injector: an injected key arrives as the key a person's press of it is") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());
    cy::Sdl3EventInjector inject;

    // Through SDL's queue and the source's watch, so the scancode table is run both ways: a key
    // the injector spelt wrongly would arrive as another key or as Unknown.
    for (const cy::input::Key key : {cy::input::Key::W, cy::input::Key::Space,
                                     cy::input::Key::LeftControl, cy::input::Key::Escape}) {
        CY_REQUIRE(inject.key(key, true).has_value());
        const auto& pending = fixture.server.pending();
        CY_REQUIRE_EQ(pending.empty(), false);
        CY_CHECK(pending[pending.size() - 1].control == cy::input::key_control(key));
        CY_CHECK_EQ(pending[pending.size() - 1].value, 1.0F);
    }
    // A key the platform has no scancode for is refused, not sent as something else.
    CY_CHECK_FALSE(inject.key(cy::input::Key::Unknown, true).has_value());
    fixture.source.detach();
}

CY_TEST_CASE("sdl3 injector: injected motion is SDL's, so the source flips its Y once") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());
    cy::Sdl3EventInjector inject;

    CY_REQUIRE(inject.mouse_motion(12.0F, -5.0F, 400.0F, 300.0F).has_value());
    CY_REQUIRE_EQ(fixture.server.pending().size(), 4);
    CY_CHECK_EQ(fixture.server.pending()[0].value, 12.0F);
    // Pushed away (SDL's -y) is +Y up in the input layer.
    CY_CHECK_EQ(fixture.server.pending()[1].value, 5.0F);
    CY_REQUIRE(
        inject.mouse_button(cy::input::MouseControl::Left, true, 400.0F, 300.0F).has_value());
    CY_CHECK(fixture.server.pending()[4].control ==
             cy::input::mouse_control(cy::input::MouseControl::Left));
    fixture.source.detach();
}

CY_TEST_CASE("sdl3 injector: a virtual gamepad's stick and trigger arrive as a pad's would") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());
    cy::Sdl3EventInjector inject;
    CY_REQUIRE(inject.attach_gamepad().has_value());
    SDL_PumpEvents();  // SDL announces the pad; the source opens it and makes it a device

    const auto pad_event = [&](cy::input::GamepadControl control) -> const cy::input::DeviceEvent* {
        const auto& pending = fixture.server.pending();
        for (cy::usize index = pending.size(); index > 0; --index) {
            if (pending[index - 1].control == cy::input::gamepad_control(control)) {
                return &pending[index - 1];
            }
        }
        return nullptr;
    };
    CY_REQUIRE(inject.gamepad_axis(cy::input::GamepadControl::LeftStickY, 1.0F).has_value());
    CY_REQUIRE(inject.gamepad_axis(cy::input::GamepadControl::RightTrigger, 1.0F).has_value());
    SDL_PumpEvents();
    const cy::input::DeviceEvent* stick = pad_event(cy::input::GamepadControl::LeftStickY);
    const cy::input::DeviceEvent* trigger = pad_event(cy::input::GamepadControl::RightTrigger);
    CY_REQUIRE(stick != nullptr);
    CY_REQUIRE(trigger != nullptr);
    // The stick pushed forward is +Y in the engine, as W is: the injector wrote SDL's -Y and the
    // source flipped it back.
    CY_CHECK(stick->value > 0.99F);
    CY_CHECK(trigger->value > 0.99F);
    CY_CHECK_FALSE(stick->device.is_null());
    // The source closes its pad first; the injector then detaches the device under it.
    fixture.source.detach();
    inject.detach_gamepad();
}
