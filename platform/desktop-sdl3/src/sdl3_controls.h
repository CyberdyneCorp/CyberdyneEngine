// SPDX-License-Identifier: MIT
#pragma once
// SDL's controls as the engine's, the input source's own translations (sdl3_input_source.cpp),
// for the event injector (sdl3_event_injector.cpp), which reads them backwards so an injected key
// is the scancode a person's press of the same key carries. Private to this directory. The SDL
// values are `int`s so this header names no SDL type.

#include <cy/servers/input/types.h>

namespace cy::sdl3 {

/// SDL's physical scancode as the engine's key; `Key::Unknown` for one it has no name for.
[[nodiscard]] input::Key key_of_scancode(int scancode) noexcept;
/// An `SDL_GamepadButton` as the engine's vendor-neutral control.
[[nodiscard]] input::GamepadControl control_of_gamepad_button(int button) noexcept;
/// An `SDL_GamepadAxis` as the engine's control.
[[nodiscard]] input::GamepadControl control_of_gamepad_axis(int axis) noexcept;

}  // namespace cy::sdl3
