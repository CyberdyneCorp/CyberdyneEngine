// SPDX-License-Identifier: MIT
#pragma once
// Synthetic platform input: REAL SDL events pushed onto SDL's own queue.
//
// A replay, a capture and a test that want to be a person at the keyboard push here, not into
// `InputServer::inject`. Everything above then sees the event exactly as it sees a person's: SDL
// queues it, `Sdl3InputSource`'s watch translates it (the scancode, the sign of the mouse's Y, the
// stick's dead range), `Sdl3DisplayServer::pump_events` drains the queue and turns a focus change
// into a `WindowEvent`. Nothing here knows a game, an action or an engine control code beyond the
// translation it runs backwards — which is the point: what this proves is the path, not the game.
//
// A GAMEPAD is SDL's own virtual joystick (`SDL_AttachVirtualJoystick`), typed as a gamepad: SDL
// announces it with SDL_EVENT_GAMEPAD_ADDED and turns every axis and button written to it into the
// gamepad events a pad on a cable produces, at the next `SDL_PumpEvents`. The input source opens
// it as it opens a real one.
//
// No SDL type appears in this header (`tools/layercheck/`): a window is SDL's numeric id, which
// `Sdl3DisplayServer::native_window_id` hands out.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/servers/input/types.h>

namespace cy {

/// SDL's event subsystem alone: no video, no window, no display. What a host that feeds its input
/// through SDL but draws nothing brings up — a headless run of a game a person would play in a
/// window, driven by `Sdl3EventInjector`. A host with a window has the display server bring SDL up
/// instead, and needs none of this.
class Sdl3EventSystem {
public:
    Sdl3EventSystem() = default;
    ~Sdl3EventSystem() { stop(); }

    Sdl3EventSystem(const Sdl3EventSystem&) = delete;
    Sdl3EventSystem& operator=(const Sdl3EventSystem&) = delete;

    [[nodiscard]] Status start() noexcept;
    void stop() noexcept;

private:
    bool started_ = false;
};

class Sdl3EventInjector {
public:
    Sdl3EventInjector() = default;
    ~Sdl3EventInjector();

    Sdl3EventInjector(const Sdl3EventInjector&) = delete;
    Sdl3EventInjector& operator=(const Sdl3EventInjector&) = delete;

    /// A key going down or up, as the scancode a person's press of it carries.
    [[nodiscard]] Status key(input::Key key, bool down) noexcept;
    /// The mouse moved by (`dx`, `dy`) WINDOW pixels — SDL's `xrel`, `yrel`: +y down — to (`x`,
    /// `y`). In relative mode a person's mouse produces exactly this.
    [[nodiscard]] Status mouse_motion(f32 dx, f32 dy, f32 x, f32 y) noexcept;
    /// A mouse button (`Left` to `Extra2`) going down or up at (`x`, `y`).
    [[nodiscard]] Status mouse_button(input::MouseControl button, bool down, f32 x, f32 y) noexcept;
    /// The wheel turned `notches` away from the player (SDL's +y) with the pointer at (`x`, `y`).
    [[nodiscard]] Status mouse_wheel(f32 notches, f32 x, f32 y) noexcept;
    /// Empty SDL's queue: what a host with no display server pumping calls once a frame, or the
    /// queue fills. The input source saw every event as it was pushed.
    static void drain() noexcept;
    /// The window SDL numbers `native_window` gaining or losing the keyboard's focus.
    [[nodiscard]] Status window_focus(u32 native_window, bool gained) noexcept;

    /// Attach a virtual gamepad. Needs SDL's gamepad subsystem, which `Sdl3InputSource::attach`
    /// brings up. Idempotent.
    [[nodiscard]] Status attach_gamepad() noexcept;
    void detach_gamepad() noexcept;
    [[nodiscard]] bool gamepad_attached() const noexcept { return joystick_ != nullptr; }
    /// A stick or trigger axis, in the ENGINE's convention — a stick's +Y is up, a trigger is
    /// [0, 1] — written to the virtual pad in SDL's, and delivered at the next pump.
    [[nodiscard]] Status gamepad_axis(input::GamepadControl axis, f32 value) noexcept;
    [[nodiscard]] Status gamepad_button(input::GamepadControl button, bool down) noexcept;

    /// Events pushed so far.
    [[nodiscard]] u64 pushed() const noexcept { return pushed_; }

private:
    [[nodiscard]] Status push(const void* sdl_event) noexcept;

    /// SDL's `SDL_Joystick*` for the virtual pad, as void* so no SDL type reaches this header.
    void* joystick_ = nullptr;
    u32 joystick_id_ = 0;
    u64 pushed_ = 0;
};

}  // namespace cy
