// SPDX-License-Identifier: MIT
// Synthetic platform input as real SDL events. See sdl3_event_injector.h.

#include <cy/platform/sdl3_event_injector.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sdl3_controls.h"

namespace cy {
namespace {

using input::GamepadControl;
using input::Key;
using input::MouseControl;

/// The scancode `key_from_scancode` turns into `key`: the translation run backwards, so the two
/// can never disagree. Zero (SDL_SCANCODE_UNKNOWN) for a key with no scancode.
[[nodiscard]] SDL_Scancode scancode_of(Key key) noexcept {
    if (key == Key::Unknown) {
        return SDL_SCANCODE_UNKNOWN;  // every scancode the engine has no name for reads as this
    }
    for (int code = 1; code < SDL_SCANCODE_COUNT; ++code) {
        if (sdl3::key_of_scancode(code) == key) {
            return static_cast<SDL_Scancode>(code);
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

[[nodiscard]] int sdl_button_of(MouseControl button) noexcept {
    switch (button) {
        case MouseControl::Left:
            return SDL_BUTTON_LEFT;
        case MouseControl::Right:
            return SDL_BUTTON_RIGHT;
        case MouseControl::Middle:
            return SDL_BUTTON_MIDDLE;
        case MouseControl::Extra1:
            return SDL_BUTTON_X1;
        case MouseControl::Extra2:
            return SDL_BUTTON_X2;
        default:
            return 0;
    }
}

[[nodiscard]] int gamepad_button_of(GamepadControl control) noexcept {
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        if (sdl3::control_of_gamepad_button(button) == control) {
            return button;
        }
    }
    return -1;
}

[[nodiscard]] int gamepad_axis_of(GamepadControl control) noexcept {
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
        if (sdl3::control_of_gamepad_axis(axis) == control) {
            return axis;
        }
    }
    return -1;
}

/// An engine axis value as SDL's `int16`: a stick's Y flipped back to SDL's +y down, the range
/// clamped, a trigger on the positive half only.
[[nodiscard]] Sint16 sdl_axis_value(int axis, f32 value) noexcept {
    const bool stick_y = axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTY;
    const bool trigger =
        axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    f32 sdl = stick_y ? -value : value;
    const f32 low = trigger ? 0.0F : -1.0F;
    sdl = std::clamp(sdl, low, 1.0F);
    return static_cast<Sint16>(std::lround(sdl * 32767.0F));
}

/// SDL's own reason, copied: SDL overwrites its buffer at the next failing call, and an `Error`
/// borrows its message.
thread_local char g_refusal[256];

[[nodiscard]] Unexpected<Error> sdl_refused() noexcept {
    (void)std::snprintf(g_refusal, sizeof(g_refusal), "%s", SDL_GetError());
    return fail(ErrorCode::Unavailable, g_refusal);
}

}  // namespace

Sdl3EventInjector::~Sdl3EventInjector() {
    detach_gamepad();
}

Status Sdl3EventInjector::push(const void* sdl_event) noexcept {
    SDL_Event copy = *static_cast<const SDL_Event*>(sdl_event);
    // Zero asks SDL to stamp it now, as it stamps a person's: the watch reads the time SDL gave it.
    copy.common.timestamp = 0;
    if (!SDL_PushEvent(&copy)) {
        return sdl_refused();
    }
    ++pushed_;
    return ok();
}

Status Sdl3EventInjector::key(Key key, bool down) noexcept {
    const SDL_Scancode scancode = scancode_of(key);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return fail(ErrorCode::InvalidArgument, "that key has no SDL scancode to inject");
    }
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.down = down;
    event.key.repeat = false;
    return push(&event);
}

Status Sdl3EventInjector::mouse_motion(f32 dx, f32 dy, f32 x, f32 y) noexcept {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.xrel = dx;
    event.motion.yrel = dy;
    event.motion.x = x;
    event.motion.y = y;
    return push(&event);
}

Status Sdl3EventInjector::mouse_button(MouseControl button, bool down, f32 x, f32 y) noexcept {
    const int sdl_button = sdl_button_of(button);
    if (sdl_button == 0) {
        return fail(ErrorCode::InvalidArgument, "that mouse control is not a button");
    }
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = static_cast<Uint8>(sdl_button);
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    return push(&event);
}

Status Sdl3EventInjector::window_focus(u32 native_window, bool gained) noexcept {
    if (native_window == 0U) {
        return fail(ErrorCode::InvalidArgument, "no window to address the focus change to");
    }
    SDL_Event event{};
    event.type = gained ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    event.window.windowID = native_window;
    return push(&event);
}

Status Sdl3EventInjector::attach_gamepad() noexcept {
    if (joystick_ != nullptr) {
        return ok();
    }
    if (SDL_WasInit(SDL_INIT_GAMEPAD) == 0U) {
        return fail(ErrorCode::Unavailable,
                    "SDL's gamepad subsystem is down; attach the input source first");
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = static_cast<Uint16>(SDL_GAMEPAD_AXIS_COUNT);
    desc.nbuttons = static_cast<Uint16>(SDL_GAMEPAD_BUTTON_COUNT);
    desc.name = "CyberEngine virtual gamepad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        return sdl_refused();
    }
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (joystick == nullptr) {
        (void)SDL_DetachVirtualJoystick(id);
        return sdl_refused();
    }
    joystick_ = joystick;
    joystick_id_ = static_cast<u32>(id);
    return ok();
}

void Sdl3EventInjector::detach_gamepad() noexcept {
    if (joystick_ == nullptr) {
        return;
    }
    SDL_CloseJoystick(static_cast<SDL_Joystick*>(joystick_));
    (void)SDL_DetachVirtualJoystick(static_cast<SDL_JoystickID>(joystick_id_));
    joystick_ = nullptr;
    joystick_id_ = 0;
}

Status Sdl3EventInjector::gamepad_axis(GamepadControl axis, f32 value) noexcept {
    const int sdl_axis = gamepad_axis_of(axis);
    if (joystick_ == nullptr || sdl_axis < 0) {
        return fail(ErrorCode::InvalidArgument, "no virtual gamepad, or that control is no axis");
    }
    if (!SDL_SetJoystickVirtualAxis(static_cast<SDL_Joystick*>(joystick_), sdl_axis,
                                    sdl_axis_value(sdl_axis, value))) {
        return sdl_refused();
    }
    ++pushed_;
    return ok();
}

Status Sdl3EventInjector::gamepad_button(GamepadControl button, bool down) noexcept {
    const int sdl_button = gamepad_button_of(button);
    if (joystick_ == nullptr || sdl_button < 0) {
        return fail(ErrorCode::InvalidArgument, "no virtual gamepad, or that control is no button");
    }
    if (!SDL_SetJoystickVirtualButton(static_cast<SDL_Joystick*>(joystick_), sdl_button, down)) {
        return sdl_refused();
    }
    ++pushed_;
    return ok();
}

}  // namespace cy
