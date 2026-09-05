// The SDL3 input event source. Task 4.1.1. See sdl3_input_source.h for why this is an event watch.

#include <cy/platform/sdl3_input_source.h>

#include <SDL3/SDL.h>

namespace cy {
namespace {

using input::Control;
using input::DeviceKind;
using input::GamepadControl;
using input::Key;
using input::MouseControl;

/// SDL's physical scancode to the engine's key numbering.
///
/// SCANCODE, NOT KEYCODE, AND THE REASON IS PLAYABILITY. A scancode is the physical key; a keycode
/// is what the layout says it produces. Binding WASD to keycodes puts a French player's movement on
/// ZQSD-shaped nonsense, and binding them to scancodes puts it under the same four fingers on every
/// layout. Text is the other stream entirely — see `input-and-actions`' "Text entry is separate" —
/// so nothing is lost by ignoring the layout here.
[[nodiscard]] Key key_from_scancode(SDL_Scancode scancode) noexcept {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<u16>(Key::A) +
                                static_cast<u16>(scancode - SDL_SCANCODE_A));
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<u16>(Key::Num1) +
                                static_cast<u16>(scancode - SDL_SCANCODE_1));
    }
    switch (scancode) {
        case SDL_SCANCODE_0:
            return Key::Num0;
        case SDL_SCANCODE_SPACE:
            return Key::Space;
        case SDL_SCANCODE_RETURN:
            return Key::Enter;
        case SDL_SCANCODE_ESCAPE:
            return Key::Escape;
        case SDL_SCANCODE_TAB:
            return Key::Tab;
        case SDL_SCANCODE_BACKSPACE:
            return Key::Backspace;
        case SDL_SCANCODE_LSHIFT:
            return Key::LeftShift;
        case SDL_SCANCODE_RSHIFT:
            return Key::RightShift;
        case SDL_SCANCODE_LCTRL:
            return Key::LeftControl;
        case SDL_SCANCODE_RCTRL:
            return Key::RightControl;
        case SDL_SCANCODE_LALT:
            return Key::LeftAlt;
        case SDL_SCANCODE_RALT:
            return Key::RightAlt;
        case SDL_SCANCODE_LEFT:
            return Key::Left;
        case SDL_SCANCODE_RIGHT:
            return Key::Right;
        case SDL_SCANCODE_UP:
            return Key::Up;
        case SDL_SCANCODE_DOWN:
            return Key::Down;
        case SDL_SCANCODE_F1:
            return Key::F1;
        case SDL_SCANCODE_F2:
            return Key::F2;
        case SDL_SCANCODE_F3:
            return Key::F3;
        case SDL_SCANCODE_F4:
            return Key::F4;
        case SDL_SCANCODE_F5:
            return Key::F5;
        case SDL_SCANCODE_F6:
            return Key::F6;
        case SDL_SCANCODE_F7:
            return Key::F7;
        case SDL_SCANCODE_F8:
            return Key::F8;
        case SDL_SCANCODE_F9:
            return Key::F9;
        case SDL_SCANCODE_F10:
            return Key::F10;
        case SDL_SCANCODE_F11:
            return Key::F11;
        case SDL_SCANCODE_F12:
            return Key::F12;
        default:
            return Key::Unknown;
    }
}

/// SDL's gamepad button to the engine's vendor-neutral numbering.
///
/// `South` rather than `A` or `Cross`: `input-and-actions` forbids gameplay depending on controller
/// button indices, and a name that means "the bottom face button" is the same on every pad.
[[nodiscard]] GamepadControl button_from_sdl(SDL_GamepadButton button) noexcept {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            return GamepadControl::South;
        case SDL_GAMEPAD_BUTTON_EAST:
            return GamepadControl::East;
        case SDL_GAMEPAD_BUTTON_WEST:
            return GamepadControl::West;
        case SDL_GAMEPAD_BUTTON_NORTH:
            return GamepadControl::North;
        case SDL_GAMEPAD_BUTTON_BACK:
            return GamepadControl::Back;
        case SDL_GAMEPAD_BUTTON_GUIDE:
            return GamepadControl::Guide;
        case SDL_GAMEPAD_BUTTON_START:
            return GamepadControl::Start;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            return GamepadControl::LeftStick;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
            return GamepadControl::RightStick;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            return GamepadControl::LeftShoulder;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            return GamepadControl::RightShoulder;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            return GamepadControl::DpadUp;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            return GamepadControl::DpadDown;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            return GamepadControl::DpadLeft;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            return GamepadControl::DpadRight;
        default:
            return GamepadControl::Unknown;
    }
}

[[nodiscard]] GamepadControl axis_from_sdl(SDL_GamepadAxis axis) noexcept {
    switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            return GamepadControl::LeftStickX;
        case SDL_GAMEPAD_AXIS_LEFTY:
            return GamepadControl::LeftStickY;
        case SDL_GAMEPAD_AXIS_RIGHTX:
            return GamepadControl::RightStickX;
        case SDL_GAMEPAD_AXIS_RIGHTY:
            return GamepadControl::RightStickY;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            return GamepadControl::LeftTrigger;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            return GamepadControl::RightTrigger;
        default:
            return GamepadControl::Unknown;
    }
}

/// An SDL axis is an `int16`; a trigger uses only the positive half.
///
/// The asymmetry of the signed range is handled rather than ignored: dividing by 32767 lets -32768
/// produce -1.00003, and a value outside [-1, 1] arriving at a dead zone that rescales by
/// `(v - t) / (1 - t)` comes out above 1 as well. Clamping here is one comparison and removes the
/// whole class.
[[nodiscard]] f32 axis_value(i16 raw, bool trigger) noexcept {
    if (trigger) {
        return static_cast<f32>(raw) / 32767.0F;
    }
    const f32 scaled = static_cast<f32>(raw) / 32767.0F;
    return scaled < -1.0F ? -1.0F : scaled;
}

/// SDL's Y axis grows downward for sticks. The engine's two-dimensional actions grow upward, so
/// "push the stick forward" and "press W" produce the same sign — which is the whole point of an
/// action being device-independent.
[[nodiscard]] bool axis_is_inverted(SDL_GamepadAxis axis) noexcept {
    return axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTY;
}

/// The trampoline. SDL takes a plain function pointer with its own signature, and
/// sdl3_input_source.h may not name `SDL_Event` — so the type-correct function is here and it
/// forwards to the class. Casting the member's `void*` signature to `SDL_EventFilter` instead would
/// be a call through an incompatible function pointer type, which is undefined even where it
/// happens to work.
bool SDLCALL event_watch(void* userdata, SDL_Event* event) {
    return Sdl3InputSource::watch(userdata, event);
}

}  // namespace

Sdl3InputSource::~Sdl3InputSource() {
    detach();
}

Status Sdl3InputSource::attach(input::InputServer& server) noexcept {
    if (server_ != nullptr) {
        return ok();
    }
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0u) {
        return fail(ErrorCode::Unavailable,
                    "SDL3 input: the event subsystem is not initialised; bring the display server "
                    "up first, or call SDL_InitSubSystem(SDL_INIT_EVENTS)");
    }
    // Gamepads are their own subsystem and are optional: a machine with none is ordinary, and a
    // dedicated server has neither. Failing here would make "no controller" a startup error.
    if (SDL_WasInit(SDL_INIT_GAMEPAD) == 0u) {
        (void)SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }

    input::DeviceDescription keyboard;
    keyboard.kind = DeviceKind::Keyboard;
    keyboard.hardware_id = Name::intern("sdl3/keyboard");
    keyboard.display_name = keyboard.hardware_id;
    keyboard.capabilities = static_cast<u16>(input::DeviceCapability::TextInput);
    auto connected_keyboard = server.devices().connect(keyboard, 0);
    if (!connected_keyboard) {
        return make_unexpected(connected_keyboard.error());
    }

    input::DeviceDescription mouse;
    mouse.kind = DeviceKind::Mouse;
    mouse.hardware_id = Name::intern("sdl3/mouse");
    mouse.display_name = mouse.hardware_id;
    auto connected_mouse = server.devices().connect(mouse, 0);
    if (!connected_mouse) {
        return make_unexpected(connected_mouse.error());
    }

    if (!SDL_AddEventWatch(&event_watch, this)) {
        return fail(ErrorCode::Unavailable, "SDL3 input: SDL_AddEventWatch failed");
    }
    keyboard_ = *connected_keyboard;
    mouse_ = *connected_mouse;
    server_ = &server;

    // Gamepads already plugged in when we attached. SDL only sends ADDED for ones that arrive
    // afterwards, so without this a controller connected before start-up is invisible — a bug that
    // reproduces on every machine except the developer's, who plugs theirs in while the game runs.
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        for (int index = 0; index < count; ++index) {
            open_gamepad(static_cast<u32>(ids[index]), 0);
        }
        SDL_free(ids);
    }
    return ok();
}

void Sdl3InputSource::detach() noexcept {
    if (server_ == nullptr) {
        return;
    }
    SDL_RemoveEventWatch(&event_watch, this);
    for (u32 index = 0; index < gamepad_count_; ++index) {
        if (gamepads_[index].handle != nullptr) {
            SDL_CloseGamepad(static_cast<SDL_Gamepad*>(gamepads_[index].handle));
        }
    }
    gamepad_count_ = 0;
    server_ = nullptr;
}

input::DeviceId Sdl3InputSource::gamepad(u32 instance_id) const noexcept {
    for (u32 index = 0; index < gamepad_count_; ++index) {
        if (gamepads_[index].instance_id == instance_id) {
            return gamepads_[index].device;
        }
    }
    return input::DeviceId{};
}

bool Sdl3InputSource::watch(void* userdata, void* sdl_event) noexcept {
    static_cast<Sdl3InputSource*>(userdata)->on_event(sdl_event);
    // True: the event stays in the queue. This source observes; the display server still pumps.
    return true;
}

void Sdl3InputSource::forward(input::DeviceId device, Control control, f32 value,
                              Nanoseconds timestamp) noexcept {
    if (!control.is_valid() || control.code == 0) {
        ++ignored_;
        return;
    }
    input::DeviceEvent event;
    event.timestamp = timestamp;
    event.device = device;
    event.control = control;
    event.value = value;
    event.source = input::EventSource::Physical;
    server_->submit(event);
    ++forwarded_;
}

void Sdl3InputSource::open_gamepad(u32 instance_id, Nanoseconds timestamp) noexcept {
    if (gamepad_count_ == kMaxGamepads || !gamepad(instance_id).is_null()) {
        return;
    }
    SDL_Gamepad* handle = SDL_OpenGamepad(static_cast<SDL_JoystickID>(instance_id));
    if (handle == nullptr) {
        return;
    }
    input::DeviceDescription description;
    description.kind = DeviceKind::Gamepad;
    // The controller's own GUID, which is what makes reconnection restore the pairing — see
    // device.h. It survives unplugging; the instance id does not.
    char guid[33] = {};
    const SDL_GUID id = SDL_GetJoystickGUIDForID(static_cast<SDL_JoystickID>(instance_id));
    SDL_GUIDToString(id, guid, sizeof(guid));
    description.hardware_id = Name::intern(guid);
    const char* name = SDL_GetGamepadName(handle);
    description.display_name = Name::intern(name != nullptr ? name : "gamepad");
    description.capabilities = static_cast<u16>(input::DeviceCapability::Rumble);

    auto connected = server_->devices().connect(description, timestamp);
    if (!connected) {
        SDL_CloseGamepad(handle);
        return;
    }
    gamepads_[gamepad_count_] = GamepadSlot{instance_id, handle, *connected};
    ++gamepad_count_;
}

void Sdl3InputSource::close_gamepad(u32 instance_id, Nanoseconds timestamp) noexcept {
    for (u32 index = 0; index < gamepad_count_; ++index) {
        if (gamepads_[index].instance_id != instance_id) {
            continue;
        }
        server_->devices().disconnect(gamepads_[index].device, timestamp);
        if (gamepads_[index].handle != nullptr) {
            SDL_CloseGamepad(static_cast<SDL_Gamepad*>(gamepads_[index].handle));
        }
        for (u32 shift = index + 1; shift < gamepad_count_; ++shift) {
            gamepads_[shift - 1] = gamepads_[shift];
        }
        --gamepad_count_;
        return;
    }
}

void Sdl3InputSource::on_event(const void* sdl_event) noexcept {
    const auto& event = *static_cast<const SDL_Event*>(sdl_event);
    // SDL3 stamps every event in nanoseconds, at the moment it was observed. Carried through
    // unchanged — `input-and-actions` requires the observation time, not the processing time.
    const auto timestamp = static_cast<Nanoseconds>(event.common.timestamp);

    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            if (event.key.repeat) {
                // A repeat is not a transition: the key never came up. Forwarding it would report a
                // press the player did not make. Repetition is `TriggerKind::Pulse`'s job, on the
                // game's own terms and at the game's own rate.
                ++ignored_;
                return;
            }
            const Key key = key_from_scancode(event.key.scancode);
            forward(keyboard_, input::key_control(key),
                    event.type == SDL_EVENT_KEY_DOWN ? 1.0F : 0.0F, timestamp);
            return;
        }

        case SDL_EVENT_MOUSE_MOTION:
            // Deltas and the absolute position are separate controls, because they are separate
            // interpretations: `xrel` is a displacement that must never be scaled by frame time,
            // and `x` is a position. See `Interpretation` in types.h.
            forward(mouse_, input::mouse_control(MouseControl::MoveX), event.motion.xrel,
                    timestamp);
            forward(mouse_, input::mouse_control(MouseControl::MoveY), -event.motion.yrel,
                    timestamp);
            forward(mouse_, input::mouse_control(MouseControl::PositionX), event.motion.x,
                    timestamp);
            forward(mouse_, input::mouse_control(MouseControl::PositionY), event.motion.y,
                    timestamp);
            return;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            MouseControl control = MouseControl::Unknown;
            switch (event.button.button) {
                case SDL_BUTTON_LEFT:
                    control = MouseControl::Left;
                    break;
                case SDL_BUTTON_RIGHT:
                    control = MouseControl::Right;
                    break;
                case SDL_BUTTON_MIDDLE:
                    control = MouseControl::Middle;
                    break;
                case SDL_BUTTON_X1:
                    control = MouseControl::Extra1;
                    break;
                case SDL_BUTTON_X2:
                    control = MouseControl::Extra2;
                    break;
                default:
                    break;
            }
            forward(mouse_, input::mouse_control(control),
                    event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1.0F : 0.0F, timestamp);
            return;
        }

        case SDL_EVENT_MOUSE_WHEEL:
            forward(mouse_, input::mouse_control(MouseControl::Wheel), event.wheel.y, timestamp);
            forward(mouse_, input::mouse_control(MouseControl::WheelX), event.wheel.x, timestamp);
            return;

        case SDL_EVENT_GAMEPAD_ADDED:
            open_gamepad(static_cast<u32>(event.gdevice.which), timestamp);
            return;

        case SDL_EVENT_GAMEPAD_REMOVED:
            close_gamepad(static_cast<u32>(event.gdevice.which), timestamp);
            return;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            const input::DeviceId device = gamepad(static_cast<u32>(event.gbutton.which));
            if (device.is_null()) {
                ++ignored_;
                return;
            }
            forward(device,
                    input::gamepad_control(
                        button_from_sdl(static_cast<SDL_GamepadButton>(event.gbutton.button))),
                    event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.0F : 0.0F, timestamp);
            return;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const input::DeviceId device = gamepad(static_cast<u32>(event.gaxis.which));
            if (device.is_null()) {
                ++ignored_;
                return;
            }
            const auto axis = static_cast<SDL_GamepadAxis>(event.gaxis.axis);
            const bool trigger =
                axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            f32 value = axis_value(event.gaxis.value, trigger);
            if (axis_is_inverted(axis)) {
                value = -value;
            }
            forward(device, input::gamepad_control(axis_from_sdl(axis)), value, timestamp);
            return;
        }

        default:
            // Window events, quit, text, everything else: not this source's. Counted so that "the
            // platform produced nothing" is distinguishable from "this source ignored it".
            ++ignored_;
            return;
    }
}

}  // namespace cy
