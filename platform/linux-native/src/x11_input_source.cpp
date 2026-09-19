// The X11 input event source. See x11_input_source.h for why it observes rather than pumps, and
// for why it has no gamepads.

#include <cy/platform/x11_input_source.h>

#include <cy/platform/x11_display_server.h>

#include <X11/Xlib.h>

#undef None
#undef Always
#undef Success

#include <linux/input-event-codes.h>

namespace cy {
namespace {

using input::Control;
using input::DeviceKind;
using input::Key;
using input::MouseControl;

// An X11 keycode is the kernel's evdev code plus 8 on every Linux X server since 2009. So the
// physical key this backend translates is `keycode - 8` compared against the kernel's own KEY_*
// numbering, which is the same physical key on every layout — see the header.
constexpr u32 kEvdevOffset = 8;

Key key_from_evdev(u32 code) noexcept {
    // The letter and digit runs are contiguous in the kernel's numbering in the ORDER OF A QWERTY
    // KEYBOARD's rows rather than alphabetically, so unlike SDL's scancodes they cannot be mapped
    // with one subtraction. A table is the honest way to say that.
    switch (code) {
        case KEY_A: return Key::A;
        case KEY_B: return Key::B;
        case KEY_C: return Key::C;
        case KEY_D: return Key::D;
        case KEY_E: return Key::E;
        case KEY_F: return Key::F;
        case KEY_G: return Key::G;
        case KEY_H: return Key::H;
        case KEY_I: return Key::I;
        case KEY_J: return Key::J;
        case KEY_K: return Key::K;
        case KEY_L: return Key::L;
        case KEY_M: return Key::M;
        case KEY_N: return Key::N;
        case KEY_O: return Key::O;
        case KEY_P: return Key::P;
        case KEY_Q: return Key::Q;
        case KEY_R: return Key::R;
        case KEY_S: return Key::S;
        case KEY_T: return Key::T;
        case KEY_U: return Key::U;
        case KEY_V: return Key::V;
        case KEY_W: return Key::W;
        case KEY_X: return Key::X;
        case KEY_Y: return Key::Y;
        case KEY_Z: return Key::Z;
        // The kernel's digit row IS contiguous, and 0 sits after 9 exactly as it does on the
        // keyboard.
        case KEY_1: return Key::Num1;
        case KEY_2: return Key::Num2;
        case KEY_3: return Key::Num3;
        case KEY_4: return Key::Num4;
        case KEY_5: return Key::Num5;
        case KEY_6: return Key::Num6;
        case KEY_7: return Key::Num7;
        case KEY_8: return Key::Num8;
        case KEY_9: return Key::Num9;
        case KEY_0: return Key::Num0;
        case KEY_SPACE: return Key::Space;
        case KEY_ENTER: return Key::Enter;
        case KEY_ESC: return Key::Escape;
        case KEY_TAB: return Key::Tab;
        case KEY_BACKSPACE: return Key::Backspace;
        case KEY_LEFTSHIFT: return Key::LeftShift;
        case KEY_RIGHTSHIFT: return Key::RightShift;
        case KEY_LEFTCTRL: return Key::LeftControl;
        case KEY_RIGHTCTRL: return Key::RightControl;
        case KEY_LEFTALT: return Key::LeftAlt;
        case KEY_RIGHTALT: return Key::RightAlt;
        case KEY_LEFT: return Key::Left;
        case KEY_RIGHT: return Key::Right;
        case KEY_UP: return Key::Up;
        case KEY_DOWN: return Key::Down;
        case KEY_F1: return Key::F1;
        case KEY_F2: return Key::F2;
        case KEY_F3: return Key::F3;
        case KEY_F4: return Key::F4;
        case KEY_F5: return Key::F5;
        case KEY_F6: return Key::F6;
        case KEY_F7: return Key::F7;
        case KEY_F8: return Key::F8;
        case KEY_F9: return Key::F9;
        case KEY_F10: return Key::F10;
        case KEY_F11: return Key::F11;
        case KEY_F12: return Key::F12;
        default: return Key::Unknown;
    }
}

// X11 numbers pointer buttons from 1, and buttons 4 to 7 are the scroll wheel's four directions
// delivered as presses rather than as an axis. That is the protocol, not a quirk of this backend.
MouseControl mouse_button_from_x11(unsigned int button) noexcept {
    switch (button) {
        case Button1: return MouseControl::Left;
        case Button2: return MouseControl::Middle;
        case Button3: return MouseControl::Right;
        case 8: return MouseControl::Extra1;
        case 9: return MouseControl::Extra2;
        default: return MouseControl::Unknown;
    }
}

}  // namespace

X11InputSource::~X11InputSource() {
    detach();
}

Status X11InputSource::attach(X11DisplayServer& display, input::InputServer& server) noexcept {
    if (server_ != nullptr) {
        return ok();
    }
    if (display.native_display() == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "X11 input: the display server is not initialised; bring it up first");
    }

    input::DeviceDescription keyboard;
    keyboard.kind = DeviceKind::Keyboard;
    keyboard.hardware_id = Name::intern("x11/keyboard");
    keyboard.display_name = keyboard.hardware_id;
    keyboard.capabilities = static_cast<u16>(input::DeviceCapability::TextInput);
    auto connected_keyboard = server.devices().connect(keyboard, 0);
    if (!connected_keyboard) {
        return make_unexpected(connected_keyboard.error());
    }

    input::DeviceDescription mouse;
    mouse.kind = DeviceKind::Mouse;
    mouse.hardware_id = Name::intern("x11/mouse");
    mouse.display_name = mouse.hardware_id;
    auto connected_mouse = server.devices().connect(mouse, 0);
    if (!connected_mouse) {
        return make_unexpected(connected_mouse.error());
    }

    keyboard_ = *connected_keyboard;
    mouse_ = *connected_mouse;
    server_ = &server;
    display_ = &display;
    have_last_ = false;
    display.set_event_observer(&X11InputSource::observe, this);
    return ok();
}

void X11InputSource::detach() noexcept {
    if (server_ == nullptr) {
        return;
    }
    if (display_ != nullptr) {
        display_->set_event_observer(nullptr, nullptr);
    }
    display_ = nullptr;
    server_ = nullptr;
}

void X11InputSource::observe(const void* x_event, void* user) noexcept {
    static_cast<X11InputSource*>(user)->on_event(x_event);
}

void X11InputSource::forward(input::DeviceId device, Control control, f32 value,
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

void X11InputSource::on_event(const void* x_event) noexcept {
    const auto& event = *static_cast<const XEvent*>(x_event);

    // X stamps key, button and motion events with the SERVER's time in milliseconds since its own
    // start. That is neither this process's monotonic clock nor a nanosecond figure, so it is
    // converted rather than carried: the engine's timestamps are nanoseconds on one clock, and two
    // clocks in one stream is a latency measurement that means nothing. The millisecond resolution
    // is the protocol's and is the ceiling on what this backend can report — an evdev source would
    // do better, and that is a second finding about doing input through a window system.
    const auto x_time_ms = static_cast<Nanoseconds>(event.xkey.time);
    const Nanoseconds timestamp = x_time_ms * 1'000'000LL;

    switch (event.type) {
        case KeyPress:
        case KeyRelease: {
            const unsigned int keycode = event.xkey.keycode;
            if (keycode < kEvdevOffset) {
                ++ignored_;
                return;
            }
            // X11 DELIVERS AUTO-REPEAT AS A PRESS/RELEASE PAIR with identical timestamps, and this
            // source does not filter it. SDL's does, because SDL marks the repeat and there is a
            // flag to read; X does not mark it at all, and the standard trick — peeking at the next
            // event for a matching KeyPress at the same time — cannot be done from inside an
            // observer that must not consume from the queue. So repeats reach the action layer on
            // this backend and do not on SDL3's. THAT DIFFERENCE IS REAL AND IT IS A FINDING: the
            // interface carries no "this was a repeat" bit, so the two backends cannot agree, and
            // `input-and-actions`' "a repeat is not a transition" cannot be honoured here without
            // one. Recorded rather than hidden behind a heuristic that would drop real keystrokes.
            const Key key = key_from_evdev(keycode - kEvdevOffset);
            forward(keyboard_, input::key_control(key), event.type == KeyPress ? 1.0F : 0.0F,
                    timestamp);
            return;
        }

        case ButtonPress:
        case ButtonRelease: {
            const unsigned int button = event.xbutton.button;
            // Buttons 4 to 7 are the wheel, and only the press half carries information: a release
            // of a wheel "button" is the protocol tidying up after itself.
            if (button >= Button4 && button <= 7) {
                if (event.type != ButtonPress) {
                    ++ignored_;
                    return;
                }
                const bool vertical = button == Button4 || button == Button5;
                const f32 value = (button == Button4 || button == 6) ? 1.0F : -1.0F;
                forward(mouse_,
                        input::mouse_control(vertical ? MouseControl::Wheel : MouseControl::WheelX),
                        value, timestamp);
                return;
            }
            forward(mouse_, input::mouse_control(mouse_button_from_x11(button)),
                    event.type == ButtonPress ? 1.0F : 0.0F, timestamp);
            return;
        }

        case MotionNotify: {
            const i32 x = event.xmotion.x;
            const i32 y = event.xmotion.y;
            // Deltas and the absolute position are separate controls because they are separate
            // interpretations — a displacement must never be scaled by frame time and a position
            // must never be accumulated. X reports only the position, so the delta is derived here,
            // which is the one place that knows the previous one.
            if (have_last_) {
                forward(mouse_, input::mouse_control(MouseControl::MoveX),
                        static_cast<f32>(x - last_x_), timestamp);
                // Y is inverted to match the SDL3 source, which negates SDL's downward-positive
                // delta. Both backends must hand the action layer the same sign or a camera would
                // invert when the platform changed.
                forward(mouse_, input::mouse_control(MouseControl::MoveY),
                        static_cast<f32>(-(y - last_y_)), timestamp);
            }
            last_x_ = x;
            last_y_ = y;
            have_last_ = true;
            forward(mouse_, input::mouse_control(MouseControl::PositionX), static_cast<f32>(x),
                    timestamp);
            forward(mouse_, input::mouse_control(MouseControl::PositionY), static_cast<f32>(y),
                    timestamp);
            return;
        }

        default:
            // Window events, expose, property changes: not this source's. Counted so that "the
            // platform produced nothing" is distinguishable from "this source ignored it".
            ++ignored_;
            return;
    }
}

}  // namespace cy
