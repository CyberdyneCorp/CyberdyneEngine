// SPDX-License-Identifier: MIT
#pragma once
// The X11 input event source: the platform end of `input-and-actions`' boundary, natively. M11.d
// task 4.1.
//
// `input-and-actions` — "The platform boundary": the platform layer produces normalised,
// **timestamped** device events carrying "a high-resolution timestamp, a stable device identifier,
// and a control identifier". Everything above translates them; nothing above sees X11, exactly as
// nothing above sees SDL.
//
// ================================================================================================
// WHY IT OBSERVES THE DISPLAY SERVER'S PUMP RATHER THAN RUNNING ONE
// ================================================================================================
//
// The same reason `Sdl3InputSource` is an event watch: an X connection has ONE event queue, and a
// second `XNextEvent` loop would consume events the display server needed — the failure looks like
// "input works when the window is idle and stops when it is being resized". X11 has no
// `SDL_AddEventWatch`, so `X11DisplayServer` offers `set_event_observer()` instead, and this class
// registers there. The display server still decides what a window event is; this sees every event
// and consumes none.
//
// The cost is the same one and it is stated rather than hidden: the observer runs on the thread
// that calls `pump_events()`, so `attach()` must be called from that thread and the `InputServer`
// it feeds must be the one that thread owns.
//
// ================================================================================================
// WHAT THIS SOURCE DELIBERATELY DOES NOT DO: GAMEPADS
// ================================================================================================
//
// **There are none here, and that is a finding rather than an omission.** A gamepad on Linux is a
// `/dev/input/event*` node found through udev and read with the evdev protocol; it has nothing to
// do with the X server, which is why a headless Linux game can read a controller and cannot open a
// window. SDL3 bundles the two behind one library and this port separates them, which is the more
// honest shape — and it means `input-and-actions`' gamepad requirements are met on Linux by SDL3's
// source and by no other, until an evdev source is written.
//
// `attach()` therefore connects a keyboard and a mouse and reports no gamepad. A caller that needs
// one on this backend finds out by asking `InputServer::devices()`, which is where that question
// belongs.
//
// ================================================================================================
// SCANCODES, NOT KEYSYMS
// ================================================================================================
//
// An X11 keycode on Linux is the kernel's evdev code plus 8 — a PHYSICAL key, independent of the
// layout. A keysym is what the layout says the key produces. Binding WASD to keysyms puts a French
// player's movement on ZQSD-shaped nonsense; binding them to physical keys puts it under the same
// four fingers on every layout. Text entry is the other stream entirely, so nothing is lost.
//
// That mapping assumes an evdev-based X server, which is every Linux X server since 2009 and is
// what the `+8` in every toolkit's keyboard code also assumes. Where it does not hold, the key maps
// to `Key::Unknown` rather than to a wrong key.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/servers/input/server.h>

namespace cy {

class X11DisplayServer;

/// Feeds an `input::InputServer` from an X11 display server's event stream.
///
/// Owns nothing but its registration and the two device identities it created. Constructing one
/// does nothing; `attach()` installs the observer and connects the keyboard and the mouse,
/// `detach()` removes it. Both are idempotent.
class X11InputSource {
public:
    X11InputSource() = default;
    ~X11InputSource();

    X11InputSource(const X11InputSource&) = delete;
    X11InputSource& operator=(const X11InputSource&) = delete;

    /// Install the observer on `display` and register the keyboard and mouse as connected devices.
    ///
    /// Assigning them to a user is the caller's decision, not this class's: `input-and-actions`
    /// requires device assignment to be **explicit**, and a platform layer that assigned the
    /// keyboard to player one would be exactly the assumption the requirement forbids.
    [[nodiscard]] Status attach(X11DisplayServer& display, input::InputServer& server) noexcept;
    void detach() noexcept;

    [[nodiscard]] bool attached() const noexcept { return server_ != nullptr; }

    [[nodiscard]] input::DeviceId keyboard() const noexcept { return keyboard_; }
    [[nodiscard]] input::DeviceId mouse() const noexcept { return mouse_; }

    /// How many events this source has forwarded, and how many it saw and deliberately did not.
    /// Reported so that "no input" can be separated into "the platform produced none" and "the
    /// action layer resolved none" — two very different bugs with the same symptom.
    [[nodiscard]] u64 forwarded() const noexcept { return forwarded_; }
    [[nodiscard]] u64 ignored() const noexcept { return ignored_; }

    /// The `X11EventObserver` trampoline. Public because a function pointer needs a linkage-visible
    /// target and for no other reason; nothing else should call it.
    static void observe(const void* x_event, void* user) noexcept;

private:
    void on_event(const void* x_event) noexcept;
    void forward(input::DeviceId device, input::Control control, f32 value,
                 Nanoseconds timestamp) noexcept;

    input::InputServer* server_ = nullptr;
    X11DisplayServer* display_ = nullptr;
    input::DeviceId keyboard_;
    input::DeviceId mouse_;

    /// The last pointer position seen, because X reports a motion event's position and not its
    /// delta. The first motion after attaching produces no delta rather than a jump from (0, 0) —
    /// a spike of several hundred units is a camera that whips round on the first mouse move.
    i32 last_x_ = 0;
    i32 last_y_ = 0;
    bool have_last_ = false;

    u64 forwarded_ = 0;
    u64 ignored_ = 0;
};

}  // namespace cy
